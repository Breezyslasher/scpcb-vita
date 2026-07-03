# Port status vs. the Blitz source (Source Code/*.bb)

A running comparison of the PS Vita port against the original game.
Updated as features land. States: **done** / **partial** / **missing**.

## World and map

| Feature | Status | Notes |
| --- | --- | --- |
| Map generation (CreateMap) | done | Hallway walk, shape/angle tables, forced iconic rooms, force-more-ROOM1 pass, 50-seed CI check |
| Zones | done | The grid is split into LCZ/HCZ/EZ bands by an exact port of Math_Core's GetZone(y), with the source's transition rows. Each band draws from its own room pool (rooms.ini Zone fields) and gets its forced iconic rooms (SetRoom queues per shape+zone: e.g. 173 in LCZ, nuke/MT/106 in HCZ, the gates/O5/cafeteria in EZ). Checkpoint rooms (room2_checkpoint_lcz_hcz / hcz_ez) are placed on the hallways that cross a band boundary. A room's runtime zone is its grid band (source r\Zone), so per-zone music, Tesla-gate variants and keycard placement all follow the band the room sits in |
| Room loading | done | Incremental (mesh/collision, then textures, then VBOs), 5x5 prefetch ring |
| Room ambience emitters | done | RMESH soundemitter entities loop the nearest in-range ambience per room |
| Grid doors between rooms | done | Placement rules, dedup, keycard checkpoints |
| Room-internal doors (FillRoom) | done | 257 extracted; types, locks, buttonless, keypad codes. The eight door types render with the right leaves: sliding doors mirror their two panels, but the big containment gate (contdoorleft/right) is two distinct halves that meet at the centre and slide apart without mirroring - matching the source, which alone among door types does not flip OBJ2 by 180. Its wider halves (~244 raw vs 203) also get a wider closed collision box so the player can't slip past a shut gate |
| Runtime-expression doors | done | The door extractor captures every fixed-coordinate FillRoom CreateDoor (257 doors). The only calls it skipped were the pocket-dimension labyrinth doors, whose coordinates come from a `For i = 0 To 9 / Select` block (loop variables): the ten KEY_005 doors are now placed explicitly at their source offsets through the dimension_106 room transform, alongside the two fake-tunnel heavy doors the extractor already got |
| Elevators | done | Riding an elevator closes the doors, blacks out for the ride (Moving loop + Close/Open sounds), then arrives at the destination and opens. Real destinations: the gate_a_entrance / gate_b_entrance elevators now travel to the actual gate_a / gate_b surface rooms (appended off-grid, each with a return elevator) and back - so the gate cars go somewhere real. The room2_mt maintenance cars ride down to a **procedurally generated tunnel maze** - a port of the source's tunnel generator: a seeded random walk lays a connected corridor network, each cell becomes a tile by its neighbour count (mt1 dead-end / mt2 corridor / mt2c corner / mt3 tee / mt4 cross) rotated to face its neighbours, at 512-raw spacing, overlaid off-grid like the intro cell with per-tile render + collision (distance-culled). Two straight cells become the elevators; the arrival car rides you back. One dead-end cell becomes the **generator room** (source MT_GENERATOR): it stashes the two source pickups, SCP-500-01 and Night Vision Goggles, on that cell. Any remaining elevator fast-travels to the next elevator in the map. Input is frozen for the ride. Approximated: the generator room's two tank props are skipped (their mesh `tank2.b3d` is absent from the port assets - only the items are placed), and the exact random-walk tuning differs |
| Levers, buttons w/ custom parents | done | 31 levers + 30 standalone buttons render and flip/press with sound. They now drive the one subsystem the port has - doors: a lever throw or an unlocked wall button opens/closes the nearest door in its room (containment chambers, guard/server/storage rooms, etc.), authorising it past any lock. The bespoke rooms keep their own handling and stay out of the generic door control: cont1_106 (the femur breaker, gated on the magnet lever), room2_nuke (Omega Warhead arming), cont1_914 (SCP-914 refinement knob) and gate_a/gate_b (the endings) - those deeper events remain unported, so their fixtures are still cosmetic. Keycard/keypad wall buttons read "won't budge" (their doors open via the door's own keycard button) |
| Decals | done | CreateDecal ported: textured floor/wall quads (corrosive, blood, blood-drop, pd, plus the water, bullet-hole and SCP-427/409 sets) with grow/fade/lifetime and alpha/multiply/additive blends. Dynamic effects (SCP-106's corrosion pool + footfall trail, the player's blood droplets) run in a transient ring buffer. FillRoom's static scatter is now placed too: a new extractor (extract_decals.py -> room_decals.h) pulls the 19 literal-coordinate splats and lays them into their rooms (rotated with the room) in a separate persistent array so the transient effects can't recycle them. When SCP-106 rots a door it now also pools corrosion at the door's foot and eats a splat into the panel face, not just the texture swap. Skipped: the loop/Rand-scattered decals (runtime coordinates) and the achievement-gated Keter/Apollyon symbols (never present without those unlocks) |
| Security cameras / particles / sprites | partial | Security cameras done: a new extractor (extract_cameras.py -> room_cameras.h) captures all 34 FillRoom CreateSecurityCam placements with their per-cam tilt / yaw / sweep amplitude / follow flag. Each renders a base + head prop (CamBase/CamHead b3d) whose head sweeps within +-Turn, tracks the player where the source sets FollowPlayer, or sits still, with the red light blinking on the source's ~1350 ms cycle. The MTF camera-detection subplot is ported too (UpdateCameraCheck): every few minutes a camera sweep fires - the "camera check" announcement, a window in which any camera whose ~60 deg cone sees the player (line-of-sight tested) flags it, then a "found"/"not found" announcement; a catch is surfaced as a scare (toast + horror sting) since there is no MTF squad to dispatch. Not ported: the live monitor render-to-texture feed and the SCP-079 broadcast (needs an FBO camera feed the Vita build avoids, and SCP-079 itself is absent). Particles (SetEmitter steam/spark/water) and the standalone sprites (screens, glows) are still to do |
| Lightmap blend model | done | The baked room lightmap (slot 0, `*_lm.png`) now modulates the diffuse instead of adding to it. Source composites 2 * diffuse * (ambient + lightmap) - the diffuse layer is Blitz TextureBlend 5 (multiply x2) - so the GL layer draws the lightmap second pass as a modulate2x (glBlendFunc GL_DST_COLOR, GL_SRC_COLOR = 2 * dst * src), darkening shadowed areas rather than washing them out the way the old additive (GL_ONE, GL_ONE) pass did. The per-room AmbientLightRoomTex term (a dim uniform add, a 2x2 render target the port has no equivalent for) is dropped, leaving 2 * diffuse * lightmap |
| Glass / alpha surfaces | done | Alpha-blended like LoadRMesh's Alpha mesh |
| Fog / HideDistance | partial | Fixed black fog; source varies HideDistance per area/event |

## Player

| Feature | Status | Notes |
| --- | --- | --- |
| Movement, sprint, crouch, stamina, blink | done | Source metrics |
| Lean | missing | D-pad left/right taken by inventory/menus |
| Health (injuries/bloodloss/sanity) | done | Damage adds injuries that bleed into bloodloss (fatal at 100, warning vignette + HUD bar); first aid clots it; sanity drains in the dark / pocket dimension / near 106 and distorts the view when low. No 008 zombie infection / vomit yet |
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
| SCP-106 | mostly done | Roaming hunter with facility BFS pathfinding + phase-behind; femur-breaker recontainment gated on the magnet lever; corrosion pool on spawn and a footfall trail; corrodes the sliding/heavy doors it passes (texture swap); a trailing faded second body wells up behind it at distance; the lens zooms inward and the view blurs (real framebuffer-copy post-process) when it holds your gaze - and at the throne / pocket-dimension exit; grab-and-wrench attack on contact (frozen input, view twist) before the pocket-dimension drag; spawn timer counts down twice as fast on aggressive difficulties and is slowed/halted by the room's DisableDecals (0 full, 1 half, 2 quarter, 3 never); a Tesla gate zap now repels it (its long-missing State 4 retreat), sending it dormant. Remaining: only the Micro-HID retreat trigger (no HID weapon in the port) |
| SCP-682 | done | The e_682_roar set-piece in room2_5_hcz/room3_hcz/room2_5_ez, ported faithfully as the source intends it: a pure audio + camera scare - 682 is never seen and deals NO damage. A distant tremor builds while the player is in the room, it roars partway through, the shake peaks, then it fades. (An earlier build fabricated an arm bursting in and hurting the player; that arm actually belongs to the Gate B ending, so it is now dormant - reserved for that ending - and the roar does no damage, matching the source.) |
| SCP-079 | done | The sentient computer in cont1_079's lower chamber (e_cont1_079). To reach it the room's elevator now descends vertically: an elevator door with a same-X/Z partner at a very different Y rides between the two levels (the arrival floor-ray drops from the destination level), so the chamber below is reachable through the KEY_CARD_4 blast door. SCP-079's console (scp_079.b3d) renders with a glowing screen that flickers its seven overlay frames; reaching the terminal wakes it - it plays its speech, the screen flickers, and it drops the "Document SCP-079" (the item template, and the other event-only documents, are now emitted by extract_items.py); approaching again after it finishes makes it refuse. Not ported: the Gate B broadcast tie-in (no gate endings) and the SCP-895 camera-feed broadcast (needs the unported live monitor feed) |
| SCP-012 | done | "A Bad Composition" in cont2_012 (Main_Core scribe_event): standing at the score pins your eyes open and floods the screen with the bloody compulsion overlay (scp_012_overlay.png), jittering it each frame like the source, with a horror sting; sanity bleeds while it holds you, and the overlay fades once you step away. The ventilation-fan / red-light dressing is not reproduced |
| SCP-895 | done | The camera coffin in cont1_895 (e_cont1_895): the chamber fills with dread scaled by how close you get - sanity bleeds, the view blurs, and the trapped guard murmurs (GuardIdle) - and a close approach makes the slumped guard corpse (guard.b3d) lurch into view with a scream (GuardScream) + horror sting + camera shake; backing off re-arms it. Not ported: the source's SCP-106 lure toward the coffin and the SCP-895 camera-feed amplification (CoffinEffect), which need the unported live monitor feed |
| SCP-096 | done | Skinned model (scp_096.b3d) spawns sitting in an HCZ room (e_096_spawn list, fallback to any room3/room4). Harmless until you look at its face (in the view cone, in range, not mid-blink); then it gets up, screams in place (~8 s, compressed from the source's ~26 s), then sprints down the player via cell pathfinding and kills on contact (blood decals, heavy shake, "SCP-096" death). Triggered/Scream sounds and the dread-zoom pulse included. Not ported: the SCRAMBLE-goggles counter, the MTF "spotted" reaction, and the exact 26 s scream |
| SCP-049 + 049-2 | done | The Plague Doctor (scp_049.b3d) spawns in cont2_049 (or a far room) and idles, then activates and walks the player down via cell pathfinding, phasing closer if they break away, killing on a touch ("SCP-049"); spotted/searching/breath voice lines. A retinue of two reanimated 049-2 shamblers (scp_049_2.b3d, shared walk phase) hunt alongside once 049 is active and also kill on contact. The Hazmat suit and SCP-714 (jade ring) now buy a few seconds when 049 grips you - it tears them off (with a warning + view/shake cues) before the cure, and fleeing lets the timer recover; once destroyed the next grip is fatal. Not ported: dynamic conversion of killed NPCs into new 049-2, and the surveillance-room subplot |
| SCP-939 | done | A blind pack predator prowling room3_storage that hunts by the noise the player makes (playerNoise: loud sprinting draws it from across the room, a still crouch slips past). Patrol -> alert (stalks the sound) -> attack (charges and bites, each bite injuring; enough bites, injuries > 4, kill "SCP-939"); attack/horror voice lines. Not ported: the human-voice "lure" mimicry and the waypoint patrol path |
| SCP-966 | done | The sleep-stalker: spawns in an HCZ room and is invisible to the naked eye - only the night-vision goggles render it. Unwatched, it creeps toward the player and saps their stamina and sanity (a drowsiness blur, "your eyelids are so heavy" toasts); watched through the goggles it hangs back. Caught while exhausted, it kills "SCP-966". Not ported: the per-instance "insomnia" item state and the many flavor whispers |
| SCP-1499-1 | done | Donning the SCP-1499 mask (use the item) exiles the player to dimension_1499. The congregation is laid out at the source's church coordinates - the king enthroned on the altar (his own seated animation + crowned texture), a guard beside him, two guards at the entrance, and citizens milling through the nave (drifting near their spots, murmuring). It starts peaceful; straying too near any member rouses the whole church, which then converges - guards quicker than citizens, the king watching from the altar - and kills on contact ("THE PEOPLE OF SCP-1499") while sanity bleeds. Taking the mask off returns you; facility SCPs are suspended meanwhile. Approximated vs source: the citizens' paired "conversation" wandering and per-member scream states are simplified to a single congregation-wide rouse |
| SCP-860-1 | done | The forest stalker in cont2_860_1 (a force-placed room): a seed-scattered copse of trees, with SCP-860-1 (scp_860_2.b3d) that advances only while unwatched and freezes the instant it is looked at (or you blink) - lurching much closer when your back is turned - and kills on contact in the dark. Approximated vs source: the full procedural forest-cell grid + path is a scattered copse, and the peek-from-behind-a-specific-tree placement is a general move-when-unseen stalk |
| Guards/MTF in gameplay | missing | Only intro figures |
| NPC waypoint pathfinding | done | 173 BFS-routes through room cells/doorways toward the player and last-seen spot |

## Events / story

| Feature | Status | Notes |
| --- | --- | --- |
| Intro sequence | done | Wake screen, bunk cinematic, PA/dialog beats, escort, chamber sequence, breach; simplified NPC theatrics |
| Per-room events (Events_Core, ~100 events) | partial | Framework + 3 self-contained events (173 ambush appearance, the "trick" scare, distant 682 roar); the rest need the unported SCPs/systems |
| Pocket dimension (dimension_106) | partial | Dragged in on 106 catch. Full 8-state flow ported (UpdateDimension106): the player is shuffled between the start room (106 orbits then lunges), four-way room (flying pillars crush), throne room (camera dragged to the throne of eyes, sanity draining, crouch to kneel), trenches, exit room (sinkhole → escape to the facility), fake HCZ tunnel, tower and labyrinth (106 hunts) via the source's random-teleport table, on the multi-mesh assembly (tunnels + dim_3/dim_4). Trenches/tower/labyrinth are structurally faithful but approximated - their bespoke props (the trench plane, the rockmoss terrain mesh, particle shadows, forced-camera kneel) need meshes/subsystems the port lacks, so those states use timed injury/106-threat gauntlets with the same exit conditions |
| Endings (Gate A / Gate B) | missing | No gates; facility has no exit condition |
| MTF sweep / announcements over time | missing | |
| Tesla gates | done | The room2_tesla_lcz/hcz/ez corridors get an electrified gate at their centre (e_tesla): idle → charge (wind-up) → zap → recharge, with the source's sounds. Caught in the inner box during the zap it kills the player ("A TESLA GATE" + white flash + shake); the arc glows additively across the corridor. The corridor's wall security camera renders now (see the cameras row); not ported: the MTF-clerk repair subplot |

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
