"""Fail-closed materialization from solver facts + authoritative manifest.

This deliberately does not call the legacy variant discovery, exit refresh,
autopromote, or prune loops.  It is invoked only by ``v2_regen``'s explicit
``--strict-manifest`` switch and writes only the caller-selected output dir.

Every build input it writes -- the bank bodies, the exact dispatch table, the
host AOT registry and the (necessarily empty) unresolved-stub unit -- is
derived from the manifest.  Nothing is scanned back out of emitted C to decide
membership; the emitted text is read only by the conformance auditors.
"""

from __future__ import annotations

from collections import Counter
import copy
import json
from pathlib import Path
import re

from v2.cfg_loader import load_bank_cfg
from v2.codegen import (set_exact_variant_selection, set_force_variant_at,
                        set_name_resolver, set_rom_size, set_valid_variants,
                        take_unresolved_call_targets)
from v2.emit_bank import BankEntry, emit_bank
from v2.mx_analysis_solver import VariantId
from v2.continuation_boundary import (audit_rows, from_manifest,
                                      set_continuation_boundaries)
from v2.emit_function import MX_SWITCH_EXPR, TIER_DOWN_HELPER
from v2.mx_manifest_consumer import (AttemptedVariant, ProducedVariant,
                                     boundary_conformance, conformance,
                                     dispatch_registry_conformance,
                                     fallback_runtime_conformance,
                                     dispatch_required, registry_required,
                                     report_json, variant_label)


STATIC = frozenset(("MATERIALIZE_EXACT", "HOST_ENTRY"))

# Generic symbol shapes.  These describe the emitter's naming convention, not
# any particular ROM: no address is spelled out anywhere in this module.
_SYMBOL_REF = re.compile(r"\bCODE_[0-9A-Fa-f]{6}_M\dX\d\b")
_SYMBOL_DEF = re.compile(
    r"^(?:RecompReturn|void)\s+(CODE_[0-9A-Fa-f]{6}_M\dX\d)\s*\(CpuState", re.M)
_VARIANT_LABEL = re.compile(r"[0-9A-Fa-f]{6}_M\dX\d")
# Any emitted variant body definition, whatever the owner's symbol spelling
# (disassembly label or synthetic CODE_ name).
_ANY_VARIANT_DEF = re.compile(
    r"^(?:RecompReturn|void)\s+(\w+_M[01]X[01])\s*\(CpuState", re.M)


def _logical(bank: int) -> int:
    return bank ^ 0x80 if bank < 0x40 else bank


def _key(entry: dict) -> VariantId:
    return VariantId(int(entry["pc24"], 16), int(entry["M"]), int(entry["X"]))


def _solver_exits(facts: dict):
    """Adapt explicit solver ExitFact records to decoder's existing API."""
    exact, modes = {}, {}
    for record in facts["variants"]:
        ident = record["variant_id"]
        key = (int(ident["pc24"], 16), int(ident["m"]), int(ident["x"]))
        fact = record["exit"]
        known = frozenset((int(v[1]), int(v[3])) for v in fact.get("modes", ())
                          if len(v) == 4 and v[0] == "M" and v[2] == "X")
        if fact["state"] == "EXACT" and len(known) == 1:
            exact[key] = next(iter(known))
        elif fact["state"] in ("SET", "UNKNOWN", "POISON") and known:
            modes[key] = known
    return exact, modes


def _build_entries(document: dict, cfg_dir: Path, metadata_path: str | None):
    """Return cloned, manifest-fixed cfgs and resolver names.

    The original cfg is used only for structural metadata (ranges, dispatch
    declarations, symbol spelling).  It is never used to choose M/X.
    """
    metadata_entries = []
    if metadata_path:
        metadata_entries = json.loads(Path(metadata_path).read_text(encoding="utf-8"))["entries"]
    cfgs = []
    source = {}
    for path in sorted(cfg_dir.glob("bank*.cfg")):
        cfg = load_bank_cfg(str(path))
        # Only fill absent structural continuation records. Existing cfg
        # records remain intact; host visibility itself is still decided from
        # the authoritative manifest below.
        existing = {entry.start & 0xffff for entry in cfg.entries}
        for item in metadata_entries:
            if int(item["bank"], 16) != cfg.bank:
                continue
            start = int(item["address"], 16)
            if start in existing:
                continue
            mode = item.get("entry_mx", [1, 1])
            cfg.entries.append(BankEntry(
                item["name"], start,
                int(item["end"], 16) if item.get("end") else None,
                int(mode[0]), int(mode[1]),
                entry_kind=item.get("kind", "function"),
                aot_entry_pc=(int(item["aot_entry_pc"], 16)
                              if item.get("aot_entry_pc") else None)))
            existing.add(start)
        bank = cfg.bank
        logical = _logical(bank)
        cfgs.append((bank, cfg))
        for entry in cfg.entries:
            source.setdefault((logical << 16) | (entry.start & 0xffff), entry)

    selected = {bank: [] for bank, _cfg in cfgs}
    selected_ids = set()
    source_names = {}
    for item in document["entries"]:
        if item["materialization_status"] not in STATIC:
            continue
        variant = _key(item)
        owner = source.get(variant.pc24)
        if owner is None:
            raise ValueError("strict manifest exact entry has no CFG owner: " +
                             variant_label(variant))
        physical_bank = (variant.pc24 >> 16) & 0xff
        bank = physical_bank ^ 0x80 if physical_bank >= 0x80 else physical_bank
        clone = copy.copy(owner)
        clone.entry_m, clone.entry_x = variant.m, variant.x
        # Host exposure is manifest authority, not a side effect of cfg clone.
        if item["materialization_status"] != "HOST_ENTRY":
            clone.aot_entry_pc = None
        else:
            # The manifest is the authoritative host-exposure decision.
            # Older generated cfg snapshots may predate the declarative
            # aot_entry_pc merge; set it in-memory only, never in the cfg.
            clone.aot_entry_pc = variant.pc24
        selected.setdefault(bank, []).append(clone)
        selected_ids.add(variant)
        if clone.name:
            source_names[variant.pc24] = clone.name
    for bank in selected:
        selected[bank].sort(key=lambda e: (e.start, e.entry_m, e.entry_x, e.name or ""))
    return cfgs, selected, selected_ids, source_names


def _physical_pc24(variant: VariantId, cfg_banks) -> int:
    """The cfg (physical) address of a logical manifest VariantId."""
    bank = (variant.pc24 >> 16) & 0xff
    for candidate in (bank, bank ^ 0x80):
        if candidate in cfg_banks:
            return (candidate << 16) | (variant.pc24 & 0xffff)
    raise ValueError("manifest VariantId has no reconstructed cfg bank: " +
                     variant_label(variant))


def _dispatch_source(document: dict, cfg_banks) -> tuple[str, dict]:
    """dispatch_v2.c from manifest authority: one slot per exact static body.

    Row key is the physical cfg PC (the runtime lookup also tries the LoROM
    mirror); slot index is the exact ``(M << 1) | X``.  No slot is ever filled
    with a sibling width.
    """
    rows: dict[int, list[str]] = {}
    declared: list[str] = []
    for item in document["entries"]:
        if not dispatch_required(item):
            continue
        variant = _key(item)
        symbol = item.get("symbol")
        if not symbol or not symbol.endswith(f"_M{variant.m}X{variant.x}"):
            raise ValueError("dispatchable manifest entry has no exact symbol: " +
                             variant_label(variant))
        physical = _physical_pc24(variant, cfg_banks)
        slots = rows.setdefault(physical, ["NULL"] * 4)
        index = (variant.m << 1) | variant.x
        if slots[index] != "NULL":
            raise ValueError("duplicate dispatch slot for " + variant_label(variant))
        slots[index] = symbol
    lines = [
        '/* Auto-generated by snesrecomp v2 strict manifest regen. Do NOT hand-edit.',
        ' *',
        ' * Exact runtime dispatch table derived from the authoritative variant',
        ' * manifest: one fnptr per MATERIALIZE_EXACT / HOST_ENTRY VariantId.',
        ' * cpu_dispatch_pc() looks up (pc24, live M/X); a NULL slot is a miss and',
        ' * never routes to a sibling width.',
        ' *',
        ' * Sorted by pc24 for binary search. variant[] holds fnptrs for',
        ' * (M0X0, M0X1, M1X0, M1X1).',
        ' */',
        '',
        '#include "cpu_state.h"',
        '',
    ]
    for pc24 in sorted(rows):
        for symbol in rows[pc24]:
            if symbol != "NULL":
                declared.append(symbol)
                lines.append(f"RecompReturn {symbol}(CpuState *cpu);")
    lines.append('')
    lines.append('const DispatchEntry g_dispatch_table[] = {')
    if not rows:
        lines.append("    { 0xFFFFFFu, { NULL, NULL, NULL, NULL } },  /* sentinel - empty manifest */")
    for pc24 in sorted(rows):
        base = next(sym for sym in rows[pc24] if sym != "NULL")[:-len("_M0X0")]
        lines.append(f"    {{ 0x{pc24:06X}u, {{ {', '.join(rows[pc24])} }} }},  /* {base} */")
    lines += ['};', '',
              "const unsigned g_dispatch_table_count = "
              "(unsigned)(sizeof(g_dispatch_table) / sizeof(g_dispatch_table[0]));",
              '']
    return "\n".join(lines) + "\n", rows


def _registry_source(document: dict, cfg_banks, owners: dict) -> tuple[str, list]:
    """aot_entries_v2.c from manifest authority.

    The runtime descriptor is keyed by logical PC alone, so each declared host
    PC contributes exactly its CFG-declared host width.  The cfg is consulted
    only to cross-check that the declaration still names the same logical PC.
    """
    rows = []
    for item in document["entries"]:
        if not registry_required(item):
            continue
        variant = _key(item)
        owner = owners.get(variant.pc24)
        declared_pc = getattr(owner, "aot_entry_pc", None)
        if declared_pc is not None and declared_pc != variant.pc24:
            raise ValueError("cfg aot_entry_pc disagrees with manifest host entry " +
                             variant_label(variant))
        kind = ("INTERP_AOT_ENTRY_CONTINUATION"
                if item.get("entry_kind") == "continuation"
                else "INTERP_AOT_ENTRY_FUNCTION")
        rows.append((variant.pc24, _physical_pc24(variant, cfg_banks),
                     item["symbol"], kind))
    rows.sort()
    for before, after in zip(rows, rows[1:]):
        if before[0] == after[0]:
            raise ValueError(f"duplicate AOT registry logical PC ${after[0]:06X}")
    lines = [
        '/* Auto-generated by snesrecomp v2 strict manifest regen. Do NOT hand-edit.',
        ' * Host-visible AOT entries: manifest HOST_ENTRY at the CFG-declared host width.',
        ' */', '', '#include "interp_bridge.h"', '']
    for _logical, _physical, symbol, _kind in rows:
        lines.append(f"RecompReturn {symbol}(CpuState *cpu);")
    if rows:
        lines.append('')
    lines.append('const InterpAotEntryDescriptor g_aot_entry_registry[] = {')
    if not rows:
        lines.append('    { 0u, NULL, INTERP_AOT_ENTRY_FUNCTION },  /* empty */')
    for logical, physical, symbol, kind in rows:
        lines.append(f"    {{ 0x{logical:06X}u, {symbol}, {kind} }},  "
                     f"/* dispatch 0x{physical:06X} */")
    lines += ['};', '', f'const unsigned g_aot_entry_registry_count = {len(rows)}u;', '']
    return "\n".join(lines), rows


_STUBS_SOURCE = """/* Auto-generated by snesrecomp v2 strict manifest regen. Do NOT hand-edit.
 *
 * Strict manifest mode emits no unresolved-call stubs: every exact reference
 * must resolve to a manifest-authorized body, and anything else is a
 * conformance failure (MC9/MC10), never a stub.  The translation unit is
 * still emitted so build systems can list it unconditionally.
 */

#include "cpu_state.h"
#include "cpu_trace.h"
#include "common_cpu_infra.h"

"""


def run(args) -> int:
    manifest_path = Path(args.strict_manifest)
    facts_path = Path(args.solver_facts)
    document = json.loads(manifest_path.read_text(encoding="utf-8"))
    facts = json.loads(facts_path.read_text(encoding="utf-8"))
    if document.get("semantic_source") != "mx_interprocedural_solver":
        raise ValueError("strict manifest has an unexpected semantic source")
    if document.get("continuation_policy") != facts.get("continuation_policy"):
        raise ValueError("strict manifest and solver facts disagree on the "
                         "continuation policy")
    required_policy = getattr(args, "require_continuation_policy", None)
    if required_policy and document.get("continuation_policy") != required_policy:
        raise ValueError(
            f"strict manifest continuation policy "
            f"{document.get('continuation_policy')!r} != required {required_policy!r}")

    from snes65816 import load_rom
    rom = load_rom(args.rom)
    set_rom_size(len(rom))
    cfgs, selected, selected_ids, name_map = _build_entries(
        document, Path(args.cfg_dir), getattr(args, "aot_metadata", None))
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    exact_exits, set_exits = _solver_exits(facts)

    # Exact selection + the manifest fixed universe disables survivor choice.
    valid = {}
    for value in selected_ids:
        valid.setdefault(value.pc24, set()).add((value.m, value.x))
        # Decoder/codegen use physical cfg banks ($00..$3f); facts and the
        # manifest use the logical LoROM mirror ($80..$bf).  This is address
        # normalization, never a semantic VariantId rewrite.
        if (value.pc24 >> 16) < 0x40:
            valid.setdefault(value.pc24 ^ 0x800000, set()).add((value.m, value.x))
        elif 0x80 <= (value.pc24 >> 16) < 0xc0:
            valid.setdefault(value.pc24 ^ 0x800000, set()).add((value.m, value.x))
    set_valid_variants({pc: frozenset(modes) for pc, modes in valid.items()})
    set_exact_variant_selection(True)
    set_name_resolver(name_map)
    set_force_variant_at({})
    # Boundary truth comes from the manifest and nowhere else.  With it
    # installed the decoder stops producing preserved-M/X successors past an
    # unproven return, so the emitter cannot consume one as authoritative.
    boundaries = from_manifest(document)
    set_continuation_boundaries(boundaries)

    cfg_banks = {bank for bank, _cfg in cfgs}
    owners = {}
    for bank, cfg in cfgs:
        for entry in cfg.entries:
            owners.setdefault((_logical(bank) << 16) | (entry.start & 0xffff), entry)
    dispatch_src, dispatch_rows = _dispatch_source(document, cfg_banks)
    registry_src, registry_rows = _registry_source(document, cfg_banks, owners)
    dispatched = {VariantId(_logical(pc >> 16) << 16 | (pc & 0xffff), (i >> 1) & 1, i & 1)
                  for pc, slots in dispatch_rows.items()
                  for i, symbol in enumerate(slots) if symbol != "NULL"}
    registered = {VariantId(logical, int(symbol[-3]), int(symbol[-1]))
                  for logical, _physical, symbol, _kind in registry_rows}

    produced, attempted, unrepresented = [], [], []
    emitted_sources = {}
    marker_rows = []
    for bank, cfg in cfgs:
        entries = selected.get(bank, [])
        ind_map = {(bank << 16) | (d["site_pc16"] & 0xffff): d
                   for d in (getattr(cfg, "indirect_dispatch", None) or [])}
        src = emit_bank(rom, bank, entries,
                        indirect_call_tables=getattr(cfg, "indirect_call_tables", None),
                        indirect_dispatch=ind_map or None,
                        data_regions=cfg.data_regions or None,
                        exclude_ranges=cfg.exclude_ranges or None,
                        callee_exit_mx=exact_exits,
                        callee_exit_mx_modes=set_exits,
                        hle_spc_upload=getattr(cfg, "hle_spc_upload", None) or None,
                        hle_func=getattr(cfg, "hle_func", None) or None,
                        hle_dispatch=getattr(cfg, "hle_dispatch", None) or None,
                        exec_bank=getattr(cfg, "exec_bank", None))
        (out_dir / f"bank{bank:02x}_v2.c").write_text(src, encoding="utf-8")
        emitted_sources[bank] = src
        for item in document["entries"]:
            v = _key(item)
            if ((v.pc24 >> 16) & 0xff) == _logical(bank) and v in selected_ids:
                produced.append(ProducedVariant(v, symbol=item.get("symbol"),
                    host_dispatch=(item["materialization_status"] == "HOST_ENTRY"
                                   and v in dispatched),
                    registry=v in registered,
                    provenance="strict-manifest emitter"))
        for line_no, line in enumerate(src.splitlines(), 1):
            if "BRK" in line or "COP" in line or "unresolvable cross-fn goto" in line:
                marker_rows.append({"file": f"bank{bank:02x}_v2.c", "line": line_no,
                                    "text": line.strip()})

    # Internal labels are physically owned by their emitted owner bodies; they
    # intentionally do not generate stand-alone C definitions.
    static_owners = {item.get("owner") for item in document["entries"]
                     if item["materialization_status"] in STATIC}
    for item in document["entries"]:
        v = _key(item)
        status = item["materialization_status"]
        if status == "INTERNAL_ONLY":
            if item.get("owner") not in static_owners:
                unrepresented.append(ProducedVariant(v, kind="internal", provenance="owner absent"))
            else:
                produced.append(ProducedVariant(v, kind="internal", provenance="strict owner representation"))
        elif status == "INTERPRETER_FALLBACK":
            pass

    fallback_report = fallback_runtime_conformance(document, emitted_sources)
    represented_fallback = set(fallback_report["represented"])
    for item in document["entries"]:
        if item["materialization_status"] == "INTERPRETER_FALLBACK":
            v = _key(item)
            if variant_label(v) in represented_fallback:
                produced.append(ProducedVariant(
                    v, kind="fallback", provenance="FB runtime conformance"))

    # Demands the emitter raised that strict mode refused to materialize.
    # An attempt is never an output; its disposition says whether the emitter
    # nevertheless left the undeclared VariantId behind in the emitted C.
    # `referenced` is derived from the emitted text, not from a PC list, so a
    # new dangling symbol in any title is caught by the same rule.
    emitted_symbols = set()
    for text in emitted_sources.values():
        emitted_symbols.update(_SYMBOL_REF.findall(text))
    defined_symbols = set()
    for text in emitted_sources.values():
        defined_symbols.update(_SYMBOL_DEF.findall(text))
    demands = take_unresolved_call_targets()
    for addr, m, x in sorted(demands):
        logical_addr = addr ^ 0x800000 if (addr >> 16) < 0x40 else addr
        v = VariantId(logical_addr, m, x)
        if v in selected_ids:
            continue
        label = variant_label(v)
        dangling = any(sym.endswith(label) and sym not in defined_symbols
                       for sym in emitted_symbols)
        attempted.append(AttemptedVariant(
            v, stage="emitter call target",
            disposition="REFERENCED" if dangling else "REJECTED",
            provenance="undeclared exact demand refused by strict manifest"))
    # Any dangling symbol the demand channel did not report is still an
    # unclassifiable output; surface it through the same generic path.
    reported = {variant_label(a.variant) for a in attempted}
    for sym in sorted(emitted_symbols - defined_symbols):
        label = sym.split("CODE_")[-1]
        if label in reported or not _VARIANT_LABEL.fullmatch(label):
            continue
        attempted.append(AttemptedVariant(
            VariantId(int(label[:6], 16), int(label[8]), int(label[10])),
            stage="emitted symbol reference", disposition="REFERENCED",
            provenance="dangling symbol with no definition in emitted output"))
    # Attribute each dangling reference to the emitted function that made
    # it, by position against the definition offsets in the same text.  This
    # is the emitter's naming convention, not a PC list.
    dangling_variants = []
    for sym in sorted(emitted_symbols - defined_symbols):
        label = _VARIANT_LABEL.search(sym)
        if not label:
            continue
        pc, mx = label.group(0).split("_")
        variant = VariantId(int(pc, 16), int(mx[1]), int(mx[3]))
        sources = set()
        for text in emitted_sources.values():
            marks = [(m.start(), m.group(1)) for m in _SYMBOL_DEF.finditer(text)]
            for hit in re.finditer(r"\b%s\b" % re.escape(sym), text):
                prior = [name for start, name in marks if start < hit.start()]
                if prior:
                    sources.add(_VARIANT_LABEL.search(prior[-1]).group(0))
        for source in sorted(sources) or [None]:
            dangling_variants.append((variant, source))
    report = conformance(document, produced, removed=(), attempted=attempted,
                         dangling=dangling_variants)
    boundary_report = boundary_conformance(
        document, boundaries, emitted_sources,
        mx_switch=MX_SWITCH_EXPR, tier_down=TIER_DOWN_HELPER)
    set_continuation_boundaries(None)
    (out_dir / "dispatch_v2.c").write_text(dispatch_src, encoding="utf-8")
    (out_dir / "aot_entries_v2.c").write_text(registry_src, encoding="utf-8")
    (out_dir / "unresolved_stubs_v2.c").write_text(_STUBS_SOURCE, encoding="utf-8")
    table_report = dispatch_registry_conformance(
        document, dispatch_src, registry_src,
        {name for text in emitted_sources.values()
         for name in _ANY_VARIANT_DEF.findall(text)})
    counts = Counter(item.kind for item in produced)
    audit = {
        "schema": 1,
        "mode": "strict_manifest",
        # Basenames only: the audit is a reproducible artifact and must not
        # depend on where a clean room happened to put its inputs.
        "manifest": manifest_path.name, "solver_facts": facts_path.name,
        "continuation_policy": document.get("continuation_policy"),
        "exit_facts": {"EXACT": len(exact_exits), "SET_OR_KNOWN": len(set_exits)},
        "semantic_additions": {"exit_refresh_produced": 0, "autopromote_produced": 0,
                               "nearest_survivor_selections": 0},
        "attempted": report["attempted"],
        "attempted_counts": report["attempted_counts"],
        "unrepresented_internal": [{"variant": variant_label(p.variant),
                                     "reason": p.provenance} for p in unrepresented],
        "prune_protected_removals": 0,
        "markers": sorted(marker_rows, key=lambda r: (r["file"], r["line"])),
        "produced_counts": dict(sorted(counts.items())),
        "sets": {"requested_static": sorted(variant_label(_key(e)) for e in document["entries"] if e["materialization_status"] in STATIC),
                 "produced_static": sorted(variant_label(p.variant) for p in produced if p.kind == "static"),
                 "requested_internal": sorted(variant_label(_key(e)) for e in document["entries"] if e["materialization_status"] == "INTERNAL_ONLY"),
                 "produced_internal": sorted(variant_label(p.variant) for p in produced if p.kind == "internal"),
                 "requested_host": sorted(variant_label(_key(e)) for e in document["entries"] if e["materialization_status"] == "HOST_ENTRY"),
                 "produced_host": sorted(variant_label(p.variant) for p in produced if p.host_dispatch),
                 "requested_registry": sorted(variant_label(_key(e)) for e in document["entries"] if registry_required(e)),
                 "produced_registry": sorted(variant_label(p.variant) for p in produced if p.registry),
                 "requested_fallback": sorted(variant_label(_key(e)) for e in document["entries"] if e["materialization_status"] == "INTERPRETER_FALLBACK"),
                 "produced_fallback": sorted(variant_label(p.variant) for p in produced if p.kind == "fallback")},
    }
    audit["continuation_boundaries"] = audit_rows(boundaries)
    (out_dir / "strict_conformance.json").write_text(report_json(report), encoding="utf-8")
    (out_dir / "strict_boundary_conformance.json").write_text(
        report_json(boundary_report), encoding="utf-8")
    (out_dir / "strict_dispatch_registry_conformance.json").write_text(
        report_json(table_report), encoding="utf-8")
    (out_dir / "strict_fallback_runtime_conformance.json").write_text(
        report_json(fallback_report), encoding="utf-8")
    (out_dir / "strict_materialization_audit.json").write_text(
        json.dumps(audit, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("strict manifest regen:", json.dumps(report["counts"], sort_keys=True))
    print("strict manifest attempts:", json.dumps(report["attempted_counts"], sort_keys=True))
    print("strict boundary audit:", json.dumps(boundary_report["counts"], sort_keys=True))
    print("boundary classification:",
          json.dumps(boundary_report["classification"], sort_keys=True))
    print("strict dispatch/registry audit:", json.dumps(table_report["counts"], sort_keys=True))
    print("strict fallback runtime audit:", json.dumps(fallback_report["counts"], sort_keys=True))
    print(f"strict dispatch: {table_report['dispatch']['rows']} exact slots over "
          f"{table_report['dispatch']['pcs']} PCs; registry: "
          f"{len(table_report['registry']['rows'])} rows")
    failed = (any(report["counts"].values()) or any(boundary_report["counts"].values())
              or any(table_report["counts"].values())
              or any(fallback_report["counts"].values()))
    return 3 if failed else 0
