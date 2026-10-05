"""Demand-driven, analysis-only interprocedural M/X solver.

This module deliberately has no dependency on code generation, strict_mx,
auto-promotion, pruning, dispatch, or the AOT registry.  Its fact graph is the
semantic source of the authoritative variant manifest consumed by the strict
materialization pipeline.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
import heapq
import json
import time
from typing import Callable, Dict, FrozenSet, Iterable, Mapping, Optional, Tuple

from v2.decoder import DecodeKey, decode_function, post_state
from snes65816 import ABS


MX = Tuple[int, int]


@dataclass(frozen=True, order=True)
class VariantId:
    pc24: int
    m: int
    x: int

    def __post_init__(self):
        if not (0 <= self.pc24 <= 0xFFFFFF and self.m in (0, 1)
                and self.x in (0, 1)):
            raise ValueError("invalid VariantId")

    @property
    def label(self) -> str:
        return f"{self.pc24:06X}_M{self.m}X{self.x}"


class FactState(Enum):
    UNDISCOVERED = "UNDISCOVERED"
    UNKNOWN = "UNKNOWN"
    EXACT = "EXACT"
    SET = "SET"
    NO_EXIT = "NO_EXIT"
    POISON = "POISON"


@dataclass(frozen=True)
class ExitFact:
    state: FactState
    modes: FrozenSet[MX] = frozenset()
    reason: Optional[str] = None

    @staticmethod
    def undiscovered() -> "ExitFact":
        return ExitFact(FactState.UNDISCOVERED)

    @staticmethod
    def known(modes: Iterable[MX]) -> "ExitFact":
        value = frozenset((m & 1, x & 1) for m, x in modes)
        if not value:
            return ExitFact.undiscovered()
        return ExitFact(FactState.EXACT if len(value) == 1 else FactState.SET,
                        value)

    @staticmethod
    def unknown(reason: str, modes: Iterable[MX] = ()) -> "ExitFact":
        return ExitFact(FactState.UNKNOWN,
                        frozenset((m & 1, x & 1) for m, x in modes), reason)

    @staticmethod
    def no_exit(reason: str) -> "ExitFact":
        return ExitFact(FactState.NO_EXIT, reason=reason)

    @staticmethod
    def poison(reason: str, modes: Iterable[MX] = ()) -> "ExitFact":
        return ExitFact(FactState.POISON,
                        frozenset((m & 1, x & 1) for m, x in modes), reason)


@dataclass(frozen=True, order=True)
class Demand:
    target: VariantId
    kind: str
    site_pc24: int
    target_set_source: str = "direct"
    via_target: Optional[VariantId] = None
    via_exit_state: Optional[str] = None


@dataclass(frozen=True, order=True)
class ContinuationBoundary:
    """A continuation whose entry M/X could not be proven statically.

    Recorded when a call site's callee *may return* but its exit M/X is not
    fully determined.  It asserts reachability of ``pc24`` without asserting
    any VariantId: ``proven_modes`` are the exit modes actually proven (and
    separately demanded as exact continuations); the residue is unproven and
    belongs to a runtime exact-M/X dispatch or interpreter boundary.

    This is deliberately NOT a VariantId and never becomes one implicitly.
    """
    pc24: int
    site_pc24: int
    via_target: VariantId
    via_exit_state: str
    proven_modes: FrozenSet[MX] = frozenset()
    kind: str = "direct-call"

    @property
    def label(self) -> str:
        return f"{self.pc24:06X}@{self.site_pc24:06X}"


@dataclass
class Derivation:
    exits: set[MX] = field(default_factory=set)
    demands: set[Demand] = field(default_factory=set)
    dependencies: set[VariantId] = field(default_factory=set)
    unknown_reasons: set[str] = field(default_factory=set)
    poison_reasons: set[str] = field(default_factory=set)
    no_exit_reasons: set[str] = field(default_factory=set)
    boundaries: set[ContinuationBoundary] = field(default_factory=set)

    def fact(self) -> ExitFact:
        if self.poison_reasons:
            return ExitFact.poison("; ".join(sorted(self.poison_reasons)), self.exits)
        if self.unknown_reasons:
            return ExitFact.unknown("; ".join(sorted(self.unknown_reasons)), self.exits)
        if self.exits:
            return ExitFact.known(self.exits)
        if self.no_exit_reasons:
            return ExitFact.no_exit("; ".join(sorted(self.no_exit_reasons)))
        return ExitFact.known(self.exits)


@dataclass(frozen=True)
class Seed:
    variant: VariantId
    origin: str
    host_reentry: bool = False


@dataclass
class NodeRecord:
    variant: VariantId
    origins: set[str] = field(default_factory=set)
    host_reentry: bool = False
    exit_fact: ExitFact = field(default_factory=ExitFact.undiscovered)
    demands: set[Demand] = field(default_factory=set)
    dependencies: set[VariantId] = field(default_factory=set)
    boundaries: set[ContinuationBoundary] = field(default_factory=set)
    process_count: int = 0


class MxAnalysisSolver:
    """Deterministic least-fixpoint worklist over a finite VariantId domain."""

    def __init__(self, analyze: Callable[[VariantId, Mapping[VariantId, ExitFact]], Derivation]):
        self._analyze = analyze
        self.nodes: Dict[VariantId, NodeRecord] = {}
        self.reverse_dependencies: Dict[VariantId, set[VariantId]] = {}
        self._heap: list[VariantId] = []
        self._queued: set[VariantId] = set()
        self.worklist_pops = 0
        self.elapsed_seconds = 0.0

    def _queue(self, variant: VariantId) -> None:
        if variant not in self._queued:
            heapq.heappush(self._heap, variant)
            self._queued.add(variant)

    def add_seed(self, seed: Seed) -> None:
        node = self.nodes.setdefault(seed.variant, NodeRecord(seed.variant))
        node.origins.add(seed.origin)
        node.host_reentry |= seed.host_reentry
        self._queue(seed.variant)

    def _demand(self, source: VariantId, demand: Demand) -> None:
        is_new = demand.target not in self.nodes
        target = self.nodes.setdefault(demand.target, NodeRecord(demand.target))
        target.origins.add(
            f"{demand.kind} from {source.label} at {demand.site_pc24:06X}")
        if is_new or target.process_count == 0:
            self._queue(demand.target)

    @staticmethod
    def _merge_fact(old: ExitFact, new: ExitFact) -> ExitFact:
        """Monotone finite-domain join; analysis facts never oscillate."""
        modes = old.modes | new.modes
        if old.state == FactState.POISON:
            return ExitFact.poison(old.reason or "poison", modes)
        if new.state == FactState.POISON:
            return ExitFact.poison(new.reason or "poison", modes)
        if old.state == FactState.UNKNOWN:
            return ExitFact.unknown(old.reason or "unknown", modes)
        if new.state == FactState.UNKNOWN:
            return ExitFact.unknown(new.reason or "unknown", modes)
        if old.state == FactState.NO_EXIT and new.state == FactState.NO_EXIT:
            return ExitFact.no_exit(old.reason or new.reason or "no normal exit")
        if old.state == FactState.NO_EXIT:
            return new if new.modes else old
        if new.state == FactState.NO_EXIT:
            return old if old.modes else new
        return ExitFact.known(modes)

    def solve(self) -> "MxAnalysisSolver":
        started = time.perf_counter()
        while self._heap:
            variant = heapq.heappop(self._heap)
            self._queued.remove(variant)
            self.worklist_pops += 1
            node = self.nodes[variant]
            node.process_count += 1
            derived = self._analyze(
                variant, {key: rec.exit_fact for key, rec in self.nodes.items()})

            for old in node.dependencies - derived.dependencies:
                self.reverse_dependencies.get(old, set()).discard(variant)
            for dep in derived.dependencies:
                self.reverse_dependencies.setdefault(dep, set()).add(variant)
            node.dependencies = set(derived.dependencies)
            node.demands = set(derived.demands)
            node.boundaries = set(derived.boundaries)
            for demand in sorted(derived.demands):
                self._demand(variant, demand)

            new_fact = self._merge_fact(node.exit_fact, derived.fact())
            if new_fact != node.exit_fact:
                node.exit_fact = new_fact
                for caller in sorted(self.reverse_dependencies.get(variant, ())):
                    self._queue(caller)

        self.elapsed_seconds = time.perf_counter() - started
        return self

    def to_dict(self) -> dict:
        variants = []
        for key in sorted(self.nodes):
            node = self.nodes[key]
            variants.append({
                "variant_id": {"pc24": f"{key.pc24:06X}", "m": key.m, "x": key.x},
                "origins": sorted(node.origins),
                "host_reentry": node.host_reentry,
                "exit": {
                    "state": node.exit_fact.state.value,
                    "modes": [f"M{m}X{x}" for m, x in sorted(node.exit_fact.modes)],
                    **({"reason": node.exit_fact.reason} if node.exit_fact.reason else {}),
                },
                "demands": [{"target": d.target.label, "kind": d.kind,
                             "site_pc24": f"{d.site_pc24:06X}",
                             "target_set_source": d.target_set_source,
                             "entry_mx": f"M{d.target.m}X{d.target.x}",
                             **({"via_target": d.via_target.label,
                                "via_exit_state": d.via_exit_state}
                                if d.via_target is not None else {})}
                            for d in sorted(node.demands)],
                "dependencies": [d.label for d in sorted(node.dependencies)],
                "continuation_boundaries": [
                    {"pc24": f"{b.pc24:06X}", "site_pc24": f"{b.site_pc24:06X}",
                     "via_target": b.via_target.label,
                     "via_exit_state": b.via_exit_state, "kind": b.kind,
                     "proven_modes": [f"M{m}X{x}" for m, x in sorted(b.proven_modes)]}
                    for b in sorted(node.boundaries)],
                "process_count": node.process_count,
            })
        return {"schema": 1, "analysis_only": True,
                "ordering": "pc24,m,x",
                # The semantic policy is part of the fact artifact, so no
                # consumer ever has to infer it from a default.
                "continuation_policy": getattr(
                    self._analyze, "continuation_policy", None),
                "variants": variants,
                "reverse_dependencies": [
                    {"callee": callee.label,
                     "callers": [v.label for v in sorted(callers)]}
                    for callee, callers in sorted(self.reverse_dependencies.items())
                    if callers],
                "statistics": self.statistics()}

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), indent=2, sort_keys=False) + "\n"

    def continuation_boundaries(self) -> list["ContinuationBoundary"]:
        """Every unresolved-M/X continuation, deterministically ordered."""
        found: set[ContinuationBoundary] = set()
        for node in self.nodes.values():
            found |= node.boundaries
        return sorted(found)

    def statistics(self) -> dict:
        by_pc: Dict[int, int] = {}
        by_mx = {f"M{m}X{x}": 0 for m in (0, 1) for x in (0, 1)}
        states = {state.value: 0 for state in FactState}
        for key, node in self.nodes.items():
            by_pc[key.pc24] = by_pc.get(key.pc24, 0) + 1
            by_mx[f"M{key.m}X{key.x}"] += 1
            states[node.exit_fact.state.value] += 1
        counts = [n.process_count for n in self.nodes.values()]
        return {
            "pcs": len(by_pc), "variants": len(self.nodes),
            "multi_variant_pcs": sum(v > 1 for v in by_pc.values()),
            "max_variants_per_pc": max(by_pc.values(), default=0),
            "variants_by_mx": by_mx, "exit_states": states,
            "worklist_pops": self.worklist_pops,
            "reprocessings": sum(max(0, n - 1) for n in counts),
            "max_process_count": max(counts, default=0),
            "reverse_dependency_edges": sum(map(len, self.reverse_dependencies.values())),
            "host_reentry_roots": sum(n.host_reentry for n in self.nodes.values()),
            "continuation_boundaries": len(self.continuation_boundaries()),
            "continuation_boundary_pcs": len(
                {b.pc24 for b in self.continuation_boundaries()}),
        }


@dataclass(frozen=True)
class RomEntry:
    variant: VariantId
    decode_bank: int
    end: Optional[int]
    entry_kind: str


class RomAnalyzer:
    """Adapter from existing decoder/CFG facts to the analysis-only solver."""

    #: Candidate policies for "callee may return, exit M/X not proven".
    #: ``stop``     legacy: truncate demand discovery at the call site.
    #: ``preserve`` assume the callee preserved the caller M/X.
    #: ``all_four`` demand the continuation at all four M/X.
    #: ``residual`` demand the continuation at each *proven* exit mode and
    #:              record the unproven residue as a ContinuationBoundary.
    CONTINUATION_POLICIES = ("stop", "preserve", "all_four", "residual")
    #: The normative policy.  ``stop`` stays selectable only explicitly, as a
    #: compatibility/debug mode and historical regression oracle.
    NORMATIVE_CONTINUATION_POLICY = "residual"

    def __init__(self, rom: bytes, entries: Iterable[RomEntry], *,
                 dispatch_helpers=None, indirect_call_tables=None,
                 indirect_dispatch=None, data_regions=None,
                 continuation_policy: str = NORMATIVE_CONTINUATION_POLICY):
        if continuation_policy not in self.CONTINUATION_POLICIES:
            raise ValueError(f"unknown continuation policy {continuation_policy!r}")
        self.continuation_policy = continuation_policy
        self.rom = rom
        self.entries = {entry.variant.pc24: entry for entry in entries}
        self.siblings: Dict[int, set[int]] = {}
        for entry in entries:
            self.siblings.setdefault(entry.decode_bank, set()).add(
                entry.variant.pc24 & 0xFFFF)
        self.dispatch_helpers = dispatch_helpers
        self.indirect_call_tables = indirect_call_tables
        self.indirect_dispatch = indirect_dispatch
        self.data_regions = data_regions
        self.indirect_events: set[tuple] = set()

    def _register_continuation(self, pc24: int, owner: RomEntry) -> None:
        """Make a call fallthrough independently decodable at any proven M/X."""
        self.entries.setdefault(pc24, RomEntry(
            VariantId(pc24, 0, 0), owner.decode_bank, owner.end, "continuation"))

    def _dispatch_targets(self, ins, key):
        entries = getattr(ins, "dispatch_entries", None)
        if entries is None:
            return ()
        kind = getattr(ins, "dispatch_kind", "short")
        bank = (ins.addr >> 16) & 0xFF
        result = []
        for raw in entries:
            if not raw:
                continue
            pc = raw & 0xFFFFFF if kind == "long" else (bank << 16) | (raw & 0xFFFF)
            result.append(VariantId(self._logical_pc(pc), key.m, key.x))
        return tuple(sorted(set(result)))

    @staticmethod
    def _logical_pc(pc24: int) -> int:
        bank = (pc24 >> 16) & 0xFF
        return (((bank ^ 0x80) if bank < 0x40 else bank) << 16) | (pc24 & 0xFFFF)

    @classmethod
    def _direct_target(cls, ins) -> Optional[int]:
        if ins.mnem == "JSR" and ins.length == 3 and ins.mode == ABS:
            return cls._logical_pc(
                (ins.addr & 0xFF0000) | (ins.operand & 0xFFFF))
        if ins.mnem == "JSL":
            return cls._logical_pc(ins.operand & 0xFFFFFF)
        return None

    def _unproven_continuation(self, out: "Derivation", entry: RomEntry,
                               ins, key: DecodeKey, callee: VariantId,
                               fact: ExitFact) -> None:
        """Handle "callee MAY RETURN but its exit M/X is not fully proven".

        Returnability and return-M/X knowledge are distinct facts.  ``UNKNOWN``
        and ``POISON`` both assert *may-return*; neither is ``NO_EXIT``, and
        neither licenses assuming the caller's M/X survived the call.  What the
        policies differ on is only how the continuation is represented.
        """
        policy = self.continuation_policy
        if policy == "stop":
            return
        return_pc = self._logical_pc((ins.addr + ins.length) & 0xFFFFFF)
        site = self._logical_pc(ins.addr & 0xFFFFFF)
        self._register_continuation(return_pc, entry)
        if policy == "preserve":
            post_m, post_x, _ = post_state(ins, key.m, key.x, key.p_stack)
            modes = [(post_m, post_x)]
            residue = frozenset()
        elif policy == "all_four":
            modes = [(m, x) for m in (0, 1) for x in (0, 1)]
            residue = frozenset()
        else:  # residual
            # Exactly the rule the indirect-JSR path already uses (R2): the
            # proven modes become exact continuations; the unproven residue
            # stays explicit instead of being widened or discarded.
            modes = sorted(fact.modes)
            residue = frozenset()
        for mode in sorted(modes):
            continuation = VariantId(return_pc, *mode)
            out.demands.add(Demand(continuation, "call-continuation", site,
                                   "direct", callee, fact.state.value))
            out.dependencies.add(continuation)
        if policy == "residual":
            out.boundaries.add(ContinuationBoundary(
                return_pc, site, callee, fact.state.value,
                frozenset(fact.modes), "direct-call"))

    def __call__(self, variant: VariantId,
                 facts: Mapping[VariantId, ExitFact]) -> Derivation:
        entry = self.entries.get(variant.pc24)
        if entry is None:
            return Derivation(unknown_reasons={"target has no CFG entry"})
        exact = {}
        mode_sets = {}
        partial = self.continuation_policy != "stop"
        for key, fact in facts.items():
            if fact.state == FactState.EXACT:
                exact[(key.pc24, key.m, key.x)] = next(iter(fact.modes))
                exact[(key.pc24 ^ 0x800000, key.m, key.x)] = next(iter(fact.modes))
            elif fact.state == FactState.SET:
                mode_sets[(key.pc24, key.m, key.x)] = fact.modes
                mode_sets[(key.pc24 ^ 0x800000, key.m, key.x)] = fact.modes
            elif partial and fact.modes and fact.state in (FactState.UNKNOWN,
                                                           FactState.POISON):
                # Proven-reachable exit modes of an otherwise unproven callee.
                # Feeding them keeps the solver's decode of the continuation
                # identical to the emitter's; the *unproven* residue is carried
                # separately as a ContinuationBoundary, never as a fact.
                mode_sets[(key.pc24, key.m, key.x)] = fact.modes
                mode_sets[(key.pc24 ^ 0x800000, key.m, key.x)] = fact.modes
        bank = entry.decode_bank
        try:
            graph = decode_function(
                self.rom, bank, variant.pc24 & 0xFFFF, variant.m, variant.x,
                end=entry.end, dispatch_helpers=self.dispatch_helpers,
                indirect_call_tables=self.indirect_call_tables,
                indirect_dispatch=self.indirect_dispatch,
                data_regions=self.data_regions, callee_exit_mx=exact,
                callee_exit_mx_modes=mode_sets,
                sibling_entry_pcs=self.siblings.get(bank, set()) - {variant.pc24 & 0xFFFF})
        except Exception as exc:
            return Derivation(poison_reasons={f"decode failed: {type(exc).__name__}: {exc}"})

        out = Derivation()
        if graph.unresolved_indirects or graph.suppressed_indirect_calls:
            out.unknown_reasons.add("unresolved or suppressed indirect control flow")
        pending = [graph.entry]
        seen = set()
        while pending:
            key = min(pending, key=lambda k: (k.pc, k.m, k.x, k.p_stack))
            pending.remove(key)
            if key in seen or key not in graph.insns:
                continue
            seen.add(key)
            di = graph.insns[key]
            ins = di.insn
            dispatch_targets = self._dispatch_targets(ins, key)
            if dispatch_targets:
                site = self._logical_pc(ins.addr & 0xFFFFFF)
                is_call = ins.mnem == "JSR" or bool(getattr(ins, "dispatch_call", False))
                edge_kind = "indirect_jsr" if is_call else "indirect_tail"
                source = ("cfg indirect_dispatch" if self.indirect_dispatch
                          and (ins.addr & 0xFFFFFF) in self.indirect_dispatch
                          else "decoder recovered target set")
                usable = []
                for target in dispatch_targets:
                    classification = "CFG_ENTRY" if target.pc24 in self.entries else "NO_CFG_ENTRY"
                    self.indirect_events.add((variant.label, site, target.label, edge_kind,
                                              source, classification))
                    if target.pc24 not in self.entries:
                        out.unknown_reasons.add(
                            f"indirect target has no CFG entry {target.label}")
                        continue
                    usable.append(target)
                    out.demands.add(Demand(target, edge_kind, site, source))
                    out.dependencies.add(target)
                known_modes = set()
                has_pending = False
                has_no_exit = False
                for target in usable:
                    fact = facts.get(target, ExitFact.undiscovered())
                    if fact.state in (FactState.EXACT, FactState.SET):
                        known_modes.update(fact.modes)
                    elif fact.state == FactState.NO_EXIT:
                        has_no_exit = True
                    elif fact.state == FactState.UNDISCOVERED:
                        has_pending = True
                    elif fact.state == FactState.UNKNOWN:
                        out.unknown_reasons.add(f"unknown indirect exit {target.label}")
                    else:
                        out.poison_reasons.add(f"poison indirect target {target.label}")
                if not is_call:
                    out.exits.update(known_modes)
                    if has_no_exit:
                        out.no_exit_reasons.add("indirect tail target has no normal exit")
                    continue
                return_pc = self._logical_pc((ins.addr + ins.length) & 0xFFFFFF)
                self._register_continuation(return_pc, entry)
                for target in usable:
                    fact = facts.get(target, ExitFact.undiscovered())
                    if fact.state in (FactState.EXACT, FactState.SET,
                                      FactState.UNKNOWN, FactState.POISON):
                        for mode in sorted(fact.modes):
                            continuation = VariantId(return_pc, *mode)
                            out.demands.add(Demand(
                                continuation, "indirect-continuation", site, source,
                                target, fact.state.value))
                            out.dependencies.add(continuation)
                            continuation_fact = facts.get(
                                continuation, ExitFact.undiscovered())
                            if continuation_fact.state in (FactState.EXACT, FactState.SET):
                                out.exits.update(continuation_fact.modes)
                            elif continuation_fact.state == FactState.UNKNOWN:
                                out.exits.update(continuation_fact.modes)
                                out.unknown_reasons.add(
                                    f"unknown indirect continuation {continuation.label}")
                            elif continuation_fact.state == FactState.POISON:
                                out.exits.update(continuation_fact.modes)
                                out.poison_reasons.add(
                                    f"poison indirect continuation {continuation.label}")
                            elif continuation_fact.state == FactState.NO_EXIT:
                                out.no_exit_reasons.add(
                                    f"indirect continuation has no normal exit {continuation.label}")
                if not known_modes:
                    if has_no_exit:
                        out.no_exit_reasons.add("all indirect callees have no normal exit")
                    continue
                continue
            target_pc = self._direct_target(ins)
            if target_pc is not None:
                post_m, post_x, _ = post_state(ins, key.m, key.x, key.p_stack)
                callee = VariantId(target_pc, post_m, post_x)
                out.demands.add(Demand(callee, "call-entry",
                                       self._logical_pc(ins.addr & 0xFFFFFF)))
                out.dependencies.add(callee)
                fact = facts.get(callee, ExitFact.undiscovered())
                if fact.state == FactState.UNDISCOVERED:
                    # No fact yet.  Not a semantic decision: the reverse
                    # dependency on `callee` re-queues this node once one
                    # exists.  Deferral, not truncation.
                    continue
                if fact.state == FactState.NO_EXIT:
                    # Proven NO_RETURN.  There is no normal continuation to
                    # demand, at any width.  (PB2/PB11 gate; R6.)
                    out.no_exit_reasons.add(f"callee has no normal exit {callee.label}")
                    continue
                if fact.state in (FactState.UNKNOWN, FactState.POISON):
                    if fact.state == FactState.UNKNOWN:
                        out.unknown_reasons.add(f"unknown callee exit {callee.label}")
                    else:
                        out.poison_reasons.add(f"poison callee {callee.label}")
                    self._unproven_continuation(out, entry, ins, key, callee, fact)
                    continue
            if ins.mnem in ("RTS", "RTL"):
                out.exits.add((ins.m_flag & 1, ins.x_flag & 1))
                continue
            if ins.mnem == "RTI":
                out.unknown_reasons.add("RTI restores dynamic P")
                continue
            if ins.mnem == "JMP" and ins.length == 4:
                post_m, post_x, _ = post_state(ins, key.m, key.x, key.p_stack)
                callee = VariantId(self._logical_pc(ins.operand & 0xFFFFFF),
                                   post_m, post_x)
                out.demands.add(Demand(callee, "jml-tail",
                                       self._logical_pc(ins.addr & 0xFFFFFF)))
                out.dependencies.add(callee)
                fact = facts.get(callee, ExitFact.undiscovered())
                if fact.state in (FactState.EXACT, FactState.SET):
                    out.exits.update(fact.modes)
                elif fact.state == FactState.UNKNOWN:
                    out.unknown_reasons.add(f"unknown tail exit {callee.label}")
                elif fact.state == FactState.POISON:
                    out.poison_reasons.add(f"poison tail {callee.label}")
                elif fact.state == FactState.NO_EXIT:
                    out.no_exit_reasons.add(f"tail has no normal exit {callee.label}")
                continue
            for succ in di.successors:
                if succ in graph.insns:
                    pending.append(succ)
                    continue
                target = VariantId(self._logical_pc(succ.pc), succ.m, succ.x)
                if target.pc24 in self.entries:
                    out.demands.add(Demand(target, "guest-tail",
                                           self._logical_pc(ins.addr & 0xFFFFFF)))
                    out.dependencies.add(target)
                    fact = facts.get(target, ExitFact.undiscovered())
                    if fact.state in (FactState.EXACT, FactState.SET):
                        out.exits.update(fact.modes)
                    elif fact.state == FactState.UNKNOWN:
                        out.unknown_reasons.add(f"unknown tail exit {target.label}")
                    elif fact.state == FactState.POISON:
                        out.poison_reasons.add(f"poison tail {target.label}")
                    elif fact.state == FactState.NO_EXIT:
                        out.no_exit_reasons.add(f"tail has no normal exit {target.label}")
                else:
                    out.unknown_reasons.add(f"external edge {target.label}")
        dependencies_resolved = all(
            facts.get(dep, ExitFact.undiscovered()).state != FactState.UNDISCOVERED
            for dep in out.dependencies)
        if not out.exits and not out.unknown_reasons and not out.poison_reasons \
                and dependencies_resolved:
            out.no_exit_reasons.add("reachable control flow has no normal exit")
        return out
