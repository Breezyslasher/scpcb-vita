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
    const char *dirs[7] = { MAP_DIR, MAP_TEXTURES_DIR, PROPS_DIR, ITEMS_DIR,
                            ITEMS_HUD_DIR, HUD_DIR, INV_ICONS_DIR };
    char path[1024];
    if (textureResolve(name, dirs, 7, path, sizeof(path))) {
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
    const char *dirs[2] = { PROPS_DIR, ITEMS_DIR };
    char path[1024];
    if (textureResolve(name, dirs, 2, path, sizeof(path))) {
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
    int level = 0;
    if (sscanf(n, "Level %d", &level) == 1
        && (strstr(n, "Key Card") || strstr(n, "key Card"))) {
        return level;
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

/* Textured 2D quad in HUD space (ortho already set, depth off). */
static void drawTexQuad(float x, float y, float w, float h, GLuint tex,
                        float alpha) {
    GLfloat verts[12] = { x, y, x + w, y, x + w, y + h,
                          x, y, x + w, y + h, x, y + h };
    GLfloat uvs[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glColor4f(1.0f, 1.0f, 1.0f, alpha);
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
        loadHudTextures();
        if (audioInit()) loadSounds();
        regenerateMap((uint32_t)(sceKernelGetProcessTimeWide() & 0xFFFFFF) | 1u);
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

        if (inputHit(ACTION_MENU)) {
            if (++menuHits >= 3) running = 0;
        }
        if (inputHit(ACTION_INVENTORY)) invOpen = !invOpen;
        if (inputHit(ACTION_LEAN_LEFT)) fogOn = !fogOn;
        if (inputHit(ACTION_USE_ITEM)) cullMode = (cullMode + 1) % 3;
        if (inputHit(ACTION_SAVE)) { /* D-pad up: walk/fly */
            walkMode = !walkMode;
            velY = 0.0f;
        }
        if (haveData && inputHit(ACTION_INTERACT)) {
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
        if (haveData) {
            doorsUpdate(&doors);
            itemSpin += 1.0f;
            if (itemSpin >= 360.0f) itemSpin -= 360.0f;
        }
        if (toastTimer > 0) toastTimer--;
        if (haveData && inputHit(ACTION_LEAN_RIGHT)) {
            regenerateMap(mapSeed * 7919u + 17u);
        }

        StickState look = inputLook();
        StickState move = inputMove();
        camYaw += look.x * 0.04f;
        camPitch += look.y * 0.03f;
        if (camPitch > 1.5f) camPitch = 1.5f;
        if (camPitch < -1.5f) camPitch = -1.5f;

        if (haveData) {
            updateActiveRooms(camPos);
        }

        float fwdX = sinf(camYaw), fwdZ = -cosf(camYaw);
        if (walkMode && haveData) {
            int crouched = inputDown(ACTION_CROUCH);
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

            /* Blink: meter drains; empty or R closes the eyes. */
            blinkTimer -= 100.0f / 600.0f; /* ~10 s */
            if (inputHit(ACTION_BLINK)) blinkTimer = 0.0f;
            if (blinkTimer <= 0.0f && blinkFrames == 0) blinkFrames = 18;
            if (blinkFrames > 0) {
                blinkFrames--;
                if (blinkFrames == 0) blinkTimer = 100.0f;
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
            float speed = 30.0f * (inputDown(ACTION_SPRINT) ? 3.0f : 1.0f);
            camPos[0] += (fwdX * -move.y + cosf(camYaw) * move.x) * speed;
            camPos[2] += (fwdZ * -move.y + sinf(camYaw) * move.x) * speed;
            if (inputDown(ACTION_BLINK)) camPos[1] += speed;
            if (inputDown(ACTION_CROUCH)) camPos[1] -= speed;
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
                 "  up: %s  start x3: exit", fps, audioStatus(),
                 audioSoundCount(), audioLoadFopenFails(),
                 audioLoadDecodeFails(), walkMode ? "WALK" : "fly");
        char invText[512] = "";
        if (invOpen) {
            snprintf(invText, sizeof(invText), "INVENTORY (%u/%u)\n",
                     inventoryCount, (unsigned)MAX_INVENTORY);
            for (unsigned ii = 0; ii < inventoryCount; ii++) {
                strncat(invText, ITEM_TEMPLATES[inventory[ii]].name,
                        sizeof(invText) - strlen(invText) - 2);
                strncat(invText, "\n", sizeof(invText) - strlen(invText) - 1);
            }
            if (inventoryCount == 0) {
                strncat(invText, "(empty)",
                        sizeof(invText) - strlen(invText) - 1);
            }
        }
        drawHud(line1, line2, toastTimer > 0 ? toastMsg : NULL,
                invOpen ? invText : NULL);

        /* Vitals meters, inventory icons and the blink blackout share
         * the HUD's ortho matrices set up by drawHud, but need flat 2D
         * state again (drawHud restores arrays/depth on exit). */
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_FOG);
        glDisable(GL_CULL_FACE);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        if (haveData && walkMode) {
            drawMeter(20, SCREEN_H - 60, hudBlinkIcon, hudBlinkBar,
                      blinkTimer / 100.0f);
            drawMeter(20, SCREEN_H - 110, hudSprintIcon, hudStaminaBar,
                      stamina / 100.0f);
        }
        if (invOpen) {
            for (unsigned ii = 0; ii < inventoryCount; ii++) {
                const char *icon = ITEM_TEMPLATES[inventory[ii]].invIcon;
                float ix = 320.0f + (ii % 5) * 90.0f;
                float iy = 140.0f + (ii / 5) * 90.0f;
                drawTexQuad(ix, iy, 80, 80, 0, 0.5f);
                if (icon[0]) {
                    drawTexQuad(ix + 8, iy + 8, 64, 64, textureGet(icon),
                                1.0f);
                }
            }
        }
        if (blinkFrames > 0) {
            drawTexQuad(0, 0, SCREEN_W, SCREEN_H, 0, 1.0f);
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
