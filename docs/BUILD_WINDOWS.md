# Building on Windows

## The short way

1. Install [MSYS2](https://www.msys2.org) in its default folder, `C:\msys64`.
2. In the repository folder, from Command Prompt or PowerShell:

   ```bat
   setup_windows.bat --install-deps
   setup_windows.bat "C:\path\to\Goof Troop (USA).sfc"
   dist\run_game.bat
   ```

That is the whole procedure. You never have to open an MSYS2 window, edit
`PATH` or look for the executable.

- `--install-deps` is needed once. It lists the packages, asks for
  confirmation, and installs them inside the MSYS2 folder only.
- Instead of typing the path you can **drag the ROM file onto
  `setup_windows.bat`** in Explorer.
- Paths with spaces work; put them in quotes when typing.
- When it finishes, setup prints `Build complete.` and the command to run.
- `run_game.bat` in the repository folder and `dist\run_game.bat` can both be
  double-clicked.

## What setup_windows.bat does

1. Finds MSYS2 (`MSYS2_ROOT` if set, otherwise `C:\msys64`).
2. Checks that the UCRT64 tools exist: GCC, CMake, Ninja, Python, SDL2. If one
   is missing it says which and prints the command that installs it.
3. Runs the build with those tools. `PATH` is changed only inside the script's
   own process; your system settings are not touched.
4. Validates the ROM, generates the C code from it, compiles, and fills
   `dist\`:

   ```
   dist\GoofTroopRecomp.exe
   dist\SDL2.dll            (and any other non-system DLL the game needs)
   dist\canonical.sfc       verified copy of your ROM
   dist\run_game.bat
   ```

`dist\` runs on its own, without MSYS2 on `PATH`. Do not share it: it contains
a copy of your ROM and code derived from it.

Settings are stored in `%APPDATA%\GoofTroopRecomp\config.ini`. Start the game
with `--portable` to keep `config.ini` next to the executable instead.

## Prerequisites installed by `--install-deps`

```
mingw-w64-ucrt-x86_64-gcc
mingw-w64-ucrt-x86_64-cmake
mingw-w64-ucrt-x86_64-ninja
mingw-w64-ucrt-x86_64-SDL2
mingw-w64-ucrt-x86_64-python
```

No separate Python installation is required.

## Clean

```bat
clean_windows.bat
```

(or `setup_windows.bat --clean`). Removes only the `build\` and `dist\`
folders next to the script.

## Troubleshooting

- **`MSYS2 was not found`** — install it from https://www.msys2.org. If it is
  not in `C:\msys64`, set `MSYS2_ROOT` for the current window and run again:

  ```bat
  set MSYS2_ROOT=D:\tools\msys64
  setup_windows.bat --install-deps
  ```

- **`Missing: ...`** — run `setup_windows.bat --install-deps`.
- **The package installation fails** — a fresh MSYS2 sometimes needs its first
  update: open "MSYS2 MSYS" from the Start menu once, run `pacman -Syu`, close
  it, and run `setup_windows.bat --install-deps` again.
- **`SUPPORTED ROM: FAIL`** — the file is not the supported revision; the
  message shows the size and SHA-256 that were found.
- **`snesrecomp content does not match external/snesrecomp.lock.json`** — a
  tool changed files under `external\snesrecomp` (typically line endings).
  Clone with git as-is (the repository's `.gitattributes` prevents conversion)
  or download the ZIP again.
- **The window stays open with "Press any key"** — that is intentional so the
  messages can be read after a double-click. Set `GOOF_NO_PAUSE=1` to disable
  it (for scripts).
- **Antivirus slows or blocks the build** — the build compiles a new
  executable; allow the repository folder if needed.

## Advanced: manual build

For development you can work inside the "MSYS2 UCRT64" shell with the same
packages installed and use the same commands as on Linux:

```sh
python tools/goof_build.py --rom /c/path/to/rom.sfc
build/cmake/frontend/GoofTroopRecomp.exe build/generated/rom/canonical.sfc
```

`tools/goof_setup.py` (what the `.bat` runs) works there too. On Windows three
Linux-only test programs are not built, so `tests/gates/run_gates.py` is a
Linux tool; `tests/run_host_tests.py` builds and runs the frontend tests.

A Linux-to-Windows cross toolchain file is provided in
`cmake/toolchains/mingw-w64-x86_64.cmake` (set `SDL2_DIR` to a MinGW SDL2
development package).
