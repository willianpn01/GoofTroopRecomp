#!/usr/bin/env python3
"""One-command bootstrap: validate ROM -> generate -> configure -> build.

    python3 tools/goof_build.py --rom /path/to/rom.sfc

  BUILD/generated/   tools/generate_aot.py output (CFG, generated C, canonical ROM copy)
  BUILD/cmake/       CMake binary directory (executables under gates/ and frontend/)

BUILD defaults to ./build next to this repository's CMakeLists.txt and must be
absent or empty: every build starts from scratch (remove it with `rm -rf build`
to rebuild).  Nothing outside BUILD is written.

Exit codes: those of generate_aot.py for generation failures; 20 build
directory not empty; 21 CMake configure failed; 22 build failed.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import generate_aot  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Validate the ROM, generate the AOT C and build.")
    ap.add_argument("--rom", type=Path, required=True, help="path to your ROM file (read-only)")
    ap.add_argument("--build-dir", type=Path, default=REPO / "build",
                    help="build directory, absent or empty (default: ./build)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--no-frontend", action="store_true",
                    help="skip the SDL2 player (headless targets only; SDL2 not needed)")
    ap.add_argument("--generator", default="Ninja" if shutil.which("ninja") else None,
                    help="CMake generator (default: Ninja when installed)")
    a = ap.parse_args(argv)

    build = a.build_dir.resolve()
    if build.exists() and (not build.is_dir() or any(build.iterdir())):
        print(f"BUILD: FAIL\n  build directory not empty: {a.build_dir}\n"
              f"  start from scratch with: rm -rf {shlex.quote(str(a.build_dir))}", file=sys.stderr)
        return 20

    rc = generate_aot.main(["--rom", str(a.rom), "--out", str(build / "generated"),
                            "--jobs", str(a.jobs)])
    if rc:
        return rc

    cmake_dir = build / "cmake"
    configure = ["cmake", "-S", str(REPO), "-B", str(cmake_dir),
                 f"-DGOOF_AOT_GENERATED_DIR={build / 'generated' / 'aot'}",
                 f"-DGOOF_BUILD_FRONTEND={'OFF' if a.no_frontend else 'ON'}"]
    if a.generator:
        configure[1:1] = ["-G", a.generator]
    print("CONFIGURE:", " ".join(shlex.quote(c) for c in configure), flush=True)
    if subprocess.run(configure).returncode:
        print("BUILD: FAIL (CMake configure)", file=sys.stderr)
        return 21
    compile_cmd = ["cmake", "--build", str(cmake_dir), "--parallel", str(max(1, a.jobs))]
    print("BUILD:", " ".join(shlex.quote(c) for c in compile_cmd), flush=True)
    if subprocess.run(compile_cmd).returncode:
        print("BUILD: FAIL (compile)", file=sys.stderr)
        return 22

    rom = build / "generated" / "rom" / "canonical.sfc"
    print("\nBUILD: PASS")
    if not a.no_frontend:
        print("Run the player:")
        player = 'GoofTroopRecomp.exe' if os.name == 'nt' else 'goof_recomp'
        print(f"  {shlex.quote(str(cmake_dir / 'frontend' / player))} {shlex.quote(str(rom))} --scale 3")
    print("Run the semantic gates:")
    print(f"  python3 {shlex.quote(str(REPO / 'tests' / 'gates' / 'run_gates.py'))} "
          f"--build-dir {shlex.quote(str(build))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
