#!/bin/sh
# Goof Troop Recomp -- one-command setup for Linux.
#
#   ./setup_linux.sh /path/to/rom.sfc     validate ROM, generate, build, fill dist/
#   ./setup_linux.sh --clean              remove build/ and dist/
#
# Then start the game with ./run_game.sh
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
if ! command -v python3 >/dev/null 2>&1; then
  echo "SETUP: FAIL -- Python 3 is not installed." >&2
  echo "  It runs the ROM validator and the code generator." >&2
  echo "  Debian / Ubuntu:  sudo apt install python3" >&2
  echo "  Fedora:           sudo dnf install python3" >&2
  echo "  Arch:             sudo pacman -S python" >&2
  echo "  Then run again:   ./setup_linux.sh /path/to/rom.sfc" >&2
  exit 30
fi
exec python3 "$here/tools/goof_setup.py" "$@"
