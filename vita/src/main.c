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

#include "formats/b3d.h"
#include "formats/rmesh.h"
#include "formats/texture.h"
#include "game/collision.h"
#include "game/doors.h"
#include "game/mapgen.h"
#include "input.h"
#include "render/scene.h"

#define STB_EASY_FONT_IMPLEMENTATION
#include "render/stb_easy_font.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define TEXTURE_CAP 256

#define DATA_ROOT "ux0:data/scpcb-ue"
#define MAP_DIR DATA_ROOT "/GFX/Map"
#define MAP_TEXTURES_DIR DATA_ROOT "/GFX/Map/Textures"
#define PROPS_DIR DATA_ROOT "/GFX/Map/Props"
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
    const char *dirs[3] = { MAP_DIR, MAP_TEXTURES_DIR, PROPS_DIR };
    char path[1024];
    if (textureResolve(name, dirs, 3, path, sizeof(path))) {
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
    const char *dirs[1] = { PROPS_DIR };
    char path[1024];
    if (textureResolve(name, dirs, 1, path, sizeof(path))) {
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
static char statusLine[256];

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

static ModelRT doorFrameRT, doorPanelRT, heavy1RT, heavy2RT;

/* Build a standalone renderable from a Props b3d. Targets > 0 scale
 * the model to that extent on the axis (CreateDoor sizes the default
 * door panel to 203 x 313 x 15 raw units); 0 keeps model units. */
static void buildModelRT(ModelRT *rt, const char *name, float tw, float th,
                         float td) {
    memset(rt, 0, sizeof(*rt));
    rt->scale[0] = rt->scale[1] = rt->scale[2] = 1.0f;
    B3DModel *model = propModelGet(name);
    if (!model) return;

    RMesh empty;
    memset(&empty, 0, sizeof(empty));
    rt->scene = sceneBuild(&empty);
    if (!rt->scene) return;
    float pos[3] = { 0, 0, 0 }, euler[3] = { 0, 0, 0 }, scl[3] = { 1, 1, 1 };
    if (!sceneAppendB3D(rt->scene, model, pos, euler, scl, NULL)) return;

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
    buildModelRT(&doorPanelRT, "Door01.b3d", 203.0f, 313.0f, 15.0f);
    buildModelRT(&heavy1RT, "HeavyDoor1.b3d", 0, 0, 0);
    buildModelRT(&heavy2RT, "HeavyDoor2.b3d", 0, 0, 0);
    buildModelRT(&doorFrameRT, "DoorFrame.b3d", 0, 0, 0);
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

        glPopMatrix();
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

static void drawHud(const char *line1, const char *line2) {
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
        if (inputHit(ACTION_INVENTORY)) fogOn = !fogOn;
        if (inputHit(ACTION_USE_ITEM)) cullMode = (cullMode + 1) % 3;
        if (inputHit(ACTION_SAVE)) { /* D-pad up: walk/fly */
            walkMode = !walkMode;
            velY = 0.0f;
        }
        if (haveData && inputHit(ACTION_INTERACT)) {
            doorsToggleNearest(&doors, camPos, 400.0f);
        }
        if (haveData) {
            doorsUpdate(&doors);
        }
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
            float speed = WALK_SPEED;
            if (crouched) {
                speed *= CROUCH_MULT;
            } else if (inputDown(ACTION_SPRINT)) {
                speed *= SPRINT_MULT;
            }
            camPos[0] += (fwdX * -move.y + cosf(camYaw) * move.x) * speed;
            camPos[2] += (fwdZ * -move.y + sinf(camYaw) * move.x) * speed;

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
                 "fps=%.0f  x: door  up: %s  dpad>: new map  tri: fog=%s"
                 "  sq: cull=%d  start x3: exit", fps,
                 walkMode ? "WALK" : "fly", fogOn ? "on" : "off", cullMode);
        drawHud(line1, line2);

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
