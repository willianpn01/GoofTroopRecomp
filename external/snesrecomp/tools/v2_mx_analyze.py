#!/usr/bin/env python3
"""Write the deterministic analysis-only M/X fact artifact."""

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "recompiler"), str(ROOT)]

from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.mx_analysis_solver import (MxAnalysisSolver, RomAnalyzer, RomEntry,
                                   Seed, VariantId)  # noqa: E402
from snes65816 import decode_insn, lorom_offset  # noqa: E402
from v2.decoder import _resolve_indirect_dispatch_targets  # noqa: E402


def build_solver(rom_path: Path, cfg_paths: list[Path],
                 ignored_seed_pcs=frozenset(),
                 continuation_policy: str = RomAnalyzer.NORMATIVE_CONTINUATION_POLICY
                 ) -> MxAnalysisSolver:
    rom = rom_path.read_bytes()
    cfgs = [load_bank_cfg(str(path)) for path in cfg_paths]
    entries = []
    seeds = []
    all_regions = []
    indirect = {}
    for cfg in cfgs:
        all_regions.extend(cfg.data_regions)
        for spec in cfg.indirect_dispatch:
            indirect[(cfg.bank << 16) | spec["site_pc16"]] = spec
        for entry in cfg.entries:
            logical_bank = cfg.bank ^ 0x80 if cfg.bank < 0x40 else cfg.bank
            variant = VariantId((logical_bank << 16) | (entry.start & 0xFFFF),
                                entry.entry_m & 1, entry.entry_x & 1)
            entries.append(RomEntry(variant, cfg.bank, entry.end, entry.entry_kind))
            if variant.pc24 not in ignored_seed_pcs:
                host = entry.entry_kind == "continuation" and entry.aot_entry_pc is not None
                origin = "host-reentry metadata" if host else "declared CFG root"
                seeds.append(Seed(variant, origin, host))
    analyzer = RomAnalyzer(rom, entries, indirect_dispatch=indirect or None,
                           data_regions=all_regions or None,
                           continuation_policy=continuation_policy)
    solver = MxAnalysisSolver(analyzer)
    solver.analysis_adapter = analyzer
    for seed in sorted(seeds, key=lambda value: value.variant):
        solver.add_seed(seed)
    return solver.solve()


def build_indirect_audit(rom_path: Path, cfg_paths: list[Path], solver) -> dict:
    rom = rom_path.read_bytes()
    cfgs = [load_bank_cfg(str(path)) for path in cfg_paths]
    cfg_banks = {cfg.bank for cfg in cfgs}
    entries = {}
    owners = []
    regions = [r for cfg in cfgs for r in cfg.data_regions]
    for cfg in cfgs:
        logical = cfg.bank ^ 0x80 if cfg.bank < 0x40 else cfg.bank
        for entry in cfg.entries:
            pc = (logical << 16) | entry.start
            entries[pc] = entry
            owners.append((logical, entry.start, entry.end, entry.name))
    event_sources = {}
    for source_variant, site, target, kind, target_source, classification in sorted(
            solver.analysis_adapter.indirect_events):
        event_sources.setdefault(site, set()).add(source_variant)
    continuation_by_site = {}
    for source, node in sorted(solver.nodes.items()):
        for demand in sorted(node.demands):
            if demand.kind != "indirect-continuation":
                continue
            continuation_by_site.setdefault(demand.site_pc24, []).append({
                "caller_variant": source.label,
                "target_variant": demand.via_target.label,
                "target_exit_state": demand.via_exit_state,
                "target_exit_mode": f"M{demand.target.m}X{demand.target.x}",
                "continuation_variant": demand.target.label,
            })
    records = []
    for cfg in sorted(cfgs, key=lambda c: c.bank):
        logical = cfg.bank ^ 0x80 if cfg.bank < 0x40 else cfg.bank
        for spec in sorted(cfg.indirect_dispatch, key=lambda s: s['site_pc16']):
            site16 = spec['site_pc16']
            site = (logical << 16) | site16
            ins = decode_insn(rom, lorom_offset(cfg.bank, site16), site16,
                              cfg.bank, m=1, x=1)
            targets = _resolve_indirect_dispatch_targets(rom, cfg.bank, ins, spec)
            kind = ("indirect_jsr" if ins.mnem == "JSR" or spec.get("ptr_call")
                    else "indirect_jml" if ins.mnem == "JML" or ins.opcode == 0xDC
                    else "rts_stack_dispatch" if ins.mnem == "PHA"
                    else "indirect_jmp")
            layout = ("computed_target" if spec.get("ptr_call")
                      else "selector_table" if len(spec.get("table_bases") or ()) >= 2
                      else "dispatch_table")
            target_records = []
            for raw in targets or ():
                if not raw:
                    continue
                target_bank = ((raw >> 16) & 0xff) if raw > 0xffff else cfg.bank
                target16 = raw & 0xffff
                target_logical = target_bank ^ 0x80 if target_bank < 0x40 else target_bank
                target = (target_logical << 16) | target16
                in_data = any(b == target_bank and s <= target16 < e for b, s, e in regions)
                owner = next((name for b, s, e, name in owners
                              if b == target_logical and e is not None and s <= target16 < e), None)
                if in_data:
                    classification = "DATA_REGION"
                elif target in entries:
                    classification = "CFG_ENTRY"
                elif owner:
                    classification = "INTERNAL_LABEL"
                elif target_bank not in cfg_banks:
                    classification = "EXTERNAL_OR_UNMODELED_BANK"
                else:
                    classification = "CODE_ENTRY_MISSING"
                target_records.append({"target_pc24": f"{target:06X}",
                                       "classification": classification,
                                       "owner": owner,
                                       "cfg_entry": target in entries,
                                       "in_data_region": in_data})
            records.append({"site_pc24": f"{site:06X}", "kind": kind,
                            "layout": layout,
                            "target_set_source": "cfg indirect_dispatch",
                            "confidence": "declared",
                            "reachable_source_variants": sorted(event_sources.get(site, ())),
                            "reachable": site in event_sources,
                            "entry_mx": sorted({s[-4:] for s in event_sources.get(site, ())}),
                            "target_count": len(target_records),
                            "targets": target_records,
                            "continuation_demands": sorted(
                                continuation_by_site.get(site, ()),
                                key=lambda item: (item["caller_variant"],
                                                  item["target_variant"],
                                                  item["continuation_variant"]))})
    kinds = {}
    classes = {}
    layouts = {}
    for rec in records:
        kinds[rec['kind']] = kinds.get(rec['kind'], 0) + 1
        layouts[rec['layout']] = layouts.get(rec['layout'], 0) + 1
        for target in rec['targets']:
            c = target['classification']
            classes[c] = classes.get(c, 0) + 1
    return {"schema": 1, "analysis_only": True,
            "summary": {"directives": len(records),
                        "reachable_directives": sum(r['reachable'] for r in records),
                        "directives_with_usable_target_sets": sum(
                            bool(r['targets']) and all(t['classification'] == 'CFG_ENTRY'
                                                       for t in r['targets']) for r in records),
                        "by_kind": dict(sorted(kinds.items())),
                        "by_layout": dict(sorted(layouts.items())),
                        "targets_by_classification": dict(sorted(classes.items()))},
            "directives": records}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--cfg", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ignore-seed-pc", action="append", default=[])
    parser.add_argument("--indirect-audit-output", type=Path)
    parser.add_argument("--continuation-policy",
                        default=RomAnalyzer.NORMATIVE_CONTINUATION_POLICY,
                        choices=RomAnalyzer.CONTINUATION_POLICIES,
                        help="semantic policy for a may-return callee with an "
                             "unproven exit M/X (default: the normative "
                             "'residual'; 'stop' is compatibility/debug only)")
    args = parser.parse_args()
    ignored = frozenset(int(value, 16) for value in args.ignore_seed_pc)
    solver = build_solver(args.rom, args.cfg, ignored,
                          args.continuation_policy)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(solver.to_json(), encoding="utf-8")
    if args.indirect_audit_output:
        audit = build_indirect_audit(args.rom, args.cfg, solver)
        args.indirect_audit_output.parent.mkdir(parents=True, exist_ok=True)
        args.indirect_audit_output.write_text(
            json.dumps(audit, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    print(solver.statistics())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
