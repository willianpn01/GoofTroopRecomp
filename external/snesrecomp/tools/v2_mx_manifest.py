#!/usr/bin/env python3
"""Build an exact-or-fallback variant manifest from solver facts."""

import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "recompiler"), str(ROOT)]

from v2.cfg_loader import load_bank_cfg  # noqa: E402
from v2.mx_authoritative_manifest import (  # noqa: E402
    OwnerInfo, build_manifest, manifest_json,
)


DEF_RE = re.compile(r"^RecompReturn\s+(\w+)_M([01])X([01])\(CpuState \*cpu\) \{")


def _logical_bank(bank):
    return bank ^ 0x80 if bank < 0x40 else bank


def load_owners(cfg_paths):
    owners = []
    for path in sorted(cfg_paths):
        cfg = load_bank_cfg(str(path))
        logical = _logical_bank(cfg.bank)
        by_pc = {}
        for entry in cfg.entries:
            pc = (logical << 16) | (entry.start & 0xffff)
            by_pc.setdefault(pc, []).append((entry.entry_m & 1, entry.entry_x & 1))
        for entry in cfg.entries:
            pc = (logical << 16) | (entry.start & 0xffff)
            owners.append(OwnerInfo(
                pc, entry.end, entry.name or f"bank_{logical:02X}_{entry.start:04X}",
                entry.entry_kind, frozenset(by_pc[pc]), cfg.strict_mx,
                entry.aot_entry_pc))
    # Multiple declared variants share structural ownership; collapse them.
    return list({(o.pc24, o.symbol): o for o in owners}.values())


def legacy_variants(paths, owners):
    name_pc = {o.symbol: o.pc24 for o in owners}
    result = set()
    for path in sorted(paths):
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = DEF_RE.match(line)
            if not match:
                continue
            stem, m, x = match.groups()
            suffix = re.search(r"([0-9A-F]{6})$", stem)
            pc = int(suffix.group(1), 16) if suffix else name_pc[stem]
            if pc < 0x400000:
                pc ^= 0x800000
            result.add(f"{pc:06X}_M{m}X{x}")
    return result


def build_audit(manifest, reconciliation, legacy_from_c):
    by_variant = {f'{e["pc24"]}_M{e["M"]}X{e["X"]}': e
                  for e in manifest["entries"]}
    s8 = []
    for rec in reconciliation["variants"]:
        if rec.get("category") != "S8":
            continue
        entry = by_variant[rec["variant"]]
        refs = [r for r in rec.get("references", ())
                if r.get("kind") == "indirect-continuation"]
        states = {r.get("via_exit_state") for r in refs}
        if "UNKNOWN" in states or "POISON" in states:
            category = "S8D"
        elif "SET" in states:
            category = "S8B"
        elif refs and states <= {"EXACT"}:
            category = "S8A"
        elif rec.get("references"):
            category = "S8C"
        else:
            category = "S8G"
        sites = sorted({r.get("site_pc24") for r in refs if r.get("site_pc24")})
        targets = sorted({r.get("via_target") for r in refs if r.get("via_target")})
        s8.append({
            "variant": rec["variant"], "owner": entry["owner"],
            "provenance": entry["provenance"], "source_callsites": sites,
            "indirect_targets": targets, "responsible_exit_modes": sorted(states),
            "transitive_chain": entry["source"], "cfg_location": entry["entry_kind"],
            "data_region": False, "external": entry["owner"] is None,
            "materializable": entry["materialization_status"] != "INTERPRETER_FALLBACK",
            "materialization_status": entry["materialization_status"],
            "category": category, "confidence": "high" if category != "S8G" else "low",
        })
    legacy_only = []
    for rec in reconciliation["variants"]:
        if rec.get("membership") != "LEGACY_ONLY":
            continue
        exact_required = rec["variant"] in by_variant
        destination = ("KEEP_AS_EXPLICIT_SEED" if exact_required else
                       "DROP_FROM_STATIC_MANIFEST")
        legacy_only.append({"variant": rec["variant"], "destination": destination,
                            "reason": rec["reason"], "exact_semantic_demand": exact_required})
    legacy = {rec["variant"] for rec in reconciliation["variants"]
              if rec["membership"] in ("COMMON", "LEGACY_ONLY")}
    authoritative = {variant for variant, entry in by_variant.items()
                     if entry["materialization_status"] in
                     ("MATERIALIZE_EXACT", "HOST_ENTRY")}
    fallback = {variant for variant, entry in by_variant.items()
                if entry["materialization_status"] == "INTERPRETER_FALLBACK"}
    conflicts = []
    for conflict in reconciliation["strict_mx_conflicts"]:
        entry = by_variant[conflict["target"]]
        conflicts.append({**conflict,
                          "authoritative_status": entry["materialization_status"],
                          "legacy_would_canonicalize": True})
    return {
        "schema": 1, "ordering": "variant",
        "s8_audit": s8,
        "s8_summary": {name: sum(r["category"] == name for r in s8)
                       for name in ("S8A", "S8B", "S8C", "S8D", "S8E", "S8F", "S8G")},
        "legacy_only": legacy_only,
        "strict_mx_conflicts": conflicts,
        "comparison": {
            "LEGACY_STATIC": len(legacy),
            "AUTHORITATIVE_STATIC": len(authoritative),
            "COMMON": len(legacy & authoritative),
            "AUTHORITATIVE_ONLY": len(authoritative - legacy),
            "LEGACY_ONLY": len(legacy - authoritative),
            "FALLBACK_ONLY": len(fallback - legacy),
            "authoritative_only": sorted(authoritative - legacy),
            "legacy_only": sorted(legacy - authoritative),
            "fallback_only": sorted(fallback - legacy),
            "legacy_c_crosscheck": {
                "parsed_count": len(legacy_from_c),
                "reconciliation_count": len(legacy),
                "matches": legacy_from_c == legacy,
                "only_parsed_c": sorted(legacy_from_c - legacy),
                "only_reconciliation": sorted(legacy - legacy_from_c),
            },
        },
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--facts", type=Path, required=True)
    ap.add_argument("--cfg", type=Path, action="append", required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--reconciliation", type=Path)
    ap.add_argument("--legacy-c", type=Path, action="append", default=[])
    ap.add_argument("--audit-output", type=Path)
    args = ap.parse_args()
    facts = json.loads(args.facts.read_text())
    owners = load_owners(args.cfg)
    document = build_manifest(facts["variants"], owners,
                              continuation_policy=facts.get("continuation_policy"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(manifest_json(document), encoding="utf-8")
    if args.audit_output:
        if not args.reconciliation:
            ap.error("--audit-output requires --reconciliation")
        reconciliation = json.loads(args.reconciliation.read_text())
        audit = build_audit(document, reconciliation,
                            legacy_variants(args.legacy_c, owners))
        args.audit_output.parent.mkdir(parents=True, exist_ok=True)
        args.audit_output.write_text(json.dumps(audit, indent=2) + "\n")
    print(json.dumps(document["statistics"], sort_keys=True))


if __name__ == "__main__":
    main()
