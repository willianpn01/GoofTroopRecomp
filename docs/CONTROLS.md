# Controls

These are the defaults built into the frontend
(`frontend/host/goof_host_input.c`). Everything in the first table can be
changed in **Settings › Input**.

## Game controls

| SNES button | Player 1 keyboard | Player 2 keyboard | Controller |
|---|---|---|---|
| Up / Down / Left / Right | Arrow keys | I / K / J / L | D-pad, or left stick |
| B | Z | N | bottom face button |
| A | X | M | right face button |
| Y | A | B | left face button |
| X | S | H | top face button |
| L | C | U | left shoulder |
| R | V | O | right shoulder |
| Start | Enter (also keypad Enter) | Right Ctrl | Start |
| Select | Right Shift | Right Alt | Back / Select / View |

Keys are physical positions (US layout names), so they stay in the same place
on other keyboard layouts.

Controller face buttons are mapped **by position**, not by the letter printed
on them: the SNES diamond is B bottom, A right, Y left, X top, and each modern
button maps to the SNES button in the same place.

## Host keys

| Key | Action |
|---|---|
| F2 | Open / close Settings. The game is paused while it is open. |
| P | Pause (game and audio stop) |
| Esc | Quit (inside Settings: back) |

F2, P and Esc are never sent to the game and cannot be assigned to a game
button.

## Controllers and players

- The first controller connected drives Player 1, the second Player 2.
- A controller keeps its player until it is unplugged; a newly connected
  controller takes the free player.
- Controllers can be connected or removed while the game is running.
- Keyboard and controller work at the same time for the same player.
- Input is ignored while the window does not have focus.

## Settings menu

Open with **F2**. Navigate with the arrow keys, Enter to confirm, Esc to go
back; on a controller, D-pad to move, bottom button to confirm, right button
to go back.

### Input

Choose Player 1 or Player 2, pick a SNES button, then *Change keyboard* or
*Change controller* and press the new key or button. *Clear* removes a
binding. Conflicting bindings are shown and must be resolved before saving.
*Reset defaults* restores the tables above. *Save* applies and writes the
configuration file.

### Video

| Item | Values |
|---|---|
| Display mode | Windowed, Fullscreen |
| Window scale | Auto, 1x–4x (default 3x) |
| Pixel aspect | Square (default), SNES 8:7 |
| Fullscreen scaling | Integer, Fit |
| VSync | On, Off |
| Filter | Nearest, Smooth (bilinear), Scanlines |

Changes preview immediately; *Save* keeps them, *Cancel changes* restores the
saved values. VSync never changes the game's speed.

### Audio

| Item | Values |
|---|---|
| Master volume | 0–100 % in 5 % steps (default 100 %) |
| Mute | On, Off |
| Mute when unfocused | On, Off |

## Configuration file

Settings are saved to `config.ini`:

- Linux: `$XDG_CONFIG_HOME/GoofTroopRecomp/config.ini`, normally
  `~/.config/GoofTroopRecomp/config.ini`
- Windows: `%APPDATA%\GoofTroopRecomp\config.ini`

Command-line options (pass them after `run_game.sh` / `run_game.bat`):

| Option | Effect |
|---|---|
| `--portable` | use `config.ini` next to the executable |
| `--config PATH` | use this configuration file |
| `--no-config` | built-in defaults; nothing is read or written |
| `--scale N` | window scale for this session only |
| `--vsync on\|off` | VSync for this session only |
| `--filter nearest\|bilinear\|scanlines` | filter for this session only |
| `--no-gamepad` | keyboard only |

Deleting `config.ini` restores all defaults.
