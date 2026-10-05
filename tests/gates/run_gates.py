#!/usr/bin/env python3
"""Run the Original-mode semantic gates against a finished build.

    python3 tests/gates/run_gates.py --build-dir build [--out build/gates]

Uses BUILD/cmake (executables) and BUILD/generated/rom/canonical.sfc (the ROM
copy written by tools/generate_aot.py); --rom overrides the ROM.  Output files
go to OUT (default BUILD/gates, must not exist).  Every expectation below is
the approved Original-mode result; nothing is re-baselined here.

Gates: E1000 (stdout+stderr), audio pins, headless neutral, campaign stdout,
binary record (campaign.rec), PBN (--quiet; rc 1 is canonical: PBN10 needs an
external reference directory), player --headless-compare, PPU window/HDMA
unit test (no ROM), and the canonical semantic fields.

Exit code 0 only when every gate matches.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
TESTS = HERE.parent

CANON_SHA = {
    "e1000.out": "5753238518c66b7d2537414b03c90c7448ccebea0184b113621896408fecf7ea",
    "e1000.err": "ec2ca34ec326544717922a1655246c2dc66f5714d4d94a351a356f6b539ce324",
    "neutral.out": "3516caf0da399e1a6a67ca87d4c398642407231d505a8c38003c874965bed605",
    "campaign.out": "da629de18e7cfede8bccc1e29ab19e272039b4d23e9e28a8c4cecbd9e283feec",
    "campaign.rec": "fe2f5697d4b907d29b89fee4dab33db16d21d1dfedda9c717b79212b69f247f7",
    "pbn.out": "a79e6d11a34c47e793faeac50a1b0fc2f49236bcd0c7223d37897625f4bb98f3",
    "player_compare.out": "c96f9a9fff53a82d075a98c4d7744c644d18cb425e12bb1468202aa9741d93b2",
}
CANON_RC = {"e1000.out": 0, "neutral.out": 0, "campaign.out": 0, "pbn.out": 1,
            "player_compare.out": 0, "ppu_window_hdma.out": 0}
CANON_FIELDS = {
    "CPU": "logical=039d1c22980acac9",
    "NMI": "ordinal_digest=e6a9e71774e713f1",
    "MASTER": "sum=429556336 final=429556336 EXACT",
    "HVBJOY": "b7565e761aa3a5e8",
    "PCM": "AUDIO1_NATIVE_PCM hash=ca81727d738b42c8 frames=640800",
    "APU RAM": "apu_ram=2c9a2d8c25bb8256",
    "PHYSICAL PERIODS": "total_physical_periods=1200",
    "IRQ": "1a3b952ad7250813",
    "JOURNAL": "ed6cda32e78adb92",
}


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path, required=True)
    ap.add_argument("--rom", type=Path, default=None)
    ap.add_argument("--out", type=Path, default=None)
    a = ap.parse_args(argv)
    build = a.build_dir.resolve()
    exe = build / "cmake"
    rom = (a.rom or build / "generated" / "rom" / "canonical.sfc").resolve()
    out = (a.out or build / "gates").resolve()
    if out.exists():
        ap.error(f"refusing: {out} exists")
    out.mkdir(parents=True)
    # GOOF_*_TRACE / SNESRECOMP_* switch on diagnostic output; never inherit them.
    env = {k: v for k, v in os.environ.items() if not k.startswith(("GOOF_", "SNESRECOMP_"))}
    rc: dict = {}

    def go(name, cmd, errname=None):
        with open(out / name, "wb") as o, open(out / (errname or name + ".stderr"), "wb") as e:
            # cwd = tests/, so the input script is passed as the same relative path
            # the approved oracle used ("input_scripts/campaign.txt").
            r = subprocess.run([str(c) for c in cmd], cwd=TESTS, env=env, stdout=o, stderr=e)
        rc[name] = r.returncode

    e1000 = exe / "gates/goof_boot_e1000"
    headless = exe / "frontend/goof_app_headless"
    go("e1000.out", [e1000, rom], "e1000.err")
    go("audio_pins.out", [e1000, rom, "--audio-pins-advisory"])
    go("neutral.out", [headless, rom])
    go("campaign.out", [headless, rom, "--input-script", "input_scripts/campaign.txt",
                        "--record", out / "campaign.rec"])
    if (exe / "gates/goof_pbn").exists():
        go("pbn.out", [exe / "gates/goof_pbn", rom, "--title-epoch", "501", "--epochs", "1000",
                       "--aot-kinds", "all", "--quiet"])
    if (exe / "frontend/goof_recomp").exists():
        go("player_compare.out", [exe / "frontend/goof_recomp", rom, "--headless-compare"])
    if (exe / "gates/goof_ppu_window_hdma_test").exists():
        go("ppu_window_hdma.out", [exe / "gates/goof_ppu_window_hdma_test"])

    res: dict = {"rom_sha256": sha(rom), "rc": rc, "sha256": {}, "match": {}}
    for name, want in CANON_SHA.items():
        p = out / name
        res["sha256"][name] = sha(p) if p.exists() else None
        res["match"][name] = res["sha256"][name] == want
    res["rc_match"] = {n: rc.get(n) == want for n, want in CANON_RC.items()}
    text = "".join((out / n).read_text(errors="replace")
                   for n in ("e1000.out", "pbn.out", "audio_pins.out") if (out / n).exists())
    res["fields"] = {k: (v in text) for k, v in CANON_FIELDS.items()}
    ok = all(res["match"].values()) and all(res["rc_match"].values()) and all(res["fields"].values())
    res["pass"] = ok
    (out / "result.json").write_text(json.dumps(res, indent=1, sort_keys=True) + "\n")

    labels = [("E1000", ["e1000.out", "e1000.err"]), ("NEUTRAL", ["neutral.out"]),
              ("CAMPAIGN", ["campaign.out"]), ("BINARY RECORD", ["campaign.rec"]),
              ("PBN", ["pbn.out"]), ("HEADLESS-COMPARE", ["player_compare.out"])]
    for label, names in labels:
        good = all(res["match"][n] for n in names) and all(res["rc_match"].get(n, True) for n in names)
        print(f"{label:18s} {'PASS' if good else 'FAIL'}")
    print(f"{'PPU WINDOW/HDMA':18s} {'PASS' if res['rc_match']['ppu_window_hdma.out'] else 'FAIL'}")
    for k, good in res["fields"].items():
        print(f"  field {k:17s} {'MATCH' if good else 'MISSING'}  {CANON_FIELDS[k]}")
    print(f"GATES: {'PASS' if ok else 'FAIL'}  ({out})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
