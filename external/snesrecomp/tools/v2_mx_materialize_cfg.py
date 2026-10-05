#!/usr/bin/env python3
"""Create experimental CFGs whose entry variants are fixed by a manifest."""

import argparse
import json
from pathlib import Path
import re


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--cfg", type=Path, action="append", required=True)
    ap.add_argument("--out-dir", type=Path, required=True)
    args = ap.parse_args()
    document = json.loads(args.manifest.read_text())
    selected = {}
    for entry in document["entries"]:
        if entry["materialization_status"] not in ("MATERIALIZE_EXACT", "HOST_ENTRY"):
            continue
        selected.setdefault(int(entry["pc24"], 16), []).append(entry)
    args.out_dir.mkdir(parents=True, exist_ok=True)
    consumed = set()
    for cfg_path in sorted(args.cfg):
        lines = cfg_path.read_text().splitlines()
        bank_match = next(re.match(r"bank\s*=\s*([0-9a-fA-F]+)", line)
                          for line in lines if re.match(r"bank\s*=", line))
        physical = int(bank_match.group(1), 16)
        logical = physical ^ 0x80 if physical < 0x40 else physical
        output = []
        emitted_pcs = set()
        for line in lines:
            tokens = line.split()
            if not tokens or tokens[0] not in ("func", "continuation_entry"):
                output.append(line)
                continue
            pc = (logical << 16) | int(tokens[2], 16)
            if pc in emitted_pcs:
                continue
            demands = selected.get(pc, ())
            if not demands:
                continue
            emitted_pcs.add(pc)
            for demand in sorted(demands, key=lambda e: (e["M"], e["X"])):
                clone = [t for t in tokens if not t.startswith("entry_mx:")]
                clone.append(f'entry_mx:{demand["M"]},{demand["X"]}')
                if demand["materialization_status"] != "HOST_ENTRY":
                    clone = [t for t in clone if not t.startswith("aot_entry_pc:")]
                output.append(" ".join(clone))
                consumed.add((pc, demand["M"], demand["X"]))
        (args.out_dir / cfg_path.name).write_text("\n".join(output) + "\n")
    expected = {(pc, e["M"], e["X"]) for pc, entries in selected.items()
                for e in entries}
    missing = sorted(expected - consumed)
    if missing:
        raise ValueError("static manifest entries have no exact CFG owner: " +
                         ", ".join(f"{pc:06X}_M{m}X{x}" for pc, m, x in missing))
    print(f"materialized {len(consumed)} exact CFG entry variants")


if __name__ == "__main__":
    main()
