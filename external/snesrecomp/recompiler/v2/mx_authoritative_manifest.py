"""Deterministic materialization decisions derived from M/X solver truth.

This module is intentionally downstream of :mod:`mx_analysis_solver`.  It may
deny materialization, but it never changes a VariantId or chooses a sibling.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
import json
from typing import Iterable, Mapping, Optional

from v2.mx_analysis_solver import VariantId


MX = tuple[int, int]


class MaterializationStatus(Enum):
    MATERIALIZE_EXACT = "MATERIALIZE_EXACT"
    INTERNAL_ONLY = "INTERNAL_ONLY"
    HOST_ENTRY = "HOST_ENTRY"
    INTERPRETER_FALLBACK = "INTERPRETER_FALLBACK"


@dataclass(frozen=True)
class OwnerInfo:
    pc24: int
    end: Optional[int]
    symbol: str
    entry_kind: str
    declared_modes: frozenset[MX]
    strict_mx: bool = False
    aot_entry_pc: Optional[int] = None

    def contains(self, pc24: int) -> bool:
        return (pc24 >> 16) == (self.pc24 >> 16) and self.end is not None \
            and (self.pc24 & 0xffff) <= (pc24 & 0xffff) < self.end


@dataclass(frozen=True)
class MaterializationPolicy:
    """Post-analysis policy. An absent allowed_modes map permits exact code."""

    allowed_modes: Optional[Mapping[int, frozenset[MX]]] = None

    def permits(self, variant: VariantId) -> bool:
        if self.allowed_modes is None:
            return True
        modes = self.allowed_modes.get(variant.pc24)
        return modes is None or (variant.m, variant.x) in modes


def _variant_from_record(record: Mapping) -> VariantId:
    value = record["variant_id"]
    return VariantId(int(value["pc24"], 16), int(value["m"]), int(value["x"]))


def _unresolved_continuations(records: Iterable[Mapping],
                              entries_by_pc: Mapping[int, "OwnerInfo"],
                              owner_list) -> list[dict]:
    """Continuations proven reachable whose entry M/X is NOT proven.

    These are deliberately kept OUT of `entries`: an entry is keyed by a
    VariantId, and the whole point of a boundary is that no VariantId is
    justified.  Materializing one, or picking a nearest survivor for it,
    would re-introduce exactly the assumption the solver refused to make.

    A consumer must transfer control here through a runtime exact-M/X
    dispatch over `proven_modes` (each of which IS a normal manifest entry),
    falling back to the interpreter for the unproven residue.
    """
    rows: dict[tuple, dict] = {}
    for record in records:
        source = _variant_from_record(record).label
        for boundary in record.get("continuation_boundaries", ()):
            pc24 = int(boundary["pc24"], 16)
            owner = entries_by_pc.get(pc24) or next(
                (o for o in owner_list if o.contains(pc24)), None)
            key = (boundary["pc24"], boundary["site_pc24"],
                   boundary["via_target"], source)
            rows[key] = {
                "pc24": boundary["pc24"],
                "site_pc24": boundary["site_pc24"],
                "source_variant": source,
                "via_target": boundary["via_target"],
                "via_exit_state": boundary["via_exit_state"],
                "kind": boundary["kind"],
                "proven_modes": list(boundary["proven_modes"]),
                "owner": owner.symbol if owner else None,
                "resolution": "runtime_exact_mx_dispatch_or_interpreter",
                "reason": ("callee may return but its exit M/X is not proven; "
                           "no VariantId is justified at this continuation"),
            }
    return [rows[k] for k in sorted(rows)]


def build_manifest(fact_records: Iterable[Mapping], owners: Iterable[OwnerInfo],
                   policy: MaterializationPolicy = MaterializationPolicy(),
                   continuation_policy: Optional[str] = None) -> dict:
    """Return a stable manifest; semantic demands are never canonicalized.

    ``continuation_policy`` is copied verbatim from the fact artifact so the
    semantic policy the manifest derives from is explicit, never a default.
    """
    owner_list = sorted(owners, key=lambda o: (o.pc24, o.symbol))
    entries_by_pc = {owner.pc24: owner for owner in owner_list}
    incoming: dict[str, list[dict]] = {}
    records = list(fact_records)
    for source in records:
        source_id = _variant_from_record(source).label
        for demand in source.get("demands", ()):
            incoming.setdefault(demand["target"], []).append({
                "source_variant": source_id,
                **demand,
            })

    entries = []
    for record in sorted(records, key=lambda r: _variant_from_record(r)):
        variant = _variant_from_record(record)
        exact_owner = entries_by_pc.get(variant.pc24)
        containing = exact_owner or next(
            (owner for owner in owner_list if owner.contains(variant.pc24)), None)
        denied = not policy.permits(variant)
        if containing is None or denied:
            status = MaterializationStatus.INTERPRETER_FALLBACK
            reason = ("materialization denied by policy; semantic demand preserved"
                      if denied else
                      "semantic target has no static CFG owner; runtime fallback required")
            kind = "external_or_runtime"
        elif exact_owner is None:
            status = MaterializationStatus.INTERNAL_ONLY
            reason = "demand is an internal label decoded within its owning entry"
            kind = "internal_label"
        elif exact_owner.aot_entry_pc is not None:
            status = MaterializationStatus.HOST_ENTRY
            reason = "exact CFG variant is an explicit host/AOT entry"
            kind = exact_owner.entry_kind
        else:
            status = MaterializationStatus.MATERIALIZE_EXACT
            reason = "exact semantic demand has a static CFG entry owner"
            kind = exact_owner.entry_kind

        exit_fact = record["exit"]
        provenance = sorted(set(record.get("origins", ())))
        sources = sorted(incoming.get(variant.label, ()), key=lambda d: (
            d["source_variant"], d.get("site_pc24", ""), d.get("kind", ""),
            d.get("via_target", "")))
        entries.append({
            "pc24": f"{variant.pc24:06X}", "M": variant.m, "X": variant.x,
            "symbol": (f"{containing.symbol}_M{variant.m}X{variant.x}"
                       if exact_owner is not None else None),
            "owner": containing.symbol if containing else None,
            "source": sources,
            "provenance": provenance,
            "entry_kind": kind,
            "materialization_status": status.value,
            "reason": reason,
            "exit": exit_fact,
            "policy": {
                "strict_mx_declared": bool(containing and containing.strict_mx),
                "declared_modes": ([f"M{m}X{x}" for m, x in
                                    sorted(containing.declared_modes)]
                                   if containing else []),
                "exact_permitted": not denied,
                "semantic_variant_rewritten": False,
            },
        })

    counts = {status.value: 0 for status in MaterializationStatus}
    for entry in entries:
        counts[entry["materialization_status"]] += 1
    return {
        "schema": 1,
        "ordering": "pc24,M,X",
        "semantic_source": "mx_interprocedural_solver",
        "continuation_policy": continuation_policy,
        "materialization_policy": "exact_or_explicit_fallback; strict_mx advisory",
        "entries": entries,
        "unresolved_mx_continuations": _unresolved_continuations(
            records, entries_by_pc, owner_list),
        "statistics": {"total_entries": len(entries), **counts,
                       "unresolved_mx_continuations": len(
                           _unresolved_continuations(records, entries_by_pc,
                                                     owner_list))},
    }


def manifest_json(document: Mapping) -> str:
    return json.dumps(document, indent=2, sort_keys=False) + "\n"


def exact_static_variants(document: Mapping) -> frozenset[VariantId]:
    statuses = {MaterializationStatus.MATERIALIZE_EXACT.value,
                MaterializationStatus.HOST_ENTRY.value}
    return frozenset(VariantId(int(e["pc24"], 16), e["M"], e["X"])
                     for e in document["entries"]
                     if e["materialization_status"] in statuses)


def resolve_exact_or_fallback(document: Mapping, requested: VariantId):
    """Return the identical VariantId when static, otherwise None. Never nearest."""
    return requested if requested in exact_static_variants(document) else None
