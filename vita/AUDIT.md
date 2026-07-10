# Port audit — source-vs-port verification (no guessing)

Produced by a six-way audit that compared `Source Code/*.bb` against
`vita/src` by reading both sides and citing line evidence. CHECKLIST
claims were re-verified, not trusted. Date: 2026-07-10.

## Verdict in one paragraph

The port faithfully covers the core horror loop: map generation, room
rendering with the source lightmap composite and zone fog, doors/keycards/
codes, collision, decals, the solo "monster" SCPs (173, 106+pocket
dimension, 096, 049, 966, 372, 513, 860-1, 1499, 935/205/895/079/012/914),
60 of the 73 Events_Core events, the intro, saves/difficulties, inventory
and item use, radio, zone/chamber/chase music, and the death menu. What is
NOT ported clusters into five systems: (1) the armed-human layer (MTF/
Nine-Tailed Fox, roaming guards, Apache, gunfire damage), (2) the two Gate
endings + warhead + ending screen/credits, (3) presentation systems
(particles, dynamic room lights, skybox, subtitles, achievements, console),
(4) save-game depth (NPC/event/world state resets on load), and (5) audio
depth (heartbeat, player breath, ~19 of 34 music tracks, crossfades,
material footstep variety).

## 1. Player / items / saves

DONE (verified): movement speeds & multipliers, falling damage with vest,
inventory slots per difficulty, difficulty table incl. Esoteric
customization, keycard levels + SCP-005 skeleton, 914 refining (large
table incl. keycard ladder, jackpots, hostile outputs), SCP-714/1499/268
wearables, document reading, blink/sprint meters.

PARTIAL:
- Stamina: missing StaminaMax caps (dark/895/pocket), crouch stamina cost,
  low-stamina breath SFX, gas-mask-tier regen.
- Blink: missing difficulty-scaled BLINKFREQ, EyeIrritation, EyeStuck,
  hold-to-keep-closed, insomnia factor.
- Health: injuries auto-clot (source only heals via HealTimer); first aid
  is instant (source is gradual); no blur/zoom from bloodloss; no SCP-427.
- Gas mask/hazmat/vest/NVG flattened to booleans: no tiers, no fog
  overlay, no NVG battery drain, no helmet.
- 914 inputs missing: SCP-1123, badges, cup, SCP-148 melt, doc cases.
MISSING: item dropping, item combining outside 914, SCRAMBLE gear as a
wearable, hand-scanner (severed hand) doors, physical-key doors,
heartbeat feedback.
PORT-ONLY DEBUG: keycard doors force-open after 3 denied presses
(doors.c ~216) — intentional stopgap, now a progression-trivializer.

Save format gaps (source Save_Core saves all of these; port does not):
all NPC state except 173's position, door locked/timer state, ground-item
positions, event states, room found/lever/grid state, decals, SCP timers
(008/409/966/1048A/268/714/294), player sub-state (BLINKFREQ, EyeStuck,
HealTimer, BlurTimer...), wearHazmat/using714, blackout/RemoteDoorOn/
facility state.

## 2. NPCs

DONE (verified): 173 (movement/blink/doors/teleport-closer/kill), 106
(spawn timer + DisableDecals scaling, chase, phase, grab->pocket drag,
femur recontainment, corrosion), 096 (trigger/chase/door smash/kill), 966,
1499-1 (citizen/guard/king), 513-1, 372, 860-1, tesla, 205/895/079/012
approximations as documented.

PARTIAL: 049 (no 049-2 conversion from kills; fixed 2-zombie retinue),
049-2 (not killable, no event spawns), 939 (single den-locked instance vs
roaming pairs), 035 (host chaser, no tentacle grabs), 205 (dread swell,
no lethal grab).

MISSING: MTF/Nine-Tailed Fox squad entirely (~1500 source lines: patrols,
shooting, SCP recontainment), roaming armed guards, Class-D/Clerk civilian
AI, Apache helicopters, any gunfire damage path, SCP-008-1 zombie AI
(prop only; the 914-bred zombie chaser exists separately), SCP-066 AI
(humming prop only), SCP-1048/1048-A AI (doodle cameo only).

Pathfinding: grid-cell BFS substitutes the source waypoint graph —
adequate for ported creatures, insufficient for MTF-style squads.

## 3. Map generation / rendering

DONE: grid hallway-walk generation, zone bands, checkpoints, forced-room
table (57 entries), commonness-weighted template pick, RMesh parse
(drawn/collision/entities/props), lightmap composite 2*diffuse*(ambient+lm),
fog model with zone colors + context overrides, decal system, collision
(sphere-vs-tri grid), security cameras + live monitor feed + 895/079
broadcast, DDS/PNG/JPG/VTEX textures.

PARTIAL: rooms.ini MaxAmount/Large not parsed; door types on grid doors
limited to default/heavy (internal doors carry all types); elevators are
fast-travel, not moving cars; props ignore per-prop hasCollision (all
props block); step sounds reduced to metal/non-metal; texture cache has
no VRAM eviction limit; access codes derived from seed, not the source
algorithm.

MISSING: particles/emitters entirely (dust, steam, gas, sparks),
dynamic room lights + sprites/coronas (RMesh light entities parsed but
discarded), skybox (gate surface has no sky), per-zone ambient color,
screen gamma, RMesh screen/waypoint entities unused (hardcoded tables
substitute).

IMPORTANT DIVERGENCE: map RNG is a custom LCG, not Blitz Rand() — the
same seed gives a DIFFERENT map than the PC game (string-seed hash also
differs). Seeds are internally consistent but not PC-compatible.

## 4. Events (73 constants audited one by one)

DONE: 60 of 73.
PARTIAL: cont1_106 (femur without radio-lure/break-free branch/corpse),
room2_6_hcz_173 (folded into generic 173-appear), room2_mt (maze +
generator room geometry; puzzle reduced).
MISSING (10): gate_a and gate_b (both ENDINGS: 682 battle, Apaches, HID
turret, MTF, warhead branch, ending screens), room2_nuke (warhead arming
Gate B depends on), cont1_005 (106 trap room), trick_item (blink bait
item), 1048_a (hostile ear-monster spawn), room2_4_hcz_106,
room3_2_ez_duck, room2_test_lcz_173 (enum stub, never wired),
room4_ic (enum stub, never wired).

## 5. Menus / UI / meta

DONE: main menu (new game with name/seed/difficulty incl. Esoteric, load
list with pages/delete, options), easter-egg seeds, pause menu (resume/
save/load/quit), death menu (source layout), HUD meters/overlays/nav,
inventory grid + use + read, document viewer, startup + intro videos,
options persistence (reduced set).

PARTIAL: messages are a single toast (no queue, no hint/tutorial
messages), loading screens show SCP number but not the jsonc lore text,
pause menu lacks Options/Achievements submenus, no quit-confirm.
MISSING: dev console (~60 commands), endings/stats screen/credits,
subtitles (whole Subtitles_Core), achievements (whole Achievements_Core),
custom maps tab, graphics options, control remapping.

## 6. Audio / music / video

DONE: zone tracks + 9 chamber overrides + 106/049/096 chase music, menu
music, radio 4 channels + 895 static override, door/button/lever/keycard/
elevator sounds, per-NPC sound sets (173/106/049/939/895/079/205/513/
035/682/1499), damage sounds, startup/intro videos via sceAvPlayer.

PARTIAL: ~19 of 34 music tracks unreachable (PD, gates, 1499, 008, 409,
1123, tunnels, 860 red, ending/credits...), no crossfade (hard cuts),
ambience is one mono channel (no pan/layering), zone one-shots missing
two sets, 12-voice mixer vs source's many channels, 096 scream cycle.
MISSING: heartbeat, player breath/cough/vomit, ending/credits music,
Master/Voice volume, deafen effect, radio squelch + user-track channel +
hand model, sound subtitles.
