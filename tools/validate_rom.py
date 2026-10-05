#!/usr/bin/env python3
"""Check that a file is the supported Goof Troop ROM.

    python3 tools/validate_rom.py path/to/GOOFT_USA.sfc [--json]

The ROM is only read.  Identity comes from config/supported_rom.json.

Exit codes:
  0  supported ROM (PASS)
  1  readable file that is not the supported ROM (wrong size or hash)
  2  command-line usage error
  3  ROM path missing, a directory, or unreadable
  4  ROM descriptor missing or malformed
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from goof_rom import (  # noqa: E402
    DESCRIPTOR_ERROR, INPUT_ERROR, ROM_MISMATCH, RomError,
    load_and_validate_rom, load_descriptor,
)


EXIT_PASS = 0
EXIT_MISMATCH = 1
EXIT_INPUT = 3
EXIT_DESCRIPTOR = 4
EXIT_FOR_KIND = {ROM_MISMATCH: EXIT_MISMATCH, INPUT_ERROR: EXIT_INPUT,
                 DESCRIPTOR_ERROR: EXIT_DESCRIPTOR}


def print_pass(report: dict) -> None:
    header = (f"{report['header_size']}-byte copier header detected and ignored"
              " (file not modified)" if report["header_present"] else "none")
    print("SUPPORTED ROM: PASS")
    print(f"  file:             {report['path']}")
    print(f"  identity:         {report['game']} ({report['region']}, "
          f"revision {report['revision']}, {report['mapping']})")
    print(f"  file size:        {report['raw_size']} bytes")
    print(f"  copier header:    {header}")
    print(f"  canonical size:   {report['canonical_size']} bytes")
    print(f"  canonical sha256: {report['canonical_sha256']}")


def print_fail(path: str, error: RomError) -> None:
    print("SUPPORTED ROM: FAIL")
    print(f"  file:   {path}")
    print(f"  reason: {error.message}")
    for key, value in error.details.items():
        if isinstance(value, dict):
            value = " ".join(f"{k}={v}" for k, v in value.items())
        print(f"  {key}: {value}")
    if error.code == "sha256" and error.details.get("internal_header_matches"):
        print("  note:   the internal header looks like the supported release;"
              " the dump is probably modified, patched or corrupted")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Check that a file is the supported Goof Troop ROM "
                    "(read-only; nothing is written).")
    parser.add_argument("rom", help="path to the ROM file")
    parser.add_argument("--json", action="store_true",
                        help="print one machine-readable JSON object")
    parser.add_argument("--descriptor", type=Path, default=None,
                        help="alternative supported_rom.json (tests/tools)")
    args = parser.parse_args(argv)

    try:
        descriptor = load_descriptor(args.descriptor)
        report = load_and_validate_rom(args.rom, descriptor).report()
    except RomError as error:
        if args.json:
            print(json.dumps({"valid": False, "path": args.rom,
                              "error_kind": error.kind, "error_code": error.code,
                              "message": error.message, **error.details},
                             indent=2, sort_keys=True))
        else:
            print_fail(args.rom, error)
        return EXIT_FOR_KIND[error.kind]

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print_pass(report)
    return EXIT_PASS


if __name__ == "__main__":
    sys.exit(main())
