#!/usr/bin/env python3
"""Build and run the host-side tests of the enhanced frontend.

    python3 tests/run_host_tests.py [--build-dir build]

Needs a finished setup (BUILD/generated/aot must exist).  Configures a second
CMake directory, BUILD/host-tests, with -DGOOF_BUILD_HOST_TESTS=ON, builds the
17 test programs and runs them with SDL's dummy video and audio drivers, so no
display, sound device or controller is needed.

  controller      controller abstraction, default mapping, player slots
  remapping       bindings, config file, Settings menu
  settings        video / audio settings and their Settings pages
  pacing          refresh-rate independent presentation, VSync
  filters         nearest / bilinear / scanlines

Exit code 0 only when every test passes.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
TESTS = Path(__file__).resolve().parent
REPO = TESTS.parent

GROUPS = {
    "E1 controller": ["controller", "host_input", "mapping_regression", "sdl_pads_virtual"],
    "E2 remapping": ["bindings", "config", "menu", "settings_sdl"],
    "E3 settings": ["present_settings", "audio_settings", "config_e3", "menu_e3", "settings_e3_sdl"],
    "E4 pacing": ["frame_pacer", "vsync_settings"],
    "E5 filters": ["filter_settings", "filter_sdl"],
}
NEEDS_SCRATCH = ("config", "settings_sdl", "settings_e3_sdl")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path, default=REPO / "build")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    a = ap.parse_args(argv)
    build = a.build_dir.resolve()
    aot = build / "generated" / "aot"
    if not (aot / "dispatch_v2.c").is_file():
        print(f"HOST TESTS: FAIL\n  {aot} not found -- run the setup first", file=sys.stderr)
        return 2
    out = build / "host-tests"
    if out.exists():
        shutil.rmtree(out)
    names = [f"goof_{t}_test" for tests in GROUPS.values() for t in tests]
    configure = ["cmake", "-S", str(REPO), "-B", str(out), f"-DGOOF_AOT_GENERATED_DIR={aot}",
                 "-DGOOF_BUILD_FRONTEND=ON", "-DGOOF_BUILD_HOST_TESTS=ON"]
    if shutil.which("ninja"):
        configure[1:1] = ["-G", "Ninja"]
    with open(build / "host-tests.log", "wb") as log:
        for cmd in (configure, ["cmake", "--build", str(out), "--parallel", str(max(1, a.jobs)),
                                "--target", *names]):
            if subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT).returncode:
                print(f"HOST TESTS: FAIL (build)\n  see {build / 'host-tests.log'}", file=sys.stderr)
                return 1

    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    suffix = ".exe" if os.name == "nt" else ""
    ok = True
    for group, tests in GROUPS.items():
        failed = []
        for short in tests:
            name = f"goof_{short}_test"
            scratch = out / "scratch" / name
            scratch.mkdir(parents=True)
            cmd = [str(out / "frontend" / (name + suffix))]
            if short == "config_e3":
                cmd += [str(TESTS / "fixtures" / "e2"), str(scratch)]
            elif short in NEEDS_SCRATCH:
                cmd.append(str(scratch))
            with open(out / "scratch" / f"{name}.log", "wb") as log:
                try:
                    rc = subprocess.run(cmd, env=env, cwd=scratch, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=300).returncode
                except subprocess.TimeoutExpired:
                    rc = -1
            if rc:
                failed.append(f"{name} (rc {rc})")
        ok = ok and not failed
        print(f"{group:16s} {'PASS' if not failed else 'FAIL  ' + ', '.join(failed)}", flush=True)
    print(f"HOST TESTS: {'PASS' if ok else 'FAIL'}  ({len(names)} programs, logs in {out / 'scratch'})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
