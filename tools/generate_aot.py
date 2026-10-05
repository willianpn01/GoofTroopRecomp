#!/usr/bin/env python3
"""Generate the AOT C for Goof Troop from the user's ROM.

    python3 tools/generate_aot.py --rom /path/to/rom.sfc --out build/generated

Steps (each one must pass before the next starts):
  1. ROM validation      tools/goof_rom.py + config/supported_rom.json
                         (exact supported revision; a 512-byte copier header
                         is stripped in memory, the user's file is only read)
  2. CFG generation      tools/generate_cfg.py (ROM analysis; the derived jump
                         tables must equal config/recomp_hints.json, then
                         config/aot_metadata.json is merged)
  3. canonical ROM copy  OUT/rom/canonical.sfc -- the engine tools and the
                         gates take a file path; local only, never distribute
  4. engine chain        snesrecomp v2_mx_analyze -> v2_mx_manifest ->
                         v2_regen --strict-manifest (continuation policy
                         'residual'); every strict conformance count must be 0
  5. provenance          OUT/generation_provenance.json (relative paths and
                         SHA-256 only; no timestamps, no absolute paths)

Outputs (OUT must be absent or empty; everything is ROM-derived, never commit):
  OUT/cfg/     bank00.cfg bank01.cfg bank02.cfg generation_manifest.json
  OUT/hints/   recomp_hints.json re-derived from the ROM (== config copy)
  OUT/rom/     canonical.sfc
  OUT/aot/     six *_v2.c files + solver facts, manifest, strict reports
  OUT/logs/    engine tool output

Exit codes: 0 ok; 1-8 as tools/generate_cfg.py (1 wrong ROM, 3 ROM
unreadable, 7 derivation != recomp_hints.json, 8 output dir); 10 engine
missing or engine lock mismatch; 11 engine chain failed; 12 strict
conformance not clean.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import generate_cfg  # noqa: E402
from romcfg import hints as hints_mod  # noqa: E402

DEFAULT_ENGINE = REPO / "external" / "snesrecomp"
ENGINE_LOCK = REPO / "external" / "snesrecomp.lock.json"
POLICY = "residual"
BANKS = ("00", "01", "02")
AOT_C = ("bank00_v2.c", "bank01_v2.c", "bank02_v2.c", "dispatch_v2.c",
         "aot_entries_v2.c", "unresolved_stubs_v2.c")
STRICT_REPORTS = ("strict_conformance.json", "strict_boundary_conformance.json",
                  "strict_dispatch_registry_conformance.json",
                  "strict_fallback_runtime_conformance.json")
EXIT_ENGINE = 10
EXIT_CHAIN = 11
EXIT_STRICT = 12


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def engine_digest(engine: Path) -> tuple[str, int]:
    """sha256 over 'sha256  relpath' lines of every engine file (sorted)."""
    lines = []
    for p in sorted(engine.rglob("*"), key=lambda q: q.relative_to(engine).as_posix()):
        if p.is_file() and "__pycache__" not in p.parts:
            lines.append(f"{sha256_file(p)}  {p.relative_to(engine).as_posix()}\n")
    return sha256_bytes("".join(lines).encode()), len(lines)


def child_env() -> dict:
    """Environment for the engine tools: inherited, minus knobs that change
    their behaviour (SNESRECOMP_*, GOOF_*, PYTHON* paths)."""
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("SNESRECOMP_", "GOOF_", "PYTHONPATH", "PYTHONHOME",
                                "PYTHONSTARTUP", "PYTHONHASHSEED"))}
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONHASHSEED"] = "0"
    return env


def run_step(cmd: list, log: Path, cwd: Path, env: dict) -> int:
    with open(log, "wb") as lf:
        return subprocess.run([str(c) for c in cmd], cwd=cwd, env=env,
                              stdout=lf, stderr=subprocess.STDOUT).returncode


def strict_counts(aot: Path) -> tuple[dict, dict]:
    checks, nonzero = {}, {}
    for name in STRICT_REPORTS:
        p = aot / name
        if not p.is_file():
            nonzero[name] = "missing"
            continue
        for k, v in json.loads(p.read_text(encoding="utf-8")).get("counts", {}).items():
            checks[k] = v
            if v:
                nonzero[k] = v
    return checks, nonzero


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Generate the Goof Troop AOT C from the user's ROM.")
    ap.add_argument("--rom", type=Path, required=True, help="path to your ROM file (read-only)")
    ap.add_argument("--out", type=Path, required=True, help="output directory (absent or empty)")
    ap.add_argument("--engine", type=Path, default=DEFAULT_ENGINE,
                    help="snesrecomp checkout (default: external/snesrecomp)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                    help="parallel emit workers for v2_regen (output does not depend on it)")
    ap.add_argument("--skip-engine-lock", action="store_true",
                    help="do not compare the engine with external/snesrecomp.lock.json")
    a = ap.parse_args(argv)

    out = a.out.resolve()
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        print(f"AOT GENERATION: FAIL\n  output directory not empty: {a.out}", file=sys.stderr)
        return generate_cfg.EXIT_OUTPUT
    engine = a.engine.resolve()
    if not (engine / "tools" / "v2_regen.py").is_file():
        print(f"AOT GENERATION: FAIL\n  snesrecomp not found at {a.engine}", file=sys.stderr)
        return EXIT_ENGINE

    # engine identity (before any output is written)
    digest, nfiles = engine_digest(engine)
    lock = json.loads(ENGINE_LOCK.read_text(encoding="utf-8")) if ENGINE_LOCK.is_file() else None
    if lock and not a.skip_engine_lock and digest != lock["content_sha256"]:
        print("AOT GENERATION: FAIL\n  snesrecomp content does not match "
              f"external/snesrecomp.lock.json (revision {lock['revision']})\n"
              f"  expected {lock['content_sha256']}\n  found    {digest}", file=sys.stderr)
        return EXIT_ENGINE

    # 1 + 2: ROM validation and CFG generation (pure until written)
    try:
        result = generate_cfg.generate(a.rom, None, generate_cfg.CONFIG / "recomp_hints.json",
                                       generate_cfg.CONFIG / "aot_metadata.json")
    except generate_cfg.GenerationError as exc:
        print(f"AOT GENERATION: FAIL (ROM validation / CFG generation)\n  {exc}", file=sys.stderr)
        return exc.code
    v = result["validated"]
    header = f"{v.header_size}-byte copier header stripped in memory" if v.header_present else "no copier header"
    print(f"ROM VALIDATION: PASS  canonical sha256 {v.canonical_sha256} ({header})")

    out.mkdir(parents=True, exist_ok=True)
    cfg, aot, logs = out / "cfg", out / "aot", out / "logs"
    generate_cfg.write_outputs(cfg, result, False)
    s = result["manifest"]["summary"]
    print(f"CFG GENERATION: PASS  {s['jump_tables']} jump tables, {s['entries']} entries, "
          f"{s['entry_mx_variants']} M/X variants, {s['aot_metadata_entries']} aot_metadata entries")

    (out / "hints").mkdir()
    hints_text = hints_mod.dumps(hints_mod.build(result["derivation"], v.descriptor.sha256))
    (out / "hints" / "recomp_hints.json").write_text(hints_text, encoding="utf-8", newline="\n")

    # 3: canonical ROM copy for path-based tools
    (out / "rom").mkdir()
    rom = out / "rom" / "canonical.sfc"
    rom.write_bytes(v.canonical)

    # 4: engine chain
    aot.mkdir()
    logs.mkdir()
    env = child_env()
    cfg_args = []
    for b in BANKS:
        cfg_args += ["--cfg", cfg / f"bank{b}.cfg"]
    facts = aot / "mx_variant_facts.json"
    manifest = aot / "mx_authoritative_variant_manifest.json"
    steps = [
        ("v2_mx_analyze", [sys.executable, engine / "tools/v2_mx_analyze.py", "--rom", rom, *cfg_args,
                           "--continuation-policy", POLICY, "--output", facts]),
        ("v2_mx_manifest", [sys.executable, engine / "tools/v2_mx_manifest.py", "--facts", facts,
                            *cfg_args, "--output", manifest]),
        ("v2_regen", [sys.executable, engine / "tools/v2_regen.py", "--rom", rom, "--cfg-dir", cfg,
                      "--out-dir", aot, "--jobs", str(max(1, a.jobs)), "--strict-manifest", manifest,
                      "--solver-facts", facts, "--require-continuation-policy", POLICY]),
    ]
    for name, cmd in steps:
        print(f"ENGINE: {name} ...", flush=True)
        rc = run_step(cmd, logs / f"{name}.log", out, env)
        if rc:
            print(f"AOT GENERATION: FAIL\n  {name} exited {rc}; see {logs / (name + '.log')}", file=sys.stderr)
            return EXIT_CHAIN
    missing = [n for n in AOT_C if not (aot / n).is_file()]
    if missing:
        print(f"AOT GENERATION: FAIL\n  missing generated C: {missing}", file=sys.stderr)
        return EXIT_CHAIN
    checks, nonzero = strict_counts(aot)
    if nonzero:
        print(f"AOT GENERATION: FAIL\n  strict conformance not clean: {nonzero}", file=sys.stderr)
        return EXIT_STRICT
    print(f"STRICT CONFORMANCE: PASS  {len(checks)} checks, 0 nonzero")

    # 5: provenance (relative paths, hashes, no timestamps)
    def rel_hashes(d: Path, pattern: str) -> dict:
        return {p.name: sha256_file(p) for p in sorted(d.glob(pattern)) if p.is_file()}
    prov = {
        "schema_version": 1,
        "rom_canonical_sha256": v.canonical_sha256,
        "rom_header_stripped": bool(v.header_present),
        "config": {n: sha256_file(generate_cfg.CONFIG / n)
                   for n in ("supported_rom.json", "recomp_hints.json", "aot_metadata.json")},
        "derived_recomp_hints_sha256": sha256_bytes(hints_text.encode("utf-8")),
        "derived_recomp_hints_equal_config":
            hints_text.encode("utf-8") == (generate_cfg.CONFIG / "recomp_hints.json").read_bytes(),
        "engine": {"revision": lock["revision"] if lock else None,
                   "content_sha256": digest, "files": nfiles,
                   "lock_checked": bool(lock) and not a.skip_engine_lock},
        "continuation_policy": POLICY,
        "cfg": rel_hashes(cfg, "*"),
        "aot_c": {n: sha256_file(aot / n) for n in AOT_C},
        "aot_json": rel_hashes(aot, "*.json"),
        "strict_checks": len(checks),
        "strict_nonzero": 0,
        "summary": s,
    }
    (out / "generation_provenance.json").write_text(json.dumps(prov, indent=1, sort_keys=True) + "\n",
                                                    encoding="utf-8", newline="\n")
    print("AOT GENERATION: PASS")
    for n in AOT_C:
        print(f"  {prov['aot_c'][n]}  aot/{n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
