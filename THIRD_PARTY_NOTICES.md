# Third-party notices and credits

This file lists third-party material that is **present in this repository**,
material that is **used but not included**, and acknowledgements. It states
facts found in the files themselves; it is not legal advice.

## Included in this repository

### snesrecomp — `external/snesrecomp/`

- Project: https://github.com/mstan/snesrecomp (Matthew Stan)
- Role: static recompilation toolchain (Python) and SNES hardware runtime (C).
- Contents: a snapshot based on upstream commit
  `93974d9b9e2e00485f795b489b823e13f4a3cec1` with changes required by this
  project. Game-specific engine tests, the launcher's image and font assets
  and one image under `.github/` are not included. The snapshot's digest is in
  `external/snesrecomp.lock.json`.
- License status: this snapshot contains no license file. Its `README.md`
  states "License: Not yet declared." The upstream project has since published
  its original code under PolyForm Noncommercial License 1.0.0. This does not
  establish the license or redistribution status of the older snapshot used
  here; that status is being confirmed before this repository is made public.
- Its own acknowledgements are in `external/snesrecomp/README.md`
  (snesrev ports, LakeSnes, snes9x, SMWDisX).

### LakeSnes — inside `external/snesrecomp/runner/src/snes/`

- Project: https://github.com/angelo-wf/lakesnes (angelo_wf), MIT License.
- `interp816.c` / `interp816.h` are derived from its 65816 CPU core; the full
  MIT notice is reproduced in `external/snesrecomp/THIRD_PARTY_ATTRIBUTION.md`.
- The snesrecomp README states that the hardware core in the same folder
  derives from LakeSnes "(and, transitively, snes9x); their respective terms
  apply to that code."
- The snesrecomp project identifies its `THIRD_PARTY_ATTRIBUTION.md` as the
  place containing required notices for binary distributions of its runner.
  This repository currently distributes source only; any future runner binary
  package must include the applicable notice text for the third-party code
  actually linked into that binary.

### libretro API header — `external/snesrecomp/tools/snesref/libretro.h`

- Copyright (C) 2010-2024 The RetroArch team; MIT license text in the file's
  header. Not used by this project's build.

### Color LUT — `external/snesrecomp/runner/src/snes/color_lut.c`

- File header: "ported from gbarecomp … via JRickey/gba-recomp crates/screen,
  © Jrickey, MIT OR Apache-2.0."

### SHA-256 — `external/snesrecomp/runner/src/sha256.c`

- File header: "reference SHA-256 implementation (public domain)".

## Used but not included

### SDL2

- https://www.libsdl.org — zlib license.
- Linked dynamically. On Windows, the setup copies `SDL2.dll` from your MSYS2
  installation into your local `dist\` folder; it is not part of this
  repository.

### Build tools

GCC / MinGW-w64, CMake, Ninja, Python and MSYS2 are installed by the user and
are not redistributed here.

## Reference data

- `gates/goof_pbn1_reference.h` holds 512 bytes of video-RAM state captured
  from a reference emulator (bsnes-mercury, Accuracy profile) running the
  game. It is the expected value of one gate.
- `gates/goof_ppu_window_hdma_test.c` contains a 13-byte HDMA table used as a
  unit-test input.

## Acknowledgements

- **Goof-Troop-Disassembly** by Yoshifanatic1
  (https://github.com/Yoshifanatic1/Goof-Troop-Disassembly, GPL-3.0) was an
  important research and reference resource. It is not distributed with this
  project, and this project's build does not require it.
- The SNES emulation and reverse-engineering community whose documentation
  made this work possible.

## Not included

No ROM, no ROM-derived generated code and no game assets are part of this
repository. Goof Troop and related names are the property of their respective
owners.
