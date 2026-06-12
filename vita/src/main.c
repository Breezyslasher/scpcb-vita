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
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include "formats/rmesh.h"
#include "formats/texture.h"
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
    const char *dirs[2] = { MAP_DIR, MAP_TEXTURES_DIR };
    char path[1024];
    if (textureResolve(name, dirs, 2, path, sizeof(path))) {
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
    for (unsigned i = 0; i < texCacheCount; i++) {
        if (texCache[i].handle) glDeleteTextures(1, &texCache[i].handle);
        free(texCache[i].name);
    }
    free(texCache);
    texCache = NULL;
    texCacheCount = 0;
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
} BatchTextures;

static Scene *scene;
static BatchTextures *batchTex;
static char roomName[256];
static char statusLine[512];

static float camPos[3];
static float camYaw, camPitch;
static float moveSpeed;

static void unloadRoom(void) {
    free(batchTex);
    batchTex = NULL;
    sceneFree(scene);
    scene = NULL;
    textureCacheClear();
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
    rmeshFree(mesh);
    if (!scene) {
        snprintf(statusLine, sizeof(statusLine), "%s: out of memory", roomName);
        return;
    }

    batchTex = (BatchTextures *)calloc(scene->batchCount ? scene->batchCount : 1,
                                       sizeof(BatchTextures));
    unsigned missing = 0;
    unsigned long long verts = 0;
    for (uint32_t i = 0; i < scene->batchCount; i++) {
        verts += scene->batches[i].vertexCount;
        if (batchTex) {
            batchTex[i].diffuse = textureGet(scene->batches[i].diffuseName);
            batchTex[i].lightmap = textureGet(scene->batches[i].lightmapName);
            if (scene->batches[i].diffuseName && !batchTex[i].diffuse) {
                missing++;
            }
        }
    }

    /* Start the camera in the middle of the room; scale movement to
     * room size so units don't matter. */
    float size[3];
    for (int i = 0; i < 3; i++) {
        camPos[i] = (scene->boundsMin[i] + scene->boundsMax[i]) * 0.5f;
        size[i] = scene->boundsMax[i] - scene->boundsMin[i];
    }
    float diag = sqrtf(size[0] * size[0] + size[1] * size[1] + size[2] * size[2]);
    moveSpeed = diag > 0.0f ? diag / 240.0f : 0.05f;
    camYaw = 0.0f;
    camPitch = 0.0f;

    snprintf(statusLine, sizeof(statusLine),
             "%s  batches=%u verts=%llu texMissing=%u", roomName,
             scene->batchCount, verts, missing);
}

/* ---------------- rendering ---------------- */

static void setPerspective(void) {
    float zNear = 0.05f, zFar = 1000.0f;
    float fovY = 60.0f * 3.14159265f / 180.0f;
    float top = zNear * tanf(fovY * 0.5f);
    float right = top * ((float)SCREEN_W / (float)SCREEN_H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-right, right, -top, top, zNear, zFar);
}

static void drawBatches(int alphaPass) {
    for (uint32_t i = 0; i < scene->batchCount; i++) {
        const SceneBatch *b = &scene->batches[i];
        if (b->alphaClip != alphaPass) continue;

        glVertexPointer(3, GL_FLOAT, sizeof(SceneVertex), &b->vertices[0].x);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(SceneVertex),
                       &b->vertices[0].r);

        /* Pass 1: diffuse * vertex color. */
        GLuint diffuse = batchTex ? batchTex[i].diffuse : 0;
        if (diffuse) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, diffuse);
            glTexCoordPointer(2, GL_FLOAT, sizeof(SceneVertex),
                              &b->vertices[0].du);
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        if (alphaPass) {
            glEnable(GL_ALPHA_TEST);
            glAlphaFunc(GL_GREATER, 0.5f);
        }
        glDrawElements(GL_TRIANGLES, (GLsizei)b->indexCount,
                       GL_UNSIGNED_SHORT, b->indices);
        if (alphaPass) {
            glDisable(GL_ALPHA_TEST);
        }

        /* Pass 2: additive lightmap (Blitz TextureBlend 3). */
        GLuint lightmap = batchTex ? batchTex[i].lightmap : 0;
        if (lightmap) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, lightmap);
            glTexCoordPointer(2, GL_FLOAT, sizeof(SceneVertex),
                              &b->vertices[0].lu);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glDepthFunc(GL_LEQUAL);
            glDrawElements(GL_TRIANGLES, (GLsizei)b->indexCount,
                           GL_UNSIGNED_SHORT, b->indices);
            glDisable(GL_BLEND);
        }
    }
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
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    glEnable(GL_DEPTH_TEST);
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
    while (running) {
        inputUpdate();

        if (inputHit(ACTION_MENU)) {
            if (++menuHits >= 3) running = 0;
        }
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

        float speed = moveSpeed * (inputDown(ACTION_SPRINT) ? 2.0f : 1.0f);
        float fwdX = sinf(camYaw), fwdZ = -cosf(camYaw);
        camPos[0] += (fwdX * -move.y + cosf(camYaw) * move.x) * speed;
        camPos[2] += (fwdZ * -move.y + sinf(camYaw) * move.x) * speed;
        if (inputDown(ACTION_BLINK)) camPos[1] += speed;
        if (inputDown(ACTION_CROUCH)) camPos[1] -= speed;

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (scene) {
            setPerspective();
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glRotatef(camPitch * 180.0f / 3.14159265f, 1, 0, 0);
            glRotatef(camYaw * 180.0f / 3.14159265f, 0, 1, 0);
            glTranslatef(-camPos[0], -camPos[1], -camPos[2]);

            glColor4f(1, 1, 1, 1);
            drawBatches(0); /* opaque */
            drawBatches(1); /* alpha-clipped */
        }

        char controls[160];
        snprintf(controls, sizeof(controls),
                 "sticks: move/look  L: sprint  dpad </>: room %u/%u  "
                 "start x3: exit", roomCount ? roomIndex + 1 : 0, roomCount);
        drawHud(statusLine, controls);

        vglSwapBuffers(GL_FALSE);
    }

    unloadRoom();
    sceKernelExitProcess(0);
    return 0;
}
