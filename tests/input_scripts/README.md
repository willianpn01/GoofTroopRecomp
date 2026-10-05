# GOOF_INPUT_V1 scripted-input assets

Each file is a deterministic LEVEL timeline over logical epochs, consumed by
`--input-script` in both `goof_app_headless` and `goof_recomp`, and presented
to the guest through the SAME `goof_app_step` input parameter a live keyboard
feeds.  There is deliberately no test-only path into guest RAM.

Format (see `frontend/goof_input.h` for the normative description):

```
<epoch>  <p1-spec>  [<p2-spec>]        # '#' starts a comment
```

A spec is `-` (neutral), a `+`-joined list of button names, or `0xNNN` in the
engine's own `Snes.input1_currentState` bit order.  An entry states the HELD
state from its epoch onwards until the next entry; epochs must be strictly
increasing.  Before the first entry the sample is neutral.

## The menu prologue

Epochs 450 / 520 / 590 are three Start presses on the title screen.  They are
not magic numbers: E407..E595 is the title/menu plateau promoted by
`GOOF_BOOT_E1000_GATE`, and this sequence is what walks the title screen
through to gameplay, with Player 1 spawning at `$0110 = $40`, `$0113 = $80`
by E961.  Any script that needs to reach gameplay reuses it verbatim.

## Files

| file | subject |
|---|---|
| `neutral.txt`       | INPUT1 -- explicit all-neutral timeline; must reproduce the canonical baseline exactly |
| `input2_menu.txt`   | INPUT2/INPUT9 -- Start press, hold, release on the title screen |
| `input3_move.txt`   | INPUT3 -- four directions and both diagonals in gameplay |
| `input4_action.txt` | INPUT4 -- the B action (`!RAM_GOOFT_Player1_HandsUpFlag`) |
| `campaign.txt`      | INPUT5/6/7/8 -- the whole vertical slice in one timeline |
