# SCP:CB Ultimate Edition Reborn — PS Vita port

This directory contains the native PS Vita port project. It builds with
[VitaSDK](https://vitasdk.org/) into an installable `.vpk` package.

**Current state: Milestone 1 — port shell.** The VPK boots on a homebrew-enabled
Vita (or PS TV), initializes the renderer, and runs a live controller test
covering every game action, both analog sticks, and the front touch panel.
Game systems are ported on top of this shell — the plan is in
[PORTING.md](PORTING.md).

## Building from CI

Go to the repository's **Actions** tab → **Build PS Vita VPK** → **Run
workflow**. A manually triggered run produces two artifacts (pushes touching
`vita/` rebuild only the first):

- `scpcb-ue-vita-vpk` — the installable `scpcb_ue_vita.vpk`.
- `scpcb-ue-vita-data` — the game data, with world textures downscaled to
  the Vita memory budget.

On a device running HENkaku/h-encore: install the VPK with VitaShell, then
copy the data package contents to `ux0:data/scpcb-ue/` (so that e.g.
`ux0:data/scpcb-ue/GFX/Map/...` exists).

## Building locally

Requires a [VitaSDK toolchain](https://vitasdk.org/) with `$VITASDK` set:

```sh
cmake -S vita -B vita/build
cmake --build vita/build
# output: vita/build/scpcb_ue_vita.vpk
```

## Controller layout

| Input        | Action                    |
| ------------ | ------------------------- |
| Left stick   | Move                      |
| Right stick  | Camera                    |
| Cross        | Interact (left mouse)     |
| Square       | Use equipped item         |
| Circle       | Crouch                    |
| Triangle     | Inventory                 |
| L trigger    | Sprint                    |
| R trigger    | Blink                     |
| D-pad left   | Lean left                 |
| D-pad right  | Lean right                |
| D-pad up     | Quick save                |
| Select       | Console                   |
| Start        | Pause menu                |
| Front touch  | Menu / inventory cursor   |

The mapping lives in `src/input.c` and mirrors the bindable action set of the
original game (`Source Code/KeyBinds_Core.bb`).
