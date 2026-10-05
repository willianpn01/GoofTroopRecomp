#!/usr/bin/env python3
"""Generate the snesrecomp CFG files for Goof Troop from the user's ROM.

    python3 tools/generate_cfg.py --rom /path/to/GOOFT_USA.sfc --output OUT_DIR

Inputs (nothing else is read):
  --rom            the user's ROM (read-only; a 512-byte copier header is
                   handled by goof_rom, never written back)
  config/supported_rom.json, config/recomp_hints.json, config/aot_metadata.json
                   (defaults next to this tool; overridable for tests)

The ROM is analysed from its reset/interrupt vectors (65816 recursive
descent, M/X propagation, indexed-jump tables, function partition).  The
freshly derived jump tables must equal recomp_hints.json exactly, otherwise
generation stops.  aot_metadata.json is then merged by apply_cfg_metadata.

Outputs in OUT_DIR (must be absent or empty; ROM-derived, never commit):
  bank00.cfg bank01.cfg bank02.cfg   CFG input of the snesrecomp v2 chain
  generation_manifest.json            input/output SHA-256 and counts
  derivation_report.json              only with --report (per-entry detail)

Exit codes:
  0  CFG generated
  1  the ROM is not the supported ROM
  2  command-line usage error
  3  ROM path missing, a directory, or unreadable
  4  supported_rom.json missing or malformed
  5  recomp_hints.json missing or malformed
  6  aot_metadata.json missing or malformed
  7  derivation disagrees with recomp_hints.json or leaves tables unresolved
  8  output directory not usable
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

TOOLS = Path(__file__).resolve().parent
CONFIG = TOOLS.parent / "config"
sys.path.insert(0, str(TOOLS))

import apply_cfg_metadata  # noqa: E402
from goof_rom import (  # noqa: E402
    DESCRIPTOR_ERROR, INPUT_ERROR, ROM_MISMATCH, RomError,
    load_and_validate_rom, load_descriptor,
)
import romcfg  # noqa: E402
from romcfg import aot_metadata, cfg_emit, derive, hints  # noqa: E402
from romcfg.cpu65816 import CODE_BANKS, Rom  # noqa: E402

EXIT_OK = 0
EXIT_FOR_KIND = {ROM_MISMATCH: 1, INPUT_ERROR: 3, DESCRIPTOR_ERROR: 4}
EXIT_HINTS = 5
EXIT_METADATA = 6
EXIT_DERIVATION = 7
EXIT_OUTPUT = 8


class GenerationError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def generate(rom_path: Path, descriptor_path: Path | None, hints_path: Path,
             metadata_path: Path | None) -> dict:
    """Pure generation (no writes).  Returns files + manifest + report data."""
    try:
        descriptor = load_descriptor(descriptor_path)
        validated = load_and_validate_rom(rom_path, descriptor)
    except RomError as exc:
        raise GenerationError(EXIT_FOR_KIND[exc.kind], exc.message) from None
    try:
        _raw, pinned, hints_bytes = hints.load(hints_path, descriptor.sha256)
    except hints.HintsError as exc:
        raise GenerationError(EXIT_HINTS, str(exc)) from None
    manifest = None
    metadata_bytes = b""
    if metadata_path is not None:
        try:
            manifest = aot_metadata.load(metadata_path, apply_cfg_metadata)
        except aot_metadata.MetadataError as exc:
            raise GenerationError(EXIT_METADATA, str(exc)) from None
        metadata_bytes = Path(metadata_path).read_bytes()

    d = derive.run(Rom(validated.canonical), hint_roots=pinned["entry_roots"])
    derived = hints.derived_tables(d)
    problems = [f"unresolved indexed jump at {s}" for s in hints.unresolved_sites(d)]
    problems += hints.diff_tables(pinned["tables"], derived)
    if problems:
        shown = "\n  ".join(problems[:20])
        more = f"\n  ... {len(problems) - 20} more" if len(problems) > 20 else ""
        raise GenerationError(EXIT_DERIVATION,
                              "fresh derivation disagrees with recomp_hints.json:\n  " + shown + more)
    try:
        modes = cfg_emit.entry_modes(d, pinned["mode_overrides"])
    except ValueError as exc:
        raise GenerationError(EXIT_HINTS, str(exc)) from None

    files: dict[str, str] = {}
    for bank in CODE_BANKS:
        name = f"bank{bank:02x}.cfg"
        text = cfg_emit.emit_bank(d, pinned["tables"], modes, bank)
        if manifest is not None:
            try:
                text = apply_cfg_metadata.apply_metadata(text, manifest, name)
            except ValueError as exc:
                raise GenerationError(EXIT_METADATA, f"aot_metadata does not merge: {exc}") from None
        files[name] = text

    res = d["analysis"]
    variants = sum(len(v) for b in CODE_BANKS for v in modes[b].values())
    summary = {
        "code_banks": list(CODE_BANKS),
        "jump_tables": len(pinned["tables"]),
        "jump_tables_with_bias": sum(1 for v in pinned["tables"].values() if v[1]),
        "entries": sum(len(modes[b]) for b in CODE_BANKS),
        "entries_per_bank": {f"{b:02x}": len(modes[b]) for b in CODE_BANKS},
        "entry_mx_variants": variants,
        "multi_mode_entries": sum(1 for b in CODE_BANKS for v in modes[b].values() if len(v) > 1),
        "pointer_literal_entries": len(d["roots"]),
        "entry_hints": len(pinned["entry_roots"]),
        "mode_hints": len(pinned["mode_overrides"]),
        "mirror_aliases": len(res.far_aliases),
        "aot_metadata_entries": len(manifest["entries"]) if manifest else 0,
        "continuation_entries": sum(t.count("\ncontinuation_entry ") for t in files.values()),
        "cfg_func_lines": sum(t.count("\nfunc ") for t in files.values()),
    }
    gen_manifest = {
        "tool": romcfg.TOOL_VERSION,
        "algorithm": hints.ALGORITHM,
        "inputs": {
            "rom_canonical_sha256": validated.canonical_sha256,
            "supported_rom_json_sha256": sha256(Path(descriptor.source).read_bytes()),
            "recomp_hints_json_sha256": sha256(hints_bytes),
            "aot_metadata_json_sha256": sha256(metadata_bytes) if manifest is not None else None,
        },
        "outputs": {name: sha256(text.encode("utf-8")) for name, text in sorted(files.items())},
        "summary": summary,
    }
    return {"files": files, "manifest": gen_manifest, "derivation": d, "modes": modes,
            "validated": validated}


def report(d, modes) -> dict:
    """Per-entry / per-site detail (ROM-derived addresses; local use only)."""
    res = d["analysis"]
    entries = {}
    for b in CODE_BANKS:
        for pc in sorted(modes[b]):
            cert = res.certain.get((b, pc)) or res.state_certain[b].get(pc) or set()
            entries[hints.pc24(b, pc)] = {
                "modes": modes[b][pc],
                "certain": all(tuple(int(v) for v in m.split(",")) in cert for m in modes[b][pc]),
                "reasons": sorted(d["reasons"][b].get(pc, ()))}
    sites = {hints.pc24(b, s): {"class": dec.cls, "method": dec.method, "count": dec.count,
                                "bias": dec.bias}
             for (b, s), dec in sorted(d["decisions"].items())}
    return {"entries": entries, "sites": sites,
            "pointer_literal_roots": [f"{hints.pc24(b, pc)}:{m},{x}:{why}" for b, pc, m, x, why in d["roots"]],
            "mirror_aliases": sorted(f"{l:06X}->{hints.pc24(pb, pc)}" for l, pb, pc in res.far_aliases),
            "instruction_starts": {f"{b:02X}": len(res.starts[b]) for b in CODE_BANKS}}


def write_outputs(out: Path, result: dict, with_report: bool) -> None:
    try:
        if out.exists() and (not out.is_dir() or any(out.iterdir())):
            raise GenerationError(EXIT_OUTPUT, f"output directory exists and is not empty: {out}")
        out.mkdir(parents=True, exist_ok=True)
        for name, text in sorted(result["files"].items()):
            (out / name).write_text(text, encoding="utf-8", newline="\n")
        (out / "generation_manifest.json").write_text(
            json.dumps(result["manifest"], indent=1, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
        if with_report:
            (out / "derivation_report.json").write_text(
                json.dumps(report(result["derivation"], result["modes"]), indent=1, sort_keys=True) + "\n",
                encoding="utf-8", newline="\n")
    except OSError as exc:
        raise GenerationError(EXIT_OUTPUT, f"cannot write {out}: {exc.strerror or exc}") from None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate snesrecomp CFG files from the user's Goof Troop ROM.")
    parser.add_argument("--rom", type=Path, required=True, help="path to the user's ROM")
    parser.add_argument("--output", type=Path, required=True,
                        help="output directory (absent or empty)")
    parser.add_argument("--report", action="store_true",
                        help="also write derivation_report.json (per-entry detail)")
    parser.add_argument("--descriptor", type=Path, default=None,
                        help="alternative supported_rom.json (tests)")
    parser.add_argument("--hints", type=Path, default=CONFIG / "recomp_hints.json",
                        help="alternative recomp_hints.json (tests)")
    parser.add_argument("--aot-metadata", type=Path, default=CONFIG / "aot_metadata.json",
                        help="alternative aot_metadata.json (tests)")
    parser.add_argument("--no-aot-metadata", action="store_true",
                        help="emit the CFG without merging aot_metadata.json")
    args = parser.parse_args(argv)
    try:
        result = generate(args.rom, args.descriptor, args.hints,
                          None if args.no_aot_metadata else args.aot_metadata)
        write_outputs(args.output, result, args.report)
    except GenerationError as exc:
        print("CFG GENERATION: FAIL", file=sys.stderr)
        print(f"  {exc}", file=sys.stderr)
        return exc.code
    v = result["validated"]
    s = result["manifest"]["summary"]
    header = f"{v.header_size}-byte copier header ignored" if v.header_present else "no copier header"
    print("CFG GENERATION: PASS")
    print(f"  rom:           canonical sha256 {v.canonical_sha256} ({header})")
    print(f"  jump tables:   {s['jump_tables']} (re-derived, equal to recomp_hints.json)")
    print(f"  entries:       {s['entries']} ({s['entry_mx_variants']} M/X variants)")
    print(f"  aot metadata:  {s['aot_metadata_entries']} entries merged "
          f"({s['continuation_entries']} continuation entries)")
    for name, digest in result["manifest"]["outputs"].items():
        print(f"  {digest}  {name}")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
