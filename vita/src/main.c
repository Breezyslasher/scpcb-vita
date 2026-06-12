/*
 * SCP - Containment Breach Ultimate Edition Reborn — PS Vita port.
 *
 * Milestone 3: VitaGL room viewer. Loads .rmesh rooms from the data
 * package (ux0:data/scpcb-ue/), uploads their textures (capped to the
 * memory budget), and renders geometry with lightmaps. Left stick
 * moves, right stick looks, D-pad left/right cycles rooms.
 *
 * Requires libshacccg.suprx (vitaGL runtime shader compiler) — see
 * vita/README.md.
 */

#include <dirent.h>
#include <math.h>
#include <stddef.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include "formats/rmesh.h"
#include "formats/texture.h"
#include "game/collision.h"
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

/* ---------------- texture cache ---------------- */

typedef struct {
    char *name;
    GLuint handle; /* 0 = lookup failed (cached negative) */
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

/* ---------------- prop model cache (per room) ---------------- */

typedef struct {
    char *name;
    B3DModel *model; /* NULL = load failed (cached negative) */
} CachedModel;

static CachedModel *modelCache;
static unsigned modelCacheCount;

static B3DModel *propModelGet(const char *rawName) {
    /* The game strips a trailing .b3d, then appends .b3d
     * (Map_Core.bb "mesh" entity). Use the final path component. */
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

/* ---------------- room list ---------------- */

static char **roomList;
static unsigned roomCount;

static void scanRooms(void) {
    DIR *d = opendir(MAP_DIR);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcasecmp(dot, ".rmesh") != 0) continue;
        char **grown = (char **)realloc(roomList,
                                        (roomCount + 1) * sizeof(char *));
        if (!grown) break;
        roomList = grown;
        roomList[roomCount] = strdup(de->d_name);
        if (roomList[roomCount]) roomCount++;
    }
    closedir(d);
}

/* ---------------- loaded room (GL side) ---------------- */

typedef struct {
    GLuint diffuse;
    GLuint lightmap;
    GLuint vbo;
    GLuint ibo;
} BatchGL;

static Scene *scene;
static BatchGL *batchTex;
static char roomName[256];
static char statusLine[512];

static float camPos[3];
static float camYaw, camPitch;
static float moveSpeed;
static float roomDiag = 100.0f;

/* Player metrics in raw mesh units: world units * 2048/8 (RoomScale).
 * Collider from Loading_Core.bb (EntityRadius 0.15/0.30), walk speed
 * from Main_Core.bb (0.018/frame, sprint x2.5). */
#define PLAYER_RADIUS 38.0f
#define EYE_HEIGHT 140.0f
#define STEP_SLACK 25.0f
#define WALK_SPEED 4.6f
#define SPRINT_MULT 2.5f
#define GRAVITY 0.5f
#define TERMINAL_FALL 20.0f

static CollisionWorld *colWorld;
static int walkMode = 0;
static float velY = 0.0f;

/* Debug toggles for on-device diagnosis (see HUD). */
static int cullMode = 0;  /* 0 = off, 1 = CW front, 2 = CCW; assets carry
                           * mixed winding, so default off */
static int fogOn = 1;

static void unloadRoom(void) {
    if (batchTex && scene) {
        for (uint32_t i = 0; i < scene->batchCount; i++) {
            if (batchTex[i].vbo) glDeleteBuffers(1, &batchTex[i].vbo);
            if (batchTex[i].ibo) glDeleteBuffers(1, &batchTex[i].ibo);
        }
    }
    free(batchTex);
    batchTex = NULL;
    sceneFree(scene);
    scene = NULL;
    collisionFree(colWorld);
    colWorld = NULL;
    textureCacheClear();
    modelCacheClear();
}

static void loadRoom(unsigned index) {
    unloadRoom();
    if (roomCount == 0) return;
    snprintf(roomName, sizeof(roomName), "%s", roomList[index % roomCount]);

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", MAP_DIR, roomName);
    char err[128];
    RMesh *mesh = rmeshLoadFile(path, err, sizeof(err));
    if (!mesh) {
        snprintf(statusLine, sizeof(statusLine), "%s: load failed (%s)",
                 roomName, err);
        return;
    }
    scene = sceneBuild(mesh);
    if (!scene) {
        rmeshFree(mesh);
        snprintf(statusLine, sizeof(statusLine), "%s: out of memory", roomName);
        return;
    }

    /* Place the room's B3D props ("mesh" entities). Entity coords are
     * already in mesh-vertex space: the game multiplies them by
     * RoomScale but also scales the whole room object by RoomScale,
     * and prop scale is local under that parent (Rooms_Core.bb 4226,
     * Map_Core.bb CreateProp). */
    unsigned props = 0, propFails = 0;
    for (uint32_t i = 0; i < mesh->entityCount; i++) {
        const RMeshEntity *e = &mesh->entities[i];
        if (e->type != RMESH_ENTITY_PROP || !e->u.prop.file) continue;
        B3DModel *model = propModelGet(e->u.prop.file);
        if (!model) {
            propFails++;
            continue;
        }
        float pos[3] = { e->x, e->y, e->z };
        float euler[3] = { e->u.prop.pitch, e->u.prop.yaw, e->u.prop.roll };
        float scl[3] = { e->u.prop.scaleX, e->u.prop.scaleY, e->u.prop.scaleZ };
        if (sceneAppendB3D(scene, model, pos, euler, scl, e->u.prop.texture)) {
            props++;
        }
    }
    colWorld = collisionBuild(scene, mesh);
    rmeshFree(mesh);
    velY = 0.0f;

    batchTex = (BatchGL *)calloc(scene->batchCount ? scene->batchCount : 1,
                                 sizeof(BatchGL));
    unsigned missing = 0;
    unsigned long long verts = 0;
    for (uint32_t i = 0; i < scene->batchCount; i++) {
        const SceneBatch *b = &scene->batches[i];
        verts += b->vertexCount;
        if (batchTex) {
            batchTex[i].diffuse = textureGet(b->diffuseName);
            batchTex[i].lightmap = textureGet(b->lightmapName);
            if (b->diffuseName && !batchTex[i].diffuse) {
                missing++;
            }
            /* Static geometry goes into VBOs once; drawing from
             * client-side arrays re-uploads every frame and was the
             * cause of single-digit FPS. */
            glGenBuffers(1, &batchTex[i].vbo);
            glBindBuffer(GL_ARRAY_BUFFER, batchTex[i].vbo);
            glBufferData(GL_ARRAY_BUFFER,
                         (GLsizeiptr)(b->vertexCount * sizeof(SceneVertex)),
                         b->vertices, GL_STATIC_DRAW);
            glGenBuffers(1, &batchTex[i].ibo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, batchTex[i].ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         (GLsizeiptr)(b->indexCount * sizeof(uint16_t)),
                         b->indices, GL_STATIC_DRAW);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    /* Start the camera in the middle of the room; scale movement to
     * room size so units don't matter. */
    float size[3];
    for (int i = 0; i < 3; i++) {
        camPos[i] = (scene->boundsMin[i] + scene->boundsMax[i]) * 0.5f;
        size[i] = scene->boundsMax[i] - scene->boundsMin[i];
    }
    float diag = sqrtf(size[0] * size[0] + size[1] * size[1] + size[2] * size[2]);
    roomDiag = diag > 0.0f ? diag : 100.0f;
    moveSpeed = roomDiag / 240.0f;
    camYaw = 0.0f;
    camPitch = 0.0f;

    snprintf(statusLine, sizeof(statusLine),
             "%s  batches=%u verts=%llu texMissing=%u props=%u/%u", roomName,
             scene->batchCount, verts, missing, props, props + propFails);
}

/* ---------------- rendering ---------------- */

static void setPerspective(void) {
    /* Room-relative depth range keeps depth precision sane regardless
     * of the file's units. */
    float zNear = roomDiag / 2000.0f;
    float zFar = roomDiag * 4.0f;
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
        /* Blitz3D/DirectX winding: clockwise faces are front. */
        glFrontFace(cullMode == 1 ? GL_CW : GL_CCW);
    }

    if (fogOn) {
        /* The game runs linear black fog from ~0.1 to ~6 world units
         * against rooms ~8 units wide; scale the same ratios to the
         * room diagonal. */
        GLfloat fogColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        glEnable(GL_FOG);
        glFogf(GL_FOG_MODE, GL_LINEAR);
        glFogfv(GL_FOG_COLOR, fogColor);
        glFogf(GL_FOG_START, roomDiag * 0.05f);
        glFogf(GL_FOG_END, roomDiag * 1.1f);
    } else {
        glDisable(GL_FOG);
    }
}

#define VTX_OFF(field) ((const void *)offsetof(SceneVertex, field))

static void drawBatches(int alphaPass) {
    if (!batchTex) return;
    for (uint32_t i = 0; i < scene->batchCount; i++) {
        const SceneBatch *b = &scene->batches[i];
        if (b->alphaClip != alphaPass) continue;

        glBindBuffer(GL_ARRAY_BUFFER, batchTex[i].vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, batchTex[i].ibo);
        glVertexPointer(3, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(x));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(SceneVertex), VTX_OFF(r));

        /* Pass 1: diffuse * vertex color. */
        GLuint diffuse = batchTex[i].diffuse;
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

        /* Pass 2: additive lightmap (Blitz TextureBlend 3). */
        GLuint lightmap = batchTex[i].lightmap;
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

static void drawText(float x, float y, const char *text) {
    static char buf[64000]; /* 16 bytes/vertex, 4 vertices/quad: 1000 quads */
    int quads = stb_easy_font_print(x, y, (char *)text, NULL, buf, sizeof(buf));

    /* stb_easy_font emits quads; draw as triangles for safety. */
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
    glScalef(2.0f, 2.0f, 1.0f); /* easy_font is tiny at native scale */
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
    glDepthFunc(GL_LEQUAL); /* lightmap pass re-draws at equal depth */
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    inputInit();
    scanRooms();

    unsigned roomIndex = 0;
    if (roomCount > 0) {
        loadRoom(roomIndex);
    } else {
        snprintf(statusLine, sizeof(statusLine),
                 "No rooms found. Install the data package to " DATA_ROOT "/");
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
        if (roomCount > 0) {
            if (inputHit(ACTION_LEAN_RIGHT)) {
                roomIndex = (roomIndex + 1) % roomCount;
                loadRoom(roomIndex);
            }
            if (inputHit(ACTION_LEAN_LEFT)) {
                roomIndex = (roomIndex + roomCount - 1) % roomCount;
                loadRoom(roomIndex);
            }
        }

        /* Camera: right stick looks, left stick moves on the view plane,
         * sprint (L) doubles speed, crouch (Circle) moves down, blink (R)
         * moves up. */
        StickState look = inputLook();
        StickState move = inputMove();
        camYaw += look.x * 0.04f;
        camPitch += look.y * 0.03f;
        if (camPitch > 1.5f) camPitch = 1.5f;
        if (camPitch < -1.5f) camPitch = -1.5f;

        if (inputHit(ACTION_INTERACT)) {
            walkMode = !walkMode;
            velY = 0.0f;
        }

        float fwdX = sinf(camYaw), fwdZ = -cosf(camYaw);
        if (walkMode && colWorld) {
            /* Walk: game speeds, gravity, slide along geometry. */
            float speed = WALK_SPEED
                        * (inputDown(ACTION_SPRINT) ? SPRINT_MULT : 1.0f);
            camPos[0] += (fwdX * -move.y + cosf(camYaw) * move.x) * speed;
            camPos[2] += (fwdZ * -move.y + sinf(camYaw) * move.x) * speed;

            velY -= GRAVITY;
            if (velY < -TERMINAL_FALL) velY = -TERMINAL_FALL;
            camPos[1] += velY;

            int pushedUp = 0;
            collisionSpherePush(colWorld, camPos, PLAYER_RADIUS, &pushedUp);

            float hitY;
            if (collisionRayDown(colWorld, camPos, EYE_HEIGHT + STEP_SLACK,
                                 &hitY)) {
                float target = hitY + EYE_HEIGHT;
                if (camPos[1] <= target) {
                    camPos[1] = target;
                    velY = 0.0f;
                }
            }
            if (pushedUp && velY < 0.0f) velY = 0.0f;
        } else {
            /* Fly: room-relative speed, vertical on R/Circle. */
            float speed = moveSpeed
                        * (inputDown(ACTION_SPRINT) ? 2.0f : 1.0f);
            camPos[0] += (fwdX * -move.y + cosf(camYaw) * move.x) * speed;
            camPos[2] += (fwdZ * -move.y + sinf(camYaw) * move.x) * speed;
            if (inputDown(ACTION_BLINK)) camPos[1] += speed;
            if (inputDown(ACTION_CROUCH)) camPos[1] -= speed;
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (scene) {
            setPerspective();
            applyDebugState();
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glRotatef(camPitch * 180.0f / 3.14159265f, 1, 0, 0);
            glRotatef(camYaw * 180.0f / 3.14159265f, 0, 1, 0);
            glTranslatef(-camPos[0], -camPos[1], -camPos[2]);

            glColor4f(1, 1, 1, 1);
            drawBatches(0); /* opaque */
            /* The game flip-copies alpha surfaces (double-sided). */
            glDisable(GL_CULL_FACE);
            drawBatches(1); /* alpha-clipped */
        }

        static const char *cullNames[3] = { "off", "cw", "ccw" };
        char controls[200];
        snprintf(controls, sizeof(controls),
                 "fps=%.0f  x: %s  dpad: room %u/%u  tri: fog=%s  sq: cull=%s"
                 "  start x3: exit", fps, walkMode ? "WALK" : "fly",
                 roomCount ? roomIndex + 1 : 0, roomCount,
                 fogOn ? "on" : "off", cullNames[cullMode]);
        drawHud(statusLine, controls);

        vglSwapBuffers(GL_FALSE);
    }

    unloadRoom();
    sceKernelExitProcess(0);
    return 0;
}
