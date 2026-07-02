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
#include "game/mapgen.h"
#include "input.h"
#include "render/scene.h"

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

static void worldToLocal(const RoomPlacement *p, const float w[3],
                         float l[3]) {
    float dx = w[0] - p->gridX * ROOM_SPACING;
    float dy = w[1];
    float dz = w[2] - p->gridY * ROOM_SPACING;
    switch (p->angle & 3) {
        case 0: l[0] = dx;  l[2] = dz;  break;
        case 1: l[0] = -dz; l[2] = dx;  break; /* Ry(+90) */
        case 2: l[0] = -dx; l[2] = -dz; break;
        case 3: l[0] = dz;  l[2] = -dx; break;
    }
    l[1] = dy;
}

static void localToWorld(const RoomPlacement *p, const float l[3],
                         float w[3]) {
    float x = 0.0f, z = 0.0f;
    switch (p->angle & 3) {
        case 0: x = l[0];  z = l[2];  break;
        case 1: x = l[2];  z = -l[0]; break; /* Ry(-90) */
        case 2: x = -l[0]; z = -l[2]; break;
        case 3: x = -l[2]; z = l[0];  break;
    }
    w[0] = x + p->gridX * ROOM_SPACING;
    w[1] = l[1];
    w[2] = z + p->gridY * ROOM_SPACING;
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
        if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1
            && activeCount < 16) {
            templateEnsure(p->templateIndex);
            if (tplRT[p->templateIndex].state == 1) {
                activeRooms[activeCount++] = p;
            }
        }
    }
}

static const char *roomNameAt(const float pos[3]) {
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
static int fogOn = 1;

/* Vitals (approximate Main_Core.bb rates). */
static float blinkTimer = 100.0f;   /* 0..100, drains ~10s */
static int blinkFrames;             /* >0 while eyes closed */
static float stamina = 100.0f;      /* sprint resource */
static int staminaBlocked;
static char statusLine[256];

/* ---------------- SCP-173 state ---------------- */

static int npc173Active;
static float npc173Pos[3];      /* floor position, raw units */
static float npc173SpawnX, npc173SpawnZ;
static float npc173YawDeg;
static int npc173DragCooldown;
static int npc173WasMoving;
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
    for (uint32_t i = 0; i < tplList.count; i++) {
        templateUnload(&tplRT[i]);
    }
    mapFree(&map);
    doorsFree(&doors);
    mapSeed = seed;
    if (mapGenerate(&tplList, mapSeed, &map)) {
        doorsGenerate(&map, &tplList, mapSeed ^ 0x9E3779B9u, &doors);
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
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GREATER, 0.5f);
        }
        glDrawElements(GL_TRIANGLES, (GLsizei)b->indexCount,
                       GL_UNSIGNED_SHORT, NULL);
        if (alphaPass) {
            glDisable(GL_ALPHA_TEST);
        }

        GLuint lightmap = gl[i].lightmap;
        if (lightmap) {
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

#define SFX_DIR DATA_ROOT "/SFX"

static int sndDoorOpen[3], sndDoorClose[3];
static int sndBigOpen[3], sndBigClose[3];
static int sndStep[8], sndRun[7];
static int sndButton[2], sndKeycardUse[2], sndDoorLock;
static int sndPick[4];
static int sndAmbience;
static int sndRattle[3], sndNeckSnap[3], sndStoneDrag;
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
    sndAmbience = audioLoad(SFX_DIR "/Ambient/Room ambience/rumble.ogg");
    audioLoopAmbience(sndAmbience, 0.30f);
}

static ModelRT doorFrameRT, doorPanelRT, heavy1RT, heavy2RT;
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
static float npc173YOff; /* lifts the model so its base sits on the floor */

static void buildNpcAssets(void) {
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

static void update173(void) {
    if (!npc173Active || deathTimer > 0 || !walkMode) return;
    float dx = camPos[0] - npc173Pos[0];
    float dz = camPos[2] - npc173Pos[2];
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist > ROOM_SPACING * 3.5f) return; /* dormant when far away */

    int seen = playerSees173();
    int moving = 0;

    /* Vertical separation: the statue kills and hunts only on its own
     * level. camPos is the eye; the player's feet are ~EYE_HEIGHT
     * below, and npc173Pos[1] is the statue's floor. */
    float feetDy = fabsf((camPos[1] - EYE_HEIGHT) - npc173Pos[1]);
    int sameLevel = feetDy < 220.0f;

    if (!seen) {
        /* Kill on contact while unobserved - never through a floor. */
        if (dist < 110.0f && sameLevel) {
            audioPlay(sndNeckSnap[rand() % 3], 1.0f, 0.0f);
            deathTimer = 180;
            return;
        }
        /* Player on another floor: hold position instead of grinding
         * into the ceiling/floor beneath them. */
        if (!sameLevel && dist < 500.0f) {
            npc173WasMoving = 0;
            return;
        }

        /* NPCs.ini Speed = 38 -> 0.38 world units/frame = ~97 raw.
         * Substep so it cannot tunnel through walls or closed doors. */
        float speed = 97.0f;
        if (speed > dist - 80.0f) speed = dist - 80.0f;
        if (speed > 0.0f) {
            int steps = (int)(speed / 25.0f) + 1;
            float sx = dx / dist * (speed / steps);
            float sz = dz / dist * (speed / steps);
            for (int s = 0; s < steps; s++) {
                npc173Pos[0] += sx;
                npc173Pos[2] += sz;
                float body[3] = { npc173Pos[0], npc173Pos[1] + 110.0f,
                                  npc173Pos[2] };
                pushWorld(body, 48.0f, NULL);
                doorsCollide(&doors, body, 48.0f);
                npc173Pos[0] = body[0];
                npc173Pos[2] = body[2];
            }
            npc173YawDeg = atan2f(dx, dz) * 180.0f / 3.14159265f;
            moving = 1;
        }

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

static void draw173(const float viewPos[3]) {
    if (!npc173Active || !npc173RT.ok) return;
    float dx = npc173Pos[0] - viewPos[0], dz = npc173Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    glPushMatrix();
    glTranslatef(npc173Pos[0], npc173Pos[1] + npc173YOff, npc173Pos[2]);
    /* +180: the model faces its own -z, so flip to face the player. */
    glRotatef(npc173YawDeg + 180.0f, 0.0f, 1.0f, 0.0f);
    drawModelRT(&npc173RT);
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
    if (best < 0 || inventoryCount >= MAX_INVENTORY) return -1;
    worldItems[best].taken = 1;
    inventory[inventoryCount++] = worldItems[best].tpl;
    return worldItems[best].tpl;
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

/* ---------------- pause menu and save/load ---------------- */

static int pauseOpen;
static int pauseSel;
static uint32_t pendingSeed;

/* 0 = title menu, 1 = playing. The world only exists after the first
 * NEW GAME / LOAD GAME. */
static int gameState;
static int titleSel;
static int worldReady;

static GLuint menuBackTex, menuTitleTex, pausePanelTex;

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
}

#define SAVE_PATH DATA_ROOT "/save.dat"

/* The map, doors, item spawns and 173 placement are all deterministic
 * from the seed, so a save only records the seed plus mutable state. */
static int saveGame(void) {
    FILE *f = fopen(SAVE_PATH, "w");
    if (!f) return 0;
    fprintf(f, "SCPVITA1\n");
    fprintf(f, "seed=%u\n", mapSeed);
    fprintf(f, "player=%f %f %f %f %f\n", camPos[0], camPos[1], camPos[2],
            camYaw, camPitch);
    fprintf(f, "vitals=%f %f\n", blinkTimer, stamina);
    fprintf(f, "npc173=%f %f %f %f %d\n", npc173Pos[0], npc173Pos[1],
            npc173Pos[2], npc173YawDeg, npc173Active);
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

static int loadGame(void) {
    FILE *f = fopen(SAVE_PATH, "r");
    if (!f) return 0;
    char line[1024];
    if (!fgets(line, sizeof(line), f)
        || strncmp(line, "SCPVITA1", 8) != 0) {
        fclose(f);
        return 0;
    }
    uint32_t seed = 0;
    float px = 0, py = 0, pz = 0, yaw = 0, pitch = 0, bt = 100, st = 100;
    float nx = 0, ny = 0, nz = 0, nyaw = 0;
    int nact = 1;
    static char invLine[640], takenLine[640], doorLine[640];
    invLine[0] = takenLine[0] = doorLine[0] = '\0';
    /* Parsed with strtof/strtoul: vitasdk newlib faults on scanf
     * float conversions. */
    while (fgets(line, sizeof(line), f)) {
        char *p;
        if (strncmp(line, "seed=", 5) == 0) {
            seed = (uint32_t)strtoul(line + 5, NULL, 10);
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
        } else if (strncmp(line, "npc173=", 7) == 0) {
            p = line + 7;
            nx = strtof(p, &p);
            ny = strtof(p, &p);
            nz = strtof(p, &p);
            nyaw = strtof(p, &p);
            nact = (int)strtol(p, &p, 10);
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

    regenerateMap(seed);
    camPos[0] = px;
    camPos[1] = py;
    camPos[2] = pz;
    camYaw = yaw;
    camPitch = pitch;
    velY = 0.0f;
    blinkTimer = bt;
    stamina = st;
    npc173Pos[0] = nx;
    npc173Pos[1] = ny;
    npc173Pos[2] = nz;
    npc173YawDeg = nyaw;
    if (!nact) npc173Active = 0;
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
        glTranslatef(d->x, 0.0f, d->z);
        glRotatef(-(float)d->angle, 0.0f, 1.0f, 0.0f);

        drawModelRT(&doorFrameRT);

        float slide = doorSlide(d);
        const ModelRT *p1 = d->heavy ? &heavy1RT : &doorPanelRT;
        const ModelRT *p2 = d->heavy ? &heavy2RT : &doorPanelRT;

        glPushMatrix();
        glTranslatef(slide, 0.0f, 0.0f);
        drawModelRT(p1);
        glPopMatrix();

        glPushMatrix();
        glRotatef(180.0f, 0.0f, 1.0f, 0.0f);
        glTranslatef(slide, 0.0f, 0.0f);
        drawModelRT(p2);
        glPopMatrix();

        /* Buttons on both sides; the 180-degree side rotation mirrors
         * the (+0.6, 0.7, -0.1) CreateDoor offset for side 1. */
        const ModelRT *btn = d->keycard > 0 ? &buttonKeycardRT : &buttonRT;
        for (int side = 0; side < 2; side++) {
            glPushMatrix();
            glRotatef((float)(side * 180), 0.0f, 1.0f, 0.0f);
            glTranslatef(0.6f * 256.0f, 0.7f * 256.0f, -0.1f * 256.0f);
            drawModelRT(btn);
            glPopMatrix();
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
    const int cols = 5;
    const int rows = MAX_INVENTORY / 5;
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

    for (int i = 0; i < MAX_INVENTORY; i++) {
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

/* A menu button in the game's style: black box, light border, caps
 * label; the selection brightens the border like the mouse-hover. */
static void drawMenuButton(float x, float y, float w, float h,
                           const char *label, int selected) {
    drawQuad(x, y, w, h, 0, 0.02f, 0.02f, 0.02f, 0.92f);
    float bc = selected ? 1.0f : 0.5f;
    drawQuad(x, y, w, 2, 0, bc, bc, bc, 1.0f);
    drawQuad(x, y + h - 2, w, 2, 0, bc, bc, bc, 1.0f);
    drawQuad(x, y, 2, h, 0, bc, bc, bc, 1.0f);
    drawQuad(x + w - 2, y, 2, h, 0, bc, bc, bc, 1.0f);
    glPushMatrix();
    glScalef(2.0f, 2.0f, 1.0f);
    float g = selected ? 1.0f : 0.78f;
    glColor4f(g, g, g, 1.0f);
    drawText((x + 18.0f) * 0.5f, (y + h * 0.5f - 7.0f) * 0.5f, label);
    glColor4f(1, 1, 1, 1);
    glPopMatrix();
}

/* Title screen: back.png fullscreen, the SCP text logo, and the
 * button column at Menu_Core.bb's design coordinates (1280x960
 * design space mapped onto 960x544). */
static void drawTitleMenu(void) {
    const float SX = SCREEN_W / 1280.0f, SY = SCREEN_H / 960.0f;
    if (menuBackTex) {
        drawTexQuad(0, 0, SCREEN_W, SCREEN_H, menuBackTex, 1.0f);
    } else {
        drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0, 1.0f);
    }
    if (menuTitleTex) {
        drawTexQuad(159.0f * SX, 170.0f * SY, 550.0f * SX, 60.0f * SY,
                    menuTitleTex, 1.0f);
    }

    char seedRow[48];
    snprintf(seedRow, sizeof(seedRow), "SEED: %u", pendingSeed);
    const char *rows[4] = { "NEW GAME", "LOAD GAME", seedRow, "QUIT" };
    float bx = 159.0f * SX, bw = 400.0f * SX, bh = 70.0f * SY;
    for (int i = 0; i < 4; i++) {
        float by = (286.0f + i * 95.0f) * SY;
        drawMenuButton(bx, by, bw, bh, rows[i], i == titleSel);
    }

    glPushMatrix();
    glScalef(2.0f, 2.0f, 1.0f);
    glColor4f(0.45f, 0.45f, 0.45f, 1.0f);
    drawText(10, (SCREEN_H - 24.0f) * 0.5f,
             "PS Vita port   dpad: select  X: confirm"
             "  seed: left/right (hold L x1000)");
    if (toastTimer > 0) {
        glColor4f(1.0f, 1.0f, 0.8f, 1.0f);
        drawText(240, 200, toastMsg);
    }
    glColor4f(1, 1, 1, 1);
    glPopMatrix();
}

/* Pause menu: the game's pause_menu.png panel with its button column
 * (PAUSED / RESUME / QUIT TO MENU labels from local.ini). */
static const char *PAUSE_ITEMS[4] = {
    "RESUME", "SAVE GAME", "LOAD GAME", "QUIT TO MENU",
};

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

    glPushMatrix();
    glScalef(2.0f, 2.0f, 1.0f);
    glColor4f(1, 1, 1, 1);
    drawText((px + 36.0f) * 0.5f, (py + 30.0f) * 0.5f, "PAUSED");
    glPopMatrix();

    for (int i = 0; i < 4; i++) {
        drawMenuButton(px + 36.0f, py + 84.0f + i * 78.0f, pw - 72.0f,
                       54.0f, PAUSE_ITEMS[i], i == pauseSel);
    }
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
        if (audioInit()) loadSounds();
        /* Boot to the title menu; the world is generated when the
         * player starts or loads a game. */
        pendingSeed = (uint32_t)(sceKernelGetProcessTimeWide() & 0xFFFFFF) | 1u;
        gameState = 0;
        titleSel = 0;
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

        /* ---- title menu state: no world simulation, just the menu ---- */
        if (haveData && gameState == 0) {
            if (inputHit(ACTION_SAVE) && titleSel > 0) titleSel--;
            if (inputDpadDownHit() && titleSel < 3) titleSel++;
            if (titleSel == 2) {
                uint32_t step = inputDown(ACTION_SPRINT) ? 1000u : 1u;
                if (inputHit(ACTION_LEAN_LEFT)) pendingSeed -= step;
                if (inputHit(ACTION_LEAN_RIGHT)) pendingSeed += step;
                if (pendingSeed == 0) pendingSeed = 1;
            }
            if (inputHit(ACTION_INTERACT)) {
                switch (titleSel) {
                    case 0: /* NEW GAME */
                    case 2: /* SEED row confirms too */
                        regenerateMap(pendingSeed);
                        gameState = 1;
                        worldReady = 1;
                        pauseOpen = 0;
                        break;
                    case 1: /* LOAD GAME */
                        if (loadGame()) {
                            snprintf(toastMsg, sizeof(toastMsg),
                                     "GAME LOADED");
                            gameState = 1;
                            worldReady = 1;
                            pauseOpen = 0;
                        } else {
                            snprintf(toastMsg, sizeof(toastMsg),
                                     "NO SAVE FOUND");
                        }
                        toastTimer = 150;
                        break;
                    case 3: /* QUIT */
                        running = 0;
                        break;
                }
            }
            /* Start returns to a game in progress (after QUIT TO MENU). */
            if (worldReady && inputHit(ACTION_MENU)) gameState = 1;

            if (toastTimer > 0) toastTimer--;
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            beginHud2D();
            drawTitleMenu();
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
            if (inputHit(ACTION_SAVE) && pauseSel > 0) pauseSel--;
            if (inputDpadDownHit() && pauseSel < 3) pauseSel++;
            if (inputHit(ACTION_INTERACT)) {
                switch (pauseSel) {
                    case 0: /* RESUME */
                        pauseOpen = 0;
                        break;
                    case 1: /* SAVE GAME */
                        snprintf(toastMsg, sizeof(toastMsg), "%s",
                                 saveGame() ? "GAME SAVED" : "SAVE FAILED");
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
                        pauseOpen = 0;
                        gameState = 0;
                        titleSel = 0;
                        pendingSeed = mapSeed;
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
            if (inputHit(ACTION_LEAN_RIGHT) && invSel < MAX_INVENTORY - 1) {
                invSel++;
            }
            if (inputHit(ACTION_SAVE) && invSel >= 5) invSel -= 5; /* up */
            if (inputDpadDownHit() && invSel + 5 < MAX_INVENTORY) {
                invSel += 5;
            }
            /* Cross reads the selected document, if any. */
            if (inputHit(ACTION_INTERACT) && invSel < (int)inventoryCount) {
                const char *doc = ITEM_TEMPLATES[inventory[invSel]].docImage;
                if (doc[0]) openDocument(doc);
            }
        } else if (!pauseOpen) {
            if (inputHit(ACTION_LEAN_LEFT)) fogOn = !fogOn;
            if (inputHit(ACTION_USE_ITEM)) cullMode = (cullMode + 1) % 3;
            if (inputHit(ACTION_SAVE)) { /* D-pad up: walk/fly */
                walkMode = !walkMode;
                velY = 0.0f;
            }
        }
        /* pausedAtFrameStart: a Cross that activated a menu entry this
         * frame must not also interact with the world. */
        if (haveData && !invOpen && !pauseOpen && !pausedAtFrameStart
            && inputHit(ACTION_INTERACT)) {
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
            itemSpin += 1.0f;
            if (itemSpin >= 360.0f) itemSpin -= 360.0f;
        }
        if (deathTimer > 0) {
            deathTimer--;
            if (deathTimer == 0) {
                spawnPlayer();
                npc173Pos[0] = npc173SpawnX;
                npc173Pos[2] = npc173SpawnZ;
                npc173WasMoving = 0;
                snprintf(toastMsg, sizeof(toastMsg),
                         "YOU WERE KILLED BY SCP-173");
                toastTimer = 240;
            }
        }
        if (toastTimer > 0) toastTimer--;

        StickState look = inputLook();
        StickState move = inputMove();
        if (invOpen || pauseOpen || deathTimer > 0) {
            /* Freeze the camera and player while a menu is open or the
             * death screen is playing. */
            look.x = look.y = 0.0f;
            move.x = move.y = 0.0f;
        }
        camYaw += look.x * 0.04f;
        camPitch += look.y * 0.03f;
        if (camPitch > 1.5f) camPitch = 1.5f;
        if (camPitch < -1.5f) camPitch = -1.5f;

        if (haveData) {
            updateActiveRooms(camPos);
        }

        float fwdX = sinf(camYaw), fwdZ = -cosf(camYaw);
        if (walkMode && haveData) {
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
                stamina += 0.25f;
                if (stamina > 100.0f) stamina = 100.0f;
                if (staminaBlocked && stamina > 25.0f) staminaBlocked = 0;
            }

            /* Blink: meter drains; empty or R closes the eyes. New
             * blinks don't start while a menu is open. */
            if (!invOpen && !pauseOpen) blinkTimer -= 100.0f / 600.0f;
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
                    camPos[1] = target;
                    velY = 0.0f;
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
            drawDoors(camPos);
            drawItems(camPos);
            draw173(camPos);
            glDisable(GL_CULL_FACE);
            for (int i = 0; i < activeCount; i++) {
                drawRoomBatches(activeRooms[i], 1);
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
        if (haveData && walkMode && !invOpen && !pauseOpen) {
            drawMeter(20, SCREEN_H - 60, hudBlinkIcon, hudBlinkBar,
                      blinkTimer / 100.0f);
            drawMeter(20, SCREEN_H - 110, hudSprintIcon, hudStaminaBar,
                      stamina / 100.0f);
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
