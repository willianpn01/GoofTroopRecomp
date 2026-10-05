# Building on Linux

## The short way

```sh
./setup_linux.sh "/path/to/Goof Troop (USA).sfc"
./run_game.sh
```

`setup_linux.sh` checks the prerequisites, validates the ROM, generates the C
code from it, compiles, and fills `dist/`. It takes well under a minute on a
recent machine. Running it again rebuilds from scratch.

The ROM path may contain spaces (quote it). If you copy the ROM into the
repository folder instead, `./setup_linux.sh` with no argument finds it; ROM
files there are ignored by git.

## Prerequisites

| Tool | Used for |
|---|---|
| Python 3.9+ | ROM validator, code generator, build driver |
| C compiler (GCC) | compiling the generated C and the game |
| CMake 3.16+ | build configuration |
| Ninja (or Make) | running the build |
| SDL2 development files, pkg-config | window, audio, controllers |

```sh
# Debian / Ubuntu
sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev python3
# Fedora
sudo dnf install gcc cmake ninja-build pkgconf-pkg-config SDL2-devel python3
# Arch
sudo pacman -S --needed base-devel cmake ninja sdl2 python
```

No Python packages are needed beyond the standard library.

## What gets generated

Everything is inside the repository and ignored by git:

```
build/generated/   ROM-derived: cfg/, hints/, aot/ (the generated C),
                   rom/canonical.sfc, logs/, generation_provenance.json
build/cmake/       CMake/Ninja output (gates/, frontend/)
dist/              GoofTroopRecomp, canonical.sfc, run_game.sh
```

`dist/` is self-contained apart from the system SDL2 library; you can copy the
folder elsewhere on the same machine. Do not share it: it contains a copy of
your ROM and code derived from it.

Settings are stored in `$XDG_CONFIG_HOME/GoofTroopRecomp/config.ini`
(normally `~/.config/GoofTroopRecomp/config.ini`). Run the game with
`--portable` to keep `config.ini` next to the executable instead.

## Clean

```sh
./setup_linux.sh --clean
```

Removes `build/` and `dist/` and nothing else.

## Manual build (developers)

The wrapper is a thin layer over these steps:

```sh
python3 tools/validate_rom.py /path/to/rom.sfc           # identity check only
python3 tools/goof_build.py --rom /path/to/rom.sfc       # generate + configure + build into ./build
build/cmake/frontend/goof_recomp build/generated/rom/canonical.sfc
```

or fully by hand:

```sh
python3 tools/generate_aot.py --rom /path/to/rom.sfc --out build/generated
cmake -S . -B build/cmake -G Ninja \
      -DGOOF_AOT_GENERATED_DIR=build/generated/aot -DGOOF_BUILD_FRONTEND=ON
cmake --build build/cmake
```

`tools/goof_build.py` requires its build directory to be absent or empty.
`-DGOOF_BUILD_FRONTEND=OFF` (or `goof_build.py --no-frontend`) builds only the
headless targets and does not need SDL2.

## Tests

After a successful setup:

```sh
python3 tests/gates/run_gates.py --build-dir build   # deterministic gates: boot, audio, input campaign, rendering
python3 tests/run_host_tests.py                      # frontend tests: controllers, remapping, settings, pacing, filters
python3 -m unittest discover -s tests                # ROM validator and generator unit tests
```

`run_gates.py` writes to `build/gates` and refuses to run if that folder
exists; delete it to run again. The unit tests use a synthetic ROM; set
`GOOF_TEST_ROM=/path/to/rom.sfc` to also run the tests that read your ROM.
The host tests use SDL's dummy drivers and need no display.

## Troubleshooting

- **`SETUP: FAIL -- missing prerequisites`** — install the listed packages
  with the command shown, then run setup again.
- **`SUPPORTED ROM: FAIL`** — the file is not the supported revision. The
  output shows the size and SHA-256 found; compare with the README.
- **`snesrecomp content does not match external/snesrecomp.lock.json`** — a
  file under `external/snesrecomp` was changed or converted (for example line
  endings). Restore the folder from the repository.
- **CMake cannot find SDL2** — install the SDL2 development package
  (`libsdl2-dev`, `SDL2-devel` or `sdl2`).
- **`build ... does not look like something setup created`** — setup only
  deletes folders it recognises. Move or remove that folder yourself.
- **The game opens but no controller is detected** — plug it in before or
  after starting; the terminal prints a `HOST_GAMEPAD` line when one is
  attached. `--no-gamepad` forces keyboard only.

Run `dist/GoofTroopRecomp` with no arguments to list every command-line option.
