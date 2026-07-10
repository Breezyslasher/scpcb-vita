# Port status vs. the Blitz source (Source Code/*.bb)

A running comparison of the PS Vita port against the original game.
Updated as features land. States: **done** / **partial** / **missing**.

A full line-evidence source-vs-port audit (2026-07-10) lives in
`vita/AUDIT.md`; the statuses below were reconciled against it.

## World and map

| Feature | Status | Notes |
| --- | --- | --- |
| Map generation (CreateMap) | done | Hallway walk, shape/angle tables, forced iconic rooms, force-more-ROOM1 pass, 50-seed CI check. Audit note: the RNG is a custom LCG, not Blitz Rand() - a given seed produces a DIFFERENT map than the PC game (string-seed hash differs too); seeds are self-consistent only. rooms.ini MaxAmount/Large fields are not parsed |
| Zones | done | The grid is split into LCZ/HCZ/EZ bands by an exact port of Math_Core's GetZone(y), with the source's transition rows. Each band draws from its own room pool (rooms.ini Zone fields) and gets its forced iconic rooms (SetRoom queues per shape+zone: e.g. 173 in LCZ, nuke/MT/106 in HCZ, the gates/O5/cafeteria in EZ). Checkpoint rooms (room2_checkpoint_lcz_hcz / hcz_ez) are placed on the hallways that cross a band boundary. A room's runtime zone is its grid band (source r\Zone), so per-zone music, Tesla-gate variants and keycard placement all follow the band the room sits in |
| Room loading | done | Incremental (mesh/collision, then textures, then VBOs), 5x5 prefetch ring |
| Room ambience emitters | done | RMESH soundemitter entities loop the nearest in-range ambience per room |
| Grid doors between rooms | done | Placement rules, dedup, keycard checkpoints |
| Room-internal doors (FillRoom) | done | 257 extracted; types, locks, buttonless, keypad codes. The extractor now also reads FillRoom's follow-up lines for door dressing: doors whose second panel the source frees (FreeEntity d\\OBJ2) render single-leaf (the 173 hallway cell/service doors), the intro cell 3-11 door gets the source's Door02.jpg retexture on a single leaf, and the statically corroded doors (Dr. L's office, cont1_035's one-sided door) render with Door01_Corrosive like the source. The eight door types render with the right leaves: sliding doors mirror their two panels, but the big containment gate (contdoorleft/right) is two distinct halves that meet at the centre and slide apart without mirroring - matching the source, which alone among door types does not flip OBJ2 by 180. Its wider halves (~244 raw vs 203) also get a wider closed collision box so the player can't slip past a shut gate |
| Runtime-expression doors | done | The door extractor captures every fixed-coordinate FillRoom CreateDoor (257 doors). The only calls it skipped were the pocket-dimension labyrinth doors, whose coordinates come from a `For i = 0 To 9 / Select` block (loop variables): the ten KEY_005 doors are now placed explicitly at their source offsets through the dimension_106 room transform, alongside the two fake-tunnel heavy doors the extractor already got |
| Elevators | done | Riding an elevator closes the doors, blacks out for the ride (Moving loop + Close/Open sounds), then arrives at the destination and opens. Real destinations: the gate_a_entrance / gate_b_entrance elevators now travel to the actual gate_a / gate_b surface rooms (appended off-grid, each with a return elevator) and back - so the gate cars go somewhere real. The room2_mt maintenance cars ride down to a **procedurally generated tunnel maze** - a port of the source's tunnel generator: a seeded random walk lays a connected corridor network, each cell becomes a tile by its neighbour count (mt1 dead-end / mt2 corridor / mt2c corner / mt3 tee / mt4 cross) rotated to face its neighbours, at 512-raw spacing, overlaid off-grid like the intro cell with per-tile render + collision (distance-culled). Two straight cells become the elevators; the arrival car rides you back. One dead-end cell becomes the **generator room** (source MT_GENERATOR): it stashes the two source pickups, SCP-500-01 and Night Vision Goggles, on that cell. Any remaining elevator fast-travels to the next elevator in the map. Input is frozen for the ride. Approximated: the generator room's two tank props are skipped (their mesh `tank2.b3d` is absent from the port assets - only the items are placed), and the exact random-walk tuning differs |
| Levers, buttons w/ custom parents | done | 31 levers + 30 standalone buttons render and flip/press with sound. They now drive the one subsystem the port has - doors: a lever throw or an unlocked wall button opens/closes the nearest door in its room (containment chambers, guard/server/storage rooms, etc.), authorising it past any lock. The bespoke rooms keep their own handling and stay out of the generic door control: cont1_106 (the femur breaker, gated on the magnet lever), room2_nuke (Omega Warhead arming), cont1_914 (SCP-914 refinement knob) and gate_a/gate_b (the endings) - those deeper events remain unported, so their fixtures are still cosmetic. Keycard/keypad wall buttons read "won't budge" (their doors open via the door's own keycard button) |
| Decals | done | CreateDecal ported: textured floor/wall quads (corrosive, blood, blood-drop, pd, plus the water, bullet-hole and SCP-427/409 sets) with grow/fade/lifetime and alpha/multiply/additive blends. Dynamic effects (SCP-106's corrosion pool + footfall trail, the player's blood droplets) run in a transient ring buffer. FillRoom's static scatter is now placed too: a new extractor (extract_decals.py -> room_decals.h) pulls the 19 literal-coordinate splats and lays them into their rooms (rotated with the room) in a separate persistent array so the transient effects can't recycle them. When SCP-106 rots a door it now also pools corrosion at the door's foot and eats a splat into the panel face, not just the texture swap. Skipped: the loop/Rand-scattered decals (runtime coordinates) and the achievement-gated Keter/Apollyon symbols (never present without those unlocks) |
| Security cameras / particles / sprites | partial | Security cameras done: a new extractor (extract_cameras.py -> room_cameras.h) captures all 34 FillRoom CreateSecurityCam placements with their per-cam tilt / yaw / sweep amplitude / follow flag. Each renders a base + head prop (CamBase/CamHead b3d) whose head sweeps within +-Turn, tracks the player where the source sets FollowPlayer, or sits still, with the red light blinking on the source's ~1350 ms cycle. The MTF camera-detection subplot is ported too (UpdateCameraCheck): every few minutes a camera sweep fires - the "camera check" announcement, a window in which any camera whose ~60 deg cone sees the player (line-of-sight tested) flags it, then a "found"/"not found" announcement; a catch is surfaced as a scare (toast + horror sting) since there is no MTF squad to dispatch. The **live monitor feed** now works: the extractor captures each Screen camera's monitor transform, the monitor props (monitor2.b3d) render, and the nearest monitor shows a live camera view - the scene rendered from that camera toward the player into a scissored backbuffer corner and copied to a texture with glCopyTexImage2D (no FBO, like the blur), refreshed every few frames. Once the player clears the LCZ, SCP-079 broadcasts SCP-895's coffin feed onto the monitors (source CoffinEffect 2/3): the feed jitters and bleeds red, and feed-less monitors flicker red static. Approximated: the feed points at the player rather than tracking the exact camera swivel (the Blitz mirroring makes precise head-orientation untestable off-device). Particles (SetEmitter steam/spark/water) and the standalone sprites (screens, glows) are still to do |
| Lightmap blend model | done | The source's exact composite, still in a single pass: Map_Core gives the _lm layer TextureBlend 3 (add) and the diffuse layer TextureBlend 5 (multiply 2x), i.e. colour = 2 x diffuse x (vertex ambient + lightmap). Unit 0 now carries the LIGHTMAP with env GL_ADD onto the baked vertex colour, unit 1 carries the DIFFUSE as a COMBINE modulate with RGB_SCALE 2 (a host scan with the port's own RMesh loader confirmed all 1575 two-layer surfaces across the 133 rooms are exactly this diffuse-flag-5 + _lm-flag-3 pair, none other). The earlier failed modulate2x attempt had multiplied without the (ambient + lightmap) sum - that is what blackened dim rooms, not the 2x itself |
| Glass / alpha surfaces | done | Alpha-blended like LoadRMesh's Alpha mesh |
| Fog / HideDistance | done | UpdateZoneColor ported: linear fog 0.1..CameraFogDist world units recomputed each frame, coloured per zone (LCZ 005005005, HCZ 007002002, EZ 007007012) with the source's context overrides - pocket dimension black, SCP-1499's dimension 096097104 at 40..80, the 860-1 forest 098133162 at 8, the surface gates and the intro block white at 5..60 (IsOutSide), the storage tunnels 002007000, and the electrical-centre blackout closing to 4. Rooms wholly past the fog wall are skipped in both world passes (with default 6-unit fog only the neighbouring rooms draw - a fill/draw-call win that matches the source's CameraRange fogDist x 1.3 far plane). Entity logic keeps the fixed VIEW_RANGE (~13.7 wu) vs the source HideDistance 17; LightVolume's per-room light scaling of the fog range is not simulated |
| Dynamic room lights / light sprites | missing | RMesh light/light_fix/spotlight entities are parsed then discarded; rooms have only baked vertex colour + lightmaps - no point-light glow, lamp flares or coloured pools (also listed under Known visual gaps) |
| Skybox / surface sky | missing | Sky_Core not ported; the gate surface areas render with no sky |
| Particles / emitters | missing | Particles_Core / Devil_Particles_Core not ported at all: no dust, steam, gas clouds, sparks or blood mist (event gas/spark beats play sound-only) |
| Per-zone ambient colour / screen gamma | missing | CurrAmbientColor zone tinting and the RenderGamma brightness post-process are absent |
| Prop collision flag | partial | All prop triangles enter the collision grid regardless of the source per-prop hasCollision flag, so decorative no-collide props block the player |

## Player

| Feature | Status | Notes |
| --- | --- | --- |
| Movement, sprint, crouch, stamina, blink | partial | Speeds/multipliers and drain/recover rates match, block-at-empty honoured. Audit gaps: no StaminaMax caps (darkness/895/pocket), no crouch-toggle stamina cost, no low-stamina breath SFX, no StaminaEffect item multiplier; blink lacks the difficulty-scaled BLINKFREQ, EyeIrritation, EyeStuck and hold-to-keep-closed |
| Death screen / game-over menu | done | The source's post-death flow: the fade holds, then the mortuary report for the killer (read from Data/local.ini's [death] section, formatted with "Subject D-9341" - 173/guards/106/096/049/049-2/939/966/860/tesla/205/035/1499/008/914/012/895 all mapped, [DATA REDACTED] fallback otherwise) with LOAD GAME / QUIT TO MENU stacked on the pause-panel art under YOU DIED plus the difficulty/save/seed lines, the report below the buttons (the source's layout). Blinking is suppressed while dead. No auto-respawn (the source has none); on Keter+ the save is deleted and only QUIT remains |
| Lean | missing | D-pad left/right taken by inventory/menus |
| Health (injuries/bloodloss/sanity) | partial | Bleed rate min(injuries,3.5)/300 matches the source; sanity drains in the dark / pocket dimension / near 106. Audit deviations: injuries auto-clot on their own (source only heals via HealTimer), first aid heals instantly instead of the gradual HealTimer, no heartbeat audio ramp, no blur/zoom from bloodloss, no SCP-427, no vomit. 008 infection exists only via the cont2_008 chamber event |
| Save types per difficulty | partial | Save-anywhere / save-on-quit / no-saves honored; Euclid's save-on-screens saves anywhere (no monitor interaction). Audit: save DEPTH is shallow - only seed, difficulty, player pos/vitals, wearables (minus hazmat/714), inventory names, 173 pos, door open bits and item taken bits persist. All other NPC state, event states, door locks, ground-item positions, decals, SCP timers and facility power/blackout reset on load |
| Console | missing | Select is reserved but no console |
| Achievements / end screen | missing | |

## Menus / UI

| Feature | Status | Notes |
| --- | --- | --- |
| Main menu (art, tabs, NEW/LOAD/OPTIONS) | done | Incl. 173 flicker, creepy strings, touch |
| New-game panel (name/seed/intro/difficulties) | done | Esoteric customization included |
| Options | partial | Music/SFX volume, sensitivity, invert Y; source's graphics/controls/advanced tabs n/a or missing |
| Load-game pages, delete confirm | done | Named saves |
| Custom maps (Load Map tab) | missing | Menu_Core's custom-map browser and the New Game map-select are not ported |
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
| Keycards, SCP-005 | done | Levels gate doors. Audit flag: the press-x3 force-open debug fallback is still active and now trivializes progression since keycards work - should be removed. Hand-scanner (severed hand) doors and physical-key doors are not implemented (mapped to plain locked) |
| Documents readable | done | Paper/note/badge docImages |
| Eyedrops, first aid, 500-01, 420-J, pizza, cup | done | |
| Gas mask, vest, NVG (both), SCP-268 | partial | All are on/off booleans. Audit gaps: no gas-mask tiers or visor fog timer, no NVG battery drain/recharge, no vest tiers or helmet or per-hit VestFactor, no SCP-268 use timer, SCRAMBLE gear exists only as a 914 output and cannot be worn. Item dropping and non-914 combining are absent |
| S-NAV (minimap, Ultimate 173 blip) | done | Explored-room map; source draws room shapes |
| Radio | done | The source's four channels on the streamed-music path: CH1 alarm broadcast (RadioAlarm0 x4 then RadioAlarm1), CH2 on-site radio show (SCPRadio0 jingle alternating segments 1..8), CH3 static carrying the MTF chatter escalation (Random0..6 once each; keyed on leaving the LCZ since no MTF squad is ported), CH4 static with the one-time story ladder (Chatter2 remote-door hint first, then OhGod while 106 roams / Chatter1 / Franklin0 / Chatter3 / 035Help0 / Chatter0 / Franklin1 / 035Help1 / Franklin2/3 on the source's randomly-advancing counter). The SCP-079/895 broadcast degrades every channel to Static895 (CoffinEffect). Channel-scripted event lines tied to unported events (e.g. per-event Franklin radio cues) fire from the CH4 ladder rather than their events |
| SCP-914 refining | done | The refinement machine in cont1_914 works end to end: the knob (scp_914_knob.b3d) and control key render, interacting at the knob cycles the five settings (Rough / Coarse / 1:1 / Fine / Very Fine), and interacting at the input booth runs the selected inventory item through - the two booth doors seal, the machine whirs (914Refine.ogg), and after a few seconds the transformed item drops in the output booth as the doors reopen. The full Use914 transformation table is ported as a 68-row data table (gas masks, NVG/SCRAMBLE, vests/helmet, hazmat, radios, S-NAVs, electronics, batteries, eyedrops, syringes, first-aid, SCP-714/860/513/1025/148, severed hands, cigarettes, pills, paper/origami, playing card/coin/quarter/Mastercard, SCP-005) plus the keycard ladder (Rough shreds, Coarse down, 1:1 the famous Playing Card, Fine up, Very Fine up two, past L5 a Mastercard) and the source Default (Rough/Coarse destroy, otherwise pass through). The source's randomness and multi-output are reproduced: the rare "jackpot" rolls fire (a gas mask 1-in-4 refines to a hazmat suit, 1-in-50/100 on Fine/Very Fine to SCP-1499; NVG 1-in-5 to Fine SCRAMBLE Gear), Electronical Components rolls one of three tech outputs on Fine/Very Fine, and cases that emit two items do (syringe/battery Very Fine, the S-NAV and E-Reader upgrades, Mastercard Coarse into quarters, SCP-005 into a keycard + severed hand + key) - all dropped in the output booth. The difficulty-specific branches are ported: refining a keycard Fine/Very Fine rolls a Mastercard with odds that climb with difficulty (Easy never, up to ~2/3 on Extreme Very Fine), reading the running difficulty tier. The two NPC-spawn outcomes are real now: Very Fine SCP-1499 breeds a hostile SCP-1499 and a refined severed hand (Fine/Very Fine) breeds an SCP-008 zombie (scp_008_1.b3d) - the creature emerges at the output booth, shambles to the player and kills on contact ("SCP-008" / "SCP-1499" death). Any output whose template the port lacks passes the item through |
| Hand items in view (equipped models) | done | The held radio draws bottom-left (source radio.png in-hand image) with its channel readout while on; the S-NAV draws its device body (navigator.png) with the live map on its screen. These are the two devices the source holds in view; wearables show as overlays instead (below) |
| Wearable visuals (mask overlay etc.) | done | The source's GFX/Overlays fullscreen art: gas-mask visor while a mask (or SCP-1499) is worn, hazmat visor for the suit, and the NVG goggle housing over the existing green boost |

## NPCs

| Feature | Status | Notes |
| --- | --- | --- |
| SCP-173 | done | Freeze-on-sight, blink interplay, last-seen search, TeleportCloser, door opening, kill + camera wrench, horror stings, head tracking |
| Skeletal animation engine | done | B3D BONE/KEYS/ANIM, CPU skin, VBO path |
| Intro guards / Class-Ds | done | Source roster, idles, walking escort, gunfire enforcement |
| SCP-106 | mostly done | Roaming hunter with facility BFS pathfinding + phase-behind; femur-breaker recontainment gated on the magnet lever; corrosion pool on spawn and a footfall trail; corrodes the sliding/heavy doors it passes (texture swap); a trailing faded second body wells up behind it at distance; the lens zooms inward and the view blurs (real framebuffer-copy post-process) when it holds your gaze - and at the throne / pocket-dimension exit; grab-and-wrench attack on contact (frozen input, view twist) before the pocket-dimension drag; spawn timer counts down twice as fast on aggressive difficulties and is slowed/halted by the room's DisableDecals (0 full, 1 half, 2 quarter, 3 never); a Tesla gate zap now repels it (its long-missing State 4 retreat), sending it dormant. Remaining: only the Micro-HID retreat trigger (no HID weapon in the port) |
| SCP-682 | done | The e_682_roar set-piece in room2_5_hcz/room3_hcz/room2_5_ez, ported faithfully as the source intends it: a pure audio + camera scare - 682 is never seen and deals NO damage. A distant tremor builds while the player is in the room, it roars partway through, the shake peaks, then it fades. (An earlier build fabricated an arm bursting in and hurting the player; that arm actually belongs to the Gate B ending, so it is now dormant - reserved for that ending - and the roar does no damage, matching the source.) |
| SCP-079 | done | The sentient computer in cont1_079's lower chamber (e_cont1_079). To reach it the room's elevator now descends vertically: an elevator door with a same-X/Z partner at a very different Y rides between the two levels (the arrival floor-ray drops from the destination level), so the chamber below is reachable through the KEY_CARD_4 blast door. SCP-079's console (scp_079.b3d) renders with a glowing screen that flickers its seven overlay frames; reaching the terminal wakes it - it plays its speech, the screen flickers, and it drops the "Document SCP-079" (the item template, and the other event-only documents, are now emitted by extract_items.py); approaching again after it finishes makes it refuse. SCP-079's SCP-895 camera-feed broadcast is ported (it scrambles the monitors red once you clear the LCZ - see the cameras row). Not ported: the Gate B broadcast tie-in (no gate endings) |
| SCP-035 | partial | The possessed Class-D host in cont1_035 (e_cont1_035): it slumps in the chamber (rendered with the Class-D skin under the scp_035_victim texture) until you look at it up close, then it gets up (GetUp.ogg) and hunts you down, killing on contact ("SCP-035"); it returns to its seat if it loses you. Not ported: the gas-lever containment puzzle that subdues it and the corrosive SCP-035 tentacles that spread from the chamber |
| SCP-513-1 | done | Ringing the SCP-513 bell (use the item) summons SCP-513-1 (skinned scp_513_1.b3d), which haunts the player for the rest of the run (UpdateNPCType513_1): it lurks at the edge of sight and flits elsewhere the moment you centre it in your gaze, tolls its bells (513_1/Bell) now and then, and its nearby presence bleeds sanity and blurs the view - a persistent scare that never touches you. The bell is consumed on the ring |
| SCP-205 | done | The shadow lamps in cont1_205 (e_cont1_205): a skinned shadow demon (scp_205_demon.b3d) rises out of the chamber floor and looms while the player is in the room, moaning (Horror.ogg) and gnawing sanity. It is seen chiefly on the observation monitor - the cont1_205 security camera's feed aims at the demon (not the player) so it fills the screen, using the live monitor feed. Once fully risen the shadow lunges - it closes on the player and grabs on contact (a "SCP-205" death), matching the source's lethal grab; sinking back when you leave. Approximated: the source's three-demon + rising-woman set-piece is one shadow demon |
| SCP-372 | done | The peripheral jumper in cont3_372 (UpdateNPCType372): a skinned creature (scp_372.b3d) that stays hidden, then now and then flits to a spot ~450 raw off to your side and lingers, bobbing and spinning - but the instant you centre it in your gaze (within ~22 deg of the view forward) it darts back toward the edge of sight, with the occasional rustle. Purely a startle; it never touches you. It vanishes after a short while and re-appears - revealed chiefly while the player is mid-blink (source BlinkTimer window), so you blink and it is suddenly there at the edge of sight |
| SCP-012 | done | "A Bad Composition" in cont2_012 (Main_Core scribe_event): standing at the score pins your eyes open and floods the screen with the bloody compulsion overlay (scp_012_overlay.png), jittering it each frame like the source, with a horror sting; sanity bleeds while it holds you, and the overlay fades once you step away. The ventilation-fan / red-light dressing is not reproduced |
| SCP-895 | done | The camera coffin in cont1_895 (e_cont1_895): the chamber fills with dread scaled by how close you get - sanity bleeds, the view blurs, and the trapped guard murmurs (GuardIdle) - and a close approach makes the slumped guard corpse (guard.b3d) lurch into view with a scream (GuardScream) + horror sting + camera shake; backing off re-arms it. The SCP-895 camera-feed amplification (CoffinEffect) is ported now that the monitor feed exists: after the player clears the LCZ, SCP-079 broadcasts the coffin feed onto the monitors, scrambling them red (see the cameras row). Not ported: the source's SCP-106 lure toward the coffin |
| SCP-096 | done | Skinned model (scp_096.b3d) spawns sitting in an HCZ room (e_096_spawn list, fallback to any room3/room4). Harmless until you look at its face (in the view cone, in range, not mid-blink); then it gets up, screams in place (~8 s, compressed from the source's ~26 s), then sprints down the player via cell pathfinding and kills on contact (blood decals, heavy shake, "SCP-096" death). Triggered/Scream sounds and the dread-zoom pulse included. Not ported: the SCRAMBLE-goggles counter, the MTF "spotted" reaction, and the exact 26 s scream |
| SCP-049 + 049-2 | partial | The Plague Doctor (scp_049.b3d) spawns in cont2_049 (or a far room) and idles, then activates and walks the player down via cell pathfinding, phasing closer if they break away, killing on a touch ("SCP-049"); spotted/searching/breath voice lines. A retinue of two reanimated 049-2 shamblers (scp_049_2.b3d, shared walk phase) hunt alongside once 049 is active and also kill on contact. The Hazmat suit and SCP-714 (jade ring) now buy a few seconds when 049 grips you - it tears them off (with a warning + view/shake cues) before the cure, and fleeing lets the timer recover; once destroyed the next grip is fatal. Not ported: dynamic conversion of killed NPCs into new 049-2, and the surveillance-room subplot |
| SCP-939 | partial | A blind pack predator prowling room3_storage that hunts by the noise the player makes (playerNoise: loud sprinting draws it from across the room, a still crouch slips past). Patrol -> alert (stalks the sound) -> attack (charges and bites, each bite injuring; enough bites, injuries > 4, kill "SCP-939"); attack/horror voice lines. It patrols - drifting between wander points around its storage room - and mimics human voices to draw the curious closer; not ported: the exact source waypoint graph |
| SCP-966 | done | The sleep-stalker: spawns in an HCZ room and is invisible to the naked eye - only the night-vision goggles render it. Unwatched, it creeps toward the player and saps their stamina and sanity (a drowsiness blur, "your eyelids are so heavy" toasts); watched through the goggles it hangs back. Caught while exhausted, it kills "SCP-966". Not ported: the per-instance "insomnia" item state and the many flavor whispers |
| SCP-1499-1 | done | Donning the SCP-1499 mask (use the item) exiles the player to dimension_1499. The congregation is laid out at the source's church coordinates - the king enthroned on the altar (his own seated animation + crowned texture), a guard beside him, two guards at the entrance, and citizens milling through the nave (drifting near their spots, murmuring). It starts peaceful; straying too near any member rouses the whole church, which then converges - guards quicker than citizens, the king watching from the altar - and kills on contact ("THE PEOPLE OF SCP-1499") while sanity bleeds. Taking the mask off returns you; facility SCPs are suspended meanwhile. Citizens now pair off to converse - drifting toward a neighbour and murmuring at each other before easing back to their post; approximated vs source: the per-member scream states remain a single congregation-wide rouse |
| SCP-860-1 | done | The forest stalker in cont2_860_1 (a force-placed room): a seed-scattered copse of trees, with SCP-860-1 (scp_860_2.b3d) that advances only while unwatched and freezes the instant it is looked at (or you blink) - lurching much closer when your back is turned - and kills on contact in the dark. The forest is now a **procedural maze** (a port of the source ForestGrid): a winding path is carved down a 10x10 grid - starting at a top column and deviating sideways within a couple of columns of centre - and a tree stands on every off-path cell, each a solid trunk that pushes the player, so you must follow the path. SCP-860-1 lurks at the far end and only advances while unwatched (a trunk on the sightline hides it), killing on contact in the dark. Approximated vs source: the two grid doors bounding the path are the room's own |
| Guards/MTF in gameplay | missing | Only intro figures and event corpses/cameos. Audit scope: the whole armed-human layer is absent - MTF/Nine-Tailed Fox squads (~1500 source lines: zone-progress spawning, patrols, shooting, SCP recontainment), roaming guards, Apache helicopters, Class-D/Clerk civilian AI, and with them any gunfire damage path. Also absent as AI (props/cameos only): SCP-008-1 zombies, SCP-066, SCP-1048 and hostile 1048-A |
| NPC waypoint pathfinding | done | 173 BFS-routes through room cells/doorways toward the player and last-seen spot |

## Events / story

| Feature | Status | Notes |
| --- | --- | --- |
| Intro sequence | done | Wake screen, bunk cinematic, PA/dialog beats, escort, chamber sequence, breach; simplified NPC theatrics |
| Per-room events (Events_Core, ~100 events) | mostly done | Audited one-by-one against the 73 Events_Core constants: 60 done, 3 partial (cont1_106 femur lacks the radio-lure/break-free branch/corpse; room2_6_hcz_173 folded into the generic 173-appear; room2_mt is maze+generator geometry with the puzzle reduced), 10 missing: gate_a and gate_b (the endings), room2_nuke (warhead arming), cont1_005 (the 106 trap room), trick_item (blink item bait), 1048_a (hostile ear-monster), room2_4_hcz_106, room3_2_ez_duck, and room2_test_lcz_173 / room4_ic (enum stubs never wired). Everything else - janitor abduction, closets kill, 970 loop, 294/458, 1123 flashback, 066, airlock, checkpoint power, elevator death, all corpse/duck/scare events - is in and driven by a two-slot-per-room system with skinned cameos and world props |
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
| Per-SCP / per-room chamber music | done | Source ShouldPlay overrides ported: entering a signature chamber swaps in that encounter's track (079/914/012/035/205/106/049 Chamber, Room3_Storage, 860_1_Blue), and a monster starting its hunt outranks it with chase music (096 Angered/Chase, 106 Chase, 049 Chase), falling back to the zone track otherwise. Polled each frame in `desiredMusicPath`; tracks stream on demand |
| Chamber music not yet wired | partial | Gate_A/B, 008, 1123, 409, 1499Dimension/Chase and PD/PDTrench tracks exist but their trigger rooms/endings are not ported, so those specific cues never fire |
| Music track coverage / crossfade | partial | Audit: ~19 of the source's 34 Music[] tracks never play (PD, PDTrench, Gate_A/B, 1499 dimension+chase, 860_2 chase, 008, 409, 1123, MaintenanceTunnels, 860_1_Red, MenuBreath, Ending, Credits, SaveMeFrom); track changes hard-cut where the source volume-fades; the mixer is 12 SFX voices + 1 ambience + 1 music channel, and room ambience is a single mono channel (no pan/layering) |
| Heartbeat / player breath | missing | The source's HeartBeatSFX rate/volume ramp (bloodloss, sanity, chases) and the stamina/gas-mask breathing loops (BreathCHN) are entirely absent; cough/vomit too. The single most audible player-feedback gap |
| Master/Voice volume, deafen | missing | Only Music+SFX sliders; no Master or Voice channel split, no UpdateDeaf flash-deafen effect |
| Startup videos | done | PlayStartupVideos ported and confirmed playing on real hardware: the four boot clips (Undertow / TSS / UET / Warning) play in order before the title menu, gated by a new "Startup videos" option. The source .wmv (WMV3) is undecodable on the Vita, so each was transcoded to H.264 MP4 (960x540, Main/L3.1, its .ogg muxed as AAC) and plays as real hardware-decoded video via sceAvPlayer. No title-card substitute (like the source, an unopenable clip is just skipped) |
| Intro cutscene (startup_Intro) | done | Real full-motion video: the source .wmv is undecodable on the Vita, but the same cutscene ships as H.264 MP4 (three sequential fragments), played on a new game via sceAvPlayer (hardware decode) drawn through vitaGL. The GPU converts the decoder's YUV frames in hardware (YVU420P2_CSC1 texture format - no shader/CPU conversion); audio runs on the BGM output port, letterboxed, any-button skip. Gated by the intro + startup-video options |
| Video memory | done | vitaGL is init'd via vglInitWithCustomThreshold leaving 16 MB CDRAM + 8 MB PHYCONT unreserved so sceAvPlayer's decoder buffers have somewhere to go (it otherwise claimed both partitions whole); the decoder allocator falls back CDRAM->PHYCONT. The boot videos also run before the heavy asset/sound load so their CPU-side demux buffers fit |
| Loading screen | done | Menu_Core RenderLoading ported: a random SCP render (GFX/LoadingScreens) over loading_back.png with its SCP-number title and the current load step + percent, shown while models/textures/sounds load instead of a black screen |
| Movie coverage | done | The source calls PlayMovie on exactly five clips - startup_Undertow/TSS/UET/Warning and startup_Intro - all of which are converted to H.264 MP4 and played. There are no other movie files (the endings/credits are rendered in-engine, not videos), so nothing remains unconverted |
| Footstep surface variants | partial | Metal step/run set on grating/panel floors via down-ray texture test. Audit: only 2 of the source's 6 material classes (no grass/wood/forest/decal sets; materials.ini StepSound not parsed generically) |
| Door/interact/horror/intro voice sets | done | |
| Ambient room emitters | done | Source AmbientSFX: every ~15-45 s a random distant one-shot from the current zone's set (SFX/Ambient/Zone1-3, or Forest in the SCP-860 room) drifts in - a scream, a groan, dripping - positioned off to one side of the player and attenuated with distance, so the facility never falls silent. Loaded on demand (the audio cache dedups by path; MAX_SOUNDS raised to fit). Suspended in the intro / pocket / mask dimensions |

## Memory model (the black-world postmortem)

The port once decoded every preloaded sound to PCM at boot (>100 MB),
running the 220 MB newlib heap to ~230 MB used / <1 MB free in-game.
Under that pressure allocations failed all over: sound decodes
(dec-fail), texture decodes (texfail), and - fatally - room-template
loads, which `templateEnsureStep` latched dead silently, so
`activeCount` stayed 0 and the whole 3D pass was skipped: black world
with a live HUD. Hardware-bisected with the on-screen `bld=` tag and
memory HUD; fixed by decoding sounds lazily on first play (boot heap
now ~42-67 MB used), retrying + logging template failures to
`ux0:data/scpcb-ue/render_log.txt`, and freeing the loading-screen art
after boot. First play of a long sound costs a one-time decode hitch.

## Room-streaming performance

The fps used to drop ~5 and stay unstable for a few seconds whenever a
new room streamed in. Profiled (static analysis + host measurement of
the real loaders, adversarially verified): texture steps decoded PNGs
2 batches per frame at ~13-60 ms each on the Vita CPU, every cache miss
also paid an unindexed readdir scan of 300+ entry directories, state 0
did mesh parse + all props + collision in one frame (150-450 ms), the
VBO upload burst added 10-40 ms, and entering a room with a new sound
emitter full-decoded its OGG on the main thread (a 30 s ambience loop =
1-3 s frozen). Fixes: per-directory listing cache in textureResolve;
the load pipeline split into small per-call units (mesh, one prop,
collision, one texture batch, 8 VBOs) driven by a ~3 ms per-frame
budget; sound PCM decodes on a low-priority decoder thread (ambience
starts when ready via audioService; small hot SFX pre-decode at boot so
first plays stay instant). Frames stalling >8 ms in the loader log to
render_log.txt, and the debug HUD shows the worst frame ms per window.
Second round (the on-device log then showed 25-171 ms texture batches,
8-209 ms props and 50-210 ms mesh parses still landing on the render
thread): all decode/parse work moved to a dedicated low-priority loader
thread - mesh+scene build, prop B3D parse, texture PNG decode, with a
few batches queued ahead so the worker pipelines - and the render
thread only consumes results, uploading at most two textures per frame.
The spawn area now loads behind a "LOADING AREA" screen instead of
freezing the first gameplay frame for ~5 s.
Third round (the log then showed one 17-133 ms hitch per room - the
collision grid build - plus a 9 s prewarm dominated by PNG decode):
collision builds moved to the loader thread too, and the data packager
now writes world textures as pre-decoded raw RGBA ("VTEX" content under
the original filenames; textureLoadFile sniffs the magic like DDS), so
the device never runs a PNG/JPG decoder for them. Requires re-running
the package-data job and reinstalling the data (~90 MB larger raw);
old PNG data still works, just slower.

## Chirality (the mirrored-world fix)

Blitz3D data is left-handed; the port drew it verbatim through a
right-handed GL pipeline, so the ENTIRE world rendered as its mirror
image (room layouts flipped versus the original game, sign text
backwards) - internally consistent, which is why gameplay worked, but
unfaithful. Fixed at the display level: the 3D views (main + camera
feed) apply a screen-X mirror, restoring original chirality without
touching any world/sim coordinate, and the handedness consumers flip to
match - front-face winding (CW under the mirror), horizontal look and
strafe input signs, 3D audio pan, the S-NAV map's X axis and heading
dot, and the monitor feed quad's U (its texture is already
chirality-correct screen pixels). Saves remain compatible.

## Known visual gaps

- No dynamic lights, flicker, or shadows (source flickers room lights).

