# Architecture

## Pipeline

```
your ROM
  │  tools/validate_rom.py, tools/goof_rom.py       exact revision check (config/supported_rom.json);
  │                                                 a 512-byte copier header is ignored in memory
  ▼
ROM-derived configuration
  │  tools/generate_cfg.py, tools/romcfg/           analyses the ROM: jump tables, entry points, M/X modes;
  │                                                 the result must equal config/recomp_hints.json,
  │                                                 then config/aot_metadata.json is merged
  ▼
static recompiler (external/snesrecomp)
  │  v2_mx_analyze → v2_mx_manifest → v2_regen      strict mode: every conformance count must be zero
  ▼
generated C  (build/generated/aot/*.c, local only)
  │  CMake                                          compiled once into goof_core with the SNES runtime
  ▼
gates (headless)                frontend (SDL2)
```

`tools/generate_aot.py` runs the first three stages; `tools/goof_build.py`
adds CMake configure and build; `tools/goof_setup.py` (behind
`setup_linux.sh` / `setup_windows.bat`) adds the prerequisite check and the
`dist/` bundle.

Nothing ROM-derived is stored in the repository. A clone is source only; the
generated C, the canonical ROM copy and the binaries exist only under `build/`
and `dist/`.

## Source layout

| Path | Contents |
|---|---|
| `config/` | supported ROM identity; recompiler hints and metadata checked against the ROM analysis |
| `tools/` | ROM validator, ROM analysis (`romcfg/`), generation and build drivers |
| `external/snesrecomp/` | the static recompiler (Python) and the SNES hardware runtime (C) |
| `runtime/` | this game's frame driver and the glue between the generated code and the runtime |
| `frontend/` | the application: `goof_app` coordinator, SDL2 player, host input/config/settings (`host/`), tests |
| `gates/` | headless deterministic test programs |
| `tests/` | gate runner, frontend test runner, tool unit tests, input scripts |
| `cmake/` | the shared core target definition and a MinGW cross toolchain |

## Build targets

- `goof_core` — generated C + SNES runtime + frame driver, compiled once and
  linked by every other target. It contains no SDL.
- `goof_app_core` / `goof_app_headless` — the application coordinator and a
  headless executable over it.
- `goof_recomp` — the player (the only target that links SDL2). It is copied
  to `dist/` as `GoofTroopRecomp`.
- `goof_boot_e1000`, `goof_headless_frames`, `goof_pbn`, … — gates.

## Original behaviour and host enhancements

The guest — CPU code, PPU, APU, timing — is the original game's and is fully
deterministic: the gates compare CPU state hashes, audio output, rendering and
recorded input campaigns against fixed expected values.

Everything the enhanced frontend adds lives on the host side, outside
`goof_core`, and cannot change what the game computes:

- **Input**: SDL game controllers and keyboard are translated to SNES button
  masks through remappable bindings.
- **Settings**: an overlay drawn by the host; the game is paused while it is
  open. Configuration is a host-side `config.ini`.
- **Presentation**: the game is stepped at the SNES's own cadence
  (60.098477556 Hz) from a host clock, independent of the display refresh
  rate. After a short stall it catches up by at most four steps; after a long
  one it re-anchors. There is no frame interpolation. VSync affects only when
  a finished frame is shown.
- **Video output**: scaling, pixel aspect and the Nearest / Smooth / Scanlines
  filters are applied to the finished 256×224 image.
- **Audio output**: volume and mute are applied to the host output stream.

The gate executables do not link the host input, settings or SDL code, so no
user configuration or device can influence a gate result.

## Tests

| Command | Covers |
|---|---|
| `python3 tests/gates/run_gates.py --build-dir build` | deterministic guest behaviour (Linux) |
| `python3 tests/run_host_tests.py` | controller abstraction, remapping, config, Settings pages, pacing, filters |
| `python3 -m unittest discover -s tests` | ROM validator and configuration generator |

None of them need anything outside this repository except your ROM.

## Line endings

`.gitattributes` fixes the policy:

- text files are LF on every platform (generated configuration is compared
  byte for byte with files in `config/`);
- `*.bat` files are CRLF;
- `external/snesrecomp/**` is never converted — the generator verifies a
  digest of that folder's exact contents (`external/snesrecomp.lock.json`),
  and some upstream files use CRLF.

## The engine lock

`external/snesrecomp.lock.json` records the digest of the vendored engine.
`tools/generate_aot.py` recomputes it before generating and stops on a
mismatch, so generated code always comes from the expected engine contents.
