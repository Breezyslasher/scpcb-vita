# Port status vs. the Blitz source (Source Code/*.bb)

A running comparison of the PS Vita port against the original game.
Updated as features land. States: **done** / **partial** / **missing**.

## World and map

| Feature | Status | Notes |
| --- | --- | --- |
| Map generation (CreateMap) | done | Hallway walk, shape/angle tables, forced iconic rooms, force-more-ROOM1 pass, 50-seed CI check |
| Zones | partial | Everything generates as one LCZ-style zone; the source's LCZ/HCZ/EZ zone bands with checkpoint transitions and per-zone room pools are not split |
| Room loading | done | Incremental (mesh/collision, then textures, then VBOs), 5x5 prefetch ring |
| Room ambience emitters | done | RMESH soundemitter entities loop the nearest in-range ambience per room |
| Grid doors between rooms | done | Placement rules, dedup, keycard checkpoints |
| Room-internal doors (FillRoom) | done | 257 extracted; types, locks, buttonless, keypad codes |
| Runtime-expression doors | missing | ~30 CreateDoor calls with computed coordinates skipped by the extractor |
| Elevators | missing | Elevator doors render but do not travel |
| Levers, buttons w/ custom parents | partial | 31 levers + 30 standalone buttons render, flip/press with sound; the events they drive are not ported so they are cosmetic-interactive |
| Security cameras / decals / particles / sprites | missing | Cosmetic layers of FillRoom |
| Lightmap blend model | partial | Source multiplies (TextureBlend 5/2) with AmbientLightRoomTex; port adds the lightmap additively |
| Glass / alpha surfaces | done | Alpha-blended like LoadRMesh's Alpha mesh |
| Fog / HideDistance | partial | Fixed black fog; source varies HideDistance per area/event |

## Player

| Feature | Status | Notes |
| --- | --- | --- |
| Movement, sprint, crouch, stamina, blink | done | Source metrics |
| Lean | missing | D-pad left/right taken by inventory/menus |
| Health | partial | Fall damage + guard gunfire only; no bleeding, infections, injuries-per-source (008, 049, 106 etc.) |
| Save types per difficulty | partial | Save-anywhere / save-on-quit / no-saves honored; Euclid's save-on-screens saves anywhere (no monitor interaction) |
| Console | missing | Select is reserved but no console |
| Achievements / end screen | missing | |

## Menus / UI

| Feature | Status | Notes |
| --- | --- | --- |
| Main menu (art, tabs, NEW/LOAD/OPTIONS) | done | Incl. 173 flicker, creepy strings, touch |
| New-game panel (name/seed/intro/difficulties) | done | Esoteric customization included |
| Options | partial | Music/SFX volume, sensitivity, invert Y; source's graphics/controls/advanced tabs n/a or missing |
| Load-game pages, delete confirm | done | Named saves |
| Pause menu | done | Difficulty-gated saving |
| HUD meters, inventory grid, documents | done | |
| Keypad UI for code doors | done | Codes generated like InitNewGame; fixed codes (Harp 7816, Dr. L 2411, 035 5731) match the source |
| In-world code monitors / screens | missing | The maintenance-tunnel code is only discoverable via the source's monitor texture, which the port does not render; documents with printed codes are readable |
| Hint messages (CreateHintMsg) | partial | Toast approximations |
| Subtitles | missing | |

## Items

| Feature | Status | Notes |
| --- | --- | --- |
| Templates + per-room literal spawns | done | 102 templates, 166 spawns |
| Runtime-expression spawns | missing | Loops/entity-relative CreateItem calls skipped |
| Keycards, SCP-005 | done | Levels gate doors; press-x3 force-open debug fallback still active |
| Documents readable | done | Paper/note/badge docImages |
| Eyedrops, first aid, 500-01, 420-J, pizza, cup | done | |
| Gas mask, vest, NVG (both), SCP-268 | done | Simplified effects |
| S-NAV (minimap, Ultimate 173 blip) | done | Explored-room map; source draws room shapes |
| Radio | partial | 4 looping stations; source has channel-specific event chatter |
| SCP-914 refining | missing | No 914 machinery |
| Hand items in view (equipped models) | missing | Source renders the held item |
| Wearable visuals (mask overlay etc.) | missing | Effects only |

## NPCs

| Feature | Status | Notes |
| --- | --- | --- |
| SCP-173 | done | Freeze-on-sight, blink interplay, last-seen search, TeleportCloser, door opening, kill + camera wrench, horror stings, head tracking |
| Skeletal animation engine | done | B3D BONE/KEYS/ANIM, CPU skin, VBO path |
| Intro guards / Class-Ds | done | Source roster, idles, walking escort, gunfire enforcement |
| SCP-106 | missing | Pocket dimension too |
| SCP-096, 049, 049-2, 939, 966, 860-1, 1499-1 | missing | |
| Guards/MTF in gameplay | missing | Only intro figures |
| NPC waypoint pathfinding | done | 173 BFS-routes through room cells/doorways toward the player and last-seen spot |

## Events / story

| Feature | Status | Notes |
| --- | --- | --- |
| Intro sequence | done | Wake screen, bunk cinematic, PA/dialog beats, escort, chamber sequence, breach; simplified NPC theatrics |
| Per-room events (Events_Core, ~100 events) | missing | 173 chamber post-intro, 914 tests, 106 victim, tesla gates, room ambushes, etc. |
| Endings (Gate A / Gate B) | missing | No gates; facility has no exit condition |
| MTF sweep / announcements over time | missing | |
| Tesla gates | missing | |

## Audio

| Feature | Status | Notes |
| --- | --- | --- |
| Mixer (3D pan/attenuate, loops) | done | 48 kHz out, 64-bit positions |
| Streamed music | done | Menu + LCZ track, radio stations |
| Per-zone music switching | done | Track follows the room zone (LCZ/HCZ/EZ) |
| Footstep surface variants | done | Metal step/run set on grating/panel floors via down-ray texture test |
| Door/interact/horror/intro voice sets | done | |
| Ambient room emitters | missing | |

## Known visual gaps

- Green-tinted windows reported on device; repo data verified neutral
  (suspect stale texture on the memory card).
- No dynamic lights, flicker, or shadows (source flickers room lights).
- No startup splash videos (WMV unsupported on Vita).
