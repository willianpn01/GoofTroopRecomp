"""Strict, post-analysis consumer for authoritative M/X manifests.

This module deliberately knows nothing about ROM addresses, CFG discovery, or
survivor selection.  Its only identity is ``(pc24, M, X)`` from the manifest.
It is shared by the experimental materializer and its conformance gate.
"""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import re
import json
from typing import Iterable, Mapping

from v2.mx_analysis_solver import VariantId


STATIC = frozenset(("MATERIALIZE_EXACT", "HOST_ENTRY"))


def variant_label(value: VariantId) -> str:
    return f"{value.pc24:06X}_M{value.m}X{value.x}"


def entry_variant(entry: Mapping) -> VariantId:
    return VariantId(int(entry["pc24"], 16), int(entry["M"]), int(entry["X"]))


@dataclass(frozen=True)
class ProducedVariant:
    variant: VariantId
    kind: str = "static"             # static | internal | fallback
    symbol: str | None = None
    host_dispatch: bool = False
    registry: bool = False
    provenance: str = "emitter"


@dataclass(frozen=True)
class AttemptedVariant:
    """A demand the emitter raised that materialization did not satisfy.

    An attempt is NOT an output.  It becomes a conformance finding only
    through its ``disposition``:

    ``REJECTED``   fail-closed and left no trace in the emitted output.  This
                   is the intended strict behaviour and is audit-only.
    ``REFERENCED`` fail-closed for materialization but the emitter still wrote
                   a reference to the undeclared VariantId into its output.
                   That reference is an unclassifiable output (MC9): it names a
                   semantic variant the manifest never authorised, and nothing
                   in the produced set can satisfy it.
    """

    variant: VariantId
    stage: str = "unknown"
    disposition: str = "REJECTED"    # REJECTED | REFERENCED
    provenance: str = "emitter"


def host_width_declared(entry: Mapping) -> bool:
    """Whether a HOST_ENTRY's own M/X is the owner's CFG-declared host width.

    Absent declared-mode policy is treated as declared (conservative: the
    registry row stays required)."""
    policy = entry.get("policy") or {}
    if "declared_modes" not in policy:
        return True
    return f"M{int(entry['M'])}X{int(entry['X'])}" in policy["declared_modes"]


def dispatch_required(entry: Mapping) -> bool:
    """Manifest categories that require an exact runtime dispatch row.

    Every static exact body (MATERIALIZE_EXACT, HOST_ENTRY) is dispatchable by
    exact (pc, live M/X).  INTERNAL_ONLY has no stand-alone symbol, fallback has
    no body, and a continuation boundary is not a VariantId at all."""
    return entry["materialization_status"] in STATIC


def registry_required(entry: Mapping) -> bool:
    """Manifest categories that require a host AOT registry row."""
    return (entry["materialization_status"] == "HOST_ENTRY"
            and host_width_declared(entry))


def requested(document: Mapping) -> dict[VariantId, Mapping]:
    return {entry_variant(e): e for e in document["entries"]}


def unresolved_continuation_pcs(document: Mapping) -> frozenset[int]:
    """PCs the manifest declares as unresolved-M/X continuation boundaries."""
    return frozenset(int(row["pc24"], 16) for row in
                     document.get("unresolved_mx_continuations", ()))


def unresolved_continuation_sources(document: Mapping) -> frozenset[str]:
    """Variant labels whose body contains an unresolved-M/X continuation.

    Coverage is attributed by SOURCE, not by the dangling target's own PC: a
    boundary truncates everything the emitter would otherwise walk to after
    the unproven return, so the defect belongs to the function that walked
    past it -- which may name any downstream target at all.
    """
    return frozenset(row["source_variant"] for row in
                     document.get("unresolved_mx_continuations", ()))


def fallback_runtime_conformance(document: Mapping,
                                 emitted_sources: Mapping[int, str]) -> dict:
    """FB gate: manifest fallback must have an emitted executable disposition.

    Accepted dispositions are an interpreter/runtime transfer naming the
    logical target, an explicit unsupported-path failure naming it, or a
    manifest reachability exclusion carrying a non-empty proof.  Merely having
    INTERPRETER_FALLBACK status is deliberately insufficient.
    """
    text = "\n".join(emitted_sources.values()).lower()
    findings = []
    represented = []
    for entry in document["entries"]:
        if entry["materialization_status"] != "INTERPRETER_FALLBACK":
            continue
        variant = entry_variant(entry)
        target = f"0x{variant.pc24:06x}u"
        transfer = any(f"{helper}(cpu, {target}" in text for helper in (
            "interp_tier_dispatch_balanced", "interp_tier_run_long_call",
            "interp_tier_run_call", "interp_tier_dispatch_bank_miss"))
        hard_failure = f"cpu_unsupported_path_balanced(cpu," in text and target in text
        exclusion = entry.get("fallback_exclusion") or {}
        excluded = (exclusion.get("classification") == "PROVEN_UNREACHABLE"
                    and bool(exclusion.get("proof")))
        if transfer or hard_failure or excluded:
            represented.append(variant)
        else:
            findings.append({"code": "FB1", "variant": variant_label(variant),
                             "reason": "fallback has no runtime transfer, hard failure, or proven exclusion"})
        # These old comments are intrinsically silent-success omissions.
        needle = f"target ${variant.pc24:06x}"
        if needle in text and (" skipped" in text[text.find(needle):text.find(needle)+240]):
            findings.append({"code": "FB2", "variant": variant_label(variant),
                             "reason": "fallback target is emitted as a skipped transfer"})
    counts = Counter(f["code"] for f in findings)
    return {"schema": 1, "gate": "fallback-runtime-conformance",
            "findings": findings,
            "counts": {"FB1": counts["FB1"], "FB2": counts["FB2"]},
            "represented": sorted(variant_label(v) for v in represented)}


def conformance(document: Mapping, produced: Iterable[ProducedVariant],
                *, removed: Iterable[ProducedVariant] = (),
                attempted: Iterable[AttemptedVariant] = (),
                dangling: Iterable[VariantId] = ()) -> dict:
    """Check exact manifest membership; every diagnostic has provenance.

    The report is intentionally data-only so a consumer can create it before
    building C.  A static body may never stand in for fallback/internal demand.
    """
    demanded = requested(document)
    output = list(produced)
    removed_output = list(removed)
    attempts = list(attempted)
    by_variant: dict[VariantId, list[ProducedVariant]] = {}
    for item in output:
        by_variant.setdefault(item.variant, []).append(item)
    findings: list[dict] = []

    def add(code: str, variant: VariantId, reason: str, provenance: str):
        findings.append({"code": code, "variant": variant_label(variant),
                         "reason": reason, "provenance": provenance})

    for variant, entry in demanded.items():
        status = entry["materialization_status"]
        items = by_variant.get(variant, [])
        statics = [p for p in items if p.kind == "static"]
        internals = [p for p in items if p.kind == "internal"]
        provenance = "; ".join(entry.get("provenance", ())) or "manifest"
        if status in STATIC and not statics:
            add("MC1", variant, "manifest static exact body is missing", provenance)
        if status in STATIC:
            for dropped in (p for p in removed_output if p.variant == variant):
                add("MC7", variant, "protected manifest root was removed by prune",
                    dropped.provenance)
        if status == "INTERPRETER_FALLBACK" and statics:
            add("MC4", variant, "fallback was materialized as static", statics[0].provenance)
        if status == "INTERPRETER_FALLBACK" and not statics:
            sibling = next((p for p in output if p.kind == "static" and
                            p.variant.pc24 == variant.pc24), None)
            if sibling:
                add("MC4", variant, "fallback was substituted by an M/X sibling", sibling.provenance)
        if status == "INTERNAL_ONLY":
            if statics or any(p.host_dispatch or p.registry for p in items):
                p = (statics or items)[0]
                add("MC5", variant, "internal-only demand was exposed as host output", p.provenance)
            if not internals:
                add("MC1", variant, "internal representation is missing", provenance)
        if status == "HOST_ENTRY":
            # The host registry contract is keyed by logical PC only, so it can
            # carry exactly the CFG-declared host width.  A further proven
            # width at the same declared host PC is still an exact static body
            # with an exact dispatch row, but is not a registry row.  Entries
            # without declared-mode policy keep the conservative contract.
            declared = host_width_declared(entry)
            if not statics or not any(
                    p.host_dispatch and (p.registry or not declared)
                    for p in statics):
                add("MC6", variant, "host exact body/dispatch/registry is incomplete", provenance)
            if not declared and any(p.registry for p in statics):
                add("MC5", variant, "undeclared host width entered the host registry",
                    statics[0].provenance)
        if status == "MATERIALIZE_EXACT" and any(p.registry for p in statics):
            add("MC5", variant, "ordinary static entry incorrectly entered host registry", statics[0].provenance)
        symbols = {p.symbol for p in statics if p.symbol}
        if len(symbols) > 1 or len(statics) > 1:
            add("MC8", variant, "duplicate or incompatible static symbol", statics[0].provenance if statics else provenance)

    for variant, items in by_variant.items():
        entry = demanded.get(variant)
        if entry is None:
            for p in items:
                add("MC2", variant, "produced VariantId is undeclared", p.provenance)
            continue
        expected = entry["materialization_status"]
        for p in items:
            if not p.provenance:
                add("MC9", variant, "output has no explainable origin", "missing")
            if expected == "INTERNAL_ONLY" and p.kind != "internal":
                add("MC5", variant, "internal-only kind mismatch", p.provenance)
            if expected == "INTERPRETER_FALLBACK" and p.kind != "fallback":
                add("MC4", variant, "fallback output has an invalid representation", p.provenance)
            if expected != "INTERPRETER_FALLBACK" and p.kind == "fallback":
                add("MC9", variant, "non-fallback output was classified as fallback", p.provenance)
    # Same-PC sibling bodies are valid.  An alias is only invalid when one
    # symbol is claimed by more than one semantic VariantId.
    symbols: dict[str, set[VariantId]] = {}
    for p in output:
        if p.kind == "static" and p.symbol:
            symbols.setdefault(p.symbol, set()).add(p.variant)
    for symbol, variants in symbols.items():
        if len(variants) > 1:
            for variant in variants:
                add("MC8", variant, f"symbol {symbol} aliases distinct VariantIds", "emitter symbol map")

    # MC3 is a focused presentation of missing required M/X plus a sibling at
    # the same PC.  MC1/MC2 remain present too, making failures actionable.
    for variant, entry in demanded.items():
        if entry["materialization_status"] not in STATIC or by_variant.get(variant):
            continue
        sibling = next((p for p in output if p.kind == "static" and p.variant.pc24 == variant.pc24), None)
        if sibling:
            add("MC3", variant, "wrong M/X sibling selected instead of exact VariantId", sibling.provenance)

    # An attempt is not an output.  Only an attempt the emitter left behind as
    # a reference is an output defect, and it is an unclassifiable one: the
    # named VariantId has no manifest status and no produced representation.
    for attempt in attempts:
        if attempt.disposition != "REFERENCED":
            continue
        if attempt.variant in demanded or attempt.variant in by_variant:
            continue
        add("MC9", attempt.variant,
            "emitted output references a VariantId with no manifest status "
            "and no produced representation", attempt.provenance)

    # MC10 DANGLING_EXACT_REFERENCE.  An emitted exact reference to a
    # VariantId that has no materialization, no dispatch, no fallback and no
    # declared dynamic transfer.  Unlike MC9 this is derived from the emitted
    # text alone, so it also catches a reference the emitter never registered
    # as an attempt.  A declared unresolved-M/X continuation does NOT excuse
    # one: the boundary says the emitter must transfer dynamically instead of
    # naming a symbol, so a named symbol there is still a defect -- but the
    # finding records the coverage so the fix is attributable.
    boundary_sources = unresolved_continuation_sources(document)
    for item in sorted(set(dangling)):
        variant, source = item if isinstance(item, tuple) else (item, None)
        if variant in by_variant:
            continue
        covered = source is not None and source in boundary_sources
        add("MC10", variant,
            "emitted exact reference resolves to no materialization, dispatch, "
            "fallback or declared dynamic transfer",
            (f"referenced from {source}, which the manifest declares an "
             "unresolved-M/X continuation for; the emitter must transfer "
             "dynamically at that boundary instead of naming an exact symbol"
             if covered else
             f"referenced from {source or 'an unattributed site'}; no declared "
             "unresolved-M/X continuation covers it"))

    counts = Counter(f["code"] for f in findings)
    return {
        "attempted": sorted(
            ({"variant": variant_label(a.variant), "stage": a.stage,
              "disposition": a.disposition, "provenance": a.provenance,
              "manifest_status": (demanded[a.variant]["materialization_status"]
                                  if a.variant in demanded else "absent")}
             for a in attempts),
            key=lambda r: (r["variant"], r["stage"])),
        "attempted_counts": {
            "total": len(attempts),
            "REJECTED": sum(a.disposition == "REJECTED" for a in attempts),
            "REFERENCED": sum(a.disposition == "REFERENCED" for a in attempts),
        },
        "schema": 1, "identity": "pc24,M,X", "findings": sorted(findings, key=lambda f: (f["code"], f["variant"], f["reason"])),
        "counts": {f"MC{i}": counts[f"MC{i}"] for i in range(1, 11)},
        "requested": {"static": sum(e["materialization_status"] in STATIC for e in demanded.values()),
                      "internal": sum(e["materialization_status"] == "INTERNAL_ONLY" for e in demanded.values()),
                      "fallback": sum(e["materialization_status"] == "INTERPRETER_FALLBACK" for e in demanded.values())},
    }


BOUNDARY_CODES = {
    "BC1": "boundary missing emitter handling",
    "BC2": "boundary emitted an exact target for an unproven M/X",
    "BC3": "proven exact mode missing from the boundary transfer",
    "BC4": "residual fallback missing",
    "BC5": "nearest-survivor selection at a boundary",
    "BC6": "dynamic transfer names the wrong continuation PC",
    "BC7": "runtime M/X taken from the wrong source",
    "BC8": "internal-only continuation promoted to a host entry",
    "BC9": "duplicate exact and dynamic path for the same mode",
    "BC10": "boundary is unclassified",
}


def _transfer_region(lines: list[str], start: int, cap: int = 16) -> list[str]:
    """The emitted lines that make up one boundary transfer.

    Two shapes: a `switch` block (proven modes present), taken to its matching
    close brace; or a bare tier-down statement, taken to the first `return`.
    Bookkeeping the per-line scanners inject (a stack pop, a trace call) may
    sit between the marker and the transfer, so neither shape can be found by
    a fixed offset.
    """
    region = [lines[start]]
    depth = 0
    opened = False
    for line in lines[start + 1:start + cap]:
        region.append(line)
        depth += line.count("{") - line.count("}")
        if "switch (" in line:
            opened = True
        if opened:
            if depth <= 0:
                break
        elif line.strip().startswith("return ") and line.rstrip().endswith(";"):
            break
    return region


def boundary_conformance(document: Mapping, table, emitted: Mapping[str, str],
                         *, mx_switch: str, tier_down: str) -> dict:
    """Audit every declared boundary against the emitted text.

    `table` is a :class:`v2.continuation_boundary.BoundaryTable`; `emitted`
    maps a file label to its generated C.  The audit is textual only in that
    it reads what the emitter WROTE -- what it compares that against is the
    manifest's own boundary rows, never a PC list and never the emitter's own
    bookkeeping, so a boundary the emitter forgot entirely is still caught.
    """
    from v2.continuation_boundary import mx_index

    findings: list[dict] = []
    text = "\n".join(emitted[key] for key in sorted(emitted))
    lines = text.splitlines()
    classifications = Counter()

    def add(code: str, spec, reason: str):
        findings.append({
            "code": code,
            "boundary": f"{spec.continuation_pc24:06X}@{spec.site_pc24:06X}",
            "classification": spec.classification,
            "reason": reason, "detail": BOUNDARY_CODES[code]})

    # A boundary's emitted transfer is located by the marker the emitter
    # writes for it; both halves of the marker are generated from the same
    # spec, so a missing or misaddressed one is a real defect, not a
    # formatting difference.
    for spec in table.specs:
        cont = spec.continuation_pc24 & 0xFFFFFF
        site = spec.site_pc24 & 0xFFFFFF
        # The marker carries the callee VARIANT, not just the continuation PC:
        # one continuation PC can be the boundary of two distinct callee
        # entry widths, proven to different degrees, and they must not be
        # audited against each other's emitted transfer.
        marker = (f"continuation boundary at ${cont:06X}: callee "
                  f"${spec.callee_pc24 & 0xFFFFFF:06X}"
                  f"_M{spec.callee_m}X{spec.callee_x}")
        transfer = f"0x{cont:06x}u, 0x{site:06x}u"
        classifications[spec.classification] += 1
        if spec.classification not in ("exact+residual", "residual-only"):
            add("BC10", spec, f"unknown classification {spec.classification!r}")
            continue
        starts = [i for i, line in enumerate(lines) if marker in line]
        if not starts:
            add("BC1", spec, "no boundary transfer was emitted for this site")
            continue
        # Every emission of the same boundary must conform, not just the first.
        region = [line for i in starts for line in _transfer_region(lines, i)]
        blob = "\n".join(region)
        if tier_down not in blob:
            add("BC4", spec, "the residual arm does not reach the interpreter tier")
        if transfer not in blob:
            add("BC6", spec, "the dynamic transfer does not name "
                             "(continuation PC, site PC)")
        if mx_switch not in blob and spec.proven_modes:
            add("BC7", spec, "the exact arms are not selected by the live CPU M/X")
        per_site_cases = [
            [int(hit) for hit in re.findall(r"case (\d+):", "\n".join(
                _transfer_region(lines, i)))] for i in starts]
        emitted_cases = {index for site_cases in per_site_cases
                         for index in site_cases}
        proven_cases = {mx_index(m, x) for m, x in spec.sorted_modes}
        for extra in sorted(emitted_cases - proven_cases):
            add("BC2", spec, f"case {extra} is not a proven exit mode")
        for missing in sorted(proven_cases - emitted_cases):
            add("BC3", spec, f"proven mode index {missing} has no exact arm")
        if "nearest survivor" in blob or "pruned ->" in blob:
            add("BC5", spec, "a pruned width was routed to a surviving sibling")
        if any(len(set(site_cases)) != len(site_cases)
               for site_cases in per_site_cases):
            add("BC9", spec, "a mode index appears on more than one arm")
        default_arms = [line for line in region
                        if line.strip().startswith("default:")]
        if spec.proven_modes and len(default_arms) != len(starts):
            add("BC9", spec, "the residual arm is missing or duplicated")
        if any(tier_down not in arm for arm in default_arms):
            add("BC5", spec, "the residual arm does not tier down")

    # BC8 is a manifest-level invariant, not a text one: a continuation the
    # boundary can reach dynamically must not have been re-labelled a host
    # entry just to make it reachable.
    # Host exposure is a property of the declared entry PC, so a second width
    # at a PC that IS a declared host entry is the same declaration, not a
    # promotion.  What BC8 forbids is a continuation PC becoming host-visible
    # with no such declaration anywhere at that PC.
    boundary_pcs = {spec.continuation_pc24 & 0xFFFFFF for spec in table.specs}
    declared_host_pcs = {
        entry_variant(entry).pc24 for entry in document["entries"]
        if any("host-reentry" in item for item in entry.get("provenance", ()))}
    for entry in document["entries"]:
        variant = entry_variant(entry)
        if variant.pc24 not in boundary_pcs:
            continue
        if (entry["materialization_status"] == "HOST_ENTRY"
                and variant.pc24 not in declared_host_pcs):
            findings.append({
                "code": "BC8",
                "boundary": variant_label(variant),
                "classification": "host-exposure",
                "reason": "continuation was exposed as a host entry without "
                          "declarative host-reentry provenance",
                "detail": BOUNDARY_CODES["BC8"]})

    counts = Counter(f["code"] for f in findings)
    return {
        "schema": 1,
        "boundaries": len(table),
        "classification": dict(sorted(classifications.items())),
        "findings": sorted(findings, key=lambda f: (f["code"], f["boundary"],
                                                    f["reason"])),
        "counts": {f"BC{i}": counts[f"BC{i}"] for i in range(1, 11)},
    }


DISPATCH_REGISTRY_CODES = {
    "DC1": "dispatch row names a VariantId the manifest does not authorize",
    "DC2": "authorized exact dispatch VariantId has no dispatch row",
    "DC3": "dispatch slot index disagrees with the symbol's M/X",
    "DC4": "dispatch symbol differs from the manifest symbol or is undefined",
    "DC5": "internal-only or fallback VariantId exposed through dispatch",
    "RC1": "registry row names a VariantId the manifest does not authorize",
    "RC2": "authorized host registry VariantId has no registry row",
    "RC3": "registry entry kind disagrees with the manifest entry kind",
    "RC4": "registry symbol differs from the manifest symbol or is undefined",
    "RC5": "internal-only, fallback or non-host VariantId entered the registry",
    "RC6": "registry logical PCs are duplicated or unsorted",
}

_DISPATCH_ROW = re.compile(
    r"^\s*\{ 0x([0-9A-Fa-f]{6})u, \{ ([^}]*) \} \},", re.M)
_REGISTRY_ROW = re.compile(
    r"^\s*\{ 0x([0-9A-Fa-f]{6})u, (\w+), (INTERP_AOT_ENTRY_\w+) \},", re.M)
_SUFFIX = re.compile(r"_M([01])X([01])$")


def _logical_pc24(pc24: int) -> int:
    bank = (pc24 >> 16) & 0xFF
    return pc24 ^ 0x800000 if bank < 0x40 else pc24


def dispatch_registry_conformance(document: Mapping, dispatch_text: str,
                                  registry_text: str,
                                  defined_symbols: Iterable[str]) -> dict:
    """Audit emitted dispatch/registry TEXT against manifest authority.

    Rows are parsed back out of the generated C, so a generator that drifted
    from the manifest (or a hand edit) is caught.  Identity is the logical
    ``(pc24, M, X)`` VariantId; physical/logical LoROM mirrors normalize.
    """
    demanded = requested(document)
    defined = set(defined_symbols)
    findings: list[dict] = []

    def add(code: str, label: str, reason: str):
        findings.append({"code": code, "variant": label, "reason": reason,
                         "detail": DISPATCH_REGISTRY_CODES[code]})

    dispatch_rows = []
    for match in _DISPATCH_ROW.finditer(dispatch_text):
        pc24 = int(match.group(1), 16)
        slots = [s.strip() for s in match.group(2).split(",")]
        if pc24 == 0xFFFFFF and all(s == "NULL" for s in slots):
            continue  # empty-table sentinel
        for index, symbol in enumerate(slots):
            if symbol == "NULL":
                continue
            suffix = _SUFFIX.search(symbol)
            if suffix is None:
                add("DC3", symbol, "dispatch slot symbol has no _MmXx suffix")
                continue
            m, x = int(suffix.group(1)), int(suffix.group(2))
            variant = VariantId(_logical_pc24(pc24), m, x)
            dispatch_rows.append((variant, symbol, index))
    dispatched = {}
    for variant, symbol, index in dispatch_rows:
        label = variant_label(variant)
        if index != ((variant.m << 1) | variant.x):
            add("DC3", label, f"symbol {symbol} sits in slot {index}")
        if variant in dispatched:
            add("DC1", label, "VariantId appears in more than one dispatch slot")
        dispatched[variant] = symbol
        entry = demanded.get(variant)
        if entry is None:
            add("DC1", label, "no manifest entry for this VariantId")
            continue
        if not dispatch_required(entry):
            add("DC5" if entry["materialization_status"] in (
                "INTERNAL_ONLY", "INTERPRETER_FALLBACK") else "DC1", label,
                f"manifest status {entry['materialization_status']}")
            continue
        if symbol != entry.get("symbol") or symbol not in defined:
            add("DC4", label, f"row symbol {symbol}, manifest symbol "
                              f"{entry.get('symbol')}, defined={symbol in defined}")
    required_dispatch = {v for v, e in demanded.items() if dispatch_required(e)}
    for variant in sorted(required_dispatch - set(dispatched)):
        add("DC2", variant_label(variant), "exact static body is not dispatchable")

    registry = {}
    previous = -1
    for match in _REGISTRY_ROW.finditer(registry_text):
        pc24, symbol, kind = int(match.group(1), 16), match.group(2), match.group(3)
        if pc24 == 0 and symbol == "NULL":
            continue  # empty-registry sentinel
        suffix = _SUFFIX.search(symbol)
        if pc24 <= previous:
            add("RC6", f"{pc24:06X}", "logical PC is not strictly increasing")
        previous = pc24
        if suffix is None:
            add("RC4", symbol, "registry symbol has no _MmXx suffix")
            continue
        variant = VariantId(_logical_pc24(pc24), int(suffix.group(1)),
                            int(suffix.group(2)))
        label = variant_label(variant)
        registry[variant] = (symbol, kind)
        entry = demanded.get(variant)
        if entry is None:
            add("RC1", label, "no manifest entry for this VariantId")
            continue
        if not registry_required(entry):
            add("RC5", label, f"manifest status {entry['materialization_status']}"
                              f", declared host width={host_width_declared(entry)}")
            continue
        expected_kind = ("INTERP_AOT_ENTRY_CONTINUATION"
                         if entry.get("entry_kind") == "continuation"
                         else "INTERP_AOT_ENTRY_FUNCTION")
        if kind != expected_kind:
            add("RC3", label, f"row kind {kind}, manifest entry_kind "
                              f"{entry.get('entry_kind')}")
        if symbol != entry.get("symbol") or symbol not in defined:
            add("RC4", label, f"row symbol {symbol}, manifest symbol "
                              f"{entry.get('symbol')}, defined={symbol in defined}")
    required_registry = {v for v, e in demanded.items() if registry_required(e)}
    for variant in sorted(required_registry - set(registry)):
        add("RC2", variant_label(variant), "declared host entry is not registered")

    counts = Counter(f["code"] for f in findings)
    return {
        "schema": 1, "identity": "pc24,M,X",
        "criteria": {
            "dispatch": "MATERIALIZE_EXACT and HOST_ENTRY (every exact static "
                        "body); never INTERNAL_ONLY, fallback or boundary residue",
            "registry": "HOST_ENTRY whose M/X is the owner's CFG-declared host "
                        "width (registry is keyed by logical PC only)",
        },
        "dispatch": {
            "rows": len(dispatched),
            "pcs": len({v.pc24 for v in dispatched}),
            "unauthorized": sorted(variant_label(v) for v in
                                   set(dispatched) - required_dispatch),
            "missing": sorted(variant_label(v) for v in
                              required_dispatch - set(dispatched)),
        },
        "registry": {
            "rows": [{"pc24": f"{v.pc24:06X}", "M": v.m, "X": v.x,
                      "symbol": symbol, "kind": kind,
                      "owner": demanded[v].get("owner") if v in demanded else None,
                      "status": (demanded[v]["materialization_status"]
                                 if v in demanded else "absent"),
                      "provenance": (demanded[v].get("provenance", [])
                                     if v in demanded else [])}
                     for v, (symbol, kind) in sorted(registry.items())],
            "unauthorized": sorted(variant_label(v) for v in
                                   set(registry) - required_registry),
            "missing": sorted(variant_label(v) for v in
                              required_registry - set(registry)),
            "host_entries_without_registry_row": sorted(
                variant_label(v) for v, e in demanded.items()
                if e["materialization_status"] == "HOST_ENTRY"
                and not registry_required(e)),
        },
        "findings": sorted(findings, key=lambda f: (f["code"], f["variant"],
                                                    f["reason"])),
        "counts": {code: counts[code] for code in DISPATCH_REGISTRY_CODES},
    }


def report_json(report: Mapping) -> str:
    return json.dumps(report, indent=2, sort_keys=True) + "\n"
