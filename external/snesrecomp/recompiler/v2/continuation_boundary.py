"""Emitter-side representation of an unresolved-M/X continuation boundary.

A :class:`BoundarySpec` is the emitter's view of the solver's
``ContinuationBoundary``: a call site whose callee *may return* but whose
return M/X is not fully proven.  It asserts reachability of
``continuation_pc24`` while asserting **no** VariantId for it.

The rule this module exists to enforce is a single one:

    known exit modes  -> exact continuation VariantIds
    unknown residual  -> a dynamic transfer decided from the live CPU M/X

Nothing here reconstructs a boundary from emitted C, from a PC list, or from
the decoder's own state.  A table is built only from solver/manifest truth and
installed for the decode + emit passes to consult, exactly the way the
decoder's inline-arg map and codegen's name resolver are installed.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Mapping, Optional


MX = tuple[int, int]

#: Canonical M/X order.  Shared by the table, the emitter and the auditor so a
#: generated switch is byte-identical across runs and machines.
MX_ORDER: tuple[MX, ...] = ((0, 0), (0, 1), (1, 0), (1, 1))


def mx_index(m: int, x: int) -> int:
    """Runtime dispatch index, matching `_cpu_dispatch_lookup` in cpu_state.c."""
    return ((m & 1) << 1) | (x & 1)


def _mirror(pc24: int) -> Optional[int]:
    """The LoROM alias of a 24-bit address, or None when it has none."""
    bank = (pc24 >> 16) & 0xFF
    if bank < 0x40 or 0x80 <= bank < 0xC0:
        return pc24 ^ 0x800000
    return None


@dataclass(frozen=True, order=True)
class BoundarySpec:
    """One declared unresolved-M/X continuation.

    ``proven_modes`` are the callee exit modes the solver actually proved.
    Each is separately a normal exact continuation demand and therefore has (or
    may have) a materialized body.  Everything outside that set is *residue*:
    unproven, and never assigned a VariantId by anyone.
    """

    site_pc24: int
    continuation_pc24: int
    callee_pc24: int
    callee_m: int
    callee_x: int
    proven_modes: frozenset[MX] = frozenset()
    via_exit_state: str = "UNKNOWN"
    kind: str = "direct-call"
    owner: Optional[str] = None
    source_variant: str = ""

    @property
    def key(self) -> tuple[int, int, int, int]:
        return (self.site_pc24, self.callee_pc24, self.callee_m, self.callee_x)

    @property
    def sorted_modes(self) -> tuple[MX, ...]:
        return tuple(mx for mx in MX_ORDER if mx in self.proven_modes)

    @property
    def classification(self) -> str:
        """How this boundary must be emitted.

        A boundary is only ever recorded for a callee that MAY return with an
        unproven exit M/X, so residue is always present; what varies is whether
        any exact mode accompanies it.  ``exact-only`` is therefore not a
        boundary state at all — a fully proven site has no boundary.
        """
        return "exact+residual" if self.proven_modes else "residual-only"


@dataclass
class BoundaryTable:
    """Site-indexed boundaries, mirror-aware, deterministic.

    Lookup identity is (site, callee, callee entry M/X).  The callee entry mode
    is part of the identity because the same static site reached at two caller
    widths invokes two different callee variants, which may have been proven to
    different degrees.
    """

    specs: tuple[BoundarySpec, ...] = ()
    _index: dict = field(default_factory=dict, repr=False)
    _sites: frozenset = field(default_factory=frozenset, repr=False)

    def __post_init__(self) -> None:
        merged: dict[tuple, BoundarySpec] = {}
        for spec in sorted(self.specs):
            existing = merged.get(spec.key)
            if existing is None:
                merged[spec.key] = spec
                continue
            # Same site + same callee variant seen from several source
            # variants.  The proven set is a property of the callee, so the
            # rows agree; take the union rather than silently preferring one.
            merged[spec.key] = BoundarySpec(
                site_pc24=existing.site_pc24,
                continuation_pc24=existing.continuation_pc24,
                callee_pc24=existing.callee_pc24,
                callee_m=existing.callee_m, callee_x=existing.callee_x,
                proven_modes=existing.proven_modes | spec.proven_modes,
                via_exit_state=existing.via_exit_state,
                kind=existing.kind, owner=existing.owner or spec.owner,
                source_variant=existing.source_variant)
        self.specs = tuple(sorted(merged.values()))
        index: dict = {}
        sites: set[int] = set()
        for spec in self.specs:
            for site in _addresses(spec.site_pc24):
                sites.add(site)
                for callee in _addresses(spec.callee_pc24):
                    index[(site, callee, spec.callee_m, spec.callee_x)] = spec
        self._index = index
        self._sites = frozenset(sites)

    def __bool__(self) -> bool:
        return bool(self.specs)

    def __len__(self) -> int:
        return len(self.specs)

    def at(self, site_pc24: int, callee_pc24: int,
           callee_m: int, callee_x: int) -> Optional[BoundarySpec]:
        return self._index.get((site_pc24 & 0xFFFFFF, callee_pc24 & 0xFFFFFF,
                                callee_m & 1, callee_x & 1))

    def has_site(self, site_pc24: int) -> bool:
        """Cheap pre-filter; a true answer still needs the exact `at` lookup."""
        return (site_pc24 & 0xFFFFFF) in self._sites

    def at_site(self, site_pc24: int) -> tuple[BoundarySpec, ...]:
        site = site_pc24 & 0xFFFFFF
        return tuple(spec for spec in self.specs
                     if site in _addresses(spec.site_pc24))


def _addresses(pc24: int) -> tuple[int, ...]:
    pc24 &= 0xFFFFFF
    mirror = _mirror(pc24)
    return (pc24,) if mirror is None else (pc24, mirror)


def _parse_mode(label: str) -> Optional[MX]:
    if len(label) == 4 and label[0] == "M" and label[2] == "X":
        return (int(label[1]) & 1, int(label[3]) & 1)
    return None


def _parse_variant(label: str) -> Optional[tuple[int, int, int]]:
    parts = label.split("_")
    if len(parts) != 2:
        return None
    mode = _parse_mode(parts[1])
    if mode is None:
        return None
    try:
        return (int(parts[0], 16), mode[0], mode[1])
    except ValueError:
        return None


def from_manifest(document: Mapping) -> BoundaryTable:
    """Build the table from an authoritative manifest's boundary section.

    The rows are the manifest's own ``unresolved_mx_continuations``; no address
    is spelled out here and nothing is inferred from the entry list.
    """
    specs = []
    for row in document.get("unresolved_mx_continuations", ()):
        callee = _parse_variant(row["via_target"])
        if callee is None:
            continue
        modes = frozenset(
            mode for mode in (_parse_mode(v) for v in row.get("proven_modes", ()))
            if mode is not None)
        specs.append(BoundarySpec(
            site_pc24=int(row["site_pc24"], 16),
            continuation_pc24=int(row["pc24"], 16),
            callee_pc24=callee[0], callee_m=callee[1], callee_x=callee[2],
            proven_modes=modes,
            via_exit_state=row.get("via_exit_state", "UNKNOWN"),
            kind=row.get("kind", "direct-call"),
            owner=row.get("owner"),
            source_variant=row.get("source_variant", "")))
    return BoundaryTable(tuple(specs))


def from_solver(solver) -> BoundaryTable:
    """Build the table straight from a solved :class:`MxAnalysisSolver`."""
    specs = []
    for boundary in solver.continuation_boundaries():
        specs.append(BoundarySpec(
            site_pc24=boundary.site_pc24,
            continuation_pc24=boundary.pc24,
            callee_pc24=boundary.via_target.pc24,
            callee_m=boundary.via_target.m, callee_x=boundary.via_target.x,
            proven_modes=frozenset(boundary.proven_modes),
            via_exit_state=boundary.via_exit_state,
            kind=boundary.kind))
    return BoundaryTable(tuple(specs))


# ── Installed table ────────────────────────────────────────────────────────
# Installed the same way the decoder's inline-arg map and codegen's name
# resolver are: a process-local, explicitly-set truth, cleared by passing None.
# `generation` participates in the decode cache key so an install can never be
# masked by a graph decoded under a different table.

_ACTIVE: BoundaryTable = BoundaryTable(())
_GENERATION = 0


def set_continuation_boundaries(table: Optional[BoundaryTable]) -> None:
    global _ACTIVE, _GENERATION
    _ACTIVE = table if table is not None else BoundaryTable(())
    _GENERATION += 1


def active_boundaries() -> BoundaryTable:
    return _ACTIVE


def generation() -> int:
    return _GENERATION


def boundary_at(site_pc24: int, callee_pc24: int,
                callee_m: int, callee_x: int) -> Optional[BoundarySpec]:
    """The declared boundary for a call site, or None. The only entry point
    the decoder and emitter use — neither ever inspects the table directly."""
    if not _ACTIVE:
        return None
    return _ACTIVE.at(site_pc24, callee_pc24, callee_m, callee_x)


def continuation_keys(spec: BoundarySpec, p_stack=()) -> tuple:
    """The exact continuation decode keys a boundary licenses, in canonical
    order — one per proven mode, none for a residual-only boundary."""
    from v2.decoder import DecodeKey
    return tuple(DecodeKey(spec.continuation_pc24 & 0xFFFFFF, m, x, p_stack)
                 for m, x in spec.sorted_modes)


def audit_rows(table: BoundaryTable) -> list[dict]:
    """Deterministic, serialisable description of every boundary."""
    return [{
        "site_pc24": f"{spec.site_pc24:06X}",
        "continuation_pc24": f"{spec.continuation_pc24:06X}",
        "callee": f"{spec.callee_pc24:06X}_M{spec.callee_m}X{spec.callee_x}",
        "via_exit_state": spec.via_exit_state,
        "kind": spec.kind,
        "owner": spec.owner,
        "source_variant": spec.source_variant,
        "proven_modes": [f"M{m}X{x}" for m, x in spec.sorted_modes],
        "classification": spec.classification,
    } for spec in table.specs]
