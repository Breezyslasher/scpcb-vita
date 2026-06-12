# PS Vita porting plan

The original game is ~56k lines of Blitz3D BASIC. Blitz3D only targets
32-bit Windows/DirectX 7, so there is no toolchain that can compile the
`.bb` sources for the Vita's ARM CPU. The port therefore follows the
"option 2" strategy: rebuild the game in C++ on VitaSDK, reusing the
community C++ rewrite of SCP:CB ([juanjp600/scpcb](https://github.com/juanjp600/scpcb),
"CBN") as the reference for game logic and asset formats, and grafting
Ultimate Edition Reborn's content (rooms, NPCs, items, events under
`Data/`, `GFX/`, `SFX/`) on top.

CBN itself runs on the PGE engine (Direct3D 11 / desktop OpenGL), which
does not run on the Vita, so its renderer is replaced rather than ported.
Game logic, formats, and structure carry over; the platform layer is new.

## Milestones

1. **Port shell** *(done — this directory)*
   - VitaSDK CMake project producing an installable VPK with LiveArea assets.
   - Full controller layer: all buttons, both sticks with deadzone handling,
     front touch, with a live on-hardware test screen (`src/main.c`).
   - Manually triggerable CI workflow building the VPK
     (`.github/workflows/build-vita-vpk.yml`).

2. **Asset pipeline**
   - RMesh (room) and B3D (model) loaders in C++ — CBN's
     `Src/Graphics/Rooms` and PGE mesh code are the reference.
   - Texture loading (PNG/JPG via libpng/libjpeg already linked).
   - Asset conversion step in CI: Vita RAM is 512 MB, so `GFX/` (≈400 MB)
     needs downscaling/recompression (PVRTC/DXT via texture tools) and
     `SFX/` (≈120 MB) conversion to OGG/AT9. Assets ship in the VPK or as
     a separate data package.

3. **Renderer**
   - Replace the shell's vita2d usage with VitaGL (OpenGL 1.x-style API
     maps well onto Blitz3D's DX7 fixed-function model: entities, brightness,
     fog, FX flags).
   - Room rendering with the original lightmaps, then entities/NPC meshes.

4. **Game systems** (grafted from CBN, adapted to UE Reborn content)
   - Player movement/collision, doors, items/inventory (touch-driven UI),
     save/load, events, NPC AI — in roughly the load-bearing order the
     Blitz3D sources define them (`Map_Core.bb`, `Items_Core.bb`,
     `NPCs_Core.bb`, `Events_Core.bb`, ...).

5. **Performance & memory**
   - The full facility won't fit in memory at desktop quality; expect
     aggressive texture budgets, room streaming, and reduced NPC counts.

## Honest expectations

Milestones 2–4 are a sustained engineering effort (the comparable
fan ports of SCP:CB to consoles took experienced homebrew developers
months). The shell, input layer, CI pipeline, and plan here are the
foundation that makes that work incremental and testable on hardware
from day one.

## Licensing

The game and source are CC BY-SA 3.0 (see the repository README); the SCP
Foundation content is CC BY-SA 3.0. Reused CBN/PGE code is subject to the
licenses in their repositories — keep attribution when grafting code.
