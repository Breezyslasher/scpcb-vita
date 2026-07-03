/*
 * SCP - Containment Breach Ultimate Edition Reborn — PS Vita port.
 *
 * Milestone 4: the facility. Generates the map (CreateMap port), loads
 * room templates on demand as the player approaches, and renders the
 * connected world with sliding doors between rooms (Cross interacts;
 * D-pad up toggles walk/fly). Requires the data package at
 * ux0:data/scpcb-ue/ and libshacccg.suprx (see vita/README.md).
 */

#include <dirent.h>
#include <math.h>
#include <sys/stat.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "audio/audio.h"
#include "formats/b3d.h"
#include "formats/rmesh.h"
#include "formats/texture.h"
#include "game/collision.h"
#include "game/doors.h"
#include "game/item_spawns.h"
#include "game/room_doors.h"
#include "game/mapgen.h"
#include "input.h"
#include "render/scene.h"
#include "render/skin.h"

#define STB_EASY_FONT_IMPLEMENTATION
#include "render/stb_easy_font.h"

/* vitasdk newlib defaults to a 32 MB heap; rooms, collision and PCM
 * audio need far more. */
unsigned int _newlib_heap_size_user = 220 * 1024 * 1024;

#define SCREEN_W 960
#define SCREEN_H 544
#define TEXTURE_CAP 256

#define DATA_ROOT "ux0:data/scpcb-ue"
#define MAP_DIR DATA_ROOT "/GFX/Map"
#define MAP_TEXTURES_DIR DATA_ROOT "/GFX/Map/Textures"
#define PROPS_DIR DATA_ROOT "/GFX/Map/Props"
#define ITEMS_DIR DATA_ROOT "/GFX/Items"
#define ITEMS_HUD_DIR DATA_ROOT "/GFX/Items/HUD Textures"
#define HUD_DIR DATA_ROOT "/GFX/HUD"
#define INV_ICONS_DIR DATA_ROOT "/GFX/Items/Inventory Icons"
#define NPCS_DIR DATA_ROOT "/GFX/NPCs"
#define MENU_DIR DATA_ROOT "/GFX/Menu"
#define ROOMS_INI DATA_ROOT "/Data/rooms.ini"

/* RoomSpacing is 8 world units = 2048 raw mesh units. */
#define ROOM_SPACING 2048.0f
#define VIEW_RANGE 3500.0f

/* Player metrics in raw mesh units (see Loading_Core.bb/Main_Core.bb):
 * collider 0.15/0.30, camera at collider + 0.6, crouch -0.3, walk
 * 0.018/frame, sprint x2.5, crouch x0.5. */
#define PLAYER_RADIUS 38.0f
#define EYE_HEIGHT 230.0f
#define CROUCH_EYE_HEIGHT 153.0f
#define BODY_DROP 150.0f
#define STEP_SLACK 25.0f
#define WALK_SPEED 4.6f
#define SPRINT_MULT 2.5f
#define CROUCH_MULT 0.5f
#define GRAVITY 0.5f
#define TERMINAL_FALL 20.0f

/* ---------------- texture cache (global, lives for the map) -------- */

typedef struct {
    char *name;
    GLuint handle;
} CachedTexture;

static CachedTexture *texCache;
static unsigned texCacheCount;

static GLuint textureGet(const char *name) {
    if (!name) return 0;
    for (unsigned i = 0; i < texCacheCount; i++) {
        if (strcmp(texCache[i].name, name) == 0) return texCache[i].handle;
    }

    GLuint handle = 0;
    const char *dirs[8] = { MAP_DIR, MAP_TEXTURES_DIR, PROPS_DIR, ITEMS_DIR,
                            ITEMS_HUD_DIR, HUD_DIR, INV_ICONS_DIR, NPCS_DIR };
    char path[1024];
    if (textureResolve(name, dirs, 8, path, sizeof(path))) {
        char err[128];
        TextureImage *img = textureLoadFile(path, TEXTURE_CAP, err, sizeof(err));
        if (img) {
            glGenTextures(1, &handle);
            glBindTexture(GL_TEXTURE_2D, handle);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)img->width,
                         (GLsizei)img->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         img->pixels);
            textureFree(img);
        }
    }

    CachedTexture *grown = (CachedTexture *)realloc(
        texCache, (texCacheCount + 1) * sizeof(CachedTexture));
    if (grown) {
        texCache = grown;
        texCache[texCacheCount].name = strdup(name);
        texCache[texCacheCount].handle = handle;
        if (texCache[texCacheCount].name) texCacheCount++;
    }
    return handle;
}

static void textureCacheClear(void) {
    glBindTexture(GL_TEXTURE_2D, 0);
    for (unsigned i = 0; i < texCacheCount; i++) {
        if (texCache[i].handle) glDeleteTextures(1, &texCache[i].handle);
        free(texCache[i].name);
    }
    free(texCache);
    texCache = NULL;
    texCacheCount = 0;
}

/* ---------------- prop model cache ---------------- */

typedef struct {
    char *name;
    B3DModel *model;
} CachedModel;

static CachedModel *modelCache;
static unsigned modelCacheCount;

static B3DModel *propModelGet(const char *rawName) {
    const char *base = rawName;
    for (const char *p = rawName; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    char name[256];
    snprintf(name, sizeof(name), "%s", base);
    size_t len = strlen(name);
    if (len > 4 && strcasecmp(name + len - 4, ".b3d") == 0) {
        name[len - 4] = '\0';
    }
    strncat(name, ".b3d", sizeof(name) - strlen(name) - 1);

    for (unsigned i = 0; i < modelCacheCount; i++) {
        if (strcmp(modelCache[i].name, name) == 0) return modelCache[i].model;
    }

    B3DModel *model = NULL;
    const char *dirs[3] = { PROPS_DIR, ITEMS_DIR, NPCS_DIR };
    char path[1024];
    if (textureResolve(name, dirs, 3, path, sizeof(path))) {
        char err[128];
        model = b3dLoadFile(path, err, sizeof(err));
    }

    CachedModel *grown = (CachedModel *)realloc(
        modelCache, (modelCacheCount + 1) * sizeof(CachedModel));
    if (grown) {
        modelCache = grown;
        modelCache[modelCacheCount].name = strdup(name);
        modelCache[modelCacheCount].model = model;
        if (modelCache[modelCacheCount].name) modelCacheCount++;
    }
    return model;
}

static void modelCacheClear(void) {
    for (unsigned i = 0; i < modelCacheCount; i++) {
        b3dFree(modelCache[i].model);
        free(modelCache[i].name);
    }
    free(modelCache);
    modelCache = NULL;
    modelCacheCount = 0;
}

/* ---------------- room templates (runtime side) ---------------- */

typedef struct {
    GLuint diffuse;
    GLuint lightmap;
    GLuint vbo;
    GLuint ibo;
} BatchGL;

typedef struct {
    int state; /* 0 = not loaded, 1 = ok, -1 = failed */
    Scene *scene;
    BatchGL *gl;
    CollisionWorld *col;
} TemplateRT;

static RoomTemplateList tplList;
static TemplateRT *tplRT;
static GeneratedMap map;
static DoorList doors;
static uint32_t mapSeed = 1;

static void templateUnload(TemplateRT *rt) {
    if (rt->gl && rt->scene) {
        for (uint32_t i = 0; i < rt->scene->batchCount; i++) {
            if (rt->gl[i].vbo) glDeleteBuffers(1, &rt->gl[i].vbo);
            if (rt->gl[i].ibo) glDeleteBuffers(1, &rt->gl[i].ibo);
        }
    }
    free(rt->gl);
    sceneFree(rt->scene);
    collisionFree(rt->col);
    memset(rt, 0, sizeof(*rt));
}

static void templateEnsure(int idx) {
    TemplateRT *rt = &tplRT[idx];
    if (rt->state != 0) return;
    rt->state = -1;

    const RoomTemplateInfo *info = &tplList.items[idx];
    if (!info->meshPath) return;

    char path[1024];
    snprintf(path, sizeof(path), MAP_DIR "/%s", info->meshPath);
    char err[128];
    RMesh *mesh = rmeshLoadFile(path, err, sizeof(err));
    if (!mesh) return;

    rt->scene = sceneBuild(mesh);
    if (!rt->scene) {
        rmeshFree(mesh);
        return;
    }
    for (uint32_t i = 0; i < mesh->entityCount; i++) {
        const RMeshEntity *e = &mesh->entities[i];
        if (e->type != RMESH_ENTITY_PROP || !e->u.prop.file) continue;
        B3DModel *model = propModelGet(e->u.prop.file);
        if (!model) continue;
        float pos[3] = { e->x, e->y, e->z };
        float euler[3] = { e->u.prop.pitch, e->u.prop.yaw, e->u.prop.roll };
        float scl[3] = { e->u.prop.scaleX, e->u.prop.scaleY, e->u.prop.scaleZ };
        sceneAppendB3D(rt->scene, model, pos, euler, scl, e->u.prop.texture);
    }
    rt->col = collisionBuild(rt->scene, mesh);
    rmeshFree(mesh);

    rt->gl = (BatchGL *)calloc(rt->scene->batchCount ? rt->scene->batchCount : 1,
                               sizeof(BatchGL));
    if (!rt->gl) return;
    for (uint32_t i = 0; i < rt->scene->batchCount; i++) {
        const SceneBatch *b = &rt->scene->batches[i];
        rt->gl[i].diffuse = textureGet(b->diffuseName);
        rt->gl[i].lightmap = textureGet(b->lightmapName);
        glGenBuffers(1, &rt->gl[i].vbo);
        glBindBuffer(GL_ARRAY_BUFFER, rt->gl[i].vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(b->vertexCount * sizeof(SceneVertex)),
                     b->vertices, GL_STATIC_DRAW);
        glGenBuffers(1, &rt->gl[i].ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rt->gl[i].ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(b->indexCount * sizeof(uint16_t)),
                     b->indices, GL_STATIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    rt->state = 1;
}

/* ---------------- placement transforms ---------------- */

/* Render transform: world = T(center) * Ry(-angle * 90deg) * local
 * (the -angle mirrors Blitz's left-handed rotation into our space). */

/* These must match drawRoomBatches' glRotatef(-angle * 90): probing
 * every grid doorway of a generated map through worldToLocal +
 * collisionRayDown finds floor at all 228 doorways with this mapping
 * (the 1/3-swapped variant misses 10). */
static void worldToLocal(const RoomPlacement *p, const float w[3],
                         float l[3]) {
    float dx = w[0] - p->gridX * ROOM_SPACING;
    float dy = w[1];
    float dz = w[2] - p->gridY * ROOM_SPACING;
    switch (p->angle & 3) {
        case 0: l[0] = dx;  l[2] = dz;  break;
        case 1: l[0] = dz;  l[2] = -dx; break;
        case 2: l[0] = -dx; l[2] = -dz; break;
        case 3: l[0] = -dz; l[2] = dx;  break;
    }
    l[1] = dy;
}

static void localToWorld(const RoomPlacement *p, const float l[3],
                         float w[3]) {
    float x = 0.0f, z = 0.0f;
    switch (p->angle & 3) {
        case 0: x = l[0];  z = l[2];  break;
        case 1: x = -l[2]; z = l[0];  break;
        case 2: x = -l[0]; z = -l[2]; break;
        case 3: x = l[2];  z = -l[0]; break;
    }
    w[0] = x + p->gridX * ROOM_SPACING;
    w[1] = l[1];
    w[2] = z + p->gridY * ROOM_SPACING;
}

/* FillRoom's room-internal doors (containment chambers, elevator
 * covers, locked service doors) from room_doors.h, placed with the
 * same transform as item spawns. */
static void spawnRoomDoors(void) {
    const int N = (int)(sizeof(ROOM_DOORS) / sizeof(ROOM_DOORS[0]));
    for (uint32_t r = 0; r < map.roomCount; r++) {
        const RoomPlacement *p = &map.rooms[r];
        const char *nm = tplList.items[p->templateIndex].name;
        for (int i = 0; i < N; i++) {
            const RoomDoorDef *rd = &ROOM_DOORS[i];
            if (strcmp(rd->room, nm) != 0) continue;
            float local[3] = { rd->x, rd->y, rd->z };
            float w[3];
            localToWorld(p, local, w);
            int a = (int)(rd->angleDeg / 90.0f + 0.5f) * 90 + p->angle * 90;
            a = ((a % 360) + 360) % 360;
            doorsAddInternal(&doors, w[0], w[1], w[2], a, rd->type,
                             rd->open, rd->keycard, rd->locked,
                             rd->nobuttons);
        }
    }
}

/* ---- intro sequence (cont1_173_intro, "placed automatically in all
 * maps" per rooms.ini): the Class-D cell block and 173's chamber on
 * an isolated off-grid cell. ---- */

#define INTRO_GX -8
#define INTRO_GY -8
static int introRoomIdx = -1;   /* index in map.rooms, -1 = none */
/* The Class-D cell interior is a separate mesh the game overlays on
 * the intro room (Rooms_Core: cont1_173_intro_player_cell.rmesh at
 * the room origin). */
static Scene *introCellScene;
static BatchGL *introCellGL;
static CollisionWorld *introCellCol;
static int introPhase = -1;     /* -1 inactive, >=0 running */
static int introTimer;
static int introGateDoor = -1;

/* The intro mesh spans several grid cells (local x -8736..2048,
 * z -3896..1503); anything within these world bounds is "in the
 * intro area". */
static int inIntroBounds(float x, float z) {
    if (introRoomIdx < 0) return 0;
    float ox = INTRO_GX * ROOM_SPACING, oz = INTRO_GY * ROOM_SPACING;
    return x > ox - 9000.0f && x < ox + 2300.0f
        && z > oz - 4200.0f && z < oz + 1800.0f;
}

static void buildIntroCell(void) {
    if (introCellScene) return; /* built once, kept for the session */
    char err[128];
    RMesh *mesh = rmeshLoadFile(MAP_DIR "/cont1_173_intro_player_cell.rmesh",
                                err, sizeof(err));
    if (!mesh) return;
    introCellScene = sceneBuild(mesh);
    if (!introCellScene) {
        rmeshFree(mesh);
        return;
    }
    introCellCol = collisionBuild(introCellScene, mesh);
    rmeshFree(mesh);
    introCellGL = (BatchGL *)calloc(
        introCellScene->batchCount ? introCellScene->batchCount : 1,
        sizeof(BatchGL));
    if (!introCellGL) return;
    for (uint32_t i = 0; i < introCellScene->batchCount; i++) {
        const SceneBatch *b = &introCellScene->batches[i];
        introCellGL[i].diffuse = textureGet(b->diffuseName);
        introCellGL[i].lightmap = textureGet(b->lightmapName);
        glGenBuffers(1, &introCellGL[i].vbo);
        glBindBuffer(GL_ARRAY_BUFFER, introCellGL[i].vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(b->vertexCount * sizeof(SceneVertex)),
                     b->vertices, GL_STATIC_DRAW);
        glGenBuffers(1, &introCellGL[i].ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, introCellGL[i].ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(b->indexCount * sizeof(uint16_t)),
                     b->indices, GL_STATIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void appendIntroRoom(void) {
    introRoomIdx = -1;
    int tplIdx = -1;
    for (uint32_t i = 0; i < tplList.count; i++) {
        if (strcmp(tplList.items[i].name, "cont1_173_intro") == 0) {
            tplIdx = (int)i;
            break;
        }
    }
    if (tplIdx < 0) return;
    RoomPlacement *grown = (RoomPlacement *)realloc(
        map.rooms, (map.roomCount + 1) * sizeof(RoomPlacement));
    if (!grown) return;
    map.rooms = grown;
    RoomPlacement *p = &map.rooms[map.roomCount];
    p->templateIndex = tplIdx;
    p->gridX = INTRO_GX;
    p->gridY = INTRO_GY;
    p->angle = 0;
    introRoomIdx = (int)map.roomCount;
    map.roomCount++;
    buildIntroCell();
}

/* Active set: placements within one cell of the player. */
static const RoomPlacement *activeRooms[16];
static int activeCount;

static void updateActiveRooms(const float pos[3]) {
    int px = (int)floorf(pos[0] / ROOM_SPACING + 0.5f);
    int py = (int)floorf(pos[2] / ROOM_SPACING + 0.5f);
    activeCount = 0;
    for (uint32_t i = 0; i < map.roomCount; i++) {
        const RoomPlacement *p = &map.rooms[i];
        int dx = p->gridX - px, dy = p->gridY - py;
        int near = dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1;
        /* The intro mesh spans several cells around its placement. */
        if ((int)i == introRoomIdx) near = inIntroBounds(pos[0], pos[2]);
        if (near && activeCount < 16) {
            templateEnsure(p->templateIndex);
            if (tplRT[p->templateIndex].state == 1) {
                activeRooms[activeCount++] = p;
            }
        }
    }
}

static int inIntroBounds(float x, float z);

static const char *roomNameAt(const float pos[3]) {
    if (inIntroBounds(pos[0], pos[2])) return "cont1_173_intro";
    int px = (int)floorf(pos[0] / ROOM_SPACING + 0.5f);
    int py = (int)floorf(pos[2] / ROOM_SPACING + 0.5f);
    for (uint32_t i = 0; i < map.roomCount; i++) {
        if (map.rooms[i].gridX == px && map.rooms[i].gridY == py) {
            return tplList.items[map.rooms[i].templateIndex].name;
        }
    }
    return "(void)";
}

/* ---------------- collision across active rooms ---------------- */

static void pushWorld(float pos[3], float radius, int *pushedUp) {
    if (introCellCol && inIntroBounds(pos[0], pos[2])) {
        float local[3] = { pos[0] - INTRO_GX * ROOM_SPACING, pos[1],
                           pos[2] - INTRO_GY * ROOM_SPACING };
        int up = 0;
        if (collisionSpherePush(introCellCol, local, radius, &up)) {
            pos[0] = local[0] + INTRO_GX * ROOM_SPACING;
            pos[1] = local[1];
            pos[2] = local[2] + INTRO_GY * ROOM_SPACING;
        }
        if (up && pushedUp) *pushedUp = 1;
    }
    for (int i = 0; i < activeCount; i++) {
        const RoomPlacement *p = activeRooms[i];
        const CollisionWorld *col = tplRT[p->templateIndex].col;
        if (!col) continue;
        float local[3];
        worldToLocal(p, pos, local);
        int up = 0;
        if (collisionSpherePush(col, local, radius, &up)) {
            localToWorld(p, local, pos);
        }
        if (up && pushedUp) *pushedUp = 1;
    }
}

static int rayDownWorld(const float origin[3], float maxDist, float *hitY) {
    int hit = 0;
    float best = -1e30f;
    if (introCellCol && inIntroBounds(origin[0], origin[2])) {
        float local[3] = { origin[0] - INTRO_GX * ROOM_SPACING, origin[1],
                           origin[2] - INTRO_GY * ROOM_SPACING };
        float y;
        if (collisionRayDown(introCellCol, local, maxDist, &y)) {
            best = y;
            hit = 1;
        }
    }
    for (int i = 0; i < activeCount; i++) {
        const RoomPlacement *p = activeRooms[i];
        const CollisionWorld *col = tplRT[p->templateIndex].col;
        if (!col) continue;
        float local[3], y;
        worldToLocal(p, origin, local);
        if (collisionRayDown(col, local, maxDist, &y) && y > best) {
            best = y;
            hit = 1;
        }
    }
    if (hit) *hitY = best;
    return hit;
}

/* ---------------- player ---------------- */

static float camPos[3];
static float camYaw, camPitch;
static float velY = 0.0f;
static int walkMode = 1;
static int cullMode = 0;

/* ---- S-NAV: explored-room tracking and the equipped navigator ---- */
#define MAX_VISITED 1024
static uint8_t roomVisited[MAX_VISITED];
static int equippedNav = -1;  /* inventory template index, -1 = none */
static int navVisible = 1;

static void markRoomVisited(const float pos[3]) {
    int gx = (int)floorf(pos[0] / ROOM_SPACING + 0.5f);
    int gy = (int)floorf(pos[2] / ROOM_SPACING + 0.5f);
    for (uint32_t i = 0; i < map.roomCount && i < MAX_VISITED; i++) {
        if (map.rooms[i].gridX == gx && map.rooms[i].gridY == gy) {
            roomVisited[i] = 1;
            return;
        }
    }
}
static int fogOn = 1;

/* Vitals (approximate Main_Core.bb rates). */
static float blinkTimer = 100.0f;   /* 0..100, drains ~10s */
static float health = 100.0f;
static float damageFlash;           /* red flash on taking damage */
static float fallPeakY;             /* apex of the current fall */
static char deathCause[64] = "SCP-173";
/* Wearables and hand items (used from the inventory). */
static int wearGasMask;
static int wearNVG;                 /* 0 none, 1 normal, 2 fine */
static int wear268;                 /* SCP-268: unseen by NPCs */
static int wearVest;
static int radioChannel = -1;       /* -1 off, 0..3 = SCPRadio0-3 */
static int blinkFrames;             /* >0 while eyes closed */
static float stamina = 100.0f;      /* sprint resource */
static int staminaBlocked;
static char statusLine[256];

/* ---------------- SCP-173 state ---------------- */

static int npc173Active;
static float npc173Pos[3];      /* floor position, raw units */
static float npc173SpawnX, npc173SpawnZ;
static float npc173YawDeg;
static float npc173HeadYawDeg;  /* head offset from the body (n\Angle) */
static int npc173DragCooldown;
static int npc173WasMoving;
static float npc173EnemyX, npc173EnemyZ; /* last seen player position */
static int npc173SpotTimer;     /* horror-sting cooldown (n\LastSeen) */
static float npc173LastDist = 1e9f;
static float npc173WanderYawDeg;
static int deathTimer;          /* >0: death screen counting down */

static void reset173(void);

static void spawnItems(void);

static void spawnPlayer(void) {
    camPos[0] = map.startX * ROOM_SPACING;
    camPos[1] = 400.0f;
    camPos[2] = map.startY * ROOM_SPACING;
    camYaw = 3.14159265f; /* face into the facility (-z) */
    camPitch = 0.0f;
    velY = 0.0f;
}

static void regenerateMap(uint32_t seed) {
    memset(roomVisited, 0, sizeof(roomVisited));
    equippedNav = -1;
    navVisible = 1;
    health = 100.0f;
    damageFlash = 0.0f;
    wearGasMask = 0;
    wearNVG = 0;
    wear268 = 0;
    wearVest = 0;
    if (radioChannel >= 0) radioChannel = -1;
    for (uint32_t i = 0; i < tplList.count; i++) {
        templateUnload(&tplRT[i]);
    }
    mapFree(&map);
    doorsFree(&doors);
    mapSeed = seed;
    if (mapGenerate(&tplList, mapSeed, &map)) {
        appendIntroRoom();
        doorsGenerate(&map, &tplList, mapSeed ^ 0x9E3779B9u, &doors);
        spawnRoomDoors();
        spawnItems();
        reset173();
        snprintf(statusLine, sizeof(statusLine), "map seed %u: %u rooms %u doors",
                 mapSeed, map.roomCount, doors.count);
    } else {
        snprintf(statusLine, sizeof(statusLine), "map generation failed");
    }
    spawnPlayer();
}

/* ---------------- rendering ---------------- */

#define VTX_OFF(field) ((const void *)offsetof(SceneVertex, field))

static void setPerspective(void) {
    float zNear = 4.0f;
    float zFar = VIEW_RANGE * 3.0f;
    float fovY = 60.0f * 3.14159265f / 180.0f;
    float top = zNear * tanf(fovY * 0.5f);
    float right = top * ((float)SCREEN_W / (float)SCREEN_H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-right, right, -top, top, zNear, zFar);
}

static void applyDebugState(void) {
    if (cullMode == 0) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(cullMode == 1 ? GL_CW : GL_CCW);
    }

    if (fogOn) {
        /* Game fog: ~0.1..6 world units (Main_Core.bb), here in raw
         * units against 8-unit rooms. */
        GLfloat fogColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        glEnable(GL_FOG);
        glFogf(GL_FOG_MODE, GL_LINEAR);
        glFogfv(GL_FOG_COLOR, fogColor);
        glFogf(GL_FOG_START, 100.0f);
        glFogf(GL_FOG_END, VIEW_RANGE * 0.75f);
    } else {
        glDisable(GL_FOG);
    }
}

static void drawBatchSet(const Scene *scene, const BatchGL *gl, int alphaPass) {
    for (uint32_t i = 0; i < scene->batchCount; i++) {
        const SceneBatch *b = &scene->batches[i];
        if (b->alphaClip != alphaPass) continue;

        glBindBuffer(GL_ARRAY_BUFFER, gl[i].vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl[i].ibo);
        glVertexPointer(3, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(x));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(SceneVertex), VTX_OFF(r));

        GLuint diffuse = gl[i].diffuse;
        if (diffuse) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, diffuse);
            glTexCoordPointer(2, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(du));
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        if (alphaPass) {
            /* The game alpha-blends these surfaces with the texture's
             * alpha (glass is ~0.13-0.7); a mask-style alpha test only
             * drops fully transparent texels. */
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GREATER, 0.01f);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        glDrawElements(GL_TRIANGLES, (GLsizei)b->indexCount,
                       GL_UNSIGNED_SHORT, NULL);
        if (alphaPass) {
            glDisable(GL_ALPHA_TEST);
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }

        GLuint lightmap = gl[i].lightmap;
        if (lightmap && !alphaPass) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, lightmap);
            glTexCoordPointer(2, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(lu));
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glDrawElements(GL_TRIANGLES, (GLsizei)b->indexCount,
                           GL_UNSIGNED_SHORT, NULL);
            glDisable(GL_BLEND);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void drawRoomBatches(const RoomPlacement *p, int alphaPass) {
    const TemplateRT *rt = &tplRT[p->templateIndex];
    if (rt->state != 1 || !rt->gl) return;

    glPushMatrix();
    glTranslatef(p->gridX * ROOM_SPACING, 0.0f, p->gridY * ROOM_SPACING);
    glRotatef(-(float)(p->angle * 90), 0.0f, 1.0f, 0.0f);
    drawBatchSet(rt->scene, rt->gl, alphaPass);
    glPopMatrix();
}

/* ---------------- door assets and drawing ---------------- */

typedef struct {
    Scene *scene;
    BatchGL *gl;
    float scale[3];
    int ok;
} ModelRT;

/* ---------------- sounds ---------------- */

/* Active difficulty effects, set when a game starts or loads (the
 * difficulty table itself lives with the menu code below). */
static int invSlotCap = 10;
static int npcAggressive = 0;

#define SFX_DIR DATA_ROOT "/SFX"

static int sndDoorOpen[3], sndDoorClose[3];
static int sndBigOpen[3], sndBigClose[3];
static int sndStep[8], sndRun[7];
static int sndButton[2], sndKeycardUse[2], sndDoorLock;
static int sndPick[4];
static int sndDamage[4];
static int sndAmbience;
static int sndRattle[3], sndNeckSnap[3], sndStoneDrag;
static int sndHorrorSpot[2], sndHorrorClose[5], sndDoor173;
static float stepAccum;

static void loadSounds(void) {
    char p[256];
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/Door/DoorOpen%d.ogg", i);
        sndDoorOpen[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/Door/DoorClose%d.ogg", i);
        sndDoorClose[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/Door/BigDoorOpen%d.ogg", i);
        sndBigOpen[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/Door/BigDoorClose%d.ogg", i);
        sndBigClose[i] = audioLoad(p);
    }
    for (int i = 0; i < 8; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/Step/Step%d.ogg", i);
        sndStep[i] = audioLoad(p);
    }
    for (int i = 0; i < 7; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/Step/Run%d.ogg", i);
        sndRun[i] = audioLoad(p);
    }
    for (int i = 0; i < 2; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/Interact/Button%d.ogg", i);
        sndButton[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/Interact/KeycardUse%d.ogg", i);
        sndKeycardUse[i] = audioLoad(p);
    }
    sndDoorLock = audioLoad(SFX_DIR "/Interact/DoorLock.ogg");
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/Character/D9341/Damage%d.ogg", i);
        sndDamage[i] = audioLoad(p);
    }
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/Interact/PickItem%d.ogg", i);
        sndPick[i] = audioLoad(p);
    }
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/SCP/173/Rattle%d.ogg", i);
        sndRattle[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/SCP/173/NeckSnap%d.ogg", i);
        sndNeckSnap[i] = audioLoad(p);
    }
    sndStoneDrag = audioLoad(SFX_DIR "/SCP/173/StoneDrag.ogg");
    /* UpdateNPCType173: HorrorSFX[3..4] on spotting it, HorrorSFX
     * [1,2,9,10,12] on a very close encounter. */
    sndHorrorSpot[0] = audioLoad(SFX_DIR "/Horror/Horror3.ogg");
    sndHorrorSpot[1] = audioLoad(SFX_DIR "/Horror/Horror4.ogg");
    sndHorrorClose[0] = audioLoad(SFX_DIR "/Horror/Horror1.ogg");
    sndHorrorClose[1] = audioLoad(SFX_DIR "/Horror/Horror2.ogg");
    sndHorrorClose[2] = audioLoad(SFX_DIR "/Horror/Horror9.ogg");
    sndHorrorClose[3] = audioLoad(SFX_DIR "/Horror/Horror10.ogg");
    sndHorrorClose[4] = audioLoad(SFX_DIR "/Horror/Horror12.ogg");
    sndDoor173 = audioLoad(SFX_DIR "/Door/DoorOpen173.ogg");
    sndAmbience = audioLoad(SFX_DIR "/Ambient/Room ambience/rumble.ogg");
    audioLoopAmbience(sndAmbience, 0.30f);
}

static ModelRT doorFrameRT, doorPanelRT, heavy1RT, heavy2RT;
static ModelRT elevatorRT, big1RT, big2RT, bigFrameRT;
static ModelRT officeRT, officeFrameRT, woodenRT, woodenFrameRT;
static ModelRT oneSidedRT, door914RT;
static ModelRT buttonRT, buttonKeycardRT;
static char toastMsg[128];
static int toastTimer;

/* Build a standalone renderable from a Props b3d. Targets > 0 scale
 * the model to that extent on the axis (CreateDoor sizes the default
 * door panel to 203 x 313 x 15 raw units); 0 keeps model units. */
static void buildModelRT(ModelRT *rt, const char *name, float tw, float th,
                         float td, const char *textureOverride) {
    memset(rt, 0, sizeof(*rt));
    rt->scale[0] = rt->scale[1] = rt->scale[2] = 1.0f;
    B3DModel *model = propModelGet(name);
    if (!model) return;

    RMesh empty;
    memset(&empty, 0, sizeof(empty));
    rt->scene = sceneBuild(&empty);
    if (!rt->scene) return;
    float pos[3] = { 0, 0, 0 }, euler[3] = { 0, 0, 0 }, scl[3] = { 1, 1, 1 };
    if (!sceneAppendB3D(rt->scene, model, pos, euler, scl, textureOverride)) {
        return;
    }

    float ext[3];
    for (int i = 0; i < 3; i++) {
        ext[i] = rt->scene->boundsMax[i] - rt->scene->boundsMin[i];
    }
    if (tw > 0 && ext[0] > 0) rt->scale[0] = tw / ext[0];
    if (th > 0 && ext[1] > 0) rt->scale[1] = th / ext[1];
    if (td > 0 && ext[2] > 0) rt->scale[2] = td / ext[2];

    rt->gl = (BatchGL *)calloc(rt->scene->batchCount ? rt->scene->batchCount : 1,
                               sizeof(BatchGL));
    if (!rt->gl) return;
    for (uint32_t i = 0; i < rt->scene->batchCount; i++) {
        const SceneBatch *b = &rt->scene->batches[i];
        rt->gl[i].diffuse = textureGet(b->diffuseName);
        rt->gl[i].lightmap = 0;
        glGenBuffers(1, &rt->gl[i].vbo);
        glBindBuffer(GL_ARRAY_BUFFER, rt->gl[i].vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(b->vertexCount * sizeof(SceneVertex)),
                     b->vertices, GL_STATIC_DRAW);
        glGenBuffers(1, &rt->gl[i].ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rt->gl[i].ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(b->indexCount * sizeof(uint16_t)),
                     b->indices, GL_STATIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    rt->ok = 1;
}

static void buildDoorAssets(void) {
    buildModelRT(&doorPanelRT, "Door01.b3d", 203.0f, 313.0f, 15.0f, NULL);
    buildModelRT(&heavy1RT, "HeavyDoor1.b3d", 0, 0, 0, NULL);
    buildModelRT(&heavy2RT, "HeavyDoor2.b3d", 0, 0, 0, NULL);
    buildModelRT(&doorFrameRT, "DoorFrame.b3d", 0, 0, 0, NULL);
    /* The remaining door types of Loading_Core.bb (scales follow
     * CreateDoor: one-sided/914 use the 203x313x15 fit, elevator and
     * office are RoomScale, big is 55x, wooden 46/44/46). */
    buildModelRT(&oneSidedRT, "Door02.b3d", 203.0f, 313.0f, 15.0f, NULL);
    buildModelRT(&door914RT, "Door02.b3d", 203.0f, 313.0f, 15.0f,
                 "Door01_914.png");
    buildModelRT(&elevatorRT, "ElevatorDoor.b3d", 0, 0, 0, NULL);
    buildModelRT(&big1RT, "contdoorleft.b3d", 0, 0, 0, NULL);
    big1RT.scale[0] = big1RT.scale[1] = big1RT.scale[2] = 55.0f;
    buildModelRT(&big2RT, "contdoorright.b3d", 0, 0, 0, NULL);
    big2RT.scale[0] = big2RT.scale[1] = big2RT.scale[2] = 55.0f;
    buildModelRT(&bigFrameRT, "ContDoorFrame.b3d", 0, 0, 0, NULL);
    bigFrameRT.scale[0] = bigFrameRT.scale[1] = bigFrameRT.scale[2] = 55.0f;
    buildModelRT(&officeRT, "officedoor.b3d", 0, 0, 0, NULL);
    buildModelRT(&officeFrameRT, "officedoorframe.b3d", 0, 0, 0, NULL);
    buildModelRT(&woodenRT, "DoorWooden.b3d", 0, 0, 0, NULL);
    woodenRT.scale[0] = 46.0f;
    woodenRT.scale[1] = 44.0f;
    woodenRT.scale[2] = 46.0f;
    buildModelRT(&woodenFrameRT, "DoorWoodenFrame.b3d", 0, 0, 0, NULL);
    /* CreateButton scales to 0.03 world units = 7.68 raw. */
    buildModelRT(&buttonRT, "Button.b3d", 0, 0, 0, NULL);
    buttonRT.scale[0] = buttonRT.scale[1] = buttonRT.scale[2] = 7.68f;
    buildModelRT(&buttonKeycardRT, "ButtonKeycard.b3d", 0, 0, 0, NULL);
    buttonKeycardRT.scale[0] = buttonKeycardRT.scale[1] =
        buttonKeycardRT.scale[2] = 7.68f;
}

static void drawModelRT(const ModelRT *rt);

/* ---------------- SCP-173 behavior ---------------- */

static ModelRT npc173RT, npc173HeadRT;
static ModelRT introGuardRT, introClassDRT, introScientistRT,
               introFranklinRT;
/* Skinned skeletons, shared per model; each figure evals its own
 * frame right before drawing. */
static SkinnedMesh *skinGuard, *skinClassD;
static float skinGuardScale = 1.0f, skinClassDScale = 1.0f;

static void buildHumanRT(ModelRT *rt, const char *model, const char *tex) {
    buildModelRT(rt, model, 0, 0, 0, tex);
    if (!rt->ok || !rt->scene) return;
    /* Uniform scale to human height, a touch under the 313-unit
     * doorway. */
    float h = rt->scene->boundsMax[1] - rt->scene->boundsMin[1];
    if (h > 0.0f) {
        float k = 285.0f / h;
        rt->scale[0] = rt->scale[1] = rt->scale[2] = k;
    }
}
static float npc173YOff; /* lifts the model so its base sits on the floor */

static void buildNpcAssets(void) {
    buildHumanRT(&introGuardRT, "guard.b3d", NULL);
    buildHumanRT(&introClassDRT, "class_d.b3d", NULL);
    {
        B3DModel *gm = propModelGet("guard.b3d");
        B3DModel *dm = propModelGet("class_d.b3d");
        skinGuard = gm ? skinnedCreate(gm) : NULL;
        skinClassD = dm ? skinnedCreate(dm) : NULL;
        float mn[3], mx[3];
        if (skinGuard) {
            skinnedBounds(skinGuard, mn, mx);
            if (mx[1] > mn[1]) skinGuardScale = 285.0f / (mx[1] - mn[1]);
        }
        if (skinClassD) {
            skinnedBounds(skinClassD, mn, mx);
            if (mx[1] > mn[1]) skinClassDScale = 285.0f / (mx[1] - mn[1]);
        }
    }
    buildHumanRT(&introScientistRT, "class_d.b3d", "scientist.png");
    buildHumanRT(&introFranklinRT, "class_d.b3d", "Franklin.png");
    buildModelRT(&npc173RT, "scp_173_body.b3d", 0, 0, 0, NULL);
    buildModelRT(&npc173HeadRT, "scp_173_head.b3d", 0, 0, 0, NULL);
    if (npc173RT.scene) {
        /* NPCs.ini: Scale = 0.3 world units of BODY mesh depth; the
         * head shares the body's factor (NPCs_Core.bb 240-244). */
        float extZ = npc173RT.scene->boundsMax[2] - npc173RT.scene->boundsMin[2];
        float k = extZ > 0.0f ? (0.3f * 256.0f) / extZ : 1.0f;
        npc173RT.scale[0] = npc173RT.scale[1] = npc173RT.scale[2] = k;
        npc173HeadRT.scale[0] = npc173HeadRT.scale[1] =
            npc173HeadRT.scale[2] = k;
        npc173YOff = -npc173RT.scene->boundsMin[1] * k;
    }
}

static void reset173(void) {
    npc173Active = 0;
    for (uint32_t i = 0; i < map.roomCount; i++) {
        const char *nm = tplList.items[map.rooms[i].templateIndex].name;
        if (strcmp(nm, "cont1_173") == 0) {
            npc173SpawnX = map.rooms[i].gridX * ROOM_SPACING;
            npc173SpawnZ = map.rooms[i].gridY * ROOM_SPACING;
            npc173Pos[0] = npc173SpawnX;
            npc173Pos[1] = 0.0f;
            npc173Pos[2] = npc173SpawnZ;
            npc173YawDeg = 0.0f;
            npc173Active = npc173RT.ok;
            break;
        }
    }
    npc173WasMoving = 0;
    npc173DragCooldown = 0;
    npc173EnemyX = npc173EnemyZ = 0.0f;
    npc173HeadYawDeg = 0.0f;
    npc173SpotTimer = 0;
    npc173LastDist = 1e9f;
    npc173WanderYawDeg = 0.0f;
    deathTimer = 0;
}

/* PlayerSees173 (NPCs_Core.bb 1166): frustum + blink only, no wall
 * occlusion. Approximated as a horizontal view cone. */
static int playerSees173(void) {
    if (blinkFrames > 0) return 0;
    float dx = npc173Pos[0] - camPos[0];
    float dz = npc173Pos[2] - camPos[2];
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist < 60.0f) return 1;
    float fwdX = sinf(camYaw), fwdZ = -cosf(camYaw);
    float dot = (dx * fwdX + dz * fwdZ) / dist;
    return dot > 0.55f; /* ~57-degree half-angle (bbox partially on screen) */
}

static int roomExistsAt(float x, float z);

/* TeleportCloser: when 173 has fallen far behind, jump it to a room
 * near the player (out of the view cone) so the hunt continues. */
static void teleport173Closer(void) {
    int best = -1, count = 0;
    float fwdX = sinf(camYaw), fwdZ = -cosf(camYaw);
    for (uint32_t i = 0; i < map.roomCount; i++) {
        float rx = map.rooms[i].gridX * ROOM_SPACING;
        float rz = map.rooms[i].gridY * ROOM_SPACING;
        float dx = rx - camPos[0], dz = rz - camPos[2];
        float d = sqrtf(dx * dx + dz * dz);
        if (d < ROOM_SPACING * 1.2f || d > ROOM_SPACING * 2.6f) continue;
        /* Never pop in on screen. */
        if ((dx * fwdX + dz * fwdZ) / d > 0.35f) continue;
        count++;
        if (rand() % count == 0) best = (int)i;
    }
    if (best < 0) return;
    float ox = npc173Pos[0], oy = npc173Pos[1], oz = npc173Pos[2];
    npc173Pos[0] = map.rooms[best].gridX * ROOM_SPACING;
    npc173Pos[2] = map.rooms[best].gridY * ROOM_SPACING;
    /* Target rooms are usually unloaded, so there may be no collision
     * to ray against; park it at the player's floor level and let the
     * floor snap correct it once the room loads. */
    float origin[3] = { npc173Pos[0], camPos[1] + 200.0f, npc173Pos[2] };
    float hitY;
    if (rayDownWorld(origin, 3000.0f, &hitY)) {
        npc173Pos[1] = hitY;
    } else {
        npc173Pos[1] = camPos[1] - EYE_HEIGHT;
    }
    if (!roomExistsAt(npc173Pos[0], npc173Pos[2])) {
        npc173Pos[0] = ox;
        npc173Pos[1] = oy;
        npc173Pos[2] = oz;
        return;
    }
    npc173EnemyX = npc173EnemyZ = 0.0f;
}

/* 173 opens unlocked keycard-free doors it is standing next to
 * (UpdateNPCType173's door pass). */
static void try173OpenDoor(void) {
    for (uint32_t i = 0; i < doors.count; i++) {
        Door *d = &doors.items[i];
        if (d->open || d->locked || d->keycard > 0) continue;
        if (d->openState > 0.0f) continue; /* mid-animation */
        if (fabsf(d->x - npc173Pos[0]) > 300.0f
            || fabsf(d->z - npc173Pos[2]) > 300.0f) {
            continue;
        }
        d->open = 1;
        float dpos[3] = { d->x, camPos[1], d->z };
        audioPlay3D(sndDoor173, dpos, camPos, camYaw, 2500.0f);
        return;
    }
}

/* There is only world (and collision) where a room exists; the grid
 * gaps between rooms are void. */
static int roomExistsAt(float x, float z) {
    if (inIntroBounds(x, z)) return 1;
    int gx = (int)floorf(x / ROOM_SPACING + 0.5f);
    int gy = (int)floorf(z / ROOM_SPACING + 0.5f);
    for (uint32_t i = 0; i < map.roomCount; i++) {
        if (map.rooms[i].gridX == gx && map.rooms[i].gridY == gy) return 1;
    }
    return 0;
}

/* Room collision only exists while a room is loaded (the 3x3 grid
 * cells around the player's cell); 173 must not walk where nothing
 * can stop it. */
static int npc173OnLoadedGround(void) {
    int pgx = (int)floorf(camPos[0] / ROOM_SPACING + 0.5f);
    int pgy = (int)floorf(camPos[2] / ROOM_SPACING + 0.5f);
    int ngx = (int)floorf(npc173Pos[0] / ROOM_SPACING + 0.5f);
    int ngy = (int)floorf(npc173Pos[2] / ROOM_SPACING + 0.5f);
    return abs(ngx - pgx) <= 1 && abs(ngy - pgy) <= 1
        && roomExistsAt(npc173Pos[0], npc173Pos[2]);
}

/* Substepped move so 173 cannot tunnel through walls or closed
 * doors; returns how far it actually got. A substep that would leave
 * the map (into a grid cell with no room) is undone. */
static float move173(float dirX, float dirZ, float speed) {
    float sx0 = npc173Pos[0], sz0 = npc173Pos[2];
    int steps = (int)(speed / 25.0f) + 1;
    for (int i = 0; i < steps; i++) {
        float px = npc173Pos[0], pz = npc173Pos[2];
        npc173Pos[0] += dirX * (speed / steps);
        npc173Pos[2] += dirZ * (speed / steps);
        float body[3] = { npc173Pos[0], npc173Pos[1] + 110.0f, npc173Pos[2] };
        pushWorld(body, 48.0f, NULL);
        doorsCollide(&doors, body, 48.0f);
        npc173Pos[0] = body[0];
        npc173Pos[2] = body[2];
        if (!roomExistsAt(npc173Pos[0], npc173Pos[2])) {
            npc173Pos[0] = px;
            npc173Pos[2] = pz;
            break;
        }
    }
    float mx = npc173Pos[0] - sx0, mz = npc173Pos[2] - sz0;
    return sqrtf(mx * mx + mz * mz);
}

static float normDeg(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

/* UpdateNPCType173 (NPCs_AI_Core.bb), distances in raw units
 * (1 world unit = 256): freezes while watched, hunts a player it can
 * reach within 10u, remembers the last seen position, wanders and
 * teleports closer when far, opens doors, snaps necks at 0.65u. */
static void update173(void) {
    if (!npc173Active || deathTimer > 0 || !walkMode) return;
    float dx = camPos[0] - npc173Pos[0];
    float dz = camPos[2] - npc173Pos[2];
    float dist = sqrtf(dx * dx + dz * dz);

    /* Vertical separation: the statue kills and hunts only on its own
     * level. camPos is the eye; the player's feet are ~EYE_HEIGHT
     * below, and npc173Pos[1] is the statue's floor. */
    float feetDy = fabsf((camPos[1] - EYE_HEIGHT) - npc173Pos[1]);
    int sameLevel = feetDy < 220.0f;

    /* "Temp" in the original: the player is a reachable target
     * (EntityVisible approximated by range + same floor). SCP-268
     * makes the wearer unnoticeable (I_268\InvisibilityOn). */
    int hasTarget = dist < 2560.0f && sameLevel && !wear268;
    int seen = playerSees173();
    int move = !(seen && dist < 3840.0f);
    int moving = 0;
    float yawToPlayer = atan2f(dx, dz) * 180.0f / 3.14159265f;

    if (npc173SpotTimer > 0) npc173SpotTimer--;

    if (!move) {
        /* Watched: hold still, stare back. */
        npc173HeadYawDeg = normDeg(yawToPlayer - npc173YawDeg);
        /* Horror sting when spotted up close after a quiet minute. */
        if (dist < 896.0f && hasTarget && npc173SpotTimer == 0) {
            audioPlay(sndHorrorSpot[rand() % 2], 0.9f, 0.0f);
            npc173SpotTimer = 3600;
        }
        if (dist < 384.0f) {
            if (rand() % 700 == 0) {
                audioPlay3D(sndRattle[rand() % 3], npc173Pos, camPos, camYaw,
                            2200.0f);
            }
            /* First moment it gets this close: a hard sting. */
            if (npc173LastDist > 512.0f && hasTarget) {
                audioPlay(sndHorrorClose[rand() % 5], 1.0f, 0.0f);
            }
        }
        npc173LastDist = dist;
    } else if (dist > 3482.0f) {
        /* Beyond ~0.8x the hide distance: hop between rooms toward
         * the player instead of pathing the whole facility. */
        if (rand() % 70 == 0) teleport173Closer();
        npc173LastDist = dist;
    } else if (!npc173OnLoadedGround()) {
        /* Its room is not loaded (or it ended up off-grid): there is
         * no collision there, so it must not walk. Hop instead. */
        if (rand() % 70 == 0) teleport173Closer();
        npc173LastDist = dist;
    } else {
        /* Tries to open doors in its way. */
        if (rand() % (npcAggressive ? 10 : 20) == 0) try173OpenDoor();

        if (hasTarget) {
            npc173EnemyX = camPos[0];
            npc173EnemyZ = camPos[2];
            npc173HeadYawDeg = 0.0f; /* body faces the player */
            if (dist < 166.0f) {
                /* Kill: neck snap, camera wrenched around. */
                snprintf(deathCause, sizeof(deathCause), "SCP-173");
                audioPlay(sndNeckSnap[rand() % 3], 1.0f, 0.0f);
                camYaw += ((rand() % 2) ? 1.0f : -1.0f)
                        * (80.0f + (float)(rand() % 21))
                        * 3.14159265f / 180.0f;
                deathTimer = 180;
                return;
            }
            float speed = 97.0f; /* NPCs.ini Speed 38 */
            if (speed > dist - 80.0f) speed = dist - 80.0f;
            if (speed > 0.0f) {
                move173(dx / dist, dz / dist, speed);
                npc173YawDeg = yawToPlayer;
                moving = 1;
            }
        } else if (npc173EnemyX != 0.0f || npc173EnemyZ != 0.0f) {
            /* Move to where the player was last seen. */
            float ex = npc173EnemyX - npc173Pos[0];
            float ez = npc173EnemyZ - npc173Pos[2];
            float ed = sqrtf(ex * ex + ez * ez);
            if (ed > 128.0f && rand() % 500 != 0) {
                move173(ex / ed, ez / ed, 97.0f);
                npc173YawDeg = atan2f(ex, ez) * 180.0f / 3.14159265f;
                npc173HeadYawDeg = normDeg(yawToPlayer - npc173YawDeg);
                moving = 1;
            } else {
                npc173EnemyX = npc173EnemyZ = 0.0f;
            }
        } else {
            /* Wander: drift forward, sometimes picking a new heading. */
            if (rand() % 400 == 0) {
                npc173WanderYawDeg = (float)(rand() % 360);
                npc173HeadYawDeg = (float)(rand() % 241) - 120.0f;
            }
            float wy = npc173WanderYawDeg * 3.14159265f / 180.0f;
            float got = move173(sinf(wy), cosf(wy), 97.0f * 0.5f);
            if (got < 10.0f) { /* stuck on a wall: turn */
                npc173WanderYawDeg = (float)(rand() % 360);
            } else {
                npc173YawDeg = npc173WanderYawDeg;
                moving = 1;
            }
        }
        npc173LastDist = dist;
    }

    if (moving) {
        /* Keep it on the floor. */
        float origin[3] = { npc173Pos[0], npc173Pos[1] + 250.0f,
                            npc173Pos[2] };
        float hitY;
        if (rayDownWorld(origin, 600.0f, &hitY)) {
            npc173Pos[1] = hitY;
        }
    }

    /* Stone drag while moving; a rattle when it halts nearby. */
    if (moving) {
        if (npc173DragCooldown-- <= 0) {
            audioPlay3D(sndStoneDrag, npc173Pos, camPos, camYaw, 2200.0f);
            npc173DragCooldown = 40;
        }
    } else if (npc173WasMoving && dist < 1200.0f) {
        audioPlay3D(sndRattle[rand() % 3], npc173Pos, camPos, camYaw, 2200.0f);
    }
    npc173WasMoving = moving;
}

/* ---- intro sequence logic ----
 * Simplified port of the e_cont1_173_intro event: wake in the Class-D
 * cell, get escorted over the PA, enter 173's chamber, containment
 * breach, escape into the facility. No NPC actors; the guards exist
 * as voice lines. */

static void gameMusicStart(void);
static void introPlaceHumans(void);
static int itemTplFind(const char *name);

static int sndIntroAttention = -1, sndIntroExitCell = -1;
static int sndIntroEscort = -1, sndIntroDone = -1;
static int sndIntroOff = -1, sndIntroVent = -1, sndIntroHorror = -1;
static int sndIntroBreach = -1;
static int sndEscortRefuse = -1, sndEscortPissed = -1, sndEscortKill = -1;
static int sndGunshot[2] = { -1, -1 };
static int sndEw[2] = { -1, -1 };
static int sndSee173 = -1, sndWhatThe = -1, sndGasp = -1;
static int sndBang[3] = { -1, -1, -1 };
static int sndCommotion[2] = { -1, -1 };
static int sndScripted = -1;

/* Wake-up cinematic: -1 = "PRESS ANY KEY TO WAKE UP" screen,
 * 0..900 = eyes-opening camera sequence, >900 = done. */
static int introCineT = -1;
static int introDark;   /* chamber lights cut before the breach */

typedef struct {
    ModelRT *rt;         /* static fallback */
    float x, y, z;       /* intro-room local, raw units */
    float yawDeg;
    SkinnedMesh *skin;   /* NULL = draw the static model */
    float skinScale;
    const char *texOverride;
    float animStart, animEnd, animSpeed; /* frames, frames/game-tick */
    float frame;
    GLuint vbo;          /* posed vertices live on the GPU; uploaded
                            only when the figure re-skins */
    int posed;           /* vbo holds a valid pose */
} IntroHuman;

/* Element buffers are per skeleton (indices never change), shared by
 * every figure using that model. */
typedef struct {
    const SkinnedMesh *skin;
    GLuint ibos[8];
    int built;
} SkinIBOCache;
static SkinIBOCache skinIBO[2];

static GLuint *skinIBOsFor(SkinnedMesh *skin) {
    for (int i = 0; i < 2; i++) {
        if (skinIBO[i].skin == skin && skinIBO[i].built) {
            return skinIBO[i].ibos;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (!skinIBO[i].built) {
            skinIBO[i].skin = skin;
            uint32_t nb = skinnedBatchCount(skin);
            if (nb > 8) nb = 8;
            for (uint32_t b = 0; b < nb; b++) {
                const SkinBatch *batch = skinnedBatch(skin, b);
                glGenBuffers(1, &skinIBO[i].ibos[b]);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, skinIBO[i].ibos[b]);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                             (GLsizeiptr)(batch->indexCount
                                          * sizeof(uint16_t)),
                             batch->indices, GL_STATIC_DRAW);
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            skinIBO[i].built = 1;
            return skinIBO[i].ibos;
        }
    }
    return NULL;
}

static IntroHuman INTRO_HUMANS[8];
static int introHumanCount;

/* The escort route from the cell block to the chamber gate, BFS'd
 * over the intro mesh floors (room-local raw units). */
static const float ESCORT_WP[][2] = {
    { -4300.0f, 856.0f },  { -7976.0f, 856.0f },  { -7976.0f, 280.0f },
    { -7912.0f, 280.0f },  { -7912.0f, -872.0f }, { -7720.0f, -872.0f },
    { -7720.0f, -1064.0f },{ -6568.0f, -1064.0f },{ -6568.0f, -1192.0f },
    { -1064.0f, -1192.0f },{ -1064.0f, -936.0f }, { -808.0f, -936.0f },
    { -808.0f, -744.0f },  { 280.0f, -744.0f },   { 280.0f, 344.0f },
};
#define ESCORT_WP_COUNT (int)(sizeof(ESCORT_WP) / sizeof(ESCORT_WP[0]))
static int escortWp;            /* current waypoint target */
static int escortWalking;
static int escortWarn;          /* 0 fine, 1 warned, 2 final, 3 firing */
static int escortTimer;
static int escortShotTick;

static void introLocal(float out[2]) {
    out[0] = camPos[0] - INTRO_GX * ROOM_SPACING;
    out[1] = camPos[2] - INTRO_GY * ROOM_SPACING;
}

static void introStart(void) {
    /* Voice lines load lazily (they cache in the sound table). */
    sndIntroAttention = audioLoad(SFX_DIR "/Room/Intro/IA/1/Attention0.ogg");
    sndIntroExitCell = audioLoad(SFX_DIR
                                 "/Room/Intro/Guard/Ulgrin/ExitCell.ogg");
    sndIntroEscort = audioLoad(SFX_DIR
                               "/Room/Intro/Guard/Ulgrin/Escort0.ogg");
    sndIntroDone = audioLoad(SFX_DIR
                             "/Room/Intro/Guard/Ulgrin/EscortDone0.ogg");
    sndIntroOff = audioLoad(SFX_DIR "/Room/Intro/IA/Off.ogg");
    sndIntroVent = audioLoad(SFX_DIR "/Room/Intro/173Vent.ogg");
    sndIntroHorror = audioLoad(SFX_DIR "/Room/Intro/Horror.ogg");
    sndIntroBreach = audioLoad(SFX_DIR
                               "/Room/Intro/IA/Scripted/Announcement0.ogg");
    sndEscortRefuse = audioLoad(SFX_DIR
                                "/Room/Intro/Guard/Ulgrin/EscortRefuse0.ogg");
    sndEscortPissed = audioLoad(
        SFX_DIR "/Room/Intro/Guard/Ulgrin/EscortPissedOff0.ogg");
    sndEscortKill = audioLoad(SFX_DIR
                              "/Room/Intro/Guard/Ulgrin/EscortKill0.ogg");
    sndGunshot[0] = audioLoad(SFX_DIR "/Character/Gunshot0.ogg");
    sndGunshot[1] = audioLoad(SFX_DIR "/Character/Gunshot1.ogg");
    sndEw[0] = audioLoad(SFX_DIR "/Room/Intro/Ew0.ogg");
    sndEw[1] = audioLoad(SFX_DIR "/Room/Intro/Ew1.ogg");
    sndSee173 = audioLoad(SFX_DIR "/Room/Intro/See173.ogg");
    sndWhatThe = audioLoad(SFX_DIR "/Room/Intro/WhatThe0a.ogg");
    sndGasp = audioLoad(SFX_DIR "/Room/Intro/ClassD/Gasp.ogg");
    for (int i = 0; i < 3; i++) {
        char bp[256];
        snprintf(bp, sizeof(bp), SFX_DIR "/Room/Intro/Bang%d.ogg", i);
        sndBang[i] = audioLoad(bp);
    }
    {
        char cp[256];
        int c = rand() % 14;
        snprintf(cp, sizeof(cp),
                 SFX_DIR "/Room/Intro/Commotion/Commotion%d.ogg", c);
        sndCommotion[0] = audioLoad(cp);
        snprintf(cp, sizeof(cp),
                 SFX_DIR "/Room/Intro/Commotion/Commotion%d.ogg",
                 (c + 7) % 14);
        sndCommotion[1] = audioLoad(cp);
        /* The chamber PA line is randomized like the game's
         * LoadEventSound(Announcement + Rand(0,6)). */
        snprintf(cp, sizeof(cp),
                 SFX_DIR "/Room/Intro/IA/Scripted/Announcement%d.ogg",
                 1 + rand() % 6);
        sndScripted = audioLoad(cp);
    }
    introCineT = -1;
    introDark = 0;
    escortWp = 0;
    escortWalking = 0;
    escortWarn = 0;
    escortTimer = 0;
    escortShotTick = 0;

    /* The chamber gate (BIG door at local 576,383). */
    introGateDoor = -1;
    for (uint32_t i = 0; i < doors.count; i++) {
        if (doors.items[i].type == 3
            && inIntroBounds(doors.items[i].x, doors.items[i].z)) {
            introGateDoor = (int)i;
            break;
        }
    }

    /* Wake up in the player's cell: its interior overlay mesh spans
     * local x -4320..-3872, z -128..512, floor y=0, with the cell
     * door at z=512 opening north onto the block corridor. */
    camPos[0] = INTRO_GX * ROOM_SPACING - 4096.0f;
    camPos[1] = EYE_HEIGHT;
    camPos[2] = INTRO_GY * ROOM_SPACING + 150.0f;
    camYaw = 3.14159265f; /* face the cell door (north) */
    camPitch = 0.0f;
    velY = 0.0f;
    npc173Active = 0; /* nothing hunts until the breach */
    introPlaceHumans();
    introPhase = 0;
    introTimer = 0;
    audioStreamMusic(DATA_ROOT "/SFX/Music/173IntroChamber.ogg", 0.5f, 1);
    snprintf(toastMsg, sizeof(toastMsg), "...");
    toastTimer = 120;
}

static void introEnd(const char *msg) {
    introPhase = -1;
    npc173Active = npc173RT.ok;
    spawnPlayer();
    gameMusicStart();
    snprintf(toastMsg, sizeof(toastMsg), "%s", msg);
    toastTimer = 240;
}

static void introUpdate(void) {
    if (introPhase < 0) return;
    introTimer++;
    float l[2];
    introLocal(l);

    /* Ulgrin leads the way: he walks the route while the player keeps
     * up, waits when they lag, and the guards open fire if they
     * refuse to follow. */
    IntroHuman *ulgrin =
        introHumanCount > 0 && INTRO_HUMANS[0].rt == &introGuardRT
            ? &INTRO_HUMANS[0]
            : NULL;
    if (ulgrin && introPhase >= 1 && introPhase <= 2) {
        float pdx = camPos[0] - (INTRO_GX * ROOM_SPACING + ulgrin->x);
        float pdz = camPos[2] - (INTRO_GY * ROOM_SPACING + ulgrin->z);
        float pdist = sqrtf(pdx * pdx + pdz * pdz);

        int walking = 0;
        if (escortWp < ESCORT_WP_COUNT && pdist < 1400.0f) {
            float tx = ESCORT_WP[escortWp][0], tz = ESCORT_WP[escortWp][1];
            float dx = tx - ulgrin->x, dz = tz - ulgrin->z;
            float d = sqrtf(dx * dx + dz * dz);
            if (d < 48.0f) {
                escortWp++;
            } else {
                float sp = 4.2f;
                ulgrin->x += dx / d * sp;
                ulgrin->z += dz / d * sp;
                ulgrin->yawDeg = -atan2f(dx, dz) * 180.0f / 3.14159265f;
                walking = 1;
                /* Keep him on the floor. */
                float o[3] = { INTRO_GX * ROOM_SPACING + ulgrin->x, 200.0f,
                               INTRO_GY * ROOM_SPACING + ulgrin->z };
                float hy;
                if (rayDownWorld(o, 600.0f, &hy)) ulgrin->y = hy;
            }
        }
        if (walking != escortWalking) {
            escortWalking = walking;
            /* Guard walk cycle is frames 1-38 at CurrSpeed*40
             * (NPCs_AI state 3/5/10); 4.2 raw/tick = 0.66 f/tick.
             * Idle 77-201, sped up a touch so it reads on screen. */
            ulgrin->animStart = walking ? 1.0f : 77.0f;
            ulgrin->animEnd = walking ? 38.0f : 201.0f;
            ulgrin->animSpeed = walking ? 0.66f : 0.4f;
            ulgrin->frame = ulgrin->animStart;
        }
        if (!walking && escortWp < ESCORT_WP_COUNT && pdist >= 1400.0f) {
            /* Waiting on a straggler: face the player. */
            ulgrin->yawDeg = -atan2f(pdx, pdz) * 180.0f / 3.14159265f;
        }
        if (escortWp >= ESCORT_WP_COUNT && introGateDoor >= 0
            && !doors.items[introGateDoor].open && introPhase == 1) {
            doors.items[introGateDoor].open = 1;
        }

        /* Refusing to follow gets you shot (EscortRefuse ->
         * EscortPissedOff -> gunfire). */
        if (pdist > 1700.0f) {
            escortTimer++;
            if (escortTimer == 240 && escortWarn == 0) {
                escortWarn = 1;
                audioPlay(sndEscortRefuse, 1.0f, 0.0f);
                snprintf(toastMsg, sizeof(toastMsg),
                         "FOLLOW THE ESCORT");
                toastTimer = 180;
            } else if (escortTimer == 480 && escortWarn == 1) {
                escortWarn = 2;
                audioPlay(sndEscortPissed, 1.0f, 0.0f);
                snprintf(toastMsg, sizeof(toastMsg),
                         "LAST WARNING - FOLLOW THE ESCORT");
                toastTimer = 180;
            } else if (escortTimer > 640) {
                if (escortWarn == 2) {
                    escortWarn = 3;
                    audioPlay(sndEscortKill, 1.0f, 0.0f);
                }
                if (++escortShotTick >= 40 && deathTimer == 0) {
                    escortShotTick = 0;
                    audioPlay(sndGunshot[rand() % 2], 1.0f, 0.0f);
                    health -= 34.0f;
                    damageFlash = 0.7f;
                    audioPlay(sndDamage[rand() % 4], 1.0f, 0.0f);
                    if (health <= 0.0f) {
                        health = 0.0f;
                        snprintf(deathCause, sizeof(deathCause),
                                 "THE GUARDS");
                        deathTimer = 180;
                    }
                }
            }
        } else if (pdist < 1200.0f) {
            escortWarn = 0;
            escortTimer = 0;
            escortShotTick = 0;
        }
    }

    /* Doors along the escort route slide open as the player or the
     * escort nears (they are locked, so buttons cannot derail the
     * sequence). */
    if (introPhase >= 1) {
        for (uint32_t i = 0; i < doors.count; i++) {
            Door *d = &doors.items[i];
            if ((int)i == introGateDoor || d->open) continue;
            if (!inIntroBounds(d->x, d->z)) continue;
            float dx = d->x - camPos[0], dz = d->z - camPos[2];
            float near2 = 400.0f * 400.0f;
            int nearDoor = dx * dx + dz * dz < near2;
            if (!nearDoor && ulgrin) {
                float ux = INTRO_GX * ROOM_SPACING + ulgrin->x - d->x;
                float uz = INTRO_GY * ROOM_SPACING + ulgrin->z - d->z;
                nearDoor = ux * ux + uz * uz < near2;
            }
            if (nearDoor) {
                d->open = 1;
                float dpos[3] = { d->x, camPos[1], d->z };
                audioPlay3D(sndDoorOpen[rand() % 3], dpos, camPos, camYaw,
                            2500.0f);
            }
        }
    }

    switch (introPhase) {
        case 0: /* wake up, then the PA rouses the block */
            if (introCineT < 0) {
                /* Black screen until any button; the cinematic then
                 * runs the original 15-second timeline. */
                introTimer = 0;
                if (inputHit(ACTION_INTERACT) || inputHit(ACTION_USE_ITEM)
                    || inputHit(ACTION_CROUCH) || inputHit(ACTION_INVENTORY)
                    || inputHit(ACTION_MENU)) {
                    introCineT = 0;
                }
                break;
            }
            if (introCineT <= 900) {
                /* Eyes opening on the bunk: pitch eases from -70 to
                 * level with a sinus wobble while the player sits up
                 * and stands (UpdateIntro's camera: smoothstepped
                 * Dist/Dist2, camera rising from the bunk to the
                 * standing eye at local -4130, 72). */
                float t = introCineT / 60.0f; /* seconds */
                float rise = t / 5.0f;
                if (rise > 1.0f) rise = 1.0f;
                rise = rise * rise * (3.0f - 2.0f * rise);
                float turn = (t - 10.0f) / 4.0f;
                if (turn < 0.0f) turn = 0.0f;
                if (turn > 1.0f) turn = 1.0f;
                turn = turn * turn * (3.0f - 2.0f * turn);
                float ox = INTRO_GX * ROOM_SPACING;
                float oz = INTRO_GY * ROOM_SPACING;
                /* Bunk at the cell's east wall; sit up (+0.2 world),
                 * then rise to the 0.9-world standing eye. */
                float bedX = -4230.0f, bedZ = 250.0f, bedY = 105.0f;
                float sitY = bedY + 0.2f * 256.0f;
                float x = bedX + (-4130.0f - bedX) * turn;
                float z = bedZ + (72.0f - bedZ) * rise;
                float y = bedY + (sitY - bedY) * rise;
                y = y + ((0.302f + 0.6f) * 256.0f - y) * turn;
                camPos[0] = ox + x;
                camPos[1] = y;
                camPos[2] = oz + z;
                camPitch = ((-70.0f + 70.0f * rise)
                            + sinf(t * 12.857f) * 5.0f * (1.0f - rise))
                         * 3.14159265f / 180.0f;
                camYaw = 3.14159265f * 0.5f + 3.14159265f * 0.5f * turn;
                if (introCineT == 168) audioPlay(sndEw[0], 1.0f, 0.0f);
                if (introCineT == 648) audioPlay(sndEw[1], 1.0f, 0.0f);
                if (introCineT == 780) {
                    audioPlay(sndStep[rand() % 8], 0.5f, 0.0f);
                }
                if (inputHit(ACTION_MENU)) introCineT = 900; /* skip */
                introCineT++;
                if (introCineT > 900) {
                    introTimer = 0;
                    camPitch = 0.0f;
                    camPos[0] = INTRO_GX * ROOM_SPACING - 4130.0f;
                    camPos[1] = EYE_HEIGHT;
                    camPos[2] = INTRO_GY * ROOM_SPACING + 72.0f;
                    velY = 0.0f;
                    snprintf(toastMsg, sizeof(toastMsg),
                             "CHECK YOUR INVENTORY FOR YOUR ORIENTATION"
                             " LEAFLET");
                    toastTimer = 300;
                }
                break;
            }
            if (introTimer == 120) audioPlay(sndIntroAttention, 1.0f, 0.0f);
            if (introTimer == 480) {
                audioPlay(sndIntroExitCell, 1.0f, 0.0f);
            }
            if (introTimer == 600) {
                introPhase = 1; /* cell door opens; route unlocks */
                snprintf(toastMsg, sizeof(toastMsg),
                         "EXIT YOUR CELL AND FOLLOW THE CORRIDOR");
                toastTimer = 240;
            }
            break;
        case 1: /* escorted through the block */
            if (introTimer == 720) audioPlay(sndIntroEscort, 1.0f, 0.0f);
            if (l[0] > -1500.0f) {
                introPhase = 2;
                introTimer = 0;
                audioPlay(sndIntroDone, 1.0f, 0.0f);
                if (introGateDoor >= 0) doors.items[introGateDoor].open = 1;
                snprintf(toastMsg, sizeof(toastMsg),
                         "ENTER THE CONTAINMENT CHAMBER");
                toastTimer = 240;
            }
            break;
        case 2: /* walk into the chamber */
            if (l[0] > 1100.0f) {
                introPhase = 3;
                introTimer = 0;
                if (introGateDoor >= 0) doors.items[introGateDoor].open = 0;
            }
            break;
        case 3: /* sealed in with the statue (UpdateIntro's chamber
                 * timeline: PA orders, murmurs, lights cut, the vent,
                 * panic, pounding, breach) */
            if (introTimer == 60) audioPlay(sndScripted, 1.0f, 0.0f);
            if (introTimer == 260) audioPlay(sndCommotion[0], 0.7f, 0.0f);
            if (introTimer == 420) {
                audioPlay(sndIntroOff, 1.0f, 0.0f);
                audioStopMusic();
                introDark = 1;
            }
            if (introTimer == 520) audioPlay(sndIntroVent, 1.0f, 0.0f);
            if (introTimer == 610) {
                audioPlay(sndWhatThe, 1.0f, 0.0f);
                audioPlay(sndGasp, 0.9f, 0.0f);
            }
            if (introTimer == 700) {
                audioPlay(sndIntroHorror, 1.0f, 0.0f);
                audioPlay(sndSee173, 1.0f, 0.0f);
            }
            if (introTimer == 760) audioPlay(sndBang[0], 1.0f, 0.0f);
            if (introTimer == 820) {
                audioPlay(sndBang[1], 1.0f, 0.0f);
                audioPlay(sndCommotion[1], 0.9f, 0.0f);
            }
            if (introTimer == 880) audioPlay(sndBang[2], 1.0f, 0.0f);
            if (introTimer == 940) {
                introDark = 0;
                audioPlay(sndIntroBreach, 1.0f, 0.0f);
                /* The breach: 173 is loose in the chamber, the gate
                 * reopens, and the player runs. */
                npc173Pos[0] = INTRO_GX * ROOM_SPACING + 1760.0f;
                npc173Pos[1] = 0.0f;
                npc173Pos[2] = INTRO_GY * ROOM_SPACING + 912.0f;
                npc173Active = npc173RT.ok;
                if (introGateDoor >= 0) doors.items[introGateDoor].open = 1;
                introPhase = 4;
                introTimer = 0;
                snprintf(toastMsg, sizeof(toastMsg),
                         "SCP-173 HAS BREACHED CONTAINMENT - RUN");
                toastTimer = 300;
            }
            break;
        case 4: /* escape back down the block */
            if (l[0] < -2200.0f || introTimer > 3600) {
                introEnd("YOU ESCAPED INTO THE FACILITY");
            }
            break;
    }
}

/* The intro's people (UpdateIntro's CreateNPC calls): the escort
 * guards outside the cell, inmates, the observation-room staff.
 * Static figures - the port has no NPC animation. */
static void introPlaceHumans(void) {
    /* AnimateNPC idle loops: guard state 7 = 77..201 @0.2,
     * Class-D idle = 210..235 @0.1; the scientist sits at a fixed
     * frame like SetNPCFrame(182). */
    IntroHuman defs[8] = {
        /* Positions from UpdateIntro (block corridor floor y=0). The
         * standing yaws face the cells like the original (the draw
         * convention showed their backs before). Ulgrin waits right
         * across from the player's cell door. The guard idle (77-201)
         * is authored very subtle; it plays at 2x so the
         * breathing/shift reads on a small screen. */
        { &introGuardRT, -4130.0f, 0.0f, 830.0f, 0.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 77 },                /* Ulgrin */
        { &introGuardRT, -3985.0f, 0.0f, 786.0f, 315.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 120 },
        { &introGuardRT, -8064.0f, 0.0f, 1096.0f, 0.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 160 },               /* radio guy */
        { &introClassDRT, -3550.0f, 0.0f, 800.0f, 180.0f,
          NULL, 1, NULL, 357, 381, 0.12f, 357 },             /* inmate */
        { &introGuardRT, 328.0f, 480.0f, 1072.0f, 0.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 40 },                /* balcony */
        { &introFranklinRT, -3424.0f, -100.0f, -2208.0f, 180.0f,
          NULL, 1, "Franklin.png", 357, 381, 0.12f, 366 },
        { &introScientistRT, -3073.0f, -315.0f, -2165.0f, 225.0f,
          NULL, 1, "scientist.png", 182, 182, 0.0f, 182 },
        { &introGuardRT, -4000.0f, 0.0f, 950.0f, 160.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 90 },
    };
    for (int i = 0; i < 8; i++) {
        if (defs[i].rt == &introGuardRT) {
            defs[i].skin = skinGuard;
            defs[i].skinScale = skinGuardScale;
        } else {
            defs[i].skin = skinClassD;
            defs[i].skinScale = skinClassDScale;
        }
    }
    /* Per-slot VBOs, created once and reused (the roster is fixed, so
     * slot i always maps to the same model). */
    static GLuint slotVbo[8];
    introHumanCount = 0;
    for (int i = 0; i < 8; i++) {
        if (!defs[i].skin && !defs[i].rt->ok) continue;
        if (defs[i].skin) {
            if (!slotVbo[i]) glGenBuffers(1, &slotVbo[i]);
            defs[i].vbo = slotVbo[i];
            defs[i].posed = 0;
        }
        INTRO_HUMANS[introHumanCount++] = defs[i];
    }
}

/* Re-skin a figure and push the pose to its VBO. */
static void poseHumanVbo(IntroHuman *h) {
    skinnedEval(h->skin, h->frame);
    uint32_t vcount;
    const SceneVertex *verts = skinnedVertices(h->skin, &vcount);
    glBindBuffer(GL_ARRAY_BUFFER, h->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vcount * sizeof(SceneVertex)),
                 verts, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    h->posed = 1;
}

/* Draw a posed skeleton from its VBO (the pose is only re-uploaded
 * when the figure re-skins; client arrays went through vitaGL's slow
 * per-draw copy path and tanked the framerate). */
static void drawSkinnedHuman(IntroHuman *h) {
    GLuint *ibos = skinIBOsFor(h->skin);
    if (!ibos || !h->posed) return;
    glBindBuffer(GL_ARRAY_BUFFER, h->vbo);
    glVertexPointer(3, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(x));
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(SceneVertex), VTX_OFF(r));
    glTexCoordPointer(2, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(du));
    uint32_t nb = skinnedBatchCount(h->skin);
    if (nb > 8) nb = 8;
    for (uint32_t b = 0; b < nb; b++) {
        const SkinBatch *batch = skinnedBatch(h->skin, b);
        GLuint tex = textureGet(h->texOverride ? h->texOverride
                                               : batch->textureName);
        if (tex) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, tex);
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibos[b]);
        glDrawElements(GL_TRIANGLES, (GLsizei)batch->indexCount,
                       GL_UNSIGNED_SHORT, NULL);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

static void drawIntroHumans(const float viewPos[3]) {
    /* Only during the escort; the breach clears the block. */
    if (introPhase < 0 || introPhase >= 4) return;
    float fwdX = sinf(camYaw), fwdZ = -cosf(camYaw);
    for (int i = 0; i < introHumanCount; i++) {
        IntroHuman *h = &INTRO_HUMANS[i];
        float wx = INTRO_GX * ROOM_SPACING + h->x;
        float wz = INTRO_GY * ROOM_SPACING + h->z;
        float dx = wx - viewPos[0], dz = wz - viewPos[2];
        float d2 = dx * dx + dz * dz;
        if (d2 > 3800.0f * 3800.0f) continue;
        /* Behind the camera: no skinning, no draw. */
        if (d2 > 600.0f * 600.0f
            && (dx * fwdX + dz * fwdZ) / sqrtf(d2) < -0.25f) {
            continue;
        }

        /* Advance the idle loop; re-skin staggered at ~30 Hz and
         * upload the pose only then. */
        if (h->skin) {
            int animating = h->animSpeed > 0.0f;
            if (animating) {
                h->frame += h->animSpeed;
                if (h->frame > h->animEnd) h->frame = h->animStart;
            }
            if (!h->posed || (animating && ((introTimer + i) & 1) == 0)) {
                poseHumanVbo(h);
            }
        }

        glPushMatrix();
        glTranslatef(wx, h->y, wz);
        glRotatef(-h->yawDeg + 180.0f, 0.0f, 1.0f, 0.0f);
        if (h->skin) {
            glScalef(h->skinScale, h->skinScale, h->skinScale);
            drawSkinnedHuman(h);
        } else {
            drawModelRT(h->rt);
        }
        glPopMatrix();
    }
}

static void draw173(const float viewPos[3]) {
    if (!npc173Active || !npc173RT.ok) return;
    float dx = npc173Pos[0] - viewPos[0], dz = npc173Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    glPushMatrix();
    glTranslatef(npc173Pos[0], npc173Pos[1] + npc173YOff, npc173Pos[2]);
    /* +180: the model faces its own -z, so flip to face the player. */
    glRotatef(npc173YawDeg + 180.0f, 0.0f, 1.0f, 0.0f);
    drawModelRT(&npc173RT);
    /* The head turns toward the player independently (the game's
     * OBJ2 with n\Angle). */
    glRotatef(npc173HeadYawDeg, 0.0f, 1.0f, 0.0f);
    drawModelRT(&npc173HeadRT);
    glPopMatrix();
}

/* ---------------- items and inventory ---------------- */

/* Templates and per-room spawns generated from the Blitz sources by
 * vita/tools/extract_items.py (item_spawns.h). */
#define ITEM_TPL_COUNT \
    ((int)(sizeof(ITEM_TEMPLATES) / sizeof(ITEM_TEMPLATES[0])))
#define MAX_WORLD_ITEMS 512
#define MAX_INVENTORY 10
#define ITEM_PICKUP_REACH 250.0f

typedef struct {
    int tpl;
    float x, y, z;
    int taken;
} WorldItem;

static WorldItem worldItems[MAX_WORLD_ITEMS];
static unsigned worldItemCount;
static int inventory[MAX_INVENTORY];
static unsigned inventoryCount;
static int invOpen;
static int invSel; /* selected slot index while the inventory is open */
static int docOpen; /* reading a document full-screen */
static GLuint docTex;
static char docTexName[80];
static float docW, docH;
static ModelRT itemRT[sizeof(ITEM_TEMPLATES) / sizeof(ITEM_TEMPLATES[0])];
static float itemSpin;

static int itemTplFind(const char *name) {
    for (int i = 0; i < ITEM_TPL_COUNT; i++) {
        if (strcmp(ITEM_TEMPLATES[i].name, name) == 0) return i;
    }
    return -1;
}

/* "Level N Key Card" / "Level N key Card" grants keycard level N. */
static int itemKeycardLevel(int tpl) {
    const char *n = ITEM_TEMPLATES[tpl].name;
    if (strncmp(n, "Level ", 6) == 0
        && (strstr(n, "Key Card") || strstr(n, "key Card"))) {
        return (int)strtol(n + 6, NULL, 10);
    }
    return 0;
}

static const ModelRT *itemModel(int tpl) {
    ModelRT *rt = &itemRT[tpl];
    if (!rt->ok && !rt->scene) {
        const ItemTemplateDef *def = &ITEM_TEMPLATES[tpl];
        buildModelRT(rt, def->model, 0, 0, 0,
                     def->texture[0] ? def->texture : NULL);
        /* ScaleEntity(worldScale) on model-native units; our raw units
         * are world * 256. */
        float k = def->worldScale * 256.0f;
        rt->scale[0] = rt->scale[1] = rt->scale[2] = k;
    }
    return rt;
}

static int playerKeycard(void) {
    int level = 0;
    for (unsigned i = 0; i < inventoryCount; i++) {
        if (strcmp(ITEM_TEMPLATES[inventory[i]].name, "SCP-005") == 0) {
            return 6; /* the skeleton key opens everything */
        }
        int l = itemKeycardLevel(inventory[i]);
        if (l > level) level = l;
    }
    return level;
}

static void worldItemAdd(int tpl, float x, float y, float z) {
    if (tpl < 0 || worldItemCount >= MAX_WORLD_ITEMS) return;
    WorldItem *w = &worldItems[worldItemCount++];
    w->tpl = tpl;
    w->x = x;
    w->y = y;
    w->z = z;
    w->taken = 0;
}

/* Place FillRoom's literal item spawns in every placed room (rotated
 * with the room), plus one keycard per zone as progression safety
 * until the scripted keycard sources are ported. */
static void spawnItems(void) {
    worldItemCount = 0;
    inventoryCount = 0;
    for (uint32_t i = 0; i < map.roomCount; i++) {
        const RoomPlacement *p = &map.rooms[i];
        const char *roomName = tplList.items[p->templateIndex].name;
        for (unsigned s = 0;
             s < sizeof(ITEM_SPAWNS) / sizeof(ITEM_SPAWNS[0]); s++) {
            if (strcmp(ITEM_SPAWNS[s].room, roomName) != 0) continue;
            float local[3] = { ITEM_SPAWNS[s].x, ITEM_SPAWNS[s].y,
                               ITEM_SPAWNS[s].z };
            float w[3];
            localToWorld(p, local, w);
            worldItemAdd(itemTplFind(ITEM_SPAWNS[s].item), w[0], w[1], w[2]);
        }
    }

    const char *zoneCard[3] = { "Level 1 key Card", "Level 3 key Card",
                                "Level 5 key Card" };
    for (int k = 0; k < 3; k++) {
        for (uint32_t i = 0; i < map.roomCount; i++) {
            const RoomPlacement *p = &map.rooms[i];
            int zone = (p->gridY < MAPGEN_GRID / 3 + 1) ? 3
                     : (p->gridY < (int)(MAPGEN_GRID * (2.0 / 3.0))) ? 2 : 1;
            if (zone != k + 1) continue;
            worldItemAdd(itemTplFind(zoneCard[k]),
                         p->gridX * ROOM_SPACING, 60.0f,
                         p->gridY * ROOM_SPACING);
            break;
        }
    }
}

static int itemPickupNearest(const float pos[3]) {
    int best = -1;
    float bestD2 = ITEM_PICKUP_REACH * ITEM_PICKUP_REACH;
    for (unsigned i = 0; i < worldItemCount; i++) {
        WorldItem *w = &worldItems[i];
        if (w->taken) continue;
        float dx = pos[0] - w->x, dz = pos[2] - w->z;
        float dy = pos[1] - EYE_HEIGHT - w->y;
        if (dy < -350.0f || dy > 350.0f) continue;
        float d2 = dx * dx + dz * dz;
        if (d2 < bestD2) {
            bestD2 = d2;
            best = (int)i;
        }
    }
    if (best < 0 || inventoryCount >= (unsigned)invSlotCap) return -1;
    worldItems[best].taken = 1;
    inventory[inventoryCount++] = worldItems[best].tpl;
    return worldItems[best].tpl;
}

static void consumeSlot(int slot) {
    for (unsigned i = (unsigned)slot; i + 1 < inventoryCount; i++) {
        inventory[i] = inventory[i + 1];
    }
    inventoryCount--;
    if (invSel >= (int)inventoryCount && invSel > 0) invSel--;
}

static void radioSetChannel(int ch);

/* Use/equip the selected inventory item (the game's right-click).
 * Returns a toast string or NULL if the item has no use action. */
static const char *useInventoryItem(int slot) {
    static char msg[96];
    if (slot < 0 || slot >= (int)inventoryCount) return NULL;
    int tpl = inventory[slot];
    const char *name = ITEM_TEMPLATES[tpl].name;

    if (strstr(name, "Eyedrops")) {
        blinkTimer = 100.0f;
        consumeSlot(slot);
        return "YOU USED THE EYEDROPS";
    }
    if (strstr(name, "S-NAV")) {
        if (equippedNav == tpl) {
            equippedNav = -1;
            snprintf(msg, sizeof(msg), "PUT AWAY THE %s", name);
        } else {
            equippedNav = tpl;
            navVisible = 1;
            snprintf(msg, sizeof(msg), "EQUIPPED THE %s", name);
        }
        return msg;
    }
    if (strstr(name, "First Aid Kit")) {
        if (health >= 100.0f) return "YOU ARE NOT INJURED";
        health = 100.0f;
        consumeSlot(slot);
        return "YOU BANDAGE YOUR WOUNDS";
    }
    if (strcmp(name, "SCP-500-01") == 0) {
        health = 100.0f;
        blinkTimer = 100.0f;
        stamina = 100.0f;
        staminaBlocked = 0;
        consumeSlot(slot);
        return "YOU SWALLOW THE PILL. YOU FEEL GREAT";
    }
    if (strcmp(name, "SCP-500") == 0) {
        return "THE BOTTLE OF PILLS - USE ONE FROM YOUR INVENTORY";
    }
    if (strstr(name, "420-J")) {
        health += 30.0f;
        if (health > 100.0f) health = 100.0f;
        consumeSlot(slot);
        return "MAN, THIS IS SOME REALLY GOOD S***";
    }
    if (strcmp(name, "Gas Mask") == 0) {
        wearGasMask = !wearGasMask;
        return wearGasMask ? "YOU PUT ON THE GAS MASK"
                           : "YOU TAKE OFF THE GAS MASK";
    }
    if (strstr(name, "Night Vision Goggles")) {
        int fine = strstr(name, "Fine") != NULL ? 2 : 1;
        wearNVG = (wearNVG == fine) ? 0 : fine;
        return wearNVG ? "YOU PUT ON THE NIGHT VISION GOGGLES"
                       : "YOU TAKE OFF THE NIGHT VISION GOGGLES";
    }
    if (strcmp(name, "SCP-268") == 0) {
        wear268 = !wear268;
        return wear268 ? "YOU PUT ON THE CAP. YOU FEEL... UNNOTICEABLE"
                       : "YOU TAKE OFF THE CAP";
    }
    if (strcmp(name, "Ballistic Vest") == 0) {
        wearVest = !wearVest;
        return wearVest ? "YOU PUT ON THE BALLISTIC VEST"
                        : "YOU TAKE OFF THE BALLISTIC VEST";
    }
    if (strcmp(name, "Radio Transceiver") == 0) {
        radioSetChannel(radioChannel >= 3 ? -1 : radioChannel + 1);
        if (radioChannel < 0) return "YOU SWITCH OFF THE RADIO";
        snprintf(msg, sizeof(msg), "RADIO: CHANNEL %d", radioChannel + 1);
        return msg;
    }
    if (strcmp(name, "Pizza Slice") == 0) {
        health += 10.0f;
        if (health > 100.0f) health = 100.0f;
        consumeSlot(slot);
        return "YOU EAT THE PIZZA SLICE. IT'S COLD";
    }
    if (strcmp(name, "Cup") == 0) {
        consumeSlot(slot);
        return "YOU DRINK THE LIQUID. IT TASTES LIKE NOTHING";
    }
    if (strcmp(name, "SCP-005") == 0) {
        return "A KEY THAT SEEMS TO FIT ANY LOCK";
    }
    if (strcmp(name, "Quarter") == 0) return "A QUARTER. HEADS";
    if (strcmp(name, "Origami") == 0) return "A PAPER CRANE";
    if (strcmp(name, "Playing Card") == 0) {
        return "THE ACE OF SPADES";
    }
    if (strcmp(name, "Wallet") == 0) {
        return "SOMEONE'S WALLET. IT'S EMPTY";
    }
    if (strcmp(name, "SCP-714") == 0) {
        return "THE JADE RING IS TOO SMALL FOR YOUR FINGERS";
    }
    return NULL;
}

static void drawItems(const float viewPos[3]) {
    for (unsigned i = 0; i < worldItemCount; i++) {
        const WorldItem *w = &worldItems[i];
        if (w->taken) continue;
        float dx = w->x - viewPos[0], dz = w->z - viewPos[2];
        if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;
        glPushMatrix();
        glTranslatef(w->x, w->y, w->z);
        glRotatef(itemSpin, 0.0f, 1.0f, 0.0f);
        drawModelRT(itemModel(w->tpl));
        glPopMatrix();
    }
}

/* ---------------- menus, difficulties, options, save/load ---------------- */

static int pauseOpen;
static int pauseSel;
static uint32_t pendingSeed;

/* 0 = main menu, 1 = playing. The world only exists after the first
 * NEW GAME / LOAD GAME. */
static int gameState;
static int titleSel;
static int worldReady;

/* Menu tab, mirroring Menu_Core.bb's MainMenuTab: 0 = button column,
 * 1 = NEW GAME, 2 = LOAD GAME, 3 = OPTIONS. */
static int menuTab;

/* ---- difficulties (Difficulty_Core.bb) ---- */

enum { SAVE_ANYWHERE = 0, SAVE_ON_SCREENS, SAVE_ON_QUIT, NO_SAVES };
enum { FACT_EASY = 0, FACT_NORMAL, FACT_HARD, FACT_EXTREME };

typedef struct {
    const char *name;
    const char *desc;
    int aggressiveNPCs;
    int slots;
    int saveType;
    int factors;
    int customizable;
    unsigned char r, g, b;
} DifficultyDef;

/* Names/colors/settings from Difficulty_Core.bb, descriptions from
 * Data/local.ini [msg]. Esoteric is player-customizable, so the table
 * is mutable. */
static DifficultyDef DIFFICULTIES[5] = {
    { "Safe",
      "The game can be saved any time. However, as in the case of SCP "
      "Objects, a Safe classification doesn't mean that handling it "
      "doesn't pose a threat.",
      0, 10, SAVE_ANYWHERE, FACT_EASY, 0, 120, 150, 50 },
    { "Euclid",
      "In Euclid difficulty, saving is only allowed at specific "
      "locations marked by lit up computer screens. Euclid-class "
      "objects are inherently unpredictable, so that reliable "
      "containment is not always possible.",
      0, 8, SAVE_ON_SCREENS, FACT_NORMAL, 0, 200, 200, 50 },
    { "Keter",
      "Keter-class objects are considered the most dangerous ones in "
      "Foundation containment. The same can be said for this "
      "difficulty level: the SCPs are more aggressive, and you have "
      "only one life - when you die, the game is over.",
      1, 6, SAVE_ON_QUIT, FACT_HARD, 0, 200, 50, 50 },
    { "Apollyon",
      "Apollyon-class object is either completely impossible to "
      "contain or about to irrevocably breach containment, resulting "
      "in unimaginable consequences. God help the humble subject "
      "attempting this difficulty.",
      1, 2, NO_SAVES, FACT_EXTREME, 0, 150, 150, 150 },
    { "Esoteric", "", 0, 10, SAVE_ANYWHERE, FACT_EASY, 1, 200, 50, 200 },
};

static const char *SAVE_TYPE_NAMES[4] = {
    "Save anywhere", "Save on screens", "Save on quit", "No saves",
};
static const char *FACTOR_NAMES[4] = { "Easy", "Normal", "Hard", "Extreme" };

static int selDiff = 1;  /* menu selection; the game defaults to Euclid */
static int gameDiff = 1; /* difficulty of the running game */

static void applyDifficulty(int d) {
    gameDiff = d;
    invSlotCap = DIFFICULTIES[d].slots;
    npcAggressive = DIFFICULTIES[d].aggressiveNPCs;
}

/* ---- new game form state ---- */

static char newName[16];
static char newSeedStr[16];
static int introEnabled = 1;
static int newSel; /* focused row */

static char curSaveName[16] = "untitled";

/* Blitz GenerateSeedNumber (Math_Core.bb): xor of chars at a rolling
 * bit shift. Text seeds hash to the same numbers as the game. */
static uint32_t seedFromString(const char *str) {
    uint32_t t = 0;
    int shift = 0;
    for (; *str; str++) {
        t ^= ((uint32_t)(unsigned char)*str) << shift;
        shift = (shift + 1) % 24;
    }
    return t ? t : 1; /* mapgen treats 0 as "no seed" */
}

/* Random seed string exactly like Menu_Core.bb: a 1-in-15 chance of an
 * easter-egg seed, otherwise 4-8 random digits/letters. */
static void randomSeedString(char *out, size_t cap) {
    static const char *EGGS[13] = {
        "NIL", "NO", "d9341", "5CP_I73", "DONTBLINK", "CRUNCH", "die",
        "HTAED", "rustledjim", "larry", "JORGE", "dirtymetal",
        "whatpumpkin",
    };
    if (rand() % 15 == 0) {
        snprintf(out, cap, "%s", EGGS[rand() % 13]);
        return;
    }
    int n = 4 + rand() % 5;
    size_t o = 0;
    for (int i = 0; i < n && o + 1 < cap; i++) {
        if (rand() % 3 == 0) {
            out[o++] = (char)('0' + rand() % 10);
        } else {
            out[o++] = (char)('a' + rand() % 26);
        }
    }
    out[o] = '\0';
}

/* ---- on-screen keyboard for the text fields ---- */

static int oskOpen;
static int oskRow, oskCol;
static char *oskTarget;
static int oskCap;
static char oskTitle[24];

static const char *OSK_ROWS[4] = {
    "ABCDEFGHIJKLM",
    "NOPQRSTUVWXYZ",
    "abcdefghijklm",
    "nopqrstuvwxyz",
};
static const char *OSK_DIGITS = "0123456789-_ ";
#define OSK_NROWS 5 /* 4 letter rows + digits row */

static const char *oskRowChars(int row) {
    return row < 4 ? OSK_ROWS[row] : OSK_DIGITS;
}

static void oskStart(char *target, int cap, const char *title) {
    oskOpen = 1;
    oskTarget = target;
    oskCap = cap;
    oskRow = 0;
    oskCol = 0;
    snprintf(oskTitle, sizeof(oskTitle), "%s", title);
}

/* ---- options (persisted like the game's options.ini) ---- */

#define OPTIONS_PATH DATA_ROOT "/options.ini"

static float optMusicVol = 1.0f;
static float optSfxVol = 1.0f;
static float optLookSens = 1.0f;
static int optInvertY = 0;
static int optSel;

static void optionsApply(void) {
    audioSetMusicVolume(optMusicVol);
    audioSetSfxVolume(optSfxVol);
}

static void optionsSave(void) {
    FILE *f = fopen(OPTIONS_PATH, "w");
    if (!f) return;
    fprintf(f, "music=%f\nsfx=%f\nsens=%f\ninverty=%d\nintro=%d\n",
            optMusicVol, optSfxVol, optLookSens, optInvertY, introEnabled);
    fclose(f);
}

static void optionsLoad(void) {
    FILE *f = fopen(OPTIONS_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "music=", 6) == 0) {
            optMusicVol = strtof(line + 6, NULL);
        } else if (strncmp(line, "sfx=", 4) == 0) {
            optSfxVol = strtof(line + 4, NULL);
        } else if (strncmp(line, "sens=", 5) == 0) {
            optLookSens = strtof(line + 5, NULL);
        } else if (strncmp(line, "inverty=", 8) == 0) {
            optInvertY = (int)strtol(line + 8, NULL, 10);
        } else if (strncmp(line, "intro=", 6) == 0) {
            introEnabled = (int)strtol(line + 6, NULL, 10);
        }
    }
    fclose(f);
    if (optMusicVol < 0.0f || optMusicVol > 1.0f) optMusicVol = 1.0f;
    if (optSfxVol < 0.0f || optSfxVol > 1.0f) optSfxVol = 1.0f;
    if (optLookSens < 0.25f || optLookSens > 3.0f) optLookSens = 1.0f;
}

static GLuint menuBackTex, menuTitleTex, pausePanelTex, menu173Tex;

/* Menu art needs to stay sharp at fullscreen, so it loads at a 1024
 * cap instead of the world texture cap. */
static GLuint loadMenuTexture(const char *name) {
    const char *dirs[1] = { MENU_DIR };
    char path[1024];
    if (!textureResolve(name, dirs, 1, path, sizeof(path))) return 0;
    char err[128];
    TextureImage *img = textureLoadFile(path, 1024, err, sizeof(err));
    if (!img) return 0;
    GLuint handle = 0;
    glGenTextures(1, &handle);
    glBindTexture(GL_TEXTURE_2D, handle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)img->width,
                 (GLsizei)img->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 img->pixels);
    textureFree(img);
    return handle;
}

static void loadMenuTextures(void) {
    menuBackTex = loadMenuTexture("back.png");
    menuTitleTex = loadMenuTexture("SCP_text.png");
    pausePanelTex = loadMenuTexture("pause_menu.png");
    menu173Tex = loadMenuTexture("scp_173_back.png");
}

/* ---- music (streamed from disk) ---- */

static void menuMusicStart(void) {
    audioStreamMusic(DATA_ROOT "/SFX/Music/Menu.ogg", 0.8f, 1);
}

/* Zone music while playing; the generated facility is the Light
 * Containment Zone (Music[0] in Loading_Core.bb). */
static void gameMusicStart(void) {
    if (radioChannel >= 0) return; /* the radio owns the music path */
    audioStreamMusic(DATA_ROOT "/SFX/Music/LightContainmentZone.ogg",
                     0.55f, 1);
}

/* Radio Transceiver: SCPRadio stations stream where the zone music
 * normally plays; switching it off restores the music. */
static void radioSetChannel(int ch) {
    radioChannel = ch;
    if (ch < 0) {
        audioStopMusic();
        gameMusicStart();
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), DATA_ROOT "/SFX/Radio/SCPRadio%d.ogg", ch);
    audioStreamMusic(path, 0.75f, 1);
}

/* ---- named saves ---- */

#define SAVES_DIR DATA_ROOT "/saves"
#define SAVE_PATH DATA_ROOT "/save.dat" /* legacy single-slot save */

static void currentSavePath(char *out, size_t cap) {
    snprintf(out, cap, SAVES_DIR "/%s.dat", curSaveName);
}

/* The map, doors, item spawns and 173 placement are all deterministic
 * from the seed, so a save only records the seed plus mutable state. */
static int saveGame(void) {
    mkdir(SAVES_DIR, 0777);
    char path[256];
    currentSavePath(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "SCPVITA2\n");
    fprintf(f, "name=%s\n", curSaveName);
    fprintf(f, "seed=%u\n", mapSeed);
    fprintf(f, "difficulty=%d\n", gameDiff);
    fprintf(f, "diffcfg=%d %d %d %d\n", DIFFICULTIES[gameDiff].aggressiveNPCs,
            DIFFICULTIES[gameDiff].slots, DIFFICULTIES[gameDiff].saveType,
            DIFFICULTIES[gameDiff].factors);
    fprintf(f, "player=%f %f %f %f %f\n", camPos[0], camPos[1], camPos[2],
            camYaw, camPitch);
    fprintf(f, "vitals=%f %f\n", blinkTimer, stamina);
    fprintf(f, "cond=%f %d %d %d %d %d\n", health, wearGasMask, wearNVG,
            wear268, wearVest, radioChannel);
    fprintf(f, "npc173=%f %f %f %f %d\n", npc173Pos[0], npc173Pos[1],
            npc173Pos[2], npc173YawDeg, npc173Active);
    fprintf(f, "nav=%s\n",
            equippedNav >= 0 ? ITEM_TEMPLATES[equippedNav].name : "");
    fprintf(f, "inv=");
    for (unsigned i = 0; i < inventoryCount; i++) {
        fprintf(f, "%s|", ITEM_TEMPLATES[inventory[i]].name);
    }
    fprintf(f, "\ntaken=");
    for (unsigned i = 0; i < worldItemCount; i++) {
        fputc(worldItems[i].taken ? '1' : '0', f);
    }
    fprintf(f, "\ndoors=");
    for (uint32_t i = 0; i < doors.count; i++) {
        fputc(doors.items[i].open ? '1' : '0', f);
    }
    fprintf(f, "\n");
    fclose(f);
    return 1;
}

static int loadGameFrom(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int version = 0;
    if (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "SCPVITA2", 8) == 0) version = 2;
        else if (strncmp(line, "SCPVITA1", 8) == 0) version = 1;
    }
    if (!version) {
        fclose(f);
        return 0;
    }
    uint32_t seed = 0;
    float px = 0, py = 0, pz = 0, yaw = 0, pitch = 0, bt = 100, st = 100;
    float nx = 0, ny = 0, nz = 0, nyaw = 0;
    float condHealth = 100.0f;
    int condWear[4] = { 0, 0, 0, 0 };
    int condRadio = -1;
    int nact = 1;
    int diff = 1;
    char name[16] = "save";
    int cfg[4] = { -1, -1, -1, -1 };
    static char invLine[640], takenLine[640], doorLine[640];
    char navLine[64] = "";
    invLine[0] = takenLine[0] = doorLine[0] = '\0';
    /* Parsed with strtof/strtoul: vitasdk newlib faults on scanf
     * float conversions. */
    while (fgets(line, sizeof(line), f)) {
        char *p;
        if (strncmp(line, "seed=", 5) == 0) {
            seed = (uint32_t)strtoul(line + 5, NULL, 10);
        } else if (strncmp(line, "name=", 5) == 0) {
            snprintf(name, sizeof(name), "%s", line + 5);
            name[strcspn(name, "\r\n")] = '\0';
        } else if (strncmp(line, "difficulty=", 11) == 0) {
            diff = (int)strtol(line + 11, NULL, 10);
            if (diff < 0 || diff > 4) diff = 1;
        } else if (strncmp(line, "diffcfg=", 8) == 0) {
            p = line + 8;
            for (int i = 0; i < 4; i++) cfg[i] = (int)strtol(p, &p, 10);
        } else if (strncmp(line, "player=", 7) == 0) {
            p = line + 7;
            px = strtof(p, &p);
            py = strtof(p, &p);
            pz = strtof(p, &p);
            yaw = strtof(p, &p);
            pitch = strtof(p, &p);
        } else if (strncmp(line, "vitals=", 7) == 0) {
            p = line + 7;
            bt = strtof(p, &p);
            st = strtof(p, &p);
        } else if (strncmp(line, "cond=", 5) == 0) {
            p = line + 5;
            condHealth = strtof(p, &p);
            condWear[0] = (int)strtol(p, &p, 10);
            condWear[1] = (int)strtol(p, &p, 10);
            condWear[2] = (int)strtol(p, &p, 10);
            condWear[3] = (int)strtol(p, &p, 10);
            condRadio = (int)strtol(p, &p, 10);
        } else if (strncmp(line, "npc173=", 7) == 0) {
            p = line + 7;
            nx = strtof(p, &p);
            ny = strtof(p, &p);
            nz = strtof(p, &p);
            nyaw = strtof(p, &p);
            nact = (int)strtol(p, &p, 10);
        } else if (strncmp(line, "nav=", 4) == 0) {
            snprintf(navLine, sizeof(navLine), "%s", line + 4);
            navLine[strcspn(navLine, "\r\n")] = '\0';
        } else if (strncmp(line, "inv=", 4) == 0) {
            snprintf(invLine, sizeof(invLine), "%s", line + 4);
        } else if (strncmp(line, "taken=", 6) == 0) {
            snprintf(takenLine, sizeof(takenLine), "%s", line + 6);
        } else if (strncmp(line, "doors=", 6) == 0) {
            snprintf(doorLine, sizeof(doorLine), "%s", line + 6);
        }
    }
    fclose(f);
    if (seed == 0) return 0;

    if (version == 2) {
        snprintf(curSaveName, sizeof(curSaveName), "%s", name);
    } else {
        snprintf(curSaveName, sizeof(curSaveName), "save");
    }
    if (DIFFICULTIES[diff].customizable && cfg[1] > 0) {
        DIFFICULTIES[diff].aggressiveNPCs = cfg[0];
        DIFFICULTIES[diff].slots = cfg[1];
        DIFFICULTIES[diff].saveType = cfg[2];
        DIFFICULTIES[diff].factors = cfg[3];
    }
    applyDifficulty(diff);

    regenerateMap(seed);
    camPos[0] = px;
    camPos[1] = py;
    camPos[2] = pz;
    camYaw = yaw;
    camPitch = pitch;
    velY = 0.0f;
    blinkTimer = bt;
    stamina = st;
    health = condHealth;
    wearGasMask = condWear[0];
    wearNVG = condWear[1];
    wear268 = condWear[2];
    wearVest = condWear[3];
    radioSetChannel(condRadio);
    npc173Pos[0] = nx;
    npc173Pos[1] = ny;
    npc173Pos[2] = nz;
    npc173YawDeg = nyaw;
    if (!nact) npc173Active = 0;
    introPhase = -1; /* saves are never mid-intro */
    if (navLine[0]) equippedNav = itemTplFind(navLine);
    inventoryCount = 0;
    char *tok = strtok(invLine, "|\n");
    while (tok && inventoryCount < MAX_INVENTORY) {
        int t = itemTplFind(tok);
        if (t >= 0) inventory[inventoryCount++] = t;
        tok = strtok(NULL, "|\n");
    }
    for (unsigned i = 0;
         i < worldItemCount && takenLine[i] && takenLine[i] != '\n'; i++) {
        worldItems[i].taken = takenLine[i] == '1';
    }
    for (uint32_t i = 0;
         i < doors.count && doorLine[i] && doorLine[i] != '\n'; i++) {
        doors.items[i].open = doorLine[i] == '1';
        doors.items[i].openState = doors.items[i].open ? 180.0f : 0.0f;
    }
    return 1;
}

static int loadGame(void) {
    char path[256];
    currentSavePath(path, sizeof(path));
    if (loadGameFrom(path)) return 1;
    return loadGameFrom(SAVE_PATH); /* legacy single-slot save */
}

/* ---- save list for the LOAD GAME tab ---- */

#define MAX_SAVELIST 32
#define SAVES_PER_PAGE 5

typedef struct {
    char path[256];
    char name[16];
    uint32_t seed;
    int diff;
} SaveEntry;

static SaveEntry saveList[MAX_SAVELIST];
static int saveCount;
static int savePage, saveSel;
static int saveConfirmDel = -1;

static int saveReadHeader(const char *path, SaveEntry *e) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int ok = 0;
    e->seed = 0;
    e->diff = 1;
    snprintf(e->name, sizeof(e->name), "save");
    for (int i = 0; i < 8 && fgets(line, sizeof(line), f); i++) {
        if (i == 0) {
            if (strncmp(line, "SCPVITA", 7) != 0) break;
            ok = 1;
        } else if (strncmp(line, "name=", 5) == 0) {
            snprintf(e->name, sizeof(e->name), "%s", line + 5);
            e->name[strcspn(e->name, "\r\n")] = '\0';
        } else if (strncmp(line, "seed=", 5) == 0) {
            e->seed = (uint32_t)strtoul(line + 5, NULL, 10);
        } else if (strncmp(line, "difficulty=", 11) == 0) {
            e->diff = (int)strtol(line + 11, NULL, 10);
            if (e->diff < 0 || e->diff > 4) e->diff = 1;
        }
    }
    fclose(f);
    snprintf(e->path, sizeof(e->path), "%s", path);
    return ok;
}

static void scanSaves(void) {
    saveCount = 0;
    savePage = 0;
    saveSel = 0;
    saveConfirmDel = -1;
    DIR *d = opendir(SAVES_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) && saveCount < MAX_SAVELIST) {
            size_t n = strlen(ent->d_name);
            if (n < 5 || strcmp(ent->d_name + n - 4, ".dat") != 0) continue;
            char path[256];
            snprintf(path, sizeof(path), SAVES_DIR "/%s", ent->d_name);
            if (saveReadHeader(path, &saveList[saveCount])) saveCount++;
        }
        closedir(d);
    }
    if (saveCount < MAX_SAVELIST
        && saveReadHeader(SAVE_PATH, &saveList[saveCount])) {
        saveCount++; /* legacy single-slot save */
    }
}

static void drawModelRT(const ModelRT *rt) {
    if (!rt->ok) return;
    glPushMatrix();
    glScalef(rt->scale[0], rt->scale[1], rt->scale[2]);
    drawBatchSet(rt->scene, rt->gl, 0);
    glPopMatrix();
}

static void drawDoors(const float viewPos[3]) {
    for (uint32_t i = 0; i < doors.count; i++) {
        const Door *d = &doors.items[i];
        float dx = d->x - viewPos[0], dz = d->z - viewPos[2];
        if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;

        glPushMatrix();
        glTranslatef(d->x, d->y, d->z);
        glRotatef(-(float)d->angle, 0.0f, 1.0f, 0.0f);

        /* Frame and panels by door type (Loading_Core.bb models). */
        const ModelRT *frame = &doorFrameRT;
        const ModelRT *p1 = &doorPanelRT, *p2 = &doorPanelRT;
        int hinged = 0, single = 0;
        switch (d->type) {
            case 1: p1 = p2 = &elevatorRT; break;
            case 2: p1 = &heavy1RT; p2 = &heavy2RT; break;
            case 3: frame = &bigFrameRT; p1 = &big1RT; p2 = &big2RT; break;
            case 4: frame = &officeFrameRT; p1 = &officeRT; hinged = 1; break;
            case 5: frame = &woodenFrameRT; p1 = &woodenRT; hinged = 1; break;
            case 6: p1 = &oneSidedRT; single = 1; break;
            case 7: p1 = &door914RT; single = 1; break;
            default: break;
        }
        drawModelRT(frame);

        float slide = doorSlide(d);
        if (hinged) {
            /* Office/wooden doors swing on a hinge instead of sliding. */
            glPushMatrix();
            glTranslatef(-92.0f, 0.0f, 0.0f);
            glRotatef(-d->openState * 0.5f, 0.0f, 1.0f, 0.0f);
            glTranslatef(92.0f, 0.0f, 0.0f);
            drawModelRT(p1);
            glPopMatrix();
        } else {
            glPushMatrix();
            glTranslatef(slide, 0.0f, 0.0f);
            drawModelRT(p1);
            glPopMatrix();

            if (!single) {
                glPushMatrix();
                glRotatef(180.0f, 0.0f, 1.0f, 0.0f);
                glTranslatef(slide, 0.0f, 0.0f);
                drawModelRT(p2);
                glPopMatrix();
            }
        }

        /* Buttons on both sides; the 180-degree side rotation mirrors
         * the (+0.6, 0.7, -0.1) CreateDoor offset for side 1. FillRoom
         * removes the buttons on some doors (914 booths, elevator
         * covers, locked service doors). Big gates are wider. */
        if (!d->nobuttons && d->type != 7) {
            float bx = d->type == 3 ? 264.0f : 0.6f * 256.0f;
            const ModelRT *btn = d->keycard > 0 ? &buttonKeycardRT
                                                : &buttonRT;
            for (int side = 0; side < 2; side++) {
                glPushMatrix();
                glRotatef((float)(side * 180), 0.0f, 1.0f, 0.0f);
                glTranslatef(bx, 0.7f * 256.0f, -0.1f * 256.0f);
                drawModelRT(btn);
                glPopMatrix();
            }
        }

        glPopMatrix();
    }
}

static GLuint hudBlinkIcon, hudBlinkBar, hudSprintIcon, hudStaminaBar;

static void loadHudTextures(void) {
    hudBlinkIcon = textureGet("blink_icon(1).png");
    hudBlinkBar = textureGet("blink_meter(1).png");
    hudSprintIcon = textureGet("sprint_icon.png");
    hudStaminaBar = textureGet("stamina_meter(1).png");
}

/* Tinted 2D quad in HUD space (ortho already set, depth off). With a
 * texture the rgb multiplies it; without one it fills solid rgb. */
static void drawQuad(float x, float y, float w, float h, GLuint tex,
                     float r, float g, float b, float a) {
    GLfloat verts[12] = { x, y, x + w, y, x + w, y + h,
                          x, y, x + w, y + h, x, y + h };
    GLfloat uvs[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glColor4f(r, g, b, a);
    if (tex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
    if (tex) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(1, 1, 1, 1);
}

/* Convenience for white-tinted textured quads. */
static void drawTexQuad(float x, float y, float w, float h, GLuint tex,
                        float alpha) {
    drawQuad(x, y, w, h, tex, 1.0f, 1.0f, 1.0f, alpha);
}

/* Icon plus a segmented bar like the game's blink/stamina meters. */
static void drawMeter(float x, float y, GLuint icon, GLuint seg,
                      float fraction) {
    drawTexQuad(x, y, 40, 40, icon, 1.0f);
    int segs = (int)(fraction * 14.0f + 0.5f);
    for (int i = 0; i < segs; i++) {
        drawTexQuad(x + 48 + i * 12, y + 10, 8, 20, seg, 1.0f);
    }
}

static void drawText(float x, float y, const char *text) {
    /* Text is flat geometry; ensure no texture bleeds through. */
    glDisable(GL_TEXTURE_2D);
    static char buf[64000];
    int quads = stb_easy_font_print(x, y, (char *)text, NULL, buf, sizeof(buf));

    static GLfloat tris[1000 * 6 * 2];
    int v = 0;
    for (int q = 0; q < quads && v + 12 <= (int)(sizeof(tris) / sizeof(tris[0])); q++) {
        float *quad = (float *)(buf + q * 4 * 16);
        float qx[4], qy[4];
        for (int k = 0; k < 4; k++) {
            qx[k] = quad[k * 4 + 0];
            qy[k] = quad[k * 4 + 1];
        }
        int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) {
            tris[v++] = qx[order[k]];
            tris[v++] = qy[order[k]];
        }
    }

    glVertexPointer(2, GL_FLOAT, 0, tris);
    glDrawArrays(GL_TRIANGLES, 0, v / 2);
}

static void drawHud(const char *line1, const char *line2,
                    const char *toast, const char *overlay) {
    /* (vitals and inventory drawn by caller via hooks below) */
    /* Defense in depth: HUD text uses client-side arrays. */
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glPushMatrix();
    glScalef(2.0f, 2.0f, 1.0f);
    if (line1) drawText(8, 8, line1);
    if (line2) drawText(8, 22, line2);
    if (toast) drawText(120, 200, toast);
    if (overlay) drawText(40, 60, overlay);
    glPopMatrix();

    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnable(GL_DEPTH_TEST);
}

/* Full graphical inventory: dimmed backdrop, slot grid with item
 * icons, a highlighted selection, and the selected item's name. Runs
 * inside the HUD ortho/flat-2D state. */
static void drawInventory(void) {
    const int cols = invSlotCap < 5 ? invSlotCap : 5;
    const int rows = (invSlotCap + 4) / 5;
    const float slot = 96.0f, pad = 12.0f;
    float gridW = cols * slot + (cols - 1) * pad;
    float gridH = rows * slot + (rows - 1) * pad;
    float ox = (SCREEN_W - gridW) * 0.5f;
    float oy = (SCREEN_H - gridH) * 0.5f + 12.0f;

    drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0.0f, 0.0f, 0.0f, 0.72f);

    glPushMatrix();
    glScalef(2.0f, 2.0f, 1.0f);
    glColor4f(0.85f, 0.92f, 0.85f, 1.0f);
    drawText(ox * 0.5f, (oy - 28.0f) * 0.5f, "INVENTORY");
    glColor4f(1, 1, 1, 1);
    glPopMatrix();

    for (int i = 0; i < invSlotCap; i++) {
        int cx = i % cols, cy = i / cols;
        float x = ox + cx * (slot + pad);
        float y = oy + cy * (slot + pad);
        int selected = (i == invSel);
        drawQuad(x, y, slot, slot, 0,
                 selected ? 0.30f : 0.12f, selected ? 0.34f : 0.14f,
                 selected ? 0.30f : 0.12f, 0.85f);
        float bc = selected ? 0.95f : 0.40f;
        drawQuad(x, y, slot, 2, 0, bc, bc, bc, 1.0f);
        drawQuad(x, y + slot - 2, slot, 2, 0, bc, bc, bc, 1.0f);
        drawQuad(x, y, 2, slot, 0, bc, bc, bc, 1.0f);
        drawQuad(x + slot - 2, y, 2, slot, 0, bc, bc, bc, 1.0f);
        if (i < (int)inventoryCount) {
            const char *icon = ITEM_TEMPLATES[inventory[i]].invIcon;
            GLuint tex = icon[0] ? textureGet(icon) : 0;
            if (tex) {
                drawTexQuad(x + 10, y + 10, slot - 20, slot - 20, tex, 1.0f);
            } else {
                drawQuad(x + 26, y + 26, slot - 52, slot - 52, 0,
                         0.6f, 0.6f, 0.25f, 1.0f);
            }
        }
    }

    glPushMatrix();
    glScalef(2.0f, 2.0f, 1.0f);
    if (invSel < (int)inventoryCount) {
        glColor4f(1.0f, 1.0f, 0.85f, 1.0f);
        drawText(ox * 0.5f, (oy + gridH + 16.0f) * 0.5f,
                 ITEM_TEMPLATES[inventory[invSel]].name);
        glColor4f(1, 1, 1, 1);
        if (ITEM_TEMPLATES[inventory[invSel]].docImage[0]) {
            drawText(ox * 0.5f, (oy + gridH + 30.0f) * 0.5f, "Cross: Read");
        }
    } else if (inventoryCount == 0) {
        drawText(ox * 0.5f, (oy + gridH + 16.0f) * 0.5f, "(empty)");
    }
    glPopMatrix();
}

/* Documents render full-screen, so load them sharper than the world
 * texture cap (581x819 native stays crisp at a 1024 cap). One doc is
 * resident at a time. */
static void openDocument(const char *imgName) {
    if (!imgName || !imgName[0]) return;
    if (strcmp(docTexName, imgName) != 0) {
        if (docTex) {
            glDeleteTextures(1, &docTex);
            docTex = 0;
        }
        docTexName[0] = '\0';
        const char *dirs[3] = { ITEMS_HUD_DIR, ITEMS_DIR, MAP_TEXTURES_DIR };
        char path[1024];
        if (textureResolve(imgName, dirs, 3, path, sizeof(path))) {
            char err[128];
            TextureImage *img = textureLoadFile(path, 1024, err, sizeof(err));
            if (img) {
                glGenTextures(1, &docTex);
                glBindTexture(GL_TEXTURE_2D, docTex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)img->width,
                             (GLsizei)img->height, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, img->pixels);
                docW = (float)img->width;
                docH = (float)img->height;
                textureFree(img);
                snprintf(docTexName, sizeof(docTexName), "%s", imgName);
            }
        }
    }
    if (docTex) docOpen = 1;
}

static void drawDocument(void) {
    /* Dim the world rather than blacking it out, so the surroundings
     * stay visible behind the page (as in the original). */
    drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0.0f, 0.0f, 0.0f, 0.6f);
    if (!docTex || docH <= 0.0f) return;
    float h = SCREEN_H * 0.92f;
    float w = h * (docW / docH);
    if (w > SCREEN_W * 0.96f) {
        w = SCREEN_W * 0.96f;
        h = w * (docH / docW);
    }
    drawTexQuad((SCREEN_W - w) * 0.5f, (SCREEN_H - h) * 0.5f, w, h, docTex,
                1.0f);
}

/* ---------------- menus (game-styled) ---------------- */

/* Flat-2D state, same setup drawHud uses. */
static void beginHud2D(void) {
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, SCREEN_W, SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glColor4f(1, 1, 1, 1);
}

static void endHud2D(void) {
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnable(GL_DEPTH_TEST);
}

/* Design-space mapping: Menu_Core.bb lays the menu out in 1280x960. */
#define MENU_SX (SCREEN_W / 1280.0f)
#define MENU_SY (SCREEN_H / 960.0f)

/* Text at an arbitrary scale and color, screen coordinates. */
static void mtext(float x, float y, float sc, float r, float g, float b,
                  const char *t) {
    glPushMatrix();
    glScalef(sc, sc, 1.0f);
    glColor4f(r, g, b, 1.0f);
    drawText(x / sc, y / sc, t);
    glColor4f(1, 1, 1, 1);
    glPopMatrix();
}

static float mtextWidth(const char *t, float sc) {
    return stb_easy_font_width((char *)t) * sc;
}

/* Centered text (the game's TextEx with centering flags). */
static void mtextC(float cx, float cy, float sc, float r, float g, float b,
                   const char *t) {
    mtext(cx - mtextWidth(t, sc) * 0.5f, cy - 4.5f * sc, sc, r, g, b, t);
}

/* Word-wrapped text in a box (the game's RowText). */
static void mtextWrap(float x, float y, float w, float sc, const char *t) {
    char line[128] = "";
    size_t ll = 0;
    float cy = y;
    const char *p = t;
    while (*p) {
        const char *sp = strchr(p, ' ');
        size_t wl = sp ? (size_t)(sp - p) : strlen(p);
        char word[64];
        if (wl >= sizeof(word)) wl = sizeof(word) - 1;
        memcpy(word, p, wl);
        word[wl] = '\0';
        char test[192];
        snprintf(test, sizeof(test), "%s%s%s", line, ll ? " " : "", word);
        if (ll && mtextWidth(test, sc) > w) {
            mtext(x, cy, sc, 1, 1, 1, line);
            cy += 11.0f * sc;
            snprintf(line, sizeof(line), "%s", word);
            ll = strlen(line);
        } else {
            snprintf(line, sizeof(line), "%s", test);
            ll = strlen(line);
        }
        p += wl;
        while (*p == ' ') p++;
    }
    if (ll) mtext(x, cy, sc, 1, 1, 1, line);
}

/* Tap state for the frame, set once per menu/pause frame. */
static int menuTapped;
static float menuTapX, menuTapY;

static void menuPollTap(void) {
    menuTapped = inputTouchTap(&menuTapX, &menuTapY);
}

static int tapIn(float x, float y, float w, float h) {
    return menuTapped && menuTapX >= x && menuTapX <= x + w
        && menuTapY >= y && menuTapY <= y + h;
}

/* A bordered box in the game's frame style; focus brightens the
 * border like the mouse-hover. */
static void drawFrame(float x, float y, float w, float h, int focused) {
    drawQuad(x, y, w, h, 0, 0.02f, 0.02f, 0.02f, 0.92f);
    float bc = focused ? 1.0f : 0.5f;
    drawQuad(x, y, w, 2, 0, bc, bc, bc, 1.0f);
    drawQuad(x, y + h - 2, w, 2, 0, bc, bc, bc, 1.0f);
    drawQuad(x, y, 2, h, 0, bc, bc, bc, 1.0f);
    drawQuad(x + w - 2, y, 2, h, 0, bc, bc, bc, 1.0f);
}

/* A menu button in the game's style: black box, light border, caps
 * label; the selection brightens the border like the mouse-hover. */
static void drawMenuButton(float x, float y, float w, float h,
                           const char *label, int selected) {
    drawFrame(x, y, w, h, selected);
    float g = selected ? 1.0f : 0.78f;
    mtextC(x + w * 0.5f, y + h * 0.5f, 2.0f, g, g, g, label);
}

/* The game's UpdateMenuTick checkbox. */
static void drawTick(float x, float y, float size, int on, int focused) {
    drawFrame(x, y, size, size, focused);
    if (on) {
        drawQuad(x + 5, y + 5, size - 10, size - 10, 0, 0.85f, 0.85f, 0.85f,
                 1.0f);
    }
}

/* ---- main menu decorations: flickering 173 and creepy strings ---- */

static int flicker173Frames, flicker173Cooldown = 240;
static const char *creepStr;
static int creepFrames, creepCooldown = 500;
static float creepX, creepY;

static const char *CREEP_STRINGS[16] = {
    "DON'T BLINK",
    "Secure. Contain. Protect.",
    "You want happy endings? Fuck you.",
    "Sometimes we would have had time to scream.",
    "NIL",
    "NO",
    "black white black white black white gray",
    "Stone doesn't care",
    "9341",
    "It controls the doors",
    "e8m106]af173o+079m895w914",
    "It has taken over everything",
    "The spiral is growing",
    "\"Some kind of gestalt effect due to massive reality damage.\"",
    "Does the Black Moon howl? Yes. No. Yes. No.",
    "NIL",
};

/* Background, bottom-center logo, and the flicker effects; drawn under
 * every menu tab (the game's RenderMainMenu). */
static void drawMenuChrome(void) {
    if (menuBackTex) {
        drawTexQuad(0, 0, SCREEN_W, SCREEN_H, menuBackTex, 1.0f);
    } else {
        drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0, 1.0f);
    }

    /* SCP-173 fades in at the bottom-right for a blink. */
    if (--flicker173Cooldown <= 0) {
        flicker173Frames = 10 + rand() % 25;
        flicker173Cooldown = 240 + rand() % 700;
    }
    if (flicker173Frames > 0) {
        flicker173Frames--;
        if (menu173Tex) {
            float w = 420.0f * MENU_SX, h = 750.0f * MENU_SY;
            drawTexQuad(SCREEN_W - w, SCREEN_H - h, w, h, menu173Tex, 1.0f);
        }
    }

    /* Creepy strings jitter in dark gray at random positions. */
    if (--creepCooldown <= 0) {
        creepFrames = 6 + rand() % 22;
        creepCooldown = 400 + rand() % 500;
        creepStr = CREEP_STRINGS[rand() % 16];
        creepX = (float)(700 + rand() % 300) * MENU_SX;
        creepY = (float)(100 + rand() % 500) * MENU_SY;
    }
    if (creepFrames > 0 && creepStr) {
        creepFrames--;
        float jx = (float)(rand() % 7 - 3), jy = (float)(rand() % 7 - 3);
        float w = mtextWidth(creepStr, 1.5f);
        float x = creepX + jx;
        if (x + w > SCREEN_W - 8.0f) x = SCREEN_W - 8.0f - w;
        mtext(x, creepY + jy, 1.5f, 0.2f, 0.2f, 0.2f, creepStr);
    }

    /* SECURE. CONTAIN. PROTECT. logo, bottom-center like the game. */
    if (menuTitleTex) {
        float w = 550.0f * MENU_SX, h = 60.0f * MENU_SY;
        drawTexQuad((SCREEN_W - w) * 0.5f, SCREEN_H - 20.0f * MENU_SY - h,
                    w, h, menuTitleTex, 1.0f);
    }
}

/* Tab header frame + BACK button (top row of every sub-tab). */
static int tapOnBackButton(void) {
    return tapIn((159.0f + 400.0f + 20.0f) * MENU_SX, 286.0f * MENU_SY,
                 160.0f * MENU_SX, 70.0f * MENU_SY);
}

static void drawTabHeader(const char *title, int backFocused) {
    float x = 159.0f * MENU_SX, y = 286.0f * MENU_SY;
    float w = 400.0f * MENU_SX, h = 70.0f * MENU_SY;
    drawFrame(x, y, w, h, 0);
    mtextC(x + w * 0.5f, y + h * 0.5f, 2.0f, 1, 1, 1, title);
    drawMenuButton(x + w + 20.0f * MENU_SX, y, 160.0f * MENU_SX, h, "BACK",
                   backFocused);
}

/* ---- on-screen keyboard ---- */

static void drawOsk(void) {
    float w = 560.0f, h = 300.0f;
    float x = (SCREEN_W - w) * 0.5f, y = (SCREEN_H - h) * 0.5f;
    drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0, 0.6f);
    drawFrame(x, y, w, h, 1);
    mtext(x + 20, y + 16, 2.0f, 1, 1, 1, oskTitle);

    char buf[32];
    snprintf(buf, sizeof(buf), "%s_", oskTarget);
    drawFrame(x + 20, y + 44, w - 40, 34, 0);
    mtext(x + 30, y + 54, 2.0f, 1, 1, 0.8f, buf);

    for (int r = 0; r < OSK_NROWS; r++) {
        const char *row = oskRowChars(r);
        for (int c = 0; row[c]; c++) {
            float cx = x + 26 + c * 39.0f, cy = y + 96 + r * 34.0f;
            int foc = (r == oskRow && c == oskCol);
            if (foc) drawQuad(cx - 4, cy - 4, 30, 26, 0, 0.35f, 0.35f, 0.2f,
                              1.0f);
            char ch[2] = { row[c] == ' ' ? '_' : row[c], 0 };
            float g = foc ? 1.0f : 0.75f;
            mtext(cx, cy, 2.0f, g, g, row[c] == ' ' ? 0.4f : g, ch);
        }
    }
    mtext(x + 20, y + h - 26, 1.5f, 0.6f, 0.6f, 0.6f,
          "X: type   Square: delete   Circle/Start: done");
}

/* Returns 1 while the keyboard is open and consuming input. */
static int oskUpdate(void) {
    if (!oskOpen) return 0;
    const char *row = oskRowChars(oskRow);
    int rowLen;
    if (inputHit(ACTION_SAVE) && oskRow > 0) oskRow--;          /* up */
    if (inputDpadDownHit() && oskRow < OSK_NROWS - 1) oskRow++; /* down */
    row = oskRowChars(oskRow);
    rowLen = (int)strlen(row);
    if (oskCol >= rowLen) oskCol = rowLen - 1;
    if (inputHit(ACTION_LEAN_LEFT) && oskCol > 0) oskCol--;
    if (inputHit(ACTION_LEAN_RIGHT) && oskCol < rowLen - 1) oskCol++;
    int typeChar = -1;
    if (inputHit(ACTION_INTERACT)) typeChar = row[oskCol];
    if (menuTapped) {
        /* Tap a key to type it; tap outside the panel to finish. */
        float w = 560.0f, h = 300.0f;
        float x = (SCREEN_W - w) * 0.5f, y = (SCREEN_H - h) * 0.5f;
        if (!tapIn(x, y, w, h)) {
            oskOpen = 0;
            return 1;
        }
        for (int r = 0; r < OSK_NROWS; r++) {
            const char *rc = oskRowChars(r);
            for (int c = 0; rc[c]; c++) {
                if (tapIn(x + 26 + c * 39.0f - 5, y + 96 + r * 34.0f - 8,
                          39.0f, 34.0f)) {
                    oskRow = r;
                    oskCol = c;
                    typeChar = rc[c];
                }
            }
        }
    }
    if (typeChar >= 0) {
        size_t len = strlen(oskTarget);
        if ((int)len < oskCap - 1) {
            oskTarget[len] = (char)typeChar;
            oskTarget[len + 1] = '\0';
        }
    }
    if (inputHit(ACTION_USE_ITEM)) { /* Square: backspace */
        size_t len = strlen(oskTarget);
        if (len) oskTarget[len - 1] = '\0';
    }
    if (inputHit(ACTION_CROUCH) || inputHit(ACTION_MENU)) { /* done */
        oskOpen = 0;
    }
    return 1;
}

/* ---- NEW GAME tab (update + draw) ---- */

static int startNewGame(void) {
    /* Trim, default the name like the game ("untitled"). */
    char *e = newName + strlen(newName);
    while (e > newName && e[-1] == ' ') *--e = '\0';
    if (!newName[0]) snprintf(newName, sizeof(newName), "untitled");
    char *b = newSeedStr + strlen(newSeedStr);
    while (b > newSeedStr && b[-1] == ' ') *--b = '\0';
    if (!newSeedStr[0]) {
        snprintf(newSeedStr, sizeof(newSeedStr), "%u",
                 (unsigned)(sceKernelGetProcessTimeWide() % 1000000000ull));
    }
    snprintf(curSaveName, sizeof(curSaveName), "%s", newName);
    applyDifficulty(selDiff);
    optionsSave(); /* persists the intro toggle like the game */
    regenerateMap(seedFromString(newSeedStr));
    pendingSeed = mapSeed;
    gameState = 1;
    worldReady = 1;
    pauseOpen = 0;
    if (introEnabled) {
        introStart();
        /* You wake with the orientation leaflet (the no-intro path
         * hands it over on spawn instead). */
        int leaf = itemTplFind("Class D Orientation Leaflet");
        if (leaf >= 0 && inventoryCount < (unsigned)invSlotCap) {
            inventory[inventoryCount++] = leaf;
        }
    } else {
        introPhase = -1;
        gameMusicStart();
    }
    return 1;
}

static void newGameTab(void) {
    const float SX = MENU_SX, SY = MENU_SY;
    DifficultyDef *df = &DIFFICULTIES[selDiff];
    int extra = df->customizable ? 4 : 0;
    int startIdx = 8 + extra;
    int backIdx = startIdx + 1;
    /* Switching off Esoteric removes its rows; keep the focus valid. */
    if (newSel > backIdx) newSel = backIdx;

    /* --- input --- */
    float x = 159.0f * SX, y = 376.0f * SY;
    if (!oskUpdate()) {
        if (inputHit(ACTION_SAVE) && newSel > 0) newSel--;
        if (inputDpadDownHit() && newSel < backIdx) newSel++;
        int esoRow = (newSel >= 8 && newSel < 8 + extra) ? newSel - 8 : -1;
        int lft = inputHit(ACTION_LEAN_LEFT), rgt = inputHit(ACTION_LEAN_RIGHT);
        /* Touch: activate the widget under the tap. */
        int act = inputHit(ACTION_INTERACT) ? newSel : -1;
        if (menuTapped) {
            if (tapIn(x + 150.0f * SX, y + 12.0f * SY, 200.0f * SX,
                      32.0f * SY)) {
                newSel = 0;
                act = 0;
            } else if (tapIn(x + 150.0f * SX, y + 52.0f * SY, 200.0f * SX,
                             32.0f * SY)) {
                newSel = 1;
                act = 1;
            } else if (tapIn(x + 270.0f * SX, y + 96.0f * SY, 54.0f * SY,
                             46.0f * SY)) {
                newSel = 2;
                act = 2;
            } else if (tapOnBackButton()) {
                newSel = backIdx;
                act = backIdx;
            } else if (tapIn(x + 420.0f * SX, y + 385.0f * SY, 160.0f * SX,
                             75.0f * SY)) {
                newSel = startIdx;
                act = startIdx;
            } else {
                for (int i = 0; i < 5; i++) {
                    if (tapIn(x + 14.0f * SX, y + (174.0f + 30.0f * i) * SY,
                              140.0f * SX, 30.0f * SY)) {
                        newSel = 3 + i;
                        act = 3 + i;
                    }
                }
                for (int i = 0; i < extra; i++) {
                    if (tapIn(x + 160.0f * SX, y + (180.0f + 30.0f * i) * SY,
                              390.0f * SX, 30.0f * SY)) {
                        newSel = 8 + i;
                        esoRow = i;
                        rgt = 1; /* tap cycles the value forward */
                    }
                }
            }
        }
        if (esoRow == 0 && (lft || rgt)) {
            df->saveType = (df->saveType + (rgt ? 1 : 3)) % 4;
        } else if (esoRow == 1 && (lft || rgt)) {
            df->aggressiveNPCs = !df->aggressiveNPCs;
        } else if (esoRow == 2 && (lft || rgt)) {
            df->slots += rgt ? 2 : -2;
            if (df->slots > 10) df->slots = 2;
            if (df->slots < 2) df->slots = 10;
        } else if (esoRow == 3 && (lft || rgt)) {
            df->factors = (df->factors + (rgt ? 1 : 3)) % 4;
        } else if (newSel == 2 && (lft || rgt)) {
            introEnabled = !introEnabled;
        }
        if (act >= 0) {
            if (act == 0) {
                oskStart(newName, (int)sizeof(newName), "NAME");
            } else if (act == 1) {
                oskStart(newSeedStr, (int)sizeof(newSeedStr), "MAP SEED");
            } else if (act == 2) {
                introEnabled = !introEnabled;
            } else if (act >= 3 && act <= 7) {
                selDiff = act - 3;
            } else if (act == startIdx) {
                if (startNewGame()) return;
            } else if (act == backIdx) {
                optionsSave();
                menuTab = 0;
                return;
            }
        }
        if (inputHit(ACTION_CROUCH)) { /* Circle backs out */
            optionsSave();
            menuTab = 0;
            return;
        }
    }

    /* --- draw (Menu_Core.bb's New Game layout) --- */
    df = &DIFFICULTIES[selDiff];
    drawTabHeader("NEW GAME", newSel == backIdx);
    drawFrame(x, y, 580.0f * SX, 345.0f * SY, 0);

    mtext(x + 20.0f * SX, y + 19.0f * SY, 1.2f, 1, 1, 1, "Name:");
    drawFrame(x + 150.0f * SX, y + 12.0f * SY, 200.0f * SX, 32.0f * SY,
              newSel == 0);
    mtext(x + 158.0f * SX, y + 21.0f * SY, 1.2f, 1, 1, 1, newName);

    mtext(x + 20.0f * SX, y + 59.0f * SY, 1.2f, 1, 1, 1, "Map seed:");
    drawFrame(x + 150.0f * SX, y + 52.0f * SY, 200.0f * SX, 32.0f * SY,
              newSel == 1);
    mtext(x + 158.0f * SX, y + 61.0f * SY, 1.2f, 1, 1, 1, newSeedStr);

    mtext(x + 20.0f * SX, y + 111.0f * SY, 1.2f, 1, 1, 1, "Intro sequence:");
    drawTick(x + 280.0f * SX, y + 102.0f * SY, 34.0f * SY, introEnabled,
             newSel == 2);

    mtext(x + 20.0f * SX, y + 151.0f * SY, 1.2f, 1, 1, 1, "Difficulty:");
    for (int i = 0; i < 5; i++) {
        const DifficultyDef *d2 = &DIFFICULTIES[i];
        drawTick(x + 20.0f * SX, y + (180.0f + 30.0f * i) * SY, 26.0f * SY,
                 selDiff == i, newSel == 3 + i);
        mtext(x + 50.0f * SX, y + (185.0f + 30.0f * i) * SY, 1.2f,
              d2->r / 255.0f, d2->g / 255.0f, d2->b / 255.0f, d2->name);
    }

    /* Description / customization frame. */
    drawFrame(x + 150.0f * SX, y + 170.0f * SY, 410.0f * SX, 160.0f * SY, 0);
    if (df->customizable) {
        char row[64];
        float rx = x + 170.0f * SX;
        snprintf(row, sizeof(row), "< Save type: %s >",
                 SAVE_TYPE_NAMES[df->saveType]);
        mtext(rx, y + 186.0f * SY, 1.2f, 1, 1, newSel == 8 ? 0.5f : 1.0f, row);
        snprintf(row, sizeof(row), "< Aggressive NPCs: %s >",
                 df->aggressiveNPCs ? "YES" : "NO");
        mtext(rx, y + 216.0f * SY, 1.2f, 1, 1, newSel == 9 ? 0.5f : 1.0f, row);
        snprintf(row, sizeof(row), "< Inventory slots: %d >", df->slots);
        mtext(rx, y + 246.0f * SY, 1.2f, 1, 1, newSel == 10 ? 0.5f : 1.0f,
              row);
        snprintf(row, sizeof(row), "< Other difficulty factors: %s >",
                 FACTOR_NAMES[df->factors]);
        mtext(rx, y + 276.0f * SY, 1.2f, 1, 1, newSel == 11 ? 0.5f : 1.0f,
              row);
    } else {
        mtextWrap(x + 160.0f * SX, y + 180.0f * SY, 390.0f * SX, 1.2f,
                  df->desc);
        /* Stats frame to the right (the game's x+590 box). */
        drawFrame(x + 590.0f * SX, y + 50.0f * SY, 350.0f * SX, 110.0f * SY,
                  0);
        char row[64];
        float rx = x + 600.0f * SX;
        snprintf(row, sizeof(row), "Save type: %s",
                 SAVE_TYPE_NAMES[df->saveType]);
        mtext(rx, y + 58.0f * SY, 1.2f, 1, 1, 1, row);
        snprintf(row, sizeof(row), "Aggressive NPCs: %s",
                 df->aggressiveNPCs ? "YES" : "NO");
        mtext(rx, y + 78.0f * SY, 1.2f, 1, 1, 1, row);
        snprintf(row, sizeof(row), "Inventory slots: %d", df->slots);
        mtext(rx, y + 98.0f * SY, 1.2f, 1, 1, 1, row);
        snprintf(row, sizeof(row), "Other difficulty factors: %s",
                 FACTOR_NAMES[df->factors]);
        mtext(rx, y + 118.0f * SY, 1.2f, 1, 1, 1, row);
        if (df->saveType == NO_SAVES) {
            mtext(rx, y + 138.0f * SY, 1.2f, 1, 0.6f, 0.6f, "No HUD");
        }
    }

    drawMenuButton(x + 420.0f * SX, y + 365.0f * SY + 20.0f * SY,
                   160.0f * SX, 75.0f * SY, "START", newSel == startIdx);

    if (oskOpen) drawOsk();
}

/* ---- LOAD GAME tab ---- */

static void loadGameTab(void) {
    const float SX = MENU_SX, SY = MENU_SY;
    int pages = (saveCount + SAVES_PER_PAGE - 1) / SAVES_PER_PAGE;
    if (pages < 1) pages = 1;
    int base = savePage * SAVES_PER_PAGE;
    int onPage = saveCount - base;
    if (onPage > SAVES_PER_PAGE) onPage = SAVES_PER_PAGE;

    /* --- input --- */
    float x = 159.0f * SX, y = 376.0f * SY;
    if (saveConfirmDel >= 0) {
        int yes = inputHit(ACTION_INTERACT);
        int no = inputHit(ACTION_CROUCH);
        if (menuTapped) {
            float w = 620.0f, h = 130.0f;
            float cx = (SCREEN_W - w) * 0.5f, cy = (SCREEN_H - h) * 0.5f;
            if (tapIn(cx, cy, w * 0.5f, h)) yes = 1;
            else no = 1;
        }
        if (yes) {
            remove(saveList[saveConfirmDel].path);
            scanSaves();
        } else if (no) {
            saveConfirmDel = -1;
        }
    } else {
        int doLoad = inputHit(ACTION_INTERACT);
        if (inputHit(ACTION_SAVE) && saveSel > 0) saveSel--;
        if (inputDpadDownHit() && saveSel < onPage - 1) saveSel++;
        int pgl = inputHit(ACTION_LEAN_LEFT), pgr = inputHit(ACTION_LEAN_RIGHT);
        if (menuTapped) {
            if (tapOnBackButton()) {
                menuTab = 0;
                return;
            }
            /* Tap a save to select it; tap the selected one to load. */
            for (int i = 0; i < onPage; i++) {
                if (tapIn(x + 20.0f * SX, y + (20.0f + i * 78.0f) * SY,
                          540.0f * SX, 66.0f * SY)) {
                    if (i == saveSel) doLoad = 1;
                    else saveSel = i;
                }
            }
            /* Page bar: left half back, right half forward. */
            if (tapIn(x + 60.0f * SX, y + 440.0f * SY, 230.0f * SX,
                      50.0f * SY)) {
                pgl = 1;
            } else if (tapIn(x + 290.0f * SX, y + 440.0f * SY, 230.0f * SX,
                             50.0f * SY)) {
                pgr = 1;
            }
        }
        if (pgl && savePage > 0) {
            savePage--;
            saveSel = 0;
        }
        if (pgr && savePage < pages - 1) {
            savePage++;
            saveSel = 0;
            base = savePage * SAVES_PER_PAGE;
            onPage = saveCount - base;
            if (onPage > SAVES_PER_PAGE) onPage = SAVES_PER_PAGE;
        }
        if (doLoad && onPage > 0) {
            if (loadGameFrom(saveList[base + saveSel].path)) {
                snprintf(toastMsg, sizeof(toastMsg), "GAME LOADED");
                toastTimer = 150;
                gameState = 1;
                worldReady = 1;
                pauseOpen = 0;
                gameMusicStart();
                return;
            }
            snprintf(toastMsg, sizeof(toastMsg), "LOAD FAILED");
            toastTimer = 150;
        }
        if (inputHit(ACTION_USE_ITEM) && onPage > 0) { /* Square deletes */
            saveConfirmDel = base + saveSel;
        }
        if (inputHit(ACTION_CROUCH)) {
            menuTab = 0;
            return;
        }
    }

    /* --- draw --- */
    drawTabHeader("LOAD GAME", 0);
    drawFrame(x, y, 580.0f * SX, 430.0f * SY, 0);

    if (saveCount == 0) {
        mtext(x + 20.0f * SX, y + 20.0f * SY, 1.5f, 1, 1, 1,
              "No saved games.");
    } else {
        for (int i = 0; i < onPage; i++) {
            const SaveEntry *e2 = &saveList[base + i];
            float ry = y + (20.0f + i * 78.0f) * SY;
            drawFrame(x + 20.0f * SX, ry, 540.0f * SX, 66.0f * SY,
                      i == saveSel);
            mtext(x + 36.0f * SX, ry + 10.0f * SY, 2.0f, 1, 1, 1, e2->name);
            char info[96];
            const DifficultyDef *d2 = &DIFFICULTIES[e2->diff];
            snprintf(info, sizeof(info), "Seed: %u", e2->seed);
            mtext(x + 36.0f * SX, ry + 40.0f * SY, 1.5f, 0.7f, 0.7f, 0.7f,
                  info);
            mtext(x + 400.0f * SX, ry + 40.0f * SY, 1.5f, d2->r / 255.0f,
                  d2->g / 255.0f, d2->b / 255.0f, d2->name);
        }
    }

    /* Page indicator frame (the game's PAGE x/y). */
    drawFrame(x + 60.0f * SX, y + 440.0f * SY, 460.0f * SX, 50.0f * SY, 0);
    char page[32];
    snprintf(page, sizeof(page), "PAGE %d/%d", savePage + 1, pages);
    mtextC(x + 290.0f * SX, y + 465.0f * SY, 2.0f, 1, 1, 1, page);
    mtext(x + 20.0f * SX, y + 500.0f * SY, 1.5f, 0.5f, 0.5f, 0.5f,
          "X: load   Square: delete   O: back   left/right: page");

    if (saveConfirmDel >= 0) {
        float w = 620.0f, h = 130.0f;
        float cx = (SCREEN_W - w) * 0.5f, cy = (SCREEN_H - h) * 0.5f;
        drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0, 0.6f);
        drawFrame(cx, cy, w, h, 1);
        mtextC(SCREEN_W * 0.5f, cy + 40, 1.5f, 1, 1, 1,
               "Are you sure you want to delete this save?");
        mtextC(SCREEN_W * 0.5f, cy + 84, 1.5f, 0.8f, 0.8f, 0.8f,
               "X: YES     O: NO");
    }
}

/* ---- OPTIONS tab ---- */

#define OPT_ROWS 4

static void optionsTab(void) {
    const float SX = MENU_SX, SY = MENU_SY;
    int backIdx = OPT_ROWS;
    float x = 159.0f * SX, y = 376.0f * SY;

    if (inputHit(ACTION_SAVE) && optSel > 0) optSel--;
    if (inputDpadDownHit() && optSel < backIdx) optSel++;
    int lft = inputHit(ACTION_LEAN_LEFT), rgt = inputHit(ACTION_LEAN_RIGHT);
    if (menuTapped) {
        if (tapOnBackButton()) {
            optionsSave();
            menuTab = 0;
            return;
        }
        for (int i = 0; i < OPT_ROWS; i++) {
            float ry = y + (30.0f + i * 55.0f) * SY;
            if (!tapIn(x, ry - 10.0f, 580.0f * SX, 34.0f)) continue;
            optSel = i;
            if (i < 2) {
                /* Tap sets the slider position directly. */
                float frac = (menuTapX - (x + 330.0f * SX))
                           / (150.0f * SX);
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                if (i == 0) optMusicVol = frac;
                else optSfxVol = frac;
                optionsApply();
            } else if (i == 3) {
                optInvertY = !optInvertY;
            } else {
                rgt = 1; /* sensitivity: tap steps forward */
            }
        }
    }
    float d = rgt ? 1.0f : (lft ? -1.0f : 0.0f);
    if (d != 0.0f) {
        switch (optSel) {
            case 0:
                optMusicVol += d * 0.1f;
                if (optMusicVol < 0.0f) optMusicVol = 0.0f;
                if (optMusicVol > 1.0f) optMusicVol = 1.0f;
                optionsApply();
                break;
            case 1:
                optSfxVol += d * 0.1f;
                if (optSfxVol < 0.0f) optSfxVol = 0.0f;
                if (optSfxVol > 1.0f) optSfxVol = 1.0f;
                optionsApply();
                break;
            case 2:
                optLookSens += d * 0.25f;
                if (optLookSens < 0.25f) optLookSens = 0.25f;
                if (optLookSens > 3.0f) optLookSens = 3.0f;
                break;
            case 3:
                optInvertY = !optInvertY;
                break;
        }
    }
    if (inputHit(ACTION_INTERACT)) {
        if (optSel == 3) optInvertY = !optInvertY;
        if (optSel == backIdx) {
            optionsSave();
            menuTab = 0;
            return;
        }
    }
    if (inputHit(ACTION_CROUCH)) {
        optionsSave();
        menuTab = 0;
        return;
    }

    drawTabHeader("OPTIONS", optSel == backIdx);
    drawFrame(x, y, 580.0f * SX, 260.0f * SY, 0);

    const char *labels[OPT_ROWS] = {
        "Music volume", "Sound volume", "Look sensitivity", "Invert Y axis",
    };
    for (int i = 0; i < OPT_ROWS; i++) {
        float ry = y + (30.0f + i * 55.0f) * SY;
        float g = optSel == i ? 1.0f : 0.75f;
        mtext(x + 24.0f * SX, ry, 1.5f, g, g, g, labels[i]);
        char val[32];
        if (i == 0) {
            snprintf(val, sizeof(val), "< %d%% >",
                     (int)(optMusicVol * 100.0f + 0.5f));
        } else if (i == 1) {
            snprintf(val, sizeof(val), "< %d%% >",
                     (int)(optSfxVol * 100.0f + 0.5f));
        } else if (i == 2) {
            snprintf(val, sizeof(val), "< %.2fx >", optLookSens);
        } else {
            snprintf(val, sizeof(val), "< %s >", optInvertY ? "ON" : "OFF");
        }
        mtext(x + 330.0f * SX, ry, 1.5f, g, g, g, val);
        /* Volume sliders like the game's option bars. */
        if (i < 2) {
            float frac = i == 0 ? optMusicVol : optSfxVol;
            drawQuad(x + 330.0f * SX, ry + 16.0f, 150.0f * SX, 6, 0, 0.25f,
                     0.25f, 0.25f, 1.0f);
            drawQuad(x + 330.0f * SX, ry + 16.0f, 150.0f * SX * frac, 6, 0,
                     0.85f, 0.85f, 0.85f, 1.0f);
        }
    }
    mtext(x + 24.0f * SX, y + 260.0f * SY + 10.0f, 1.5f, 0.5f, 0.5f, 0.5f,
          "left/right: adjust   O: back");
}

/* ---- main button column ---- */

static void mainButtonsTab(int *running) {
    const float SX = MENU_SX, SY = MENU_SY;
    if (inputHit(ACTION_SAVE) && titleSel > 0) titleSel--;
    if (inputDpadDownHit() && titleSel < 3) titleSel++;
    int act = inputHit(ACTION_INTERACT) ? titleSel : -1;
    for (int i = 0; menuTapped && i < 4; i++) {
        if (tapIn(159.0f * SX, (286.0f + i * 100.0f) * SY, 400.0f * SX,
                  70.0f * SY)) {
            titleSel = i;
            act = i;
        }
    }
    if (act >= 0) {
        switch (act) {
            case 0: /* NEW GAME */
                newName[0] = '\0';
                randomSeedString(newSeedStr, sizeof(newSeedStr));
                newSel = 0;
                oskOpen = 0;
                menuTab = 1;
                return;
            case 1: /* LOAD GAME */
                scanSaves();
                menuTab = 2;
                return;
            case 2: /* OPTIONS */
                optSel = 0;
                menuTab = 3;
                return;
            case 3: /* QUIT */
                *running = 0;
                return;
        }
    }
    /* Start returns to a game in progress (after QUIT TO MENU). */
    if (worldReady && inputHit(ACTION_MENU)) {
        gameState = 1;
        gameMusicStart();
        return;
    }

    static const char *rows[4] = { "NEW GAME", "LOAD GAME", "OPTIONS",
                                   "QUIT" };
    float bx = 159.0f * SX, bw = 400.0f * SX, bh = 70.0f * SY;
    for (int i = 0; i < 4; i++) {
        drawMenuButton(bx, (286.0f + i * 100.0f) * SY, bw, bh, rows[i],
                       i == titleSel);
    }
    if (worldReady) {
        mtext(bx, (286.0f + 4 * 100.0f) * SY, 1.5f, 0.6f, 0.6f, 0.6f,
              "Start: return to game");
    }
}

/* One full menu frame: input + draw. Draws inside HUD 2D state. */
static void menuFrame(int *running) {
    menuPollTap();
    drawMenuChrome();
    switch (menuTab) {
        case 1: newGameTab(); break;
        case 2: loadGameTab(); break;
        case 3: optionsTab(); break;
        default: mainButtonsTab(running); break;
    }
    if (toastTimer > 0) {
        mtextC(SCREEN_W * 0.5f, 200.0f, 2.0f, 1.0f, 1.0f, 0.8f, toastMsg);
    }
}

/* Player control is held during the wake-up screen and cinematic. */
static int introCameraLocked(void) {
    return introPhase == 0 && introCineT <= 900;
}

/* Back to the main menu (boot, QUIT TO MENU, permadeath). */
static void enterMenu(void) {
    gameState = 0;
    menuTab = 0;
    titleSel = 0;
    pauseOpen = 0;
    invOpen = 0;
    docOpen = 0;
    menuMusicStart();
}

/* Pause menu: the game's pause_menu.png panel with its button column
 * (PAUSED / RESUME / QUIT TO MENU labels from local.ini). */
static void drawPauseMenu(void) {
    drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0.0f, 0.0f, 0.0f, 0.55f);

    /* pause_menu.png is 600x673; keep its aspect. */
    float ph = 470.0f, pw = ph * (600.0f / 673.0f);
    float px = 70.0f, py = (SCREEN_H - ph) * 0.5f;
    if (pausePanelTex) {
        drawTexQuad(px, py, pw, ph, pausePanelTex, 1.0f);
    } else {
        drawQuad(px, py, pw, ph, 0, 0.05f, 0.05f, 0.05f, 0.95f);
    }

    mtext(px + 36.0f, py + 24.0f, 2.0f, 1, 1, 1, "PAUSED");

    int saveType = DIFFICULTIES[gameDiff].saveType;
    const char *items[4] = {
        "RESUME",
        saveType == SAVE_ON_QUIT ? "SAVE & QUIT"
                                 : (saveType == NO_SAVES ? "SAVING DISABLED"
                                                         : "SAVE GAME"),
        "LOAD GAME",
        "QUIT TO MENU",
    };
    for (int i = 0; i < 4; i++) {
        drawMenuButton(px + 36.0f, py + 84.0f + i * 78.0f, pw - 72.0f,
                       54.0f, items[i], i == pauseSel);
    }
}

/* S-NAV overlay: explored rooms around the player on a green LCD
 * grid; the Ultimate model also shows SCP-173 as a hostile blip. */
static void drawNav(void) {
    const float PANEL = 200.0f;
    const int R = 4; /* rooms shown each side of the player */
    float ox = SCREEN_W - PANEL - 18.0f, oy = SCREEN_H - PANEL - 60.0f;
    float cell = PANEL / (float)(2 * R + 1);

    drawQuad(ox - 5, oy - 5, PANEL + 10, PANEL + 10, 0, 0.02f, 0.08f, 0.02f,
             0.88f);
    float bc = 0.1f, bg = 0.75f;
    drawQuad(ox - 5, oy - 5, PANEL + 10, 2, 0, bc, bg, bc, 1.0f);
    drawQuad(ox - 5, oy + PANEL + 3, PANEL + 10, 2, 0, bc, bg, bc, 1.0f);
    drawQuad(ox - 5, oy - 5, 2, PANEL + 10, 0, bc, bg, bc, 1.0f);
    drawQuad(ox + PANEL + 3, oy - 5, 2, PANEL + 10, 0, bc, bg, bc, 1.0f);

    float pgx = camPos[0] / ROOM_SPACING;
    float pgy = camPos[2] / ROOM_SPACING;
    float cx0 = ox + PANEL * 0.5f, cy0 = oy + PANEL * 0.5f;

    for (uint32_t i = 0; i < map.roomCount && i < MAX_VISITED; i++) {
        if (!roomVisited[i]) continue;
        float dxc = map.rooms[i].gridX - pgx;
        float dyc = map.rooms[i].gridY - pgy;
        if (fabsf(dxc) > R || fabsf(dyc) > R) continue;
        float cx = cx0 + dxc * cell, cy = cy0 + dyc * cell;
        drawQuad(cx - cell * 0.40f, cy - cell * 0.40f, cell * 0.80f,
                 cell * 0.80f, 0, 0.05f, 0.45f, 0.08f, 0.95f);
    }

    /* Player: bright blip with a heading dot ("north" = -z, up). */
    drawQuad(cx0 - 4, cy0 - 4, 8, 8, 0, 0.5f, 1.0f, 0.5f, 1.0f);
    float hx = cx0 + sinf(camYaw) * cell * 0.45f;
    float hy = cy0 - cosf(camYaw) * cell * 0.45f;
    drawQuad(hx - 2, hy - 2, 4, 4, 0, 0.8f, 1.0f, 0.8f, 1.0f);

    /* SCP-173 blip on the Ultimate model. */
    if (equippedNav >= 0
        && strstr(ITEM_TEMPLATES[equippedNav].name, "Ultimate")
        && npc173Active) {
        float dxc = npc173Pos[0] / ROOM_SPACING - pgx;
        float dyc = npc173Pos[2] / ROOM_SPACING - pgy;
        if (fabsf(dxc) <= R && fabsf(dyc) <= R) {
            float cx = cx0 + dxc * cell, cy = cy0 + dyc * cell;
            drawQuad(cx - 3, cy - 3, 6, 6, 0, 1.0f, 0.15f, 0.1f, 1.0f);
        }
    }

    glPushMatrix();
    glScalef(1.5f, 1.5f, 1.0f);
    glColor4f(0.4f, 0.9f, 0.4f, 1.0f);
    drawText((ox - 2) / 1.5f, (oy - 22.0f) / 1.5f,
             equippedNav >= 0 ? ITEM_TEMPLATES[equippedNav].name : "S-NAV");
    glColor4f(1, 1, 1, 1);
    glPopMatrix();
}

int main(void) {
    vglInit(0x800000);

    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    inputInit();

    int haveData = templatesLoad(ROOMS_INI, &tplList) && tplList.count > 0;
    if (haveData) {
        tplRT = (TemplateRT *)calloc(tplList.count, sizeof(TemplateRT));
        buildDoorAssets();
        buildNpcAssets();
        loadHudTextures();
        loadMenuTextures();
        optionsLoad();
        mkdir(SAVES_DIR, 0777);
        if (audioInit()) {
            loadSounds();
            optionsApply();
        }
        /* Boot to the title menu; the world is generated when the
         * player starts or loads a game. */
        srand((unsigned)sceKernelGetProcessTimeWide());
        pendingSeed = (uint32_t)(sceKernelGetProcessTimeWide() & 0xFFFFFF) | 1u;
        enterMenu();
    } else {
        snprintf(statusLine, sizeof(statusLine),
                 "Data not found. Install the data package to " DATA_ROOT "/");
    }

    int running = 1;
    int menuHits = 0;
    uint64_t fpsLast = sceKernelGetProcessTimeWide();
    unsigned fpsFrames = 0;
    float fps = 0.0f;
    while (running) {
        fpsFrames++;
        uint64_t now = sceKernelGetProcessTimeWide();
        if (now - fpsLast >= 500000) {
            fps = fpsFrames * 1000000.0f / (float)(now - fpsLast);
            fpsFrames = 0;
            fpsLast = now;
        }

        inputUpdate();

        /* ---- main menu state: no world simulation, just the menu ---- */
        if (haveData && gameState == 0) {
            if (toastTimer > 0) toastTimer--;
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            beginHud2D();
            menuFrame(&running);
            endHud2D();
            vglSwapBuffers(GL_FALSE);
            continue;
        }

        if (haveData && inputHit(ACTION_MENU)) {
            pauseOpen = !pauseOpen;
            if (pauseOpen) {
                invOpen = 0;
                docOpen = 0;
                pauseSel = 0;
            }
        } else if (!haveData && inputHit(ACTION_MENU)) {
            if (++menuHits >= 3) running = 0;
        }
        int pausedAtFrameStart = pauseOpen;
        if (pauseOpen) {
            menuPollTap();
            if (inputHit(ACTION_SAVE) && pauseSel > 0) pauseSel--;
            if (inputDpadDownHit() && pauseSel < 3) pauseSel++;
            int pact = inputHit(ACTION_INTERACT) ? pauseSel : -1;
            /* Tap a pause button (same geometry as drawPauseMenu). */
            float ph = 470.0f, pw = ph * (600.0f / 673.0f);
            float ppx = 70.0f, ppy = (SCREEN_H - ph) * 0.5f;
            for (int i = 0; menuTapped && i < 4; i++) {
                if (tapIn(ppx + 36.0f, ppy + 84.0f + i * 78.0f, pw - 72.0f,
                          54.0f)) {
                    pauseSel = i;
                    pact = i;
                }
            }
            if (pact >= 0) {
                switch (pact) {
                    case 0: /* RESUME */
                        pauseOpen = 0;
                        break;
                    case 1: /* SAVE GAME (gated by the difficulty) */
                        if (introPhase >= 0) {
                            snprintf(toastMsg, sizeof(toastMsg),
                                     "YOU CANNOT SAVE DURING THE INTRO");
                        } else if (DIFFICULTIES[gameDiff].saveType
                                   == NO_SAVES) {
                            snprintf(toastMsg, sizeof(toastMsg),
                                     "SAVING IS DISABLED ON THIS DIFFICULTY");
                        } else if (DIFFICULTIES[gameDiff].saveType
                                   == SAVE_ON_QUIT) {
                            if (saveGame()) {
                                enterMenu();
                            } else {
                                snprintf(toastMsg, sizeof(toastMsg),
                                         "SAVE FAILED");
                            }
                        } else {
                            snprintf(toastMsg, sizeof(toastMsg), "%s",
                                     saveGame() ? "GAME SAVED"
                                                : "SAVE FAILED");
                        }
                        toastTimer = 150;
                        break;
                    case 2: /* LOAD GAME */
                        if (loadGame()) {
                            snprintf(toastMsg, sizeof(toastMsg),
                                     "GAME LOADED");
                            pauseOpen = 0;
                        } else {
                            snprintf(toastMsg, sizeof(toastMsg),
                                     "NO SAVE FOUND");
                        }
                        toastTimer = 150;
                        break;
                    case 3: /* QUIT TO MENU */
                        pendingSeed = mapSeed;
                        enterMenu();
                        break;
                }
            }
        }
        if (!pauseOpen && inputHit(ACTION_INVENTORY)) {
            invOpen = !invOpen;
            if (!invOpen) docOpen = 0;
        }
        if (invOpen && docOpen) {
            /* Reading a document: Cross or Circle closes it. */
            if (inputHit(ACTION_INTERACT) || inputHit(ACTION_CROUCH)) {
                docOpen = 0;
            }
        } else if (invOpen) {
            /* D-pad navigates the inventory grid while it is open. */
            if (inputHit(ACTION_LEAN_LEFT) && invSel > 0) invSel--;
            if (inputHit(ACTION_LEAN_RIGHT) && invSel < invSlotCap - 1) {
                invSel++;
            }
            if (inputHit(ACTION_SAVE) && invSel >= 5) invSel -= 5; /* up */
            if (inputDpadDownHit() && invSel + 5 < invSlotCap) {
                invSel += 5;
            }
            /* Cross reads the selected document, if any. */
            if (inputHit(ACTION_INTERACT) && invSel < (int)inventoryCount) {
                const char *doc = ITEM_TEMPLATES[inventory[invSel]].docImage;
                if (doc[0]) openDocument(doc);
            }
            /* Square uses/equips the selected item (right-click). */
            if (inputHit(ACTION_USE_ITEM)) {
                const char *r = useInventoryItem(invSel);
                if (r) {
                    snprintf(toastMsg, sizeof(toastMsg), "%s", r);
                    toastTimer = 150;
                }
            }
        } else if (!pauseOpen) {
            if (inputHit(ACTION_LEAN_LEFT)) fogOn = !fogOn;
            if (inputHit(ACTION_USE_ITEM) && equippedNav >= 0) {
                navVisible = !navVisible;
            }
            if (inputHit(ACTION_SAVE)) { /* D-pad up: walk/fly */
                walkMode = !walkMode;
                velY = 0.0f;
            }
        }
        /* pausedAtFrameStart: a Cross that activated a menu entry this
         * frame must not also interact with the world. */
        if (haveData && !invOpen && !pauseOpen && !pausedAtFrameStart
            && !introCameraLocked() && inputHit(ACTION_INTERACT)) {
            int picked = itemPickupNearest(camPos);
            if (picked >= 0) {
                snprintf(toastMsg, sizeof(toastMsg), "PICKED UP %s",
                         ITEM_TEMPLATES[picked].name);
                toastTimer = 150;
                audioPlay(sndPick[rand() % 4], 0.8f, 0.0f);
            } else {
            Door *pressed = NULL;
            switch (doorsPressButton(&doors, camPos, playerKeycard(),
                                     &pressed)) {
                case DOOR_PRESS_TOGGLED:
                    if (pressed) {
                        float dpos[3] = { pressed->x, camPos[1], pressed->z };
                        int v = rand() % 3;
                        int snd = pressed->heavy
                                ? (pressed->open ? sndBigOpen[v]
                                                 : sndBigClose[v])
                                : (pressed->open ? sndDoorOpen[v]
                                                 : sndDoorClose[v]);
                        audioPlay(pressed->keycard > 0
                                      ? sndKeycardUse[rand() % 2]
                                      : sndButton[rand() % 2], 0.9f, 0.0f);
                        audioPlay3D(snd, dpos, camPos, camYaw, 2500.0f);
                    }
                    break;
                case DOOR_PRESS_KEYCARD:
                    snprintf(toastMsg, sizeof(toastMsg),
                             "ACCESS DENIED - LEVEL %d KEYCARD REQUIRED"
                             " (press x3 to force)",
                             pressed ? pressed->keycard : 0);
                    toastTimer = 150;
                    audioPlay(sndDoorLock, 0.9f, 0.0f);
                    break;
                case DOOR_PRESS_LOCKED:
                    snprintf(toastMsg, sizeof(toastMsg), "THE DOOR IS LOCKED");
                    toastTimer = 150;
                    audioPlay(sndDoorLock, 0.9f, 0.0f);
                    break;
                default:
                    break;
            }
            }
        }
        if (haveData && !pauseOpen) {
            doorsUpdate(&doors);
            update173();
            introUpdate();
            itemSpin += 1.0f;
            if (itemSpin >= 360.0f) itemSpin -= 360.0f;
        }
        if (deathTimer > 0) {
            deathTimer--;
            if (deathTimer == 0) {
                if (DIFFICULTIES[gameDiff].saveType >= SAVE_ON_QUIT) {
                    /* One life (Keter and up): the run and its save are
                     * gone; back to the main menu. */
                    char sp[256];
                    currentSavePath(sp, sizeof(sp));
                    remove(sp);
                    worldReady = 0;
                    enterMenu();
                    snprintf(toastMsg, sizeof(toastMsg),
                             "YOU DIED - GAME OVER");
                    toastTimer = 240;
                    continue;
                }
                if (introPhase >= 0) {
                    introPhase = -1;
                    gameMusicStart();
                }
                spawnPlayer();
                npc173Pos[0] = npc173SpawnX;
                npc173Pos[2] = npc173SpawnZ;
                npc173WasMoving = 0;
                npc173EnemyX = npc173EnemyZ = 0.0f;
                npc173LastDist = 1e9f;
                health = 100.0f;
                snprintf(toastMsg, sizeof(toastMsg),
                         "YOU WERE KILLED BY %s", deathCause);
                toastTimer = 240;
            }
        }
        if (toastTimer > 0) toastTimer--;

        StickState look = inputLook();
        StickState move = inputMove();
        if (invOpen || pauseOpen || deathTimer > 0 || introCameraLocked()) {
            /* Freeze the camera and player while a menu is open, the
             * death screen is playing, or the intro wake-up cinematic
             * drives the camera. */
            look.x = look.y = 0.0f;
            move.x = move.y = 0.0f;
        }
        camYaw += look.x * 0.04f * optLookSens;
        camPitch += look.y * 0.03f * optLookSens * (optInvertY ? -1.0f : 1.0f);
        if (camPitch > 1.5f) camPitch = 1.5f;
        if (camPitch < -1.5f) camPitch = -1.5f;

        if (haveData) {
            updateActiveRooms(camPos);
            markRoomVisited(camPos);
        }

        float fwdX = sinf(camYaw), fwdZ = -cosf(camYaw);
        if (haveData && introCameraLocked()) {
            /* The wake-up cinematic drives the camera; gravity and
             * the floor snap would stand the player straight up. */
        } else if (walkMode && haveData) {
            int crouched = inputDown(ACTION_CROUCH) && !invOpen && !pauseOpen;
            float eye = crouched ? CROUCH_EYE_HEIGHT : EYE_HEIGHT;
            /* Stamina: drains while sprinting, blocks at empty until
             * partially recovered. */
            int wantSprint = inputDown(ACTION_SPRINT) && !crouched;
            int moving = (move.x != 0.0f || move.y != 0.0f);
            int sprinting = wantSprint && !staminaBlocked && moving;
            if (sprinting) {
                stamina -= 0.45f;
                if (stamina <= 0.0f) {
                    stamina = 0.0f;
                    staminaBlocked = 1;
                }
            } else {
                /* The gas mask steadies your breathing. */
                stamina += wearGasMask ? 0.5f : 0.25f;
                if (stamina > 100.0f) stamina = 100.0f;
                if (staminaBlocked && stamina > 25.0f) staminaBlocked = 0;
            }

            /* Blink: meter drains; empty or R closes the eyes. New
             * blinks don't start while a menu is open. */
            if (!invOpen && !pauseOpen && wearNVG != 2) {
                blinkTimer -= 100.0f / 600.0f;
            }
            if (!invOpen && !pauseOpen && inputHit(ACTION_BLINK)) {
                blinkTimer = 0.0f;
            }
            if (!invOpen && !pauseOpen && blinkTimer <= 0.0f
                && blinkFrames == 0) {
                blinkFrames = 18;
            }

            float speed = WALK_SPEED;
            if (crouched) {
                speed *= CROUCH_MULT;
            } else if (sprinting) {
                speed *= SPRINT_MULT;
            }
            float mvx = (fwdX * -move.y + cosf(camYaw) * move.x) * speed;
            float mvz = (fwdZ * -move.y + sinf(camYaw) * move.x) * speed;
            camPos[0] += mvx;
            camPos[2] += mvz;

            /* Footstep cadence by distance walked (PlayStepSound). */
            if (velY > -1.0f) {
                stepAccum += sqrtf(mvx * mvx + mvz * mvz);
                float strideLen = 170.0f;
                if (stepAccum >= strideLen) {
                    stepAccum = 0.0f;
                    if (sprinting) {
                        audioPlay(sndRun[rand() % 7], 0.7f, 0.0f);
                    } else {
                        audioPlay(sndStep[rand() % 8],
                                  crouched ? 0.25f : 0.5f, 0.0f);
                    }
                }
            }

            velY -= GRAVITY;
            if (velY < -TERMINAL_FALL) velY = -TERMINAL_FALL;
            camPos[1] += velY;
            if (velY >= 0.0f || camPos[1] > fallPeakY) {
                fallPeakY = camPos[1];
            }

            float body[3] = { camPos[0], camPos[1] - BODY_DROP, camPos[2] };
            pushWorld(body, PLAYER_RADIUS, NULL);
            camPos[0] = body[0];
            camPos[2] = body[2];

            int pushedUp = 0;
            pushWorld(camPos, PLAYER_RADIUS, &pushedUp);
            doorsCollide(&doors, camPos, PLAYER_RADIUS);

            float hitY;
            if (rayDownWorld(camPos, eye + STEP_SLACK, &hitY)) {
                float target = hitY + eye;
                if (camPos[1] <= target) {
                    /* Landing after a long drop hurts (the game's
                     * falling damage); the ballistic vest absorbs
                     * some of it. ~2.7 units is safe, ~8 is lethal. */
                    float drop = fallPeakY - target;
                    if (drop > 700.0f && deathTimer == 0) {
                        float dmg = (drop - 700.0f) * 0.08f;
                        if (wearVest) dmg *= 0.6f;
                        health -= dmg;
                        damageFlash = 0.6f;
                        audioPlay(sndDamage[rand() % 4], 1.0f, 0.0f);
                        if (health <= 0.0f) {
                            health = 0.0f;
                            snprintf(deathCause, sizeof(deathCause),
                                     "THE FALL");
                            deathTimer = 180;
                        }
                    }
                    camPos[1] = target;
                    velY = 0.0f;
                    fallPeakY = camPos[1];
                }
            }
            if (pushedUp && velY < 0.0f) velY = 0.0f;
            /* Fell out of the world: respawn. */
            if (camPos[1] < -4000.0f) spawnPlayer();
        } else {
            /* Fly: no vertical movement while a menu is open (Circle
             * closes the reader, R is the manual blink). */
            float speed = 30.0f * (inputDown(ACTION_SPRINT) ? 3.0f : 1.0f);
            camPos[0] += (fwdX * -move.y + cosf(camYaw) * move.x) * speed;
            camPos[2] += (fwdZ * -move.y + sinf(camYaw) * move.x) * speed;
            if (!invOpen && !pauseOpen && inputDown(ACTION_BLINK)) {
                camPos[1] += speed;
            }
            if (!invOpen && !pauseOpen && inputDown(ACTION_CROUCH)) {
                camPos[1] -= speed;
            }
        }

        /* A started blink always finishes counting down, whatever mode
         * or menu the player switches to (the quad itself is hidden
         * while a menu is open). */
        if (blinkFrames > 0) {
            blinkFrames--;
            if (blinkFrames == 0) blinkTimer = 100.0f;
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (haveData && activeCount > 0) {
            setPerspective();
            applyDebugState();
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glRotatef(camPitch * 180.0f / 3.14159265f, 1, 0, 0);
            glRotatef(camYaw * 180.0f / 3.14159265f, 0, 1, 0);
            glTranslatef(-camPos[0], -camPos[1], -camPos[2]);

            glColor4f(1, 1, 1, 1);
            for (int i = 0; i < activeCount; i++) {
                drawRoomBatches(activeRooms[i], 0);
            }
            if (introCellGL && inIntroBounds(camPos[0], camPos[2])) {
                glPushMatrix();
                glTranslatef(INTRO_GX * ROOM_SPACING, 0.0f,
                             INTRO_GY * ROOM_SPACING);
                drawBatchSet(introCellScene, introCellGL, 0);
                glPopMatrix();
            }
            drawDoors(camPos);
            drawItems(camPos);
            draw173(camPos);
            drawIntroHumans(camPos);
            glDisable(GL_CULL_FACE);
            for (int i = 0; i < activeCount; i++) {
                drawRoomBatches(activeRooms[i], 1);
            }
            if (introCellGL && inIntroBounds(camPos[0], camPos[2])) {
                glPushMatrix();
                glTranslatef(INTRO_GX * ROOM_SPACING, 0.0f,
                             INTRO_GY * ROOM_SPACING);
                drawBatchSet(introCellScene, introCellGL, 1);
                glPopMatrix();
            }
        }

        char line1[320];
        snprintf(line1, sizeof(line1), "%s   [%s]", statusLine,
                 haveData ? roomNameAt(camPos) : "-");
        char line2[200];
        snprintf(line2, sizeof(line2),
                 "fps=%.0f  au=%d/%d open-fail=%d dec-fail=%d  x: door"
                 "  up: %s  start: menu", fps, audioStatus(),
                 audioSoundCount(), audioLoadFopenFails(),
                 audioLoadDecodeFails(), walkMode ? "WALK" : "fly");
        drawHud(line1, line2, toastTimer > 0 ? toastMsg : NULL, NULL);

        /* Vitals meters, the inventory and the blink blackout share the
         * HUD's ortho matrices set up by drawHud, but need flat 2D
         * state again (drawHud restores arrays/depth on exit). */
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_FOG);
        glDisable(GL_CULL_FACE);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        /* Night vision: brighten the scene green (additive). */
        if (haveData && wearNVG && !invOpen && !pauseOpen
            && deathTimer == 0) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glEnable(GL_BLEND);
            GLfloat nvVerts[12] = { 0, 0, SCREEN_W, 0, SCREEN_W, SCREEN_H,
                                    0, 0, SCREEN_W, SCREEN_H, 0, SCREEN_H };
            glDisable(GL_TEXTURE_2D);
            glColor4f(0.05f, 0.5f, 0.08f, wearNVG == 2 ? 0.4f : 0.28f);
            glVertexPointer(2, GL_FLOAT, 0, nvVerts);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glDisable(GL_BLEND);
            glColor4f(1, 1, 1, 1);
        }
        /* Injuries: a red vignette that deepens as health drops, plus
         * a flash when damage lands. */
        if (haveData && deathTimer == 0) {
            float hurt = (100.0f - health) / 100.0f * 0.30f + damageFlash;
            if (damageFlash > 0.0f) damageFlash -= 0.02f;
            if (hurt > 0.01f) {
                drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0.6f, 0.0f, 0.0f,
                         hurt > 0.8f ? 0.8f : hurt);
            }
        }
        if (haveData && walkMode && !invOpen && !pauseOpen) {
            drawMeter(20, SCREEN_H - 60, hudBlinkIcon, hudBlinkBar,
                      blinkTimer / 100.0f);
            drawMeter(20, SCREEN_H - 110, hudSprintIcon, hudStaminaBar,
                      stamina / 100.0f);
            if (equippedNav >= 0 && navVisible) drawNav();
        }
        if (invOpen) {
            /* Reading a document dims the world and shows the page;
             * otherwise show the inventory grid. */
            if (docOpen) {
                drawDocument();
            } else {
                drawInventory();
            }
        }
        if (pauseOpen) {
            drawPauseMenu();
        }
        /* Death: fade to black with a red flash and a message. */
        if (deathTimer > 0) {
            float t = 1.0f - deathTimer / 180.0f; /* 0 -> 1 */
            float red = t < 0.2f ? (0.2f - t) * 2.5f : 0.0f;
            drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, red, 0.0f, 0.0f,
                     0.35f + 0.65f * t);
            glPushMatrix();
            glScalef(3.0f, 3.0f, 1.0f);
            glColor4f(0.8f, 0.1f, 0.1f, 1.0f);
            drawText(SCREEN_W / 6.0f - 34.0f, SCREEN_H / 6.0f, "YOU DIED");
            glColor4f(1, 1, 1, 1);
            glPopMatrix();
        }

        /* Chamber lights cut before the breach. */
        if (introDark && deathTimer == 0) {
            drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0.0f, 0.0f, 0.0f, 0.6f);
        }
        /* The intro opens on a black screen until a button wakes the
         * player up. */
        if (introPhase == 0 && introCineT < 0) {
            drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0.0f, 0.0f, 0.0f, 1.0f);
            glPushMatrix();
            glScalef(2.0f, 2.0f, 1.0f);
            glColor4f(0.75f, 0.75f, 0.75f, 1.0f);
            drawText((SCREEN_W / 2.0f - 160.0f) / 2.0f,
                     (SCREEN_H / 2.0f - 8.0f) / 2.0f,
                     "PRESS ANY KEY TO WAKE UP");
            glColor4f(1, 1, 1, 1);
            glPopMatrix();
        }
        /* Eyes closed: solid black, but never over an open menu. */
        if (blinkFrames > 0 && !invOpen && !pauseOpen) {
            drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0.0f, 0.0f, 0.0f, 1.0f);
        }
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnable(GL_DEPTH_TEST);

        vglSwapBuffers(GL_FALSE);
    }

    if (haveData) {
        for (uint32_t i = 0; i < tplList.count; i++) {
            templateUnload(&tplRT[i]);
        }
        free(tplRT);
        mapFree(&map);
        templatesFree(&tplList);
    }
    textureCacheClear();
    modelCacheClear();
    sceKernelExitProcess(0);
    return 0;
}
