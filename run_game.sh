#!/bin/sh
# Goof Troop Recomp -- start the game built by ./setup_linux.sh
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
if [ ! -x "$here/dist/run_game.sh" ]; then
  echo "The game has not been built yet. Run:" >&2
  echo "  ./setup_linux.sh /path/to/rom.sfc" >&2
  exit 1
fi
exec "$here/dist/run_game.sh" "$@"
