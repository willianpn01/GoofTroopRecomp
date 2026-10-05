# GoofTroopRecomp

An unofficial static recompilation of **Goof Troop** for SNES, with an
enhanced native frontend for Linux and Windows. You provide a supported ROM;
this project does not distribute the game ROM. The build validates your ROM
and generates the game code locally on your computer.

This repository is currently undergoing final pre-publication review. It is
private while the redistribution status of the historical snesrecomp snapshot
used here is being confirmed.

## Features

- Native Linux and Windows application; no emulator setup is required.
- Modern controller support, including hot-plugging and two players.
- Keyboard and controller remapping for both players.
- Persistent settings and an in-game Settings UI (**F2**).
- Fullscreen and windowed modes, with scaling options from 1x–4x, automatic,
  integer and fit-to-screen.
- Square and SNES 8:7 pixel aspect options.
- Master volume, mute and mute when the window is unfocused.
- VSync option and refresh-independent presentation: gameplay runs at the
  SNES rate (about 60.1 Hz) on displays with different refresh rates.
- Nearest, Bilinear/Smooth and Scanlines image filters.

## Requirements

- A supported **Goof Troop (USA)** ROM; see [Supported ROM](#supported-rom).
- **Windows:** MSYS2 installed in its default location (`C:\msys64`). The
  setup script installs the required compiler and build tools.
- **Linux:** Python 3, GCC or another compatible C compiler, CMake, Ninja (or
  Make), pkg-config and SDL2 development files. Debian/Ubuntu install details
  are in [docs/BUILD_LINUX.md](docs/BUILD_LINUX.md).

## Windows Quick Start

1. Install [MSYS2](https://www.msys2.org) once, using its default folder
   (`C:\msys64`).
2. Clone or download this repository.
3. Open Command Prompt or PowerShell in the repository folder and run once:

   ```bat
   setup_windows.bat --install-deps
   ```

4. Build with your ROM:

   ```bat
   setup_windows.bat "C:\path\to\GOOFT_USA.sfc"
   ```

   You can also drag and drop the ROM file onto `setup_windows.bat` in
   Explorer.
5. After setup reports a successful build, start the game:

   ```bat
   dist\run_game.bat
   ```

`setup_windows.bat` detects and configures the MSYS2 UCRT64 environment by
itself. For normal use, you do not need to edit `PATH`, open a UCRT64 shell,
or search for an executable in the CMake build folders. The finished game is
placed in `dist\`.

If a dependency is missing, the script identifies the missing tool or package
and prints the MSYS2 command to install it. Run the printed install command
from the script as directed, then run setup again. Advanced instructions are
in [docs/BUILD_WINDOWS.md](docs/BUILD_WINDOWS.md).

## Linux Quick Start

Install the listed prerequisites for your distribution in
[docs/BUILD_LINUX.md](docs/BUILD_LINUX.md), then run from the repository root:

```sh
./setup_linux.sh "/path/to/GOOFT_USA.sfc"
./run_game.sh
```

The setup script validates the ROM, generates the required code and builds
the game automatically. The finished executable is placed in `dist/`. The
generated ROM copy and ROM-derived code stay local and are ignored by Git.
Advanced build instructions are in [docs/BUILD_LINUX.md](docs/BUILD_LINUX.md).

## Supported ROM

The supported payload is **Goof Troop (USA)** with these exact identifiers:

| Property | Value |
|---|---|
| Payload size | 524288 bytes |
| SHA-256 | `2bb368c47189ce813ad716eef16c01cd47685cb98e2c1cb35fa6f0173c97dd7c` |
| MD5 | `bb6a1198e291c8ae58e9581a4296ed4d` |

An optional exact 512-byte copier header is accepted and removed during local
normalization. Other revisions, regions and modified files are rejected. No
ROM download is provided by this project.

## Controls / Settings

| Action | Player 1 keyboard | Player 2 keyboard | Controller |
|---|---|---|---|
| D-pad | Arrow keys | I / K / J / L | D-pad or left stick |
| B / A | Z / X | N / M | Bottom / right face button |
| Y / X | A / S | B / H | Left / top face button |
| L / R | C / V | U / O | Shoulder buttons |
| Start | Enter | Right Ctrl | Start |
| Select | Right Shift | Right Alt | Back / Select / View |

Press **F2** to open Input, Video and Audio settings. Controls can be remapped
there; **P** pauses and **Esc** quits. Settings persist between sessions.
Details are in [docs/CONTROLS.md](docs/CONTROLS.md).

## Building manually

The setup scripts are recommended for normal builds. Developers can follow
the advanced platform guides: [Windows](docs/BUILD_WINDOWS.md) and
[Linux](docs/BUILD_LINUX.md). The build validates the supplied ROM before
generating code or compiling.

## Project structure

- `frontend/` — native application, input, settings and presentation.
- `runtime/` — game frame integration and host hooks.
- `tools/` — ROM validation, code generation and build setup.
- `external/snesrecomp/` — pinned static recompiler and SNES runtime snapshot.
- `docs/` — architecture, controls and platform build guides.

Generated C, build outputs and the normalized ROM are created locally under
ignored `build/` and `dist/` directories.

## License

Original GoofTroopRecomp project code is provided under the **PolyForm
Noncommercial License 1.0.0**; see [LICENSE](LICENSE). This license applies
only to original project code and does not change the terms for third-party
components.

## Credits

GoofTroopRecomp uses [snesrecomp](https://github.com/mstan/snesrecomp) as a
core part of its static recompilation toolchain. The project would not have
been possible in its current form without that work. The included snapshot
comes from an older revision whose README said its license had not yet been
declared; the redistribution status of this exact historical snapshot is
being confirmed before public release. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for details.

Historical research also consulted existing Goof Troop disassembly and
reference work. That material is not included and is not required to build
this project. SDL2 provides window, audio and controller support; third-party
components retain their own license terms.

## Third-party notices

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for attribution and
licensing information for components included in or used by this project.

## Current status / limitations

This project targets one verified ROM revision and provides windowed and
fullscreen presentation with the scaling and filters listed above. Widescreen
is not implemented. The repository is undergoing private review while the
historical snesrecomp snapshot's redistribution status is confirmed.
