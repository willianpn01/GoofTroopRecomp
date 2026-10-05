#!/usr/bin/env python3
"""Re-derive recomp_hints.json from the user's ROM (maintainer tool).

    python3 tools/derive_recomp_hints.py --rom /path/to/GOOFT_USA.sfc --output hints.json
    python3 tools/derive_recomp_hints.py --rom ROM --check config/recomp_hints.json

The jump-table rows come from the same ROM analysis generate_cfg.py runs.
--check exits 0 only when the derivation reproduces the given file byte for
byte.  Exit codes as generate_cfg.py (1-4 ROM/descriptor, 7 check failed,
8 output not writable).
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from goof_rom import RomError, load_and_validate_rom, load_descriptor  # noqa: E402
from generate_cfg import EXIT_FOR_KIND  # noqa: E402
from romcfg import derive, hints  # noqa: E402
from romcfg.cpu65816 import Rom  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Re-derive recomp_hints.json from the ROM.")
    parser.add_argument("--rom", type=Path, required=True)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--output", type=Path, help="write the derived file here (must not exist)")
    group.add_argument("--check", type=Path, help="compare with an existing file")
    parser.add_argument("--descriptor", type=Path, default=None)
    args = parser.parse_args(argv)
    try:
        descriptor = load_descriptor(args.descriptor)
        validated = load_and_validate_rom(args.rom, descriptor)
    except RomError as exc:
        print(f"RECOMP HINTS: FAIL\n  {exc.message}", file=sys.stderr)
        return EXIT_FOR_KIND[exc.kind]
    d = derive.run(Rom(validated.canonical))
    unresolved = hints.unresolved_sites(d)
    text = hints.dumps(hints.build(d, descriptor.sha256))
    if args.check:
        try:
            same = args.check.read_bytes() == text.encode("utf-8")
        except OSError as exc:
            print(f"RECOMP HINTS: FAIL\n  cannot read {args.check}: {exc.strerror or exc}", file=sys.stderr)
            return 7
        print(f"RECOMP HINTS: {'PASS' if same and not unresolved else 'FAIL'}")
        print(f"  {args.check}: {'reproduced byte-identically' if same else 'DIFFERS from fresh derivation'}")
        print(f"  unresolved sites: {len(unresolved)}")
        return 0 if same and not unresolved else 7
    if args.output.exists():
        print(f"RECOMP HINTS: FAIL\n  refusing to overwrite {args.output}", file=sys.stderr)
        return 8
    try:
        args.output.write_text(text, encoding="utf-8", newline="\n")
    except OSError as exc:
        print(f"RECOMP HINTS: FAIL\n  cannot write {args.output}: {exc.strerror or exc}", file=sys.stderr)
        return 8
    print(f"RECOMP HINTS: written {args.output} (unresolved sites: {len(unresolved)})")
    return 0 if not unresolved else 7


if __name__ == "__main__":
    sys.exit(main())
