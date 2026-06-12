# SCP:CB Ultimate Edition Reborn — PS Vita port

This directory contains the native PS Vita port project. It builds with
[VitaSDK](https://vitasdk.org/) into an installable `.vpk` package.

**Current state: Milestone 3 — room viewer.** The VPK boots on a
homebrew-enabled Vita (or PS TV) and renders the game's actual rooms: it
loads `.rmesh` files from the data package, uploads their textures (capped
to the memory budget), and draws geometry with lightmaps. Fly through with
the sticks; cycle rooms with the D-pad. The roadmap is in
[PORTING.md](PORTING.md).

Note: like most vitaGL homebrew, the app requires `libshacccg.suprx` (the
runtime shader compiler) to be installed on the device — see
https://vita.hacks.guide/ for the standard extraction steps.

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
