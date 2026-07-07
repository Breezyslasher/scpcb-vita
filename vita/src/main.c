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
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include "audio/audio.h"
#include "video/video.h"
#include "formats/b3d.h"
#include "formats/rmesh.h"
#include "formats/texture.h"
#include "game/collision.h"
#include "game/doors.h"
#include "game/item_spawns.h"
#include "game/room_doors.h"
#include "game/room_fixtures.h"
#include "game/room_decals.h"
#include "game/room_cameras.h"
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
/* Shown in the debug HUD so a stale VPK install is instantly visible. */
#define PORT_BUILD_TAG "perf2"

/* Diagnostic switch: set to 1 to skip ALL video playback (boot clips
 * and intro). The diag2-novid device test proved the video player was
 * NOT the cause of the black world, so videos are back on. */
#define DIAG_DISABLE_VIDEOS 0

/* Render/load diagnostics -> ux0:data/scpcb-ue/render_log.txt.
 * Flushed per line so a crash keeps it. */
static void plog(const char *fmt, ...) {
    static FILE *lf;
    if (!lf) lf = fopen(DATA_ROOT "/render_log.txt", "w");
    if (!lf) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(lf, fmt, ap);
    va_end(ap);
    fputc('\n', lf);
    fflush(lf);
}

static int tplFailCount; /* room templates currently in a failed state */

#define MAP_DIR DATA_ROOT "/GFX/Map"
#define MAP_TEXTURES_DIR DATA_ROOT "/GFX/Map/Textures"
#define PROPS_DIR DATA_ROOT "/GFX/Map/Props"
#define ITEMS_DIR DATA_ROOT "/GFX/Items"
#define ITEMS_HUD_DIR DATA_ROOT "/GFX/Items/HUD Textures"
#define HUD_DIR DATA_ROOT "/GFX/HUD"
#define INV_ICONS_DIR DATA_ROOT "/GFX/Items/Inventory Icons"
#define NPCS_DIR DATA_ROOT "/GFX/NPCs"
#define DECALS_DIR DATA_ROOT "/GFX/Decals"
#define OVERLAYS_DIR DATA_ROOT "/GFX/Overlays"
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
static int texFailCount; /* textures that resolved but failed to load */

/* Upload a decoded image (NULL = failed/unresolved -> cached as 0 so
 * it is not retried) and record it under `name`. GL thread only. */
static GLuint textureCacheInsert(const char *name, TextureImage *img) {
    GLuint handle = 0;
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

static GLuint textureGet(const char *name) {
    if (!name) return 0;
    for (unsigned i = 0; i < texCacheCount; i++) {
        if (strcmp(texCache[i].name, name) == 0) return texCache[i].handle;
    }

    GLuint handle = 0;
    const char *dirs[10] = { MAP_DIR, MAP_TEXTURES_DIR, PROPS_DIR, ITEMS_DIR,
                             ITEMS_HUD_DIR, HUD_DIR, INV_ICONS_DIR, NPCS_DIR,
                             DECALS_DIR, OVERLAYS_DIR };
    char path[1024];
    if (textureResolve(name, dirs, 10, path, sizeof(path))) {
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
        } else {
            texFailCount++;
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

/* Canonical cache key for a prop model reference: basename, forced to
 * a single ".b3d" suffix. */
static void propModelKey(const char *rawName, char *name, size_t nameLen) {
    const char *base = rawName;
    for (const char *p = rawName; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    snprintf(name, nameLen, "%s", base);
    size_t len = strlen(name);
    if (len > 4 && strcasecmp(name + len - 4, ".b3d") == 0) {
        name[len - 4] = '\0';
    }
    strncat(name, ".b3d", nameLen - strlen(name) - 1);
}

/* Record a loaded model (NULL = failed, cached so it is not retried). */
static void modelCacheInsert(const char *name, B3DModel *model) {
    CachedModel *grown = (CachedModel *)realloc(
        modelCache, (modelCacheCount + 1) * sizeof(CachedModel));
    if (grown) {
        modelCache = grown;
        modelCache[modelCacheCount].name = strdup(name);
        modelCache[modelCacheCount].model = model;
        if (modelCache[modelCacheCount].name) modelCacheCount++;
    }
}

static B3DModel *propModelGet(const char *rawName) {
    char name[256];
    propModelKey(rawName, name, sizeof(name));

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
    modelCacheInsert(name, model);
    return model;
}

/* ---------------- async asset loader ----------------
 * A low-priority worker decodes texture PNGs, parses prop B3Ds and
 * builds room meshes/scenes off the render thread. Measured on device
 * when this ran inline: 25-171 ms per texture batch, 8-209 ms per prop
 * and 50-210 ms per mesh parse - each one a blown 16.6 ms frame. The
 * render thread only uploads finished images (a couple of ms) and
 * inserts cache entries. Per-slot handshake: stage 0 (free, owned by
 * the render thread) -> 1 (queued, owned by the worker) -> 2 (done,
 * owned by the render thread again); barriers order the payload. */
enum { LOADJOB_TEXTURE = 1, LOADJOB_MODEL = 2, LOADJOB_ROOM = 3 };
#define LOADJOB_MAX 24

typedef struct {
    int type;
    char name[192];       /* cache key (texture/model) */
    char path[1024];      /* resolved file path */
    char err[96];         /* worker's failure reason */
    TextureImage *img;    /* LOADJOB_TEXTURE result */
    B3DModel *model;      /* LOADJOB_MODEL result */
    RMesh *mesh;          /* LOADJOB_ROOM results */
    Scene *scene;
    volatile int stage;
} LoadJob;

static LoadJob loadJobs[LOADJOB_MAX];
static volatile int loaderRunning;
static int loaderOk; /* thread alive: async paths permitted */

static int loaderLoop(SceSize argSize, void *argp) {
    (void)argSize;
    (void)argp;
    while (loaderRunning) {
        int worked = 0;
        for (int i = 0; i < LOADJOB_MAX; i++) {
            LoadJob *j = &loadJobs[i];
            if (j->stage != 1) continue;
            __sync_synchronize();
            if (j->type == LOADJOB_TEXTURE) {
                j->img = textureLoadFile(j->path, TEXTURE_CAP, j->err,
                                         sizeof(j->err));
            } else if (j->type == LOADJOB_MODEL) {
                j->model = b3dLoadFile(j->path, j->err, sizeof(j->err));
            } else {
                j->mesh = rmeshLoadFile(j->path, j->err, sizeof(j->err));
                j->scene = j->mesh ? sceneBuild(j->mesh) : NULL;
                if (j->mesh && !j->scene) {
                    rmeshFree(j->mesh);
                    j->mesh = NULL;
                    snprintf(j->err, sizeof(j->err), "sceneBuild failed");
                }
            }
            __sync_synchronize();
            j->stage = 2;
            worked = 1;
        }
        if (!worked) sceKernelDelayThread(4000);
    }
    return sceKernelExitDeleteThread(0);
}

/* Find a free slot and queue a job. Returns the slot or -1 (ring full).
 * Render thread only. */
static int loadJobPost(int type, const char *name, const char *path) {
    for (int i = 0; i < LOADJOB_MAX; i++) {
        LoadJob *j = &loadJobs[i];
        if (j->stage != 0) continue;
        j->type = type;
        snprintf(j->name, sizeof(j->name), "%s", name ? name : "");
        snprintf(j->path, sizeof(j->path), "%s", path);
        j->err[0] = '\0';
        j->img = NULL;
        j->model = NULL;
        j->mesh = NULL;
        j->scene = NULL;
        __sync_synchronize();
        j->stage = 1;
        return i;
    }
    return -1;
}

/* Is a texture/model job for this cache key already queued or done? */
static int loadJobFind(int type, const char *name) {
    for (int i = 0; i < LOADJOB_MAX; i++) {
        if (loadJobs[i].stage != 0 && loadJobs[i].type == type
            && strcmp(loadJobs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* Consume finished texture/model jobs: upload + cache-insert on the GL
 * thread. Room jobs are consumed by their owning template instead. At
 * most two uploads per frame so a burst of completions cannot stall. */
static void loaderPump(void) {
    int consumed = 0;
    for (int i = 0; i < LOADJOB_MAX && consumed < 2; i++) {
        LoadJob *j = &loadJobs[i];
        if (j->stage != 2 || j->type == LOADJOB_ROOM) continue;
        __sync_synchronize();
        if (j->type == LOADJOB_TEXTURE) {
            /* A blocking load may have raced the same name in; keep the
             * first entry. */
            int exists = 0;
            for (unsigned c = 0; c < texCacheCount; c++) {
                if (strcmp(texCache[c].name, j->name) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (exists) {
                if (j->img) textureFree(j->img);
            } else {
                if (!j->img) texFailCount++;
                textureCacheInsert(j->name, j->img);
            }
            j->img = NULL;
        } else {
            int exists = 0;
            for (unsigned c = 0; c < modelCacheCount; c++) {
                if (strcmp(modelCache[c].name, j->name) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (!exists) modelCacheInsert(j->name, j->model);
            j->model = NULL; /* cached, or dropped after a lost race */
        }
        j->stage = 0;
        consumed++;
    }
}

/* Async textureGet: cache hit returns the handle; a miss queues the
 * decode and reports pending (handle arrives via loaderPump). Falls
 * back to the synchronous path if the worker is unavailable. */
static GLuint textureGetAsync(const char *name, int *pending) {
    *pending = 0;
    if (!name) return 0;
    for (unsigned i = 0; i < texCacheCount; i++) {
        if (strcmp(texCache[i].name, name) == 0) return texCache[i].handle;
    }
    if (!loaderOk) return textureGet(name);
    if (loadJobFind(LOADJOB_TEXTURE, name) >= 0) {
        *pending = 1;
        return 0;
    }
    const char *dirs[10] = { MAP_DIR, MAP_TEXTURES_DIR, PROPS_DIR, ITEMS_DIR,
                             ITEMS_HUD_DIR, HUD_DIR, INV_ICONS_DIR, NPCS_DIR,
                             DECALS_DIR, OVERLAYS_DIR };
    char path[1024];
    if (!textureResolve(name, dirs, 10, path, sizeof(path))) {
        return textureCacheInsert(name, NULL); /* unresolved: cache the 0 */
    }
    if (loadJobPost(LOADJOB_TEXTURE, name, path) < 0) {
        *pending = 1; /* ring full; retry next frame */
        return 0;
    }
    *pending = 1;
    return 0;
}

/* Async propModelGet, same contract. */
static B3DModel *propModelGetAsync(const char *rawName, int *pending) {
    *pending = 0;
    char name[256];
    propModelKey(rawName, name, sizeof(name));
    for (unsigned i = 0; i < modelCacheCount; i++) {
        if (strcmp(modelCache[i].name, name) == 0) return modelCache[i].model;
    }
    if (!loaderOk) return propModelGet(rawName);
    if (loadJobFind(LOADJOB_MODEL, name) >= 0) {
        *pending = 1;
        return NULL;
    }
    const char *dirs[3] = { PROPS_DIR, ITEMS_DIR, NPCS_DIR };
    char path[1024];
    if (!textureResolve(name, dirs, 3, path, sizeof(path))) {
        modelCacheInsert(name, NULL);
        return NULL;
    }
    if (loadJobPost(LOADJOB_MODEL, name, path) >= 0) {
        *pending = 1;
    } else {
        *pending = 1; /* ring full; retry next frame */
    }
    return NULL;
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

#define MAX_ROOM_EMITTERS 6
typedef struct {
    float x, y, z;    /* room-local raw units */
    int id;           /* RoomAmbience index (1-based in the file) */
    float range;      /* raw units */
} RoomEmitter;

typedef struct {
    int state;        /* 0 = not loaded, 1 = ok, -1 = failed,
                         3 = props loading, 4 = collision pending,
                         2 = textures loading, 5 = VBO uploads */
    int retries;      /* failed attempts so far (transient OOM retries) */
    int loadJob;      /* slot+1 of an in-flight async room job, 0 none */
    uint32_t texDone; /* batches with textures resolved (state 2) */
    uint32_t propDone;/* mesh entities examined for props (state 3) */
    uint32_t vboDone; /* batches with buffers uploaded (state 5) */
    RMesh *mesh;      /* kept alive between states 0 and 4 */
    Scene *scene;
    BatchGL *gl;
    CollisionWorld *col;
    RoomEmitter emitters[MAX_ROOM_EMITTERS];
    int emitterCount;
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
    if (rt->loadJob > 0) {
        /* An async room job is in flight (map regen mid-stream): wait
         * for the worker, then discard its result. */
        LoadJob *j = &loadJobs[rt->loadJob - 1];
        while (j->stage == 1) sceKernelDelayThread(1000);
        if (j->stage == 2) {
            __sync_synchronize();
            sceneFree(j->scene);
            rmeshFree(j->mesh);
            j->scene = NULL;
            j->mesh = NULL;
            j->stage = 0;
        }
    }
    free(rt->gl);
    sceneFree(rt->scene);
    collisionFree(rt->col);
    rmeshFree(rt->mesh); /* set only while a load is mid-flight */
    memset(rt, 0, sizeof(*rt));
}

/* One increment of room loading (the same spread-the-work idea as
 * the skinned figures: never do a whole room in one frame unless the
 * player is standing in it). Step 1 parses the mesh and builds the
 * scene and collision; each following step resolves the textures of
 * a couple of batches; the last one uploads the VBOs. */
/* A template load failure is usually transient (a malloc failing under
 * heap pressure), so failures log their cause, count into the HUD's
 * tpl= readout and retry on later calls instead of silently latching
 * the room black forever (which skipped the whole 3D pass when every
 * template failed). */
#define TPL_MAX_RETRIES 8

/* One SMALL unit of loading per call, so the per-frame budget loop in
 * updateActiveRooms can spread a room's load over many frames. With the
 * async loader the heavy work (mesh parse, PNG decode, B3D parse) runs
 * on the worker thread and the render thread only consumes results:
 *   0: post mesh+scene job (or build inline when sync)       -> 6 / 3
 *   6: waiting on the worker's mesh+scene                    -> 3
 *   3: ONE prop model appended per call (async loads)        -> 4
 *   4: collision build, mesh freed                           -> 2
 *   2: ONE batch's textures per call (async decodes)         -> 5
 *   5: a few VBO/IBO uploads per call                        -> 1
 * (Measured on device when done inline: 50-210 ms mesh parse, 8-209 ms
 * per prop, 25-171 ms per texture batch - each a blown frame.)
 * Returns 1 when the call made progress, 0 when it is waiting on the
 * worker (or the template is terminal). */
static void templateRoomFailed(TemplateRT *rt, int idx, const char *what,
                               const char *err) {
    if (rt->retries == 1) tplFailCount++;
    plog("tpl FAIL %s idx=%d: %s (try %d)", what, idx, err, rt->retries);
    rt->state = -1;
}

static void templateRoomReady(TemplateRT *rt, int idx) {
    rt->emitterCount = 0;
    for (uint32_t i = 0; i < rt->mesh->entityCount; i++) {
        const RMeshEntity *e = &rt->mesh->entities[i];
        if (e->type != RMESH_ENTITY_SOUND_EMITTER) continue;
        if (rt->emitterCount >= MAX_ROOM_EMITTERS) break;
        RoomEmitter *em = &rt->emitters[rt->emitterCount++];
        em->x = e->x;
        em->y = e->y;
        em->z = e->z;
        em->id = e->u.soundEmitter.id;
        /* File range is raw; the game loops within `range` and uses
         * the same units the emitter position is in. */
        em->range = e->u.soundEmitter.range;
        if (em->range < 256.0f) em->range = 1024.0f;
    }
    if (rt->retries > 1) {
        tplFailCount--; /* a retry recovered it */
        plog("tpl RECOVERED idx=%d (try %d)", idx, rt->retries);
    }
    rt->propDone = 0;
    rt->state = 3;
}

static int templateEnsureStep(int idx, int allowAsync) {
    TemplateRT *rt = &tplRT[idx];
    if (rt->state == 1) return 0;
    if (rt->state == -1) {
        if (rt->retries >= TPL_MAX_RETRIES) return 0;
        rt->state = 0; /* try again */
    }

    if (rt->state == 0) {
        const RoomTemplateInfo *info = &tplList.items[idx];
        if (!info->meshPath) {
            rt->retries = TPL_MAX_RETRIES; /* permanent: nothing to load */
            rt->state = -1;
            return 0;
        }
        char path[1024];
        snprintf(path, sizeof(path), MAP_DIR "/%s", info->meshPath);
        if (allowAsync && loaderOk) {
            int slot = loadJobPost(LOADJOB_ROOM, info->meshPath, path);
            if (slot < 0) return 0; /* ring full: retry next frame */
            rt->retries++;
            rt->loadJob = slot + 1;
            rt->state = 6;
            return 1;
        }
        rt->retries++;
        char err[128];
        rt->mesh = rmeshLoadFile(path, err, sizeof(err));
        if (!rt->mesh) {
            templateRoomFailed(rt, idx, "rmesh", err);
            return 1;
        }
        rt->scene = sceneBuild(rt->mesh);
        if (!rt->scene) {
            rmeshFree(rt->mesh);
            rt->mesh = NULL;
            templateRoomFailed(rt, idx, "sceneBuild", "");
            return 1;
        }
        templateRoomReady(rt, idx);
        return 1;
    }

    if (rt->state == 6) {
        /* Mesh+scene building on the worker. */
        LoadJob *j = &loadJobs[rt->loadJob - 1];
        if (j->stage != 2) return 0;
        __sync_synchronize();
        rt->mesh = j->mesh;
        rt->scene = j->scene;
        j->mesh = NULL;
        j->scene = NULL;
        char err[96];
        snprintf(err, sizeof(err), "%s", j->err);
        j->stage = 0;
        rt->loadJob = 0;
        if (!rt->mesh || !rt->scene) {
            if (rt->mesh) rmeshFree(rt->mesh);
            rt->mesh = NULL;
            rt->scene = NULL;
            templateRoomFailed(rt, idx, "async room", err);
            return 1;
        }
        templateRoomReady(rt, idx);
        return 1;
    }

    if (rt->state == 3) {
        /* One prop per call; cold B3D parses run on the worker. */
        while (rt->propDone < rt->mesh->entityCount) {
            const RMeshEntity *e = &rt->mesh->entities[rt->propDone];
            if (e->type != RMESH_ENTITY_PROP || !e->u.prop.file) {
                rt->propDone++;
                continue;
            }
            int pending = 0;
            B3DModel *model =
                allowAsync ? propModelGetAsync(e->u.prop.file, &pending)
                           : propModelGet(e->u.prop.file);
            if (pending) return 0;
            rt->propDone++;
            if (!model) continue;
            float pos[3] = { e->x, e->y, e->z };
            float euler[3] = { e->u.prop.pitch, e->u.prop.yaw,
                               e->u.prop.roll };
            float scl[3] = { e->u.prop.scaleX, e->u.prop.scaleY,
                             e->u.prop.scaleZ };
            sceneAppendB3D(rt->scene, model, pos, euler, scl,
                           e->u.prop.texture);
            return 1; /* one appended; the rest on later calls */
        }
        rt->state = 4;
        return 1;
    }

    if (rt->state == 4) {
        /* Collision includes the prop triangles, so it must run after
         * every prop is appended. */
        rt->col = collisionBuild(rt->scene, rt->mesh);
        rmeshFree(rt->mesh);
        rt->mesh = NULL;
        rt->gl = (BatchGL *)calloc(
            rt->scene->batchCount ? rt->scene->batchCount : 1,
            sizeof(BatchGL));
        if (!rt->gl) {
            sceneFree(rt->scene);
            rt->scene = NULL;
            collisionFree(rt->col);
            rt->col = NULL;
            templateRoomFailed(rt, idx, "gl-calloc", "");
            return 1;
        }
        rt->texDone = 0;
        rt->state = 2;
        return 1;
    }

    if (rt->state == 2) {
        /* One batch's textures per call; decodes run on the worker and
         * the next few batches are queued ahead so it stays fed. */
        if (rt->texDone < rt->scene->batchCount) {
            const SceneBatch *b = &rt->scene->batches[rt->texDone];
            GLuint d, l;
            if (allowAsync && loaderOk) {
                int p1 = 0, p2 = 0;
                d = textureGetAsync(b->diffuseName, &p1);
                l = textureGetAsync(b->lightmapName, &p2);
                for (uint32_t k = rt->texDone + 1;
                     k < rt->scene->batchCount && k < rt->texDone + 4; k++) {
                    int pk = 0;
                    (void)textureGetAsync(rt->scene->batches[k].diffuseName,
                                          &pk);
                    (void)textureGetAsync(rt->scene->batches[k].lightmapName,
                                          &pk);
                }
                if (p1 || p2) return 0;
            } else {
                d = textureGet(b->diffuseName);
                l = textureGet(b->lightmapName);
            }
            rt->gl[rt->texDone].diffuse = d;
            rt->gl[rt->texDone].lightmap = l;
            rt->texDone++;
            return 1;
        }
        rt->vboDone = 0;
        rt->state = 5;
        return 1;
    }

    /* state 5: a few VBO/IBO uploads per call (the full-room burst was
     * a 10-40 ms spike on prop-heavy rooms). */
    uint32_t end = rt->vboDone + 8;
    if (end > rt->scene->batchCount) end = rt->scene->batchCount;
    for (; rt->vboDone < end; rt->vboDone++) {
        const SceneBatch *b = &rt->scene->batches[rt->vboDone];
        glGenBuffers(1, &rt->gl[rt->vboDone].vbo);
        glBindBuffer(GL_ARRAY_BUFFER, rt->gl[rt->vboDone].vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(b->vertexCount * sizeof(SceneVertex)),
                     b->vertices, GL_STATIC_DRAW);
        glGenBuffers(1, &rt->gl[rt->vboDone].ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rt->gl[rt->vboDone].ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(b->indexCount * sizeof(uint16_t)),
                     b->indices, GL_STATIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    if (rt->vboDone < rt->scene->batchCount) return 1;
    rt->state = 1;
    return 1;
}

/* Load a room to completion (the cell the player stands in, and the
 * spawn-area prewarm behind the loading screen). Synchronous work; if
 * an async job for this template is already in flight, wait for the
 * worker rather than duplicating its work. */
static void templateEnsure(int idx) {
    while (tplRT[idx].state != 1 && tplRT[idx].state != -1) {
        if (!templateEnsureStep(idx, 0)) {
            sceKernelDelayThread(500);
        }
    }
}

/* Defined after the loading screen: loads the spawn area behind it. */
static void prewarmSpawnArea(void);

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

/* Keypad access codes: the fixed ones from Map_Core plus the
 * per-run set derived from Dr. Maynard's like InitNewGame (Maynard is
 * 4 random digits; O5/maintenance/Gears are multiples mod 10000,
 * bumped past 1000). Deterministic from the map seed so saves
 * reproduce them. */
static int accessCodes[8];

static void generateAccessCodes(uint32_t seed) {
    uint32_t h = seed * 2654435761u;
    int maynard = 0;
    for (int i = 0; i < 4; i++) {
        maynard += (int)((h >> (i * 8)) % 10u) * (int)powf(10.0f, (float)i);
    }
    if (maynard == 7816 || maynard == 5731 || maynard == 2411) maynard++;
    accessCodes[0] = 0;
    accessCodes[1] = 7816;                    /* CODE_DR_HARP */
    accessCodes[2] = 2411;                    /* CODE_DR_L */
    accessCodes[3] = 5731;                    /* CODE_CONT1_035 */
    accessCodes[4] = maynard;                 /* CODE_DR_MAYNARD */
    accessCodes[5] = (maynard * 2) % 10000;   /* CODE_O5_COUNCIL */
    if (accessCodes[5] < 1000) accessCodes[5] += 1000;
    accessCodes[6] = (maynard * 3) % 10000;   /* CODE_MAINTENANCE_TUNNELS */
    if (accessCodes[6] < 1000) accessCodes[6] += 1000;
    accessCodes[7] = (maynard * 4) % 10000;   /* CODE_DR_GEARS */
    if (accessCodes[7] < 1000) accessCodes[7] += 1000;
}

/* FillRoom's room-internal doors (containment chambers, elevator
 * covers, locked service doors) from room_doors.h, placed with the
 * same transform as item spawns. */
static void spawnRoomDecals(void); /* defined with the decal system */

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
                             rd->nobuttons,
                             rd->codeId ? accessCodes[rd->codeId] : 0);
        }
    }
}

/* ---- per-room event system (Events_Core) ---- */
enum { EV_NONE = 0, EV_173_APPEAR, EV_TRICK, EV_682_ROAR };
#define MAX_EVENT_ROOMS 1024
static int roomEventId[MAX_EVENT_ROOMS];
static float roomEventState[MAX_EVENT_ROOMS];
static float camShake;          /* decays; jitters the view rotation */
static float cameraZoom;         /* FOV narrowing (deg) - 106's dread pulse */
static float blurAmount;         /* 0..1 screen blur (me\BlurVolume) */
static unsigned gTick;           /* gameplay frame counter (effect phases) */
static uint32_t eventRng;        /* per-run event RNG */
static int sndHorror11 = -1, snd682Roar = -1;

/* Room levers and standalone buttons (room_fixtures.h). Cosmetic +
 * interactive fixtures: levers flip on/off, buttons depress and
 * click. World coords via the same room transform as items/doors. */
typedef struct {
    float x, y, z;
    float yawDeg;   /* world yaw of the base */
    int on;
    float pitch;    /* animated handle pitch, 80 (off) .. -80 (on) */
} WorldLever;

typedef struct {
    float x, y, z;
    float pitch, yaw, roll; /* world eulers */
    int btnId;
    int locked;
    float press;    /* 0..1 depress animation */
} WorldButton;

#define MAX_FIXTURES 96
static WorldLever worldLevers[MAX_FIXTURES];
static int worldLeverCount;
static WorldButton worldButtons[MAX_FIXTURES];
static int worldButtonCount;

/* A security camera (CreateSecurityCam): a fixed base with a head that
 * sweeps within +-turn or tracks the player, red light blinking. */
typedef struct {
    float x, y, z;      /* base world position (raw) */
    float roomYaw;      /* base mount yaw (room angle) */
    float yawBase;      /* head yaw centre (room angle + sc\Angle) */
    float pitch;        /* head down-tilt (Pitch1) */
    float turn;         /* sweep amplitude, 0 = static */
    int follow;         /* track the player */
    float currAngle;    /* animated sweep offset */
    int dir;            /* sweep direction */
    float headYaw, headPitch; /* animated head orientation (world) */
    int screen;         /* feeds a monitor */
    float mx, my, mz;   /* monitor world position */
    float myaw, mpitch; /* monitor facing (world) */
} WorldCamera;

#define MAX_CAMERAS 48
static WorldCamera worldCameras[MAX_CAMERAS];
static int worldCameraCount;
static float camCheckTimer;   /* MTF camera sweep: >0 while one runs */
static int camCheckSpotted;   /* a camera caught the player this sweep */
/* SCP-895 CoffinEffect / SCP-079 broadcast: once the player leaves the
 * first zone, SCP-079 broadcasts SCP-895's coffin feed onto the monitors
 * (source CoffinEffect 2/3), scrambling them red. */
static int leftFirstZone;

static void spawnRoomCameras(void) {
    worldCameraCount = 0;
    camCheckTimer = 0.0f;
    camCheckSpotted = 0;
    leftFirstZone = 0;
    const int NC = (int)(sizeof(ROOM_CAMERAS) / sizeof(ROOM_CAMERAS[0]));
    for (uint32_t r = 0;
         r < map.roomCount && worldCameraCount < MAX_CAMERAS; r++) {
        const RoomPlacement *p = &map.rooms[r];
        const char *nm = tplList.items[p->templateIndex].name;
        float roomYaw = (float)(p->angle * 90);
        for (int i = 0; i < NC && worldCameraCount < MAX_CAMERAS; i++) {
            const RoomCameraDef *cd = &ROOM_CAMERAS[i];
            if (strcmp(cd->room, nm) != 0) continue;
            float local[3] = { cd->x, cd->y, cd->z }, w[3];
            localToWorld(p, local, w);
            WorldCamera *c = &worldCameras[worldCameraCount++];
            c->x = w[0]; c->y = w[1]; c->z = w[2];
            c->roomYaw = roomYaw;
            c->yawBase = roomYaw + cd->angle;
            c->pitch = cd->pitch;
            c->turn = cd->turn;
            c->follow = cd->follow;
            c->currAngle = 0.0f;
            c->dir = 0;
            c->headYaw = c->yawBase;
            c->headPitch = cd->pitch;
            c->screen = cd->screen;
            if (cd->screen) {
                float ml[3] = { cd->mx, cd->my, cd->mz }, mw[3];
                localToWorld(p, ml, mw);
                c->mx = mw[0]; c->my = mw[1]; c->mz = mw[2];
                c->myaw = roomYaw + cd->myaw;
                c->mpitch = cd->mpitch;
            } else {
                c->mx = c->my = c->mz = 0.0f;
                c->myaw = c->mpitch = 0.0f;
            }
        }
    }
}

static void spawnRoomFixtures(void) {
    worldLeverCount = 0;
    worldButtonCount = 0;
    const int NL = (int)(sizeof(ROOM_LEVERS) / sizeof(ROOM_LEVERS[0]));
    const int NB = (int)(sizeof(ROOM_BUTTONS) / sizeof(ROOM_BUTTONS[0]));
    for (uint32_t r = 0; r < map.roomCount; r++) {
        const RoomPlacement *p = &map.rooms[r];
        const char *nm = tplList.items[p->templateIndex].name;
        float roomYaw = (float)(p->angle * 90);
        for (int i = 0; i < NL && worldLeverCount < MAX_FIXTURES; i++) {
            const RoomLeverDef *ld = &ROOM_LEVERS[i];
            if (strcmp(ld->room, nm) != 0) continue;
            float local[3] = { ld->x, ld->y, ld->z }, w[3];
            localToWorld(p, local, w);
            WorldLever *lv = &worldLevers[worldLeverCount++];
            lv->x = w[0]; lv->y = w[1]; lv->z = w[2];
            lv->yawDeg = ld->rotDeg + roomYaw;
            lv->on = ld->on;
            lv->pitch = ld->on ? -80.0f : 80.0f;
        }
        for (int i = 0; i < NB && worldButtonCount < MAX_FIXTURES; i++) {
            const RoomButtonDef *bd = &ROOM_BUTTONS[i];
            if (strcmp(bd->room, nm) != 0) continue;
            float local[3] = { bd->x, bd->y, bd->z }, w[3];
            localToWorld(p, local, w);
            WorldButton *bt = &worldButtons[worldButtonCount++];
            bt->x = w[0]; bt->y = w[1]; bt->z = w[2];
            bt->pitch = bd->pitch;
            bt->yaw = bd->yaw + roomYaw;
            bt->roll = bd->roll;
            bt->btnId = bd->btnId;
            bt->locked = bd->locked;
            bt->press = 0.0f;
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

/* SCP-106's pocket dimension: a small off-grid room the player is
 * dragged to when 106 catches them; escape by reaching the exit
 * before the timer runs out. */
#define PD_GX -16
#define PD_GY -16
/* SCP-1499's mask dimension: donning the mask exiles the player to
 * dimension_1499 where its hooded people swarm; taking the mask off
 * returns them. An off-grid room like the pocket dimension. */
#define MASK_GX -16
#define MASK_GY 16
static int maskRoomIdx = -1;
static int maskEntries;       /* mask uses this run (re-entry ambush) */
/* Elevators (Map_Core UpdateElevators). The source's cars run to Gate
 * A/B and the maintenance tunnels, which aren't ported, so the port
 * networks the in-map elevator doors: riding one carries the player to
 * the next elevator with a doors-close / blackout ride / doors-open. */
enum { ELEV_IDLE = 0, ELEV_CLOSE, ELEV_TRAVEL, ELEV_ARRIVE };
static int elevState;
static float elevTimer;
static float elevDest[3];     /* arrival position */
static float elevRayFromY = 1500.0f; /* height to drop the arrival floor
                                        ray from (raised for the vertical
                                        cont1_079 car to a lower level) */
static int elevDoorA = -1, elevDoorB = -1; /* doors to open on arrival */
/* The gate surfaces are real elevator destinations: gate_a_entrance /
 * gate_b_entrance (in the facility) ride to the appended gate_a / gate_b
 * surface rooms and back. */
#define GATEA_GX -20
#define GATEA_GY -20
#define GATEB_GX -24
#define GATEB_GY -20
static int gateARoomIdx = -1, gateBRoomIdx = -1;
static int gateADoor = -1, gateBDoor = -1;   /* surface return elevators */
static float gateAEntr[3], gateBEntr[3];     /* facility landings */
static int gateAEntrOK, gateBEntrOK;
static int inMask;            /* wearing SCP-1499, in its dimension */
static float maskReturn[4];   /* pre-mask pos + yaw */
#define MAX_1499 8
static float npc1499Pos[MAX_1499][3];
static float npc1499Yaw[MAX_1499];
static float npc1499Home[MAX_1499][3]; /* peaceful post / wander anchor */
static int npc1499Type[MAX_1499];      /* 0 citizen 1 king-guard 2 king
                                          3 front-guard */
static int npc1499Active[MAX_1499];
static int npc1499Aggro[MAX_1499];     /* this member has turned hostile */
static float npc1499Frame[MAX_1499];   /* per-member animation phase */
static int npc1499Chat[MAX_1499];      /* citizen's conversation partner, -1 none */
static int npc1499Count;
static int pdRoomIdx = -1;
static int inPocket;          /* player is in the pocket dimension */
static float pocketTimer;     /* frames until it collapses (death) */
static float pdCircle;        /* 106's orbit angle in the start room */
static int pdLunging;         /* 106 has broken off to attack */
/* The 8-state flow (Events_Core UpdateDimension106): the player is
 * shuffled between sub-rooms of the composite - the start room, the
 * four-way / throne / trenches / exit area (dim_3) and the tower /
 * labyrinth area (dim_4) - by choice and by random teleport, until an
 * exit is found or 106 / a hazard kills them. */
enum { PD_START = 0, PD_FOURWAY, PD_THRONE, PD_TRENCHES, PD_EXIT,
       PD_FAKETUNNEL, PD_TOWER, PD_LABYRINTH };
static int pdState;
static float pdEventState;    /* e\EventState per-state timer */
static float pdStateTimer;    /* frames since the last state change */
static int pd106Hidden;       /* 106 not present in this sub-room */
static int npc106Contained;   /* femur breaker: 106 permanently stopped */
static float femurTimer;      /* femur breaker sequence progress */
static float femurSpot[3];    /* world pos of the breaker pit */
static int sndMagnetUp = -1, sndMagnetDown = -1;
static float pdReturn[4];     /* pre-catch pos + yaw to restore */
static int sndPdEnter = -1, sndPdExit = -1, sndPdRumble = -1,
           sndPdExplode = -1;
static int snd096Trigger = -1, snd096Scream = -1;
static int snd049Breath = -1, snd049Horror = -1;
static int snd049Spot[3] = { -1, -1, -1 }, snd049Search[3] = { -1, -1, -1 };
static int snd939Attack[3] = { -1, -1, -1 }, snd939Horror = -1;
static int snd939Lure[3] = { -1, -1, -1 };
static int snd1499Enter = -1, snd1499Exit = -1, snd1499Trig = -1;
static int snd1499Idle[4] = { -1, -1, -1, -1 };

/* Tesla gates (Events_Core e_tesla): the room2_tesla_* corridors have
 * an electrified gate at their centre that idles, charges, zaps and
 * recharges. Caught in the zap it kills the player ("tesla" death) and
 * repels SCP-106 (its long-missing State 4 retreat). */
#define MAX_TESLA 8
typedef struct {
    float x, z;        /* gate centre (room centre), raw world */
    int state;         /* 0 idle 1 charge 2 zap 3 recharge */
    float timer;       /* EventState2 analogue (frames) */
} TeslaGate;
static TeslaGate teslaGates[MAX_TESLA];
static int teslaCount;
static int sndTeslaIdle = -1, sndTeslaWind = -1, sndTeslaShock = -1,
           sndTeslaPower = -1;
/* MTF camera-check announcements (UpdateCameraCheck). */
static int sndCamCheck = -1, sndCamFound[2] = { -1, -1 }, sndCamNoFound = -1;
/* SCP-079 speech (cont1_079 event). */
static int snd079Speech = -1, snd079Refuse = -1;
/* SCP-895 camera coffin (cont1_895): the slumped guard's idle murmur and
 * scream. */
static int snd895Idle[3] = { -1, -1, -1 }, snd895Scream[3] = { -1, -1, -1 };
/* SCP-205 (cont1_205): the shadow demon's horror cue. */
static int snd205Horror = -1;
/* SCP-914 (cont1_914): the refinement whir. */
static int snd914 = -1;
/* SCP-513-1's bells (declared early: loaded in loadSounds). */
static int snd513Bell[3] = { -1, -1, -1 };
static int npc513Active;   /* reset in regenerateMap, so declared early */
/* SCP-035's get-up sound + victim texture (declared early: loaded in
 * loadSounds / buildDoorAssets). */
static int snd035GetUp = -1;
static GLuint tex035;
static GLuint teslaArcTex;
static float teslaFlash;       /* white screen flash when it zaps nearby */

static int inPdBounds(float x, float z) {
    if (pdRoomIdx < 0) return 0;
    float ox = PD_GX * ROOM_SPACING, oz = PD_GY * ROOM_SPACING;
    return x > ox - 600.0f && x < ox + 600.0f
        && z > oz - 600.0f && z < oz + 600.0f;
}

/* The pocket dimension's FillRoom sub-meshes (Rooms_Core r_dimension_106):
 * dimension_106_2 copied eight times into a ring around the start room,
 * plus dimension_106_3 (the throne room) and dimension_106_4 (the
 * pillar room) set far along +z. Each unique mesh loads once; instances
 * share it with their own offset + yaw, overlaid on the base
 * dimension_106 room exactly like the intro cell overlays the intro
 * room. Built once and kept for the session. */
typedef struct {
    Scene *scene;
    BatchGL *gl;
    CollisionWorld *col;
} PdMesh;
typedef struct {
    int mesh;          /* index into pdMesh[] */
    float off[3];      /* world offset from the PD origin */
    float yawDeg;      /* Blitz yaw of the instance */
} PdInstance;
#define PD_MESH_MAX 3
#define PD_INST_MAX 12
static PdMesh pdMesh[PD_MESH_MAX];
static int pdMeshCount;
static PdInstance pdInst[PD_INST_MAX];
static int pdInstCount;

static int buildPocketMesh(const char *path) {
    if (pdMeshCount >= PD_MESH_MAX) return -1;
    char err[128];
    RMesh *mesh = rmeshLoadFile(path, err, sizeof(err));
    if (!mesh) return -1;
    Scene *sc = sceneBuild(mesh);
    if (!sc) { rmeshFree(mesh); return -1; }
    CollisionWorld *col = collisionBuild(sc, mesh);
    rmeshFree(mesh);
    BatchGL *gl = (BatchGL *)calloc(sc->batchCount ? sc->batchCount : 1,
                                    sizeof(BatchGL));
    if (!gl) return -1;
    for (uint32_t i = 0; i < sc->batchCount; i++) {
        const SceneBatch *b = &sc->batches[i];
        gl[i].diffuse = textureGet(b->diffuseName);
        gl[i].lightmap = textureGet(b->lightmapName);
        glGenBuffers(1, &gl[i].vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gl[i].vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(b->vertexCount * sizeof(SceneVertex)),
                     b->vertices, GL_STATIC_DRAW);
        glGenBuffers(1, &gl[i].ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl[i].ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(b->indexCount * sizeof(uint16_t)),
                     b->indices, GL_STATIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    int idx = pdMeshCount++;
    pdMesh[idx].scene = sc;
    pdMesh[idx].gl = gl;
    pdMesh[idx].col = col;
    return idx;
}

static void buildPocketComposite(void) {
    if (pdMeshCount) return; /* built once, kept for the session */
    int tun = buildPocketMesh(MAP_DIR "/dimension_106_2.rmesh");
    int throne = buildPocketMesh(MAP_DIR "/dimension_106_3.rmesh");
    int pillar = buildPocketMesh(MAP_DIR "/dimension_106_4.rmesh");
    pdInstCount = 0;
    /* Eight tunnels: Angle = (i-1)*45, offset Cos/Sin * 512 raw around
     * the origin, RotateEntity yaw = Angle-90. */
    if (tun >= 0) {
        for (int k = 0; k < 8 && pdInstCount < PD_INST_MAX; k++) {
            float ang = (float)k * 45.0f;
            float a = ang * 3.14159265f / 180.0f;
            PdInstance *in = &pdInst[pdInstCount++];
            in->mesh = tun;
            in->off[0] = cosf(a) * 512.0f;
            in->off[1] = 0.0f;
            in->off[2] = sinf(a) * 512.0f;
            in->yawDeg = ang - 90.0f;
        }
    }
    /* Throne room at r\z + 32 blitz (x256 = 8192 raw); pillar room at
     * r\z + 64 (16384 raw). Both unrotated. */
    if (throne >= 0 && pdInstCount < PD_INST_MAX) {
        PdInstance *in = &pdInst[pdInstCount++];
        in->mesh = throne;
        in->off[0] = 0.0f; in->off[1] = 0.0f; in->off[2] = 8192.0f;
        in->yawDeg = 0.0f;
    }
    if (pillar >= 0 && pdInstCount < PD_INST_MAX) {
        PdInstance *in = &pdInst[pdInstCount++];
        in->mesh = pillar;
        in->off[0] = 0.0f; in->off[1] = 0.0f; in->off[2] = 16384.0f;
        in->yawDeg = 0.0f;
    }
}

/* World<->instance-local for a PD sub-mesh. The render path does
 * glTranslate(PDorigin + off) then glRotatef(-yaw), i.e.
 * world = R(-yaw) * local + T; these invert it for collision. */
static void pdInstToLocal(const PdInstance *in, const float w[3],
                          float l[3]) {
    float ox = PD_GX * ROOM_SPACING, oz = PD_GY * ROOM_SPACING;
    float dx = w[0] - (ox + in->off[0]);
    float dz = w[2] - (oz + in->off[2]);
    float a = in->yawDeg * 3.14159265f / 180.0f;
    float c = cosf(a), s = sinf(a);
    l[0] = c * dx + s * dz;
    l[1] = w[1] - in->off[1];
    l[2] = -s * dx + c * dz;
}

static void pdInstToWorld(const PdInstance *in, const float l[3],
                          float w[3]) {
    float ox = PD_GX * ROOM_SPACING, oz = PD_GY * ROOM_SPACING;
    float a = in->yawDeg * 3.14159265f / 180.0f;
    float c = cosf(a), s = sinf(a);
    w[0] = c * l[0] - s * l[2] + ox + in->off[0];
    w[1] = l[1] + in->off[1];
    w[2] = s * l[0] + c * l[2] + oz + in->off[2];
}

/* The maintenance tunnels: a port of the source's procedural tunnel grid
 * (Events_Core ~4300-4600). A random walk lays a connected corridor
 * network; each cell then becomes a tile by its neighbour count - 1
 * dead-end (mt1), 2 straight/bent (mt2/mt2c), 3 tee (mt3), 4 cross (mt4)
 * - rotated so its openings face the occupied neighbours, at 512-raw
 * (2-blitz) spacing. Two straight cells become the elevators. Overlaid
 * off-grid like the intro cell; built once, kept for the run. */
#define MT_GX -20
#define MT_GY -24
#define MT_GRID 15
#define MT_CELL 512.0f
#define MT_MESH_MAX 7
#define MT_INST_MAX 200
typedef struct {
    Scene *scene;
    BatchGL *gl;
    CollisionWorld *col;
} MtMesh;
typedef struct {
    int mesh;         /* index into mtMesh[] */
    float wx, wz;     /* world position */
    int yaw;          /* 0..3, times 90 deg */
} MtInstance;
static MtMesh mtMesh[MT_MESH_MAX];
static int mtMeshCount;
static MtInstance mtInst[MT_INST_MAX];
static int mtInstCount;
static float mtMinX, mtMaxX, mtMinZ, mtMaxZ; /* bounds for culling */
static float mtArrive[3];      /* first-elevator landing */
static int mtElevDoorA = -1;   /* the tunnel's return elevator */
static float mtEntr[3];        /* facility room2_mt landing */
static int mtEntrOK;
static float mtGen[3];         /* generator room cell (props + items) */
static int mtGenOK;

static int inMtBounds(float x, float z) {
    if (!mtInstCount) return 0;
    return x > mtMinX - 700.0f && x < mtMaxX + 700.0f
        && z > mtMinZ - 700.0f && z < mtMaxZ + 700.0f;
}

static int buildMtMesh(const char *path) {
    if (mtMeshCount >= MT_MESH_MAX) return -1;
    char err[128];
    RMesh *mesh = rmeshLoadFile(path, err, sizeof(err));
    if (!mesh) return -1;
    Scene *sc = sceneBuild(mesh);
    if (!sc) { rmeshFree(mesh); return -1; }
    CollisionWorld *col = collisionBuild(sc, mesh);
    rmeshFree(mesh);
    BatchGL *gl = (BatchGL *)calloc(sc->batchCount ? sc->batchCount : 1,
                                    sizeof(BatchGL));
    if (!gl) return -1;
    for (uint32_t i = 0; i < sc->batchCount; i++) {
        const SceneBatch *b = &sc->batches[i];
        gl[i].diffuse = textureGet(b->diffuseName);
        gl[i].lightmap = textureGet(b->lightmapName);
        glGenBuffers(1, &gl[i].vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gl[i].vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(b->vertexCount * sizeof(SceneVertex)),
                     b->vertices, GL_STATIC_DRAW);
        glGenBuffers(1, &gl[i].ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl[i].ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(b->indexCount * sizeof(uint16_t)),
                     b->indices, GL_STATIC_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    int idx = mtMeshCount++;
    mtMesh[idx].scene = sc;
    mtMesh[idx].gl = gl;
    mtMesh[idx].col = col;
    return idx;
}

/* MT tile mesh indices (Loading_Core MTModelID order). */
enum { MT_M_DEADEND = 0, MT_M_CORR, MT_M_CORNER, MT_M_TEE, MT_M_CROSS,
       MT_M_ELEV, MT_M_GEN };

static void buildMtMaze(void) {
    if (mtMeshCount) return; /* built once */
    buildMtMesh(MAP_DIR "/mt1.rmesh");
    buildMtMesh(MAP_DIR "/mt2.rmesh");
    buildMtMesh(MAP_DIR "/mt2c.rmesh");
    buildMtMesh(MAP_DIR "/mt3.rmesh");
    buildMtMesh(MAP_DIR "/mt4.rmesh");
    buildMtMesh(MAP_DIR "/mt2_elevator.rmesh");
    buildMtMesh(MAP_DIR "/mt1_generator.rmesh");
    if (mtMeshCount < 6) return;

    static signed char grid[MT_GRID * MT_GRID];
    memset(grid, 0, sizeof(grid));
    /* Random walk from the centre (deterministic from the seed). */
    uint32_t rng = mapSeed ^ 0x4D41494Eu;
    int x = MT_GRID / 2, y = MT_GRID / 2, dir = (int)(rng & 3);
    grid[x + y * MT_GRID] = 1;
    int count = 1, target = 26 + (int)((rng >> 8) % 16u);
    int guard = 0;
    while (count < target && guard++ < 400) {
        rng = rng * 1664525u + 1013904223u;
        int run = 1 + (int)((rng >> 16) % 4u);
        for (int s = 0; s < run; s++) {
            int nx = x, ny = y;
            if (dir == 0) ny--;
            else if (dir == 1) nx++;
            else if (dir == 2) ny++;
            else nx--;
            if (nx < 1 || nx >= MT_GRID - 1 || ny < 1 || ny >= MT_GRID - 1) {
                break;
            }
            x = nx; y = ny;
            if (!grid[x + y * MT_GRID]) { grid[x + y * MT_GRID] = 1; count++; }
        }
        rng = rng * 1664525u + 1013904223u;
        dir += ((rng >> 16) & 1u) ? 1 : -1;
        dir = (dir + 4) & 3;
    }
    /* Replace each occupied cell with its orthogonal neighbour count. */
    static signed char cnt[MT_GRID * MT_GRID];
    for (int j = 0; j < MT_GRID; j++) {
        for (int i = 0; i < MT_GRID; i++) {
            if (!grid[i + j * MT_GRID]) { cnt[i + j * MT_GRID] = 0; continue; }
            int n = 0;
            if (j + 1 < MT_GRID && grid[i + (j + 1) * MT_GRID]) n++;
            if (j - 1 >= 0 && grid[i + (j - 1) * MT_GRID]) n++;
            if (i + 1 < MT_GRID && grid[(i + 1) + j * MT_GRID]) n++;
            if (i - 1 >= 0 && grid[(i - 1) + j * MT_GRID]) n++;
            cnt[i + j * MT_GRID] = (signed char)n;
        }
    }
    /* Two straight-corridor cells become the elevators. */
    int firstX = -1, firstY = -1, lastX = -1, lastY = -1;
    for (int j = 1; j < MT_GRID - 1; j++) {
        for (int i = 1; i < MT_GRID - 1; i++) {
            if (cnt[i + j * MT_GRID] != 2) continue;
            int horiz = grid[(i + 1) + j * MT_GRID]
                     && grid[(i - 1) + j * MT_GRID];
            int vert = grid[i + (j + 1) * MT_GRID]
                    && grid[i + (j - 1) * MT_GRID];
            if (!horiz && !vert) continue;
            if (firstX < 0) { firstX = i; firstY = j; }
            else { lastX = i; lastY = j; }
        }
    }
    /* Lay the tiles. */
    float ox = MT_GX * ROOM_SPACING, oz = MT_GY * ROOM_SPACING;
    mtInstCount = 0;
    mtMinX = mtMinZ = 1e30f; mtMaxX = mtMaxZ = -1e30f;
    for (int j = 0; j < MT_GRID && mtInstCount < MT_INST_MAX; j++) {
        for (int i = 0; i < MT_GRID && mtInstCount < MT_INST_MAX; i++) {
            int n = cnt[i + j * MT_GRID];
            if (n <= 0) continue;
            int up = j - 1 >= 0 && grid[i + (j - 1) * MT_GRID];
            int down = j + 1 < MT_GRID && grid[i + (j + 1) * MT_GRID];
            int right = i + 1 < MT_GRID && grid[(i + 1) + j * MT_GRID];
            int left = i - 1 >= 0 && grid[(i - 1) + j * MT_GRID];
            int mesh = MT_M_CROSS, yaw = 0;
            int isElev = (i == firstX && j == firstY)
                      || (i == lastX && j == lastY);
            if (n == 1) {
                mesh = MT_M_DEADEND;
                yaw = right ? 1 : left ? 3 : down ? 2 : 0;
            } else if (n == 2) {
                int horiz = right && left, vert = up && down;
                if (horiz || vert) {
                    mesh = isElev ? MT_M_ELEV : MT_M_CORR;
                    rng = rng * 1664525u + 1013904223u;
                    int flip = (int)((rng >> 16) & 1u);
                    yaw = horiz ? (flip * 2 + 1) : (flip * 2);
                } else {
                    mesh = MT_M_CORNER;
                    yaw = (down && right) ? 0 : (down && left) ? 1
                        : (up && right) ? 3 : 2;
                }
            } else if (n == 3) {
                mesh = MT_M_TEE;
                yaw = (down && right && left) ? 1
                    : (up && right && left) ? 3
                    : (right && up && down) ? 0 : 2;
            } else {
                mesh = MT_M_CROSS;
                rng = rng * 1664525u + 1013904223u;
                yaw = (int)((rng >> 16) % 4u);
            }
            MtInstance *in = &mtInst[mtInstCount++];
            in->mesh = mesh;
            in->wx = ox + (float)i * MT_CELL;
            in->wz = oz + (float)j * MT_CELL;
            in->yaw = yaw;
            if (in->wx < mtMinX) mtMinX = in->wx;
            if (in->wx > mtMaxX) mtMaxX = in->wx;
            if (in->wz < mtMinZ) mtMinZ = in->wz;
            if (in->wz > mtMaxZ) mtMaxZ = in->wz;
            if (isElev) {
                /* Land in the first elevator cell. */
                if (i == firstX && j == firstY) {
                    mtArrive[0] = in->wx;
                    mtArrive[2] = in->wz;
                    mtArrive[1] = 0.0f;
                }
            }
        }
    }
    /* Fallback if no straight cell was found for a landing. */
    if (mtArrive[0] == 0.0f && mtArrive[2] == 0.0f && mtInstCount) {
        mtArrive[0] = mtInst[0].wx;
        mtArrive[2] = mtInst[0].wz;
    }
    /* Turn a dead-end into the generator room (source MT_GENERATOR): the
     * mt1_generator tile shares the dead-end's single opening, so the
     * swap keeps its rotation. Its props/items spawn there. */
    mtGenOK = 0;
    for (int m = mtInstCount - 1; m >= 0; m--) {
        if (mtInst[m].mesh == MT_M_DEADEND && mtMesh[MT_M_GEN].scene) {
            mtInst[m].mesh = MT_M_GEN;
            mtGen[0] = mtInst[m].wx;
            mtGen[1] = 0.0f;
            mtGen[2] = mtInst[m].wz;
            mtGenOK = 1;
            break;
        }
    }
}

/* World<->tile-local for a 90-degree-rotated MT tile (render does
 * glTranslate(w) then glRotatef(-yaw*90), matching the room transform). */
static void mtToLocal(const MtInstance *in, const float w[3], float l[3]) {
    float dx = w[0] - in->wx, dz = w[2] - in->wz;
    switch (in->yaw & 3) {
        case 0: l[0] = dx;  l[2] = dz;  break;
        case 1: l[0] = dz;  l[2] = -dx; break;
        case 2: l[0] = -dx; l[2] = -dz; break;
        case 3: l[0] = -dz; l[2] = dx;  break;
    }
    l[1] = w[1];
}

static void mtToWorld(const MtInstance *in, const float l[3], float w[3]) {
    float x = 0.0f, z = 0.0f;
    switch (in->yaw & 3) {
        case 0: x = l[0];  z = l[2];  break;
        case 1: x = -l[2]; z = l[0];  break;
        case 2: x = -l[0]; z = -l[2]; break;
        case 3: x = l[2];  z = -l[0]; break;
    }
    w[0] = x + in->wx;
    w[1] = l[1];
    w[2] = z + in->wz;
}

static void appendPocketRoom(void) {
    pdRoomIdx = -1;
    int tplIdx = -1;
    for (uint32_t i = 0; i < tplList.count; i++) {
        if (strcmp(tplList.items[i].name, "dimension_106") == 0) {
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
    p->gridX = PD_GX;
    p->gridY = PD_GY;
    p->angle = 0;
    pdRoomIdx = (int)map.roomCount;
    map.roomCount++;
    buildPocketComposite();
}

/* The pocket-dimension labyrinth doors (Rooms_Core dimension_106, the
 * "For i = 0 To 9 / Select" block the door extractor skips because the
 * coordinates come from loop variables). Ten KEY_005 DEFAULT_DOORs at
 * fixed local offsets - x = xTemp, y = 2574, z = 32 + zTemp in raw
 * units - placed with the same room transform as every other
 * dimension_106 door. Call after spawnRoomDoors (which runs after
 * doorsGenerate resets the list). */
static void spawnPocketLabyrinthDoors(void) {
    if (pdRoomIdx < 0) return;
    static const struct { float x, z, ang; } PD_LABYRINTH[10] = {
        { 5187.0f, 2555.0f, 180.0f }, { 5521.0f, 1673.0f, 180.0f },
        { 9128.0f, 2192.0f, 180.0f }, { 8523.0f, 1760.0f, 180.0f },
        { 9880.0f, 1244.0f, 180.0f }, { 5299.0f,  392.0f,  90.0f },
        { 7807.0f, 1291.0f,  90.0f }, { 8196.0f, 1436.0f,  90.0f },
        { 8143.0f,  392.0f,  90.0f }, { 9709.0f,  920.0f,  90.0f },
    };
    const RoomPlacement *p = &map.rooms[pdRoomIdx];
    for (int i = 0; i < 10; i++) {
        float local[3] = { PD_LABYRINTH[i].x, 2574.0f, PD_LABYRINTH[i].z };
        float w[3];
        localToWorld(p, local, w);
        int a = (int)(PD_LABYRINTH[i].ang / 90.0f + 0.5f) * 90 + p->angle * 90;
        a = ((a % 360) + 360) % 360;
        doorsAddInternal(&doors, w[0], w[1], w[2], a, 0, 0, 0, 0, 0, 0);
    }
}

static void appendMaskRoom(void) {
    maskRoomIdx = -1;
    int tplIdx = -1;
    for (uint32_t i = 0; i < tplList.count; i++) {
        if (strcmp(tplList.items[i].name, "dimension_1499") == 0) {
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
    p->gridX = MASK_GX;
    p->gridY = MASK_GY;
    p->angle = 0;
    maskRoomIdx = (int)map.roomCount;
    map.roomCount++;
}

/* Append the gate_a / gate_b surface rooms off-grid and give each a
 * return elevator, and record the facility gate-entrance landings, so
 * the entrance elevators actually travel to the surfaces. Call after the
 * grid doors are generated (it appends internal elevator doors). */
static void appendGateRooms(void) {
    gateARoomIdx = gateBRoomIdx = -1;
    gateADoor = gateBDoor = -1;
    struct { const char *name; int gx, gy; int *idx, *door; } G[2] = {
        { "gate_a", GATEA_GX, GATEA_GY, &gateARoomIdx, &gateADoor },
        { "gate_b", GATEB_GX, GATEB_GY, &gateBRoomIdx, &gateBDoor },
    };
    for (int g = 0; g < 2; g++) {
        int tplIdx = -1;
        for (uint32_t i = 0; i < tplList.count; i++) {
            if (strcmp(tplList.items[i].name, G[g].name) == 0) {
                tplIdx = (int)i;
                break;
            }
        }
        if (tplIdx < 0) continue;
        RoomPlacement *grown = (RoomPlacement *)realloc(
            map.rooms, (map.roomCount + 1) * sizeof(RoomPlacement));
        if (!grown) return;
        map.rooms = grown;
        RoomPlacement *p = &map.rooms[map.roomCount];
        p->templateIndex = tplIdx;
        p->gridX = G[g].gx;
        p->gridY = G[g].gy;
        p->angle = 0;
        *G[g].idx = (int)map.roomCount;
        map.roomCount++;
        /* A call elevator on the surface, to ride back. */
        doorsAddInternal(&doors, G[g].gx * ROOM_SPACING, 0.0f,
                         G[g].gy * ROOM_SPACING, 0, 1, 0, 0, 0, 0, 0);
        *G[g].door = (int)doors.count - 1;
    }
    /* Facility landings (the entrance rooms are force-placed). */
    gateAEntrOK = gateBEntrOK = 0;
    for (uint32_t i = 0; i < map.roomCount; i++) {
        const char *nm = tplList.items[map.rooms[i].templateIndex].name;
        if (!gateAEntrOK && strcmp(nm, "gate_a_entrance") == 0) {
            gateAEntr[0] = map.rooms[i].gridX * ROOM_SPACING;
            gateAEntr[1] = 0.0f;
            gateAEntr[2] = map.rooms[i].gridY * ROOM_SPACING;
            gateAEntrOK = 1;
        }
        if (!gateBEntrOK && strcmp(nm, "gate_b_entrance") == 0) {
            gateBEntr[0] = map.rooms[i].gridX * ROOM_SPACING;
            gateBEntr[1] = 0.0f;
            gateBEntr[2] = map.rooms[i].gridY * ROOM_SPACING;
            gateBEntrOK = 1;
        }
    }
}

/* Build the maintenance-tunnel overlay, give it a return elevator, and
 * record the facility room2_mt landing so those cars ride to it and
 * back. Call after the grid doors are generated. */
static void setupMaintenanceTunnel(void) {
    buildMtMaze();
    mtElevDoorA = -1;
    mtEntrOK = 0;
    if (mtInstCount) {
        doorsAddInternal(&doors, mtArrive[0], 0.0f, mtArrive[2], 0, 1, 0, 0,
                         0, 0, 0);
        mtElevDoorA = (int)doors.count - 1;
    }
    for (uint32_t i = 0; i < map.roomCount; i++) {
        if (strcmp(tplList.items[map.rooms[i].templateIndex].name,
                   "room2_mt") == 0) {
            mtEntr[0] = map.rooms[i].gridX * ROOM_SPACING;
            mtEntr[1] = 0.0f;
            mtEntr[2] = map.rooms[i].gridY * ROOM_SPACING;
            mtEntrOK = 1;
            break;
        }
    }
}

/* Active set: placements within one cell of the player. */
static const RoomPlacement *activeRooms[16];
static int activeCount;

static void updateActiveRooms(const float pos[3]) {
    int px = (int)floorf(pos[0] / ROOM_SPACING + 0.5f);
    int py = (int)floorf(pos[2] / ROOM_SPACING + 0.5f);
    loaderPump(); /* land finished decodes (a couple of ms at most) */
    activeCount = 0;
    /* One loading increment per frame: the room under the player
     * loads to completion (it is the floor), everything else -
     * neighbors first, then a 5x5 prefetch ring - advances one small
     * step per frame so crossings stop hitching. */
    int stepIdx = -1;
    int stepScore = 1000;
    for (uint32_t i = 0; i < map.roomCount; i++) {
        const RoomPlacement *p = &map.rooms[i];
        int dx = p->gridX - px, dy = p->gridY - py;
        int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        int ring = adx > ady ? adx : ady;
        int near = ring <= 1;
        int self = dx == 0 && dy == 0;
        /* The intro mesh spans several cells around its placement. */
        if ((int)i == introRoomIdx) {
            near = inIntroBounds(pos[0], pos[2]);
            self = near;
        }
        TemplateRT *rt = &tplRT[p->templateIndex];
        if (self && rt->state != 1 && rt->state != -1) {
            /* The floor under the player cannot wait; log the stall so
             * device captures show when the prefetch lost the race. */
            uint64_t t0 = sceKernelGetProcessTimeWide();
            templateEnsure(p->templateIndex);
            uint64_t us = sceKernelGetProcessTimeWide() - t0;
            if (us > 8000) {
                plog("hitch: blocking room load idx=%d %ums",
                     p->templateIndex, (unsigned)(us / 1000));
            }
        } else if (ring <= 2 && rt->state != 1 && rt->state != -1) {
            /* Prefer nearer rings; among equals prefer rooms already
             * mid-load so they finish sooner. */
            int score = ring * 2 + (rt->state == 0 ? 1 : 0);
            if (score < stepScore) {
                stepScore = score;
                stepIdx = p->templateIndex;
            }
        }
        if (near && rt->state == 1 && activeCount < 16) {
            activeRooms[activeCount++] = p;
        }
    }
    if (stepIdx >= 0) {
        /* Loading budget: ~3 ms of streaming work per frame. The heavy
         * lifting (decode/parse) runs on the worker; a step returning 0
         * is waiting on it, so stop for this frame. */
        uint64_t t0 = sceKernelGetProcessTimeWide();
        uint64_t us;
        do {
            if (!templateEnsureStep(stepIdx, 1)) break;
            us = sceKernelGetProcessTimeWide() - t0;
        } while (tplRT[stepIdx].state != 1 && tplRT[stepIdx].state != -1
                 && us < 3000);
        us = sceKernelGetProcessTimeWide() - t0;
        if (us > 8000) {
            plog("hitch: stream step idx=%d state=%d %ums", stepIdx,
                 tplRT[stepIdx].state, (unsigned)(us / 1000));
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

/* Zone of the player's current room (1 LCZ, 2 HCZ, 3 EZ), for per-zone
 * music/spawns. Matches the source, where a room's zone is its grid band
 * (r\Zone = GetZone(y)), not its template's declared Zone list - a room
 * valid in several zones takes the zone of the band it was placed in.
 * Defaults to LCZ. */
static int zoneAt(const float pos[3]) {
    if (inIntroBounds(pos[0], pos[2])) return 1;
    int px = (int)floorf(pos[0] / ROOM_SPACING + 0.5f);
    int py = (int)floorf(pos[2] / ROOM_SPACING + 0.5f);
    for (uint32_t i = 0; i < map.roomCount; i++) {
        if (map.rooms[i].gridX == px && map.rooms[i].gridY == py) {
            return mapZoneOf(map.rooms[i].gridY);
        }
    }
    return 1;
}

/* DisableDecals of the player's current room (0..3), which also gates
 * SCP-106's spawn timer (source UpdateNPCType106 State 1). The pocket
 * dimension and intro count as fully open (0). */
static int roomDisableDecalsAt(const float pos[3]) {
    if (inIntroBounds(pos[0], pos[2]) || inPdBounds(pos[0], pos[2])) return 0;
    int px = (int)floorf(pos[0] / ROOM_SPACING + 0.5f);
    int py = (int)floorf(pos[2] / ROOM_SPACING + 0.5f);
    for (uint32_t i = 0; i < map.roomCount; i++) {
        if (map.rooms[i].gridX == px && map.rooms[i].gridY == py) {
            return tplList.items[map.rooms[i].templateIndex].disableDecals;
        }
    }
    return 0;
}

/* Materials with stepsound=1 in Data/materials.ini (metal floors). */
static int textureIsMetal(const char *name) {
    if (!name) return 0;
    static const char *METAL[] = {
        "metal3.jpg", "dirtymetal.jpg", "metalpanels.png",
        "metalpanels2.png", "metalpanels2blood1.png",
        "metalpanels2blood2.png", "metal.jpg", "metal_darker.jpg",
        "controlpanel.jpg",
    };
    for (unsigned i = 0; i < sizeof(METAL) / sizeof(METAL[0]); i++) {
        if (strcasecmp(name, METAL[i]) == 0) return 1;
    }
    return 0;
}

/* The floor material under the player: ray straight down against the
 * scene batches of the room the player stands in and return whether
 * the topmost floor surface below the feet is metal. Called only at
 * footstep cadence, so the per-triangle test is cheap enough. */
static int floorIsMetal(const float pos[3]);

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
    if (mtInstCount && inMtBounds(pos[0], pos[2])) {
        for (int m = 0; m < mtInstCount; m++) {
            const MtInstance *in = &mtInst[m];
            float cdx = pos[0] - in->wx, cdz = pos[2] - in->wz;
            if (cdx * cdx + cdz * cdz > 600.0f * 600.0f) continue;
            const CollisionWorld *col = mtMesh[in->mesh].col;
            if (!col) continue;
            float local[3];
            mtToLocal(in, pos, local);
            int up = 0;
            if (collisionSpherePush(col, local, radius, &up)) {
                mtToWorld(in, local, pos);
            }
            if (up && pushedUp) *pushedUp = 1;
        }
    }
    if (pdMeshCount && inPocket) {
        for (int m = 0; m < pdInstCount; m++) {
            const PdMesh *pm = &pdMesh[pdInst[m].mesh];
            if (!pm->col) continue;
            float local[3];
            pdInstToLocal(&pdInst[m], pos, local);
            int up = 0;
            if (collisionSpherePush(pm->col, local, radius, &up)) {
                pdInstToWorld(&pdInst[m], local, pos);
            }
            if (up && pushedUp) *pushedUp = 1;
        }
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
    if (mtInstCount && inMtBounds(origin[0], origin[2])) {
        for (int m = 0; m < mtInstCount; m++) {
            const MtInstance *in = &mtInst[m];
            float cdx = origin[0] - in->wx, cdz = origin[2] - in->wz;
            if (cdx * cdx + cdz * cdz > 600.0f * 600.0f) continue;
            const CollisionWorld *col = mtMesh[in->mesh].col;
            if (!col) continue;
            float local[3], y;
            mtToLocal(in, origin, local);
            if (collisionRayDown(col, local, maxDist, &y)) {
                float wl[3] = { local[0], y, local[2] }, wout[3];
                mtToWorld(in, wl, wout);
                if (wout[1] > best) { best = wout[1]; hit = 1; }
            }
        }
    }
    if (pdMeshCount && inPocket) {
        for (int m = 0; m < pdInstCount; m++) {
            const PdMesh *pm = &pdMesh[pdInst[m].mesh];
            if (!pm->col) continue;
            float local[3], y;
            pdInstToLocal(&pdInst[m], origin, local);
            if (collisionRayDown(pm->col, local, maxDist, &y)) {
                float wl[3] = { local[0], y, local[2] }, wout[3];
                pdInstToWorld(&pdInst[m], wl, wout);
                if (wout[1] > best) { best = wout[1]; hit = 1; }
            }
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

/* True line-of-sight: 1 if the segment a->b is clear of room geometry,
 * 0 if a wall/prop occludes it. Tested per active room in that room's
 * local space (plus the intro cell and pocket-dimension composite). A
 * margin stops the ray short of the target's own body. */
static int lineOfSight(const float a[3], const float b[3]) {
    float wdx = b[0] - a[0], wdy = b[1] - a[1], wdz = b[2] - a[2];
    float dist = sqrtf(wdx * wdx + wdy * wdy + wdz * wdz);
    if (dist < 60.0f) return 1;
    float maxD = dist - 40.0f;
    if (maxD < 1.0f) return 1;

    if (introCellCol
        && (inIntroBounds(a[0], a[2]) || inIntroBounds(b[0], b[2]))) {
        float la[3] = { a[0] - INTRO_GX * ROOM_SPACING, a[1],
                        a[2] - INTRO_GY * ROOM_SPACING };
        float dir[3] = { wdx, wdy, wdz };
        if (collisionRayHit(introCellCol, la, dir, maxD)) return 0;
    }
    if (pdMeshCount && inPocket) {
        for (int m = 0; m < pdInstCount; m++) {
            const PdMesh *pm = &pdMesh[pdInst[m].mesh];
            if (!pm->col) continue;
            float la[3], lb[3];
            pdInstToLocal(&pdInst[m], a, la);
            pdInstToLocal(&pdInst[m], b, lb);
            float dir[3] = { lb[0] - la[0], lb[1] - la[1], lb[2] - la[2] };
            if (collisionRayHit(pm->col, la, dir, maxD)) return 0;
        }
    }
    for (int i = 0; i < activeCount; i++) {
        const RoomPlacement *p = activeRooms[i];
        const CollisionWorld *col = tplRT[p->templateIndex].col;
        if (!col) continue;
        float la[3], lb[3];
        worldToLocal(p, a, la);
        worldToLocal(p, b, lb);
        float dir[3] = { lb[0] - la[0], lb[1] - la[1], lb[2] - la[2] };
        if (collisionRayHit(col, la, dir, maxD)) return 0;
    }
    return 1;
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
static float health = 100.0f;       /* derived HUD value = 100 - bloodloss */
/* Injury / bleeding / sanity model (Main_Core.bb): damage adds
 * injuries; injuries bleed into bloodloss; bloodloss >= 100 is fatal;
 * first aid clots it. Sanity drops in the dark / pocket dimension and
 * near SCP-106, distorting the view when low. */
static float injuries;              /* 0..~5 */
static float bloodloss;             /* 0..100 */
static float sanity = 100.0f;       /* 0..100 (source uses 0..-850) */
static float damageFlash;           /* red flash on taking damage */
static float fallPeakY;             /* apex of the current fall */
static char deathCause[64] = "SCP-173";
/* Wearables and hand items (used from the inventory). */
static int wearGasMask;
static int wearNVG;                 /* 0 none, 1 normal, 2 fine */
static int wear268;                 /* SCP-268: unseen by NPCs */
static int wearVest;
static int wearHazmat;              /* delays SCP-049's "cure" */
static int using714;                /* SCP-714 jade ring: wards off 049 */
static float rmHazmatTimer = 500.0f; /* 049 tearing the suit off */
static float rm714Timer = 500.0f;    /* 049 forcing the ring off */
static int radioChannel = -1;       /* -1 off, 0..3 = SCPRadio0-3 */
static int currentMusicZone = -1;   /* per-zone music tracking */
static int currentAmbienceId;       /* active room ambience emitter id */
static int blinkFrames;             /* >0 while eyes closed */
static float stamina = 100.0f;      /* sprint resource */
static float playerNoise;           /* 0..1 how loud the player is (SCP-939) */
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

static void spawnRoomEvents(void);
static void updateRoomEvents(void);
static void reset106(void);
static void enterPocketDimension(void);
static void updatePocketDimension(void);
static void teslaSpawn(void);
static void spawn096(void);
static void reset096(void);
static void spawn049(void);
static void reset049(void);
static void spawn939(void);
static void reset939(void);
static void spawn966(void);
static void spawn860(void);
static void reset860(void);
static void removeInventoryByName(const char *name);
static void consumeSlot(int slot);
static void spawn079(void);
static void spawn895(void);
static void spawn012(void);
static void spawn372(void);
static void spawn205(void);
static void spawn914(void);
static void spawn035(void);

static void regenerateMap(uint32_t seed) {
    memset(roomVisited, 0, sizeof(roomVisited));
    currentMusicZone = -1;
    currentAmbienceId = 0;
    inPocket = 0;
    inMask = 0;
    maskEntries = 0;
    elevState = ELEV_IDLE;
    npc1499Count = 0;
    npc513Active = 0;
    pdLunging = 0;
    femurTimer = 0.0f;
    npc106Contained = 0;
    injuries = 0.0f;
    bloodloss = 0.0f;
    sanity = 100.0f;
    equippedNav = -1;
    navVisible = 1;
    health = 100.0f;
    damageFlash = 0.0f;
    wearGasMask = 0;
    wearNVG = 0;
    wear268 = 0;
    wearVest = 0;
    wearHazmat = 0;
    using714 = 0;
    rmHazmatTimer = 500.0f;
    rm714Timer = 500.0f;
    if (radioChannel >= 0) radioChannel = -1;
    for (uint32_t i = 0; i < tplList.count; i++) {
        templateUnload(&tplRT[i]);
    }
    mapFree(&map);
    doorsFree(&doors);
    mapSeed = seed;
    if (mapGenerate(&tplList, mapSeed, &map)) {
        generateAccessCodes(mapSeed);
        appendIntroRoom();
        appendPocketRoom();
        appendMaskRoom();
        doorsGenerate(&map, &tplList, mapSeed ^ 0x9E3779B9u, &doors);
        spawnRoomDoors();
        spawnPocketLabyrinthDoors();
        appendGateRooms();
        setupMaintenanceTunnel();
        spawnRoomFixtures();
        spawnRoomCameras();
        spawnRoomEvents();
        teslaSpawn();
        spawn096();
        spawn049();
        spawn939();
        spawn966();
        spawn860();
        spawn079();
        spawn895();
        spawn012();
        spawn372();
        spawn205();
        spawn914();
        spawn035();
        eventRng = mapSeed ^ 0xA5A5F00Du;
        spawnItems();
        spawnRoomDecals();
        reset173();
        reset106();
        {
            struct mallinfo mi = mallinfo();
            plog("map gen: seed %u rooms %u; heap used=%uK free=%uK",
                 mapSeed, map.roomCount, (unsigned)mi.uordblks / 1024,
                 (unsigned)mi.fordblks / 1024);
        }
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
    /* SCP-106 zooms the lens in unnervingly when it is watching
     * (me\CurrCameraZoom); clamp so it never crosses over. */
    float fov = 60.0f - cameraZoom;
    if (fov < 25.0f) fov = 25.0f;
    float fovY = fov * 3.14159265f / 180.0f;
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
            /* Additive lightmap. A modulate2x pass (GL_DST_COLOR,
             * GL_SRC_COLOR) is mathematically closer to the source's
             * 2 * diffuse * (ambient + lightmap), but on device it
             * darkened the world to near-black: the RMESH vertex colours
             * are baked into BOTH passes (the colour array stays bound,
             * so the result is 2 * diffuse * lm * vc^2) and the port has
             * no AmbientLightRoomTex floor, so dim lightmaps multiply to
             * black. Bisected on hardware to the modulate change; keep
             * the known-good additive until a corrected modulate (single
             * vc + ambient floor) can be tested on device. */
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

static int floorIsMetal(const float pos[3]) {
    int px = (int)floorf(pos[0] / ROOM_SPACING + 0.5f);
    int py = (int)floorf(pos[2] / ROOM_SPACING + 0.5f);
    const RoomPlacement *rp = NULL;
    for (int i = 0; i < activeCount; i++) {
        if (activeRooms[i]->gridX == px && activeRooms[i]->gridY == py) {
            rp = activeRooms[i];
            break;
        }
    }
    if (!rp) return 0;
    const TemplateRT *rt = &tplRT[rp->templateIndex];
    if (rt->state != 1 || !rt->scene) return 0;

    float local[3];
    worldToLocal(rp, pos, local);
    float feet = local[1] - EYE_HEIGHT; /* local floor is y ~ 0 */
    float bestY = -1e30f;
    const char *bestTex = NULL;
    for (uint32_t b = 0; b < rt->scene->batchCount; b++) {
        const SceneBatch *bt = &rt->scene->batches[b];
        for (uint32_t t = 0; t + 2 < bt->indexCount; t += 3) {
            const SceneVertex *v0 = &bt->vertices[bt->indices[t]];
            const SceneVertex *v1 = &bt->vertices[bt->indices[t + 1]];
            const SceneVertex *v2 = &bt->vertices[bt->indices[t + 2]];
            /* Barycentric point-in-triangle on XZ. */
            float d = (v1->z - v2->z) * (v0->x - v2->x)
                    + (v2->x - v1->x) * (v0->z - v2->z);
            if (fabsf(d) < 1e-4f) continue;
            float a = ((v1->z - v2->z) * (local[0] - v2->x)
                     + (v2->x - v1->x) * (local[2] - v2->z)) / d;
            float bb = ((v2->z - v0->z) * (local[0] - v2->x)
                      + (v0->x - v2->x) * (local[2] - v2->z)) / d;
            float c = 1.0f - a - bb;
            if (a < -0.02f || bb < -0.02f || c < -0.02f) continue;
            float y = a * v0->y + bb * v1->y + c * v2->y;
            if (y <= feet + 40.0f && y > bestY) {
                bestY = y;
                bestTex = bt->diffuseName;
            }
        }
    }
    return textureIsMetal(bestTex);
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
/* Difficulty tier (source SelectedDifficulty\OtherFactors): 0 Easy .. 3
 * Extreme. Set by applyDifficulty; used by SCP-914's keycard Mastercard
 * odds. */
static int gDiffFactor = 1;

#define SFX_DIR DATA_ROOT "/SFX"

static int sndDoorOpen[3], sndDoorClose[3];
static int sndElevOpen[3], sndElevClose[3], sndElevMove = -1;
static int sndBigOpen[3], sndBigClose[3];
static int sndStep[8], sndRun[7];
static int sndStepMetal[8], sndRunMetal[8];
static int sndButton[2], sndKeycardUse[2], sndDoorLock, sndLever;
static GLuint texButtonRed;
static int sndPick[4];
static int sndDamage[4];
static int sndAmbience;
static int sndRattle[3], sndNeckSnap[3], sndStoneDrag;
static int snd106Corr[3], snd106Wall[3], snd106Decay[4];
static int snd106Laugh, snd106Breath;
static int sndFemur = -1;
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
        snprintf(p, sizeof(p), SFX_DIR "/Door/ElevatorOpen%d.ogg", i);
        sndElevOpen[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/Door/ElevatorClose%d.ogg", i);
        sndElevClose[i] = audioLoad(p);
    }
    sndElevMove = audioLoad(SFX_DIR "/General/Elevator/Moving.ogg");
    for (int i = 0; i < 8; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/Step/Step%d.ogg", i);
        sndStep[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/Step/StepMetal%d.ogg", i);
        sndStepMetal[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/Step/RunMetal%d.ogg", i);
        sndRunMetal[i] = audioLoad(p);
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
    sndLever = audioLoad(SFX_DIR "/Interact/LeverFlip.ogg");
    sndHorror11 = audioLoad(SFX_DIR "/Horror/Horror11.ogg");
    snd682Roar = audioLoad(SFX_DIR "/SCP/682/Roar.ogg");
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
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/SCP/106/Corrosion%d.ogg", i);
        snd106Corr[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/SCP/106/WallDecay%d.ogg", i);
        snd106Wall[i] = audioLoad(p);
    }
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/SCP/106/Decay%d.ogg", i);
        snd106Decay[i] = audioLoad(p);
    }
    snd106Laugh = audioLoad(SFX_DIR "/SCP/106/Laugh.ogg");
    snd106Breath = audioLoad(SFX_DIR "/SCP/106/Breathing.ogg");
    sndFemur = audioLoad(SFX_DIR "/Room/106Chamber/FemurBreaker.ogg");
    sndMagnetUp = audioLoad(SFX_DIR "/Room/106Chamber/MagnetUp.ogg");
    sndMagnetDown = audioLoad(SFX_DIR "/Room/106Chamber/MagnetDown.ogg");
    sndPdEnter = audioLoad(SFX_DIR "/Room/PocketDimension/Enter.ogg");
    sndPdExit = audioLoad(SFX_DIR "/Room/PocketDimension/Exit.ogg");
    sndPdRumble = audioLoad(SFX_DIR "/Room/PocketDimension/Rumble.ogg");
    sndPdExplode = audioLoad(SFX_DIR "/Room/PocketDimension/Explosion.ogg");
    snd096Trigger = audioLoad(SFX_DIR "/SCP/096/Triggered.ogg");
    snd096Scream = audioLoad(SFX_DIR "/SCP/096/Scream.ogg");
    snd049Breath = audioLoad(SFX_DIR "/SCP/049/Breath.ogg");
    snd049Horror = audioLoad(SFX_DIR "/SCP/049/Horror.ogg");
    snd939Horror = audioLoad(SFX_DIR "/SCP/939/Horror.ogg");
    for (int i = 0; i < 3; i++) {
        char p[128];
        snprintf(p, sizeof(p), SFX_DIR "/SCP/939/0Attack%d.ogg", i);
        snd939Attack[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/SCP/939/0Lure%d.ogg", i);
        snd939Lure[i] = audioLoad(p);
    }
    snd1499Enter = audioLoad(SFX_DIR "/SCP/1499/Enter.ogg");
    snd1499Exit = audioLoad(SFX_DIR "/SCP/1499/Exit.ogg");
    snd1499Trig = audioLoad(SFX_DIR "/SCP/1499/Triggered.ogg");
    for (int i = 0; i < 4; i++) {
        char p[128];
        snprintf(p, sizeof(p), SFX_DIR "/SCP/1499/Idle%d.ogg", i);
        snd1499Idle[i] = audioLoad(p);
    }
    for (int i = 0; i < 3; i++) {
        char p[128];
        snprintf(p, sizeof(p), SFX_DIR "/SCP/049/Spotted%d.ogg", i);
        snd049Spot[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/SCP/049/Searching%d.ogg", i);
        snd049Search[i] = audioLoad(p);
    }
    sndTeslaIdle = audioLoad(SFX_DIR "/Room/Tesla/Idle.ogg");
    sndTeslaWind = audioLoad(SFX_DIR "/Room/Tesla/WindUp.ogg");
    sndTeslaShock = audioLoad(SFX_DIR "/Room/Tesla/Shock.ogg");
    sndTeslaPower = audioLoad(SFX_DIR "/Room/Tesla/PowerUp.ogg");
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
    sndCamCheck = audioLoad(SFX_DIR "/Character/MTF/AnnouncCameraCheck.ogg");
    sndCamFound[0] = audioLoad(SFX_DIR "/Character/MTF/AnnouncCameraFound1.ogg");
    sndCamFound[1] = audioLoad(SFX_DIR "/Character/MTF/AnnouncCameraFound2.ogg");
    sndCamNoFound = audioLoad(SFX_DIR "/Character/MTF/AnnouncCameraNoFound.ogg");
    snd079Speech = audioLoad(SFX_DIR "/SCP/079/Speech.ogg");
    snd079Refuse = audioLoad(SFX_DIR "/SCP/079/Refuse.ogg");
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/Room/895Chamber/GuardIdle%d.ogg", i);
        snd895Idle[i] = audioLoad(p);
        snprintf(p, sizeof(p), SFX_DIR "/Room/895Chamber/GuardScream%d.ogg", i);
        snd895Scream[i] = audioLoad(p);
    }
    snd205Horror = audioLoad(SFX_DIR "/SCP/205/Horror.ogg");
    snd914 = audioLoad(SFX_DIR "/SCP/513/914Refine.ogg");
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof(p), SFX_DIR "/SCP/513_1/Bell%d.ogg", i);
        snd513Bell[i] = audioLoad(p);
    }
    snd035GetUp = audioLoad(SFX_DIR "/SCP/035/GetUp.ogg");
    sndAmbience = audioLoad(SFX_DIR "/Ambient/Room ambience/rumble.ogg");
    audioLoopAmbience(sndAmbience, 0.30f);
}

static ModelRT doorFrameRT, doorPanelRT, heavy1RT, heavy2RT;
static ModelRT elevatorRT, big1RT, big2RT, bigFrameRT;
static ModelRT officeRT, officeFrameRT, woodenRT, woodenFrameRT;
static ModelRT oneSidedRT, door914RT;
static ModelRT buttonRT, buttonKeycardRT;
static ModelRT buttonKeypadRT, buttonScannerRT, buttonElevatorRT;
static ModelRT leverBaseRT, leverHandleRT;
/* Security cameras (CreateSecurityCam): a static base + a head that
 * sweeps or tracks the player, its red light blinking. */
static ModelRT camBaseRT, camHeadRT;
static GLuint camHeadRedTex;
/* Live monitor feed: the nearest Screen camera renders the scene to a
 * texture (glCopyTexImage2D from a viewport corner, no FBO - matching the
 * blur post-process) that its monitor displays. */
static ModelRT monitorRT;
#define MON_FEED_SIZE 256
static GLuint monFeedTex;
static int monFeedCam = -1;   /* worldCameras index the feed shows */
static float monFeedTick;
static int monFeedActive;     /* a feed was captured this frame */
/* SCP-106 rots the doors it passes (UpdateNPCType106 swaps the panel /
 * frame texture): Door01_Corrosive for sliding doors, containment for
 * heavy. Loaded with the door assets. */
static GLuint doorCorrTex, doorCorrHeavyTex;
/* Pocket dimension's flying pillars: animated props that orbit and
 * kill on contact (PD_FourWayRoom). */
static ModelRT pdPillarRT;
static float pdPillar[2][3];  /* world positions, updated per frame */
static int pdPillarsOn;       /* active this frame */
/* SCP-682's arm: at a roar's climax a huge reptilian arm smashes
 * through and sweeps the room (Events_Core scp_682_arm), then retracts. */
static ModelRT arm682RT;
/* SCP-860-1's forest room copse (built with the door assets). */
static ModelRT tree860RT;
static int arm682Active;
static float arm682Roll;      /* swing angle, 180 -> 360 */
static float arm682Pos[3];
static float arm682Yaw;
/* SCP-079 (cont1_079): the sentient computer in the lower chamber. Its
 * screen flickers overlay frames as it speaks; approaching the terminal
 * plays its speech and drops the SCP-079 document. */
static ModelRT scp079RT;
static GLuint scp079Ov[7];
static int scp079Ok;          /* placed in this map */
static float scp079Pos[3];    /* computer world position (raw) */
static float scp079Yaw;       /* world yaw */
static int scp079State;       /* 0 idle, 1 speaking, 2 spoken/cooldown */
static float scp079Timer;     /* state timer (frames) */
static int scp079OvFrame;     /* current flicker overlay 0..6 */
static float scp079FlickT;    /* flicker cadence */
static int scp079DocDone;     /* the document has been dropped */
/* SCP-895 (cont1_895): the camera coffin. A dread aura fills the chamber
 * and a slumped guard corpse is revealed on a close approach. */
static int scp895Ok;
static float scp895Coffin[3]; /* coffin trigger world position */
static float scp895GuardYaw;  /* corpse yaw (room-facing) */
static int scp895State;       /* 0 dread, 1 guard revealed/screaming */
static float scp895Timer;
static float scp895IdleT;     /* idle-murmur cadence */
/* SCP-012 (cont2_012): "A Bad Composition". Standing at the score forces
 * the eyes open and floods the screen with a bloody compulsion overlay. */
static int scp012Ok;
static float scp012Pos[3];    /* the score's world position */
static GLuint scp012OvTex;    /* scp_012_overlay.png */
static float scp012Comp;      /* 0..1 compulsion strength (drives overlay) */
/* SCP-914 (cont1_914): the refinement machine. A knob (5 settings) + two
 * booths; feeding it the selected item and running it transforms the item
 * per the setting (Use914). */
static ModelRT knob914RT, key914RT;
static int scp914Ok;
static float scp914In[3];     /* input booth (Objects[2]) */
static float scp914Out[3];    /* output booth (Objects[3]) */
static float scp914Knob[3];   /* the setting knob (Objects[1]) */
static float scp914Yaw;       /* room yaw */
static int scp914Setting;     /* -2 ROUGH .. 2 VERYFINE */
static int scp914State;       /* 0 idle, 1 running */
static float scp914Timer;
static int scp914RefineTpl = -1; /* item being refined */
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
    buildModelRT(&buttonKeypadRT, "ButtonCode.b3d", 0, 0, 0, NULL);
    buildModelRT(&buttonScannerRT, "ButtonScanner.b3d", 0, 0, 0, NULL);
    buildModelRT(&buttonElevatorRT, "ButtonElevator.b3d", 0, 0, 0, NULL);
    buttonKeypadRT.scale[0] = buttonKeypadRT.scale[1] =
        buttonKeypadRT.scale[2] = 7.68f;
    buttonScannerRT.scale[0] = buttonScannerRT.scale[1] =
        buttonScannerRT.scale[2] = 7.68f;
    buttonElevatorRT.scale[0] = buttonElevatorRT.scale[1] =
        buttonElevatorRT.scale[2] = 7.68f;
    /* CreateLever scales base and handle to 0.036 world = 9.216 raw. */
    buildModelRT(&leverBaseRT, "LeverBase.b3d", 0, 0, 0, NULL);
    buildModelRT(&leverHandleRT, "LeverHandle.b3d", 0, 0, 0, NULL);
    leverBaseRT.scale[0] = leverBaseRT.scale[1] = leverBaseRT.scale[2]
        = 9.216f;
    leverHandleRT.scale[0] = leverHandleRT.scale[1] = leverHandleRT.scale[2]
        = 9.216f;
    buildModelRT(&pdPillarRT, "dimension_106_pillar.b3d", 0, 0, 0, NULL);
    pdPillarRT.scale[0] = pdPillarRT.scale[1] = pdPillarRT.scale[2] = 256.0f;
    buildModelRT(&arm682RT, "scp_682_arm.b3d", 0, 0, 0, NULL);
    arm682RT.scale[0] = arm682RT.scale[1] = arm682RT.scale[2] = 40.0f;
    buildModelRT(&tree860RT, "tree.b3d", 0, 0, 0, NULL);
    tree860RT.scale[0] = tree860RT.scale[1] = tree860RT.scale[2] = 256.0f;
    doorCorrTex = textureGet("Door01_Corrosive.png");
    doorCorrHeavyTex = textureGet("containment_doors_Corrosive.png");
    teslaArcTex = textureGet("tesla_overlay.png");
    /* Security cameras: base ScaleEntity 0.0015, head 0.01 (world), so
     * *256 in raw. camera(1) is the default head texture, camera(2) the
     * red-light one. */
    buildModelRT(&camBaseRT, "CamBase.b3d", 0, 0, 0, NULL);
    camBaseRT.scale[0] = camBaseRT.scale[1] = camBaseRT.scale[2] = 0.384f;
    buildModelRT(&camHeadRT, "CamHead.b3d", 0, 0, 0, "camera(1).png");
    camHeadRT.scale[0] = camHeadRT.scale[1] = camHeadRT.scale[2] = 2.56f;
    camHeadRedTex = textureGet("camera(2).png");
    /* Monitor prop for the camera feed (source Scale = RoomScale*1.8). */
    buildModelRT(&monitorRT, "monitor2.b3d", 0, 0, 0, NULL);
    monitorRT.scale[0] = monitorRT.scale[1] = monitorRT.scale[2] = 1.8f;
    tex035 = textureGet("scp_035_victim.png");
    /* SCP-914's knob and control key (source ScaleEntity RoomScale -> 1). */
    buildModelRT(&knob914RT, "scp_914_knob.b3d", 0, 0, 0, NULL);
    knob914RT.scale[0] = knob914RT.scale[1] = knob914RT.scale[2] = 1.0f;
    buildModelRT(&key914RT, "scp_914_key.b3d", 0, 0, 0, NULL);
    key914RT.scale[0] = key914RT.scale[1] = key914RT.scale[2] = 1.0f;
    /* SCP-079's computer (ScaleEntity 1.3 world -> *256 raw) and its
     * seven screen-overlay frames. */
    buildModelRT(&scp079RT, "scp_079.b3d", 0, 0, 0, NULL);
    scp079RT.scale[0] = scp079RT.scale[1] = scp079RT.scale[2] = 332.8f;
    for (int i = 0; i < 7; i++) {
        char nm[48];
        snprintf(nm, sizeof(nm), "scp_079_overlay(%d).png", i + 1);
        scp079Ov[i] = textureGet(nm);
    }
    scp012OvTex = textureGet("scp_012_overlay.png");
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

/* ---- SCP-106: a slow roaming hunter that spawns near the player,
 * pursues, and drags them under on contact (UpdateNPCType106). ---- */
static SkinnedMesh *skin106;
static float skin106Scale = 1.0f;
static GLuint vbo106;
static int posed106;
enum { N106_DORMANT = 0, N106_SPAWNING, N106_CHASING, N106_SINKING,
       N106_GRABBING };
static int npc106State;
static float npc106GrabTimer;   /* grab-and-wrench progress before the PD */
static int npc106Active;
static float npc106Pos[3];
static float npc106YawDeg;
static float npc106Frame;
static float npc106Timer;      /* dormant spawn countdown */
static int npc106Cool;         /* teleport-behind cooldown */
static float npc106ChaseTimer; /* sinks away when it runs out unseen (State3) */

/* ---- SCP-096: harmless while unseen; looking at its face enrages it,
 * and after a scream it sprints down and kills you (UpdateNPCType096).
 * Anim frames from the source: sit 936..1263, get-up 193..311, scream
 * 1471..1556, run 737..935. ---- */
static SkinnedMesh *skin096;
static float skin096Scale = 1.0f;
static GLuint vbo096;
static int posed096;
enum { S096_IDLE = 0, S096_TRIGGERED, S096_CHASE };
static int npc096State;
static int npc096Active;
static float npc096Pos[3];
static float npc096YawDeg;
static float npc096Frame;
static float npc096ScreamTimer; /* frames spent screaming before it runs */

/* ---- SCP-049 (the Plague Doctor) and its reanimated 049-2 - slow,
 * relentless pursuers that kill on a touch (UpdateNPCType049 / 049_2).
 * 049 frames: idle 269..345, walk 346..463, kill 537..660; 049-2 walk
 * 705..794. ---- */
static SkinnedMesh *skin049;
static float skin049Scale = 1.0f;
static GLuint vbo049;
static int posed049;
enum { S049_IDLE = 0, S049_PURSUE, S049_KILL };
static int npc049State;
static int npc049Active;
static float npc049Pos[3];
static float npc049YawDeg;
static float npc049Frame;
static float npc049Timer;       /* activation delay */
static int npc049Cool;          /* teleport-closer cooldown */
#define MAX_0492 3
static SkinnedMesh *skin0492;
static float skin0492Scale = 1.0f;
static GLuint vbo0492;
static int posed0492;
static float npc0492Frame;      /* shared walk phase (skinned once) */
static float npc0492Pos[MAX_0492][3];
static float npc0492Yaw[MAX_0492];
static int npc0492Active[MAX_0492];
static int npc0492Count;
static float npc0492Cool;       /* shared bite cadence */

/* ---- SCP-939: a blind pack predator in room3_storage that hunts by
 * sound - loud players draw it, a hushed crouch slips past. Bites hurt;
 * enough bites kill (UpdateNPCType939). Frames: idle 290..405, walk
 * 644..683, lunge 449..464, bite 18..68. ---- */
static SkinnedMesh *skin939;
static float skin939Scale = 1.0f;
static GLuint vbo939;
static int posed939;
enum { S939_PATROL = 0, S939_ALERT, S939_ATTACK };
static int npc939State;
static int npc939Active;
static float npc939Pos[3];
static float npc939YawDeg;
static float npc939Frame;
static float npc939Cool;        /* bite cadence */
static float npc939Home[2];     /* room centre it patrols around */
static float npc939Wander[2];   /* current patrol target */

/* ---- SCP-966: a sleep-stalker invisible to the naked eye - only the
 * night-vision goggles reveal it. It creeps closer and saps the player's
 * rest; exhausted and caught, it kills (UpdateNPCType966). ---- */
static SkinnedMesh *skin966;
static float skin966Scale = 1.0f;
static GLuint vbo966;
static int posed966;
static int npc966Active;
static float npc966Pos[3];
static float npc966YawDeg;
static float npc966Frame;
static float npc966Drowsy;      /* insomnia buildup / aggression */

/* ---- SCP-372: the peripheral jumper (cont3_372). Invisible until it
 * flits into view; centring it in your gaze makes it dart back to the
 * edge of sight (UpdateNPCType372). ---- */
static SkinnedMesh *skin372;
static float skin372Scale = 1.0f;
static GLuint vbo372;
static int posed372;
static int npc372Ok;            /* present in this map */
static int npc372Idle;          /* 1 = hidden, 0 = flitting nearby */
static float npc372Pos[3];
static float npc372Frame;
static float npc372State;       /* remaining active duration */

/* ---- SCP-205: the shadow demon of the lamps (cont1_205). It rises and
 * looms in the chamber while the player is there, seen chiefly on the
 * observation monitor the camera feeds. ---- */
static SkinnedMesh *skin205;
static float skin205Scale = 1.0f;
static GLuint vbo205;
static int posed205;
static int npc205Ok;
static float npc205Pos[3];      /* demon current position (world) */
static float npc205Home[3];     /* its spawn, returned to when you leave */
static float npc205Yaw;
static float npc205Frame;
static float npc205Rise;        /* 0..1 how far it has risen/loomed */
static float npc205Horror;      /* horror-cue cadence */

/* ---- Hostiles SCP-914 can produce: an SCP-008 zombie (from a refined
 * severed hand) or a hostile SCP-1499 (from Very Fine SCP-1499). One at a
 * time; it shambles to the player and kills on contact. ---- */
static SkinnedMesh *skin008;
static float skin008Scale = 1.0f;
static GLuint rhVbo;             /* shared render buffer for the hostile */
static int rhActive;
static int rhType;               /* 0 = SCP-008, 1 = SCP-1499 */
static float rhPos[3];
static float rhYaw;
static float rhFrame;
static int refineHostilePending = -1; /* set by refine914 (-1 none) */

/* ---- SCP-513-1: rung up by the SCP-513 bell, it haunts the player -
 * flitting to the edge of sight and ringing bells (UpdateNPCType513_1).
 * A persistent scare, never lethal. ---- */
static SkinnedMesh *skin513;
static float skin513Scale = 1.0f;
static GLuint vbo513;
static int posed513;
static float npc513Pos[3];
static float npc513Yaw;
static float npc513Frame;
static float npc513Timer;      /* reposition cadence */
static float npc513Bell;       /* bell-ring cadence */

/* ---- SCP-035: the possessive mask (cont1_035). Its Class-D host sits
 * slumped until you look at it up close, then it gets up and hunts.
 * (tex035 declared with the early texture globals.) ---- */
static GLuint vbo035;
static int npc035Ok;
static int npc035State;        /* 0 seated, 1 risen/hunting */
static float npc035Pos[3];
static float npc035Home[3];
static float npc035Yaw;
static float npc035Frame;

/* ---- SCP-1499-1: the hooded people of the mask dimension. They roam
 * and, once roused, converge and kill on contact (UpdateNPCType1499_1).
 * Shared walk phase, drawn per instance. ---- */
static SkinnedMesh *skin1499;
static float skin1499Scale = 1.0f;
static GLuint vbo1499;
static GLuint tex1499King;      /* the king's distinct texture */

/* ---- SCP-860-1: the thing in the forest room (cont2_860_1). It only
 * moves while unwatched - looked at (or blinked away from) it freezes -
 * and kills on contact. Trees dot the room (UpdateNPCType860_2). ---- */
static SkinnedMesh *skin860;
static float skin860Scale = 1.0f;
static GLuint vbo860;
static int posed860;
static int npc860Active;
static float npc860Pos[3];
static float npc860YawDeg;
static float npc860Frame;
static int npc860Cool;
#define MAX_TREES 96   /* a 10x10 forest grid minus the carved path */
static float tree860Pos[MAX_TREES][3];
static int tree860Count;

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
        B3DModel *m106 = propModelGet("scp_106.b3d");
        skin106 = m106 ? skinnedCreate(m106) : NULL;
        if (skin106) {
            skinnedBounds(skin106, mn, mx);
            if (mx[1] > mn[1]) skin106Scale = 300.0f / (mx[1] - mn[1]);
        }
        B3DModel *m096 = propModelGet("scp_096.b3d");
        skin096 = m096 ? skinnedCreate(m096) : NULL;
        if (skin096) {
            skinnedBounds(skin096, mn, mx);
            /* Taller than 106 - it's a lanky ~2.4 m creature. */
            if (mx[1] > mn[1]) skin096Scale = 460.0f / (mx[1] - mn[1]);
        }
        B3DModel *m049 = propModelGet("scp_049.b3d");
        skin049 = m049 ? skinnedCreate(m049) : NULL;
        if (skin049) {
            skinnedBounds(skin049, mn, mx);
            if (mx[1] > mn[1]) skin049Scale = 300.0f / (mx[1] - mn[1]);
        }
        B3DModel *m0492 = propModelGet("scp_049_2.b3d");
        skin0492 = m0492 ? skinnedCreate(m0492) : NULL;
        if (skin0492) {
            skinnedBounds(skin0492, mn, mx);
            if (mx[1] > mn[1]) skin0492Scale = 285.0f / (mx[1] - mn[1]);
        }
        B3DModel *m939 = propModelGet("scp_939.b3d");
        skin939 = m939 ? skinnedCreate(m939) : NULL;
        if (skin939) {
            skinnedBounds(skin939, mn, mx);
            if (mx[1] > mn[1]) skin939Scale = 320.0f / (mx[1] - mn[1]);
        }
        B3DModel *m966 = propModelGet("scp_966.b3d");
        skin966 = m966 ? skinnedCreate(m966) : NULL;
        if (skin966) {
            skinnedBounds(skin966, mn, mx);
            if (mx[1] > mn[1]) skin966Scale = 300.0f / (mx[1] - mn[1]);
        }
        B3DModel *m1499 = propModelGet("scp_1499_1.b3d");
        skin1499 = m1499 ? skinnedCreate(m1499) : NULL;
        if (skin1499) {
            skinnedBounds(skin1499, mn, mx);
            if (mx[1] > mn[1]) skin1499Scale = 285.0f / (mx[1] - mn[1]);
            tex1499King = textureGet("scp_1499_1_king.png");
        }
        B3DModel *m860 = propModelGet("scp_860_2.b3d");
        skin860 = m860 ? skinnedCreate(m860) : NULL;
        if (skin860) {
            skinnedBounds(skin860, mn, mx);
            if (mx[1] > mn[1]) skin860Scale = 320.0f / (mx[1] - mn[1]);
        }
        B3DModel *m372 = propModelGet("scp_372.b3d");
        skin372 = m372 ? skinnedCreate(m372) : NULL;
        if (skin372) {
            skinnedBounds(skin372, mn, mx);
            if (mx[1] > mn[1]) skin372Scale = 200.0f / (mx[1] - mn[1]);
        }
        B3DModel *m205 = propModelGet("scp_205_demon.b3d");
        skin205 = m205 ? skinnedCreate(m205) : NULL;
        if (skin205) {
            skinnedBounds(skin205, mn, mx);
            if (mx[1] > mn[1]) skin205Scale = 300.0f / (mx[1] - mn[1]);
        }
        B3DModel *m008 = propModelGet("scp_008_1.b3d");
        skin008 = m008 ? skinnedCreate(m008) : NULL;
        if (skin008) {
            skinnedBounds(skin008, mn, mx);
            if (mx[1] > mn[1]) skin008Scale = 300.0f / (mx[1] - mn[1]);
        }
        B3DModel *m513 = propModelGet("scp_513_1.b3d");
        skin513 = m513 ? skinnedCreate(m513) : NULL;
        if (skin513) {
            skinnedBounds(skin513, mn, mx);
            if (mx[1] > mn[1]) skin513Scale = 240.0f / (mx[1] - mn[1]);
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
    if (dot <= 0.55f) return 0; /* ~57-degree half-angle */
    /* And a wall must not block the view (source EntityVisible). */
    float head[3] = { npc173Pos[0], npc173Pos[1] + 200.0f, npc173Pos[2] };
    return lineOfSight(camPos, head);
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
    if (inPdBounds(x, z)) return 1;
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
/* Waypoint navigation: BFS over occupied room cells (4-connected,
 * which is how CreateMap links rooms) from 173's cell toward a goal
 * cell, then aim at the center of the next cell on the path so 173
 * rounds corners through doorways instead of grinding straight into
 * walls. Falls back to the direct vector when goal and 173 share a
 * cell or no path exists. */
static int cellIndexAt(float x, float z) {
    int gx = (int)floorf(x / ROOM_SPACING + 0.5f);
    int gy = (int)floorf(z / ROOM_SPACING + 0.5f);
    for (uint32_t i = 0; i < map.roomCount; i++) {
        if (map.rooms[i].gridX == gx && map.rooms[i].gridY == gy) {
            return (int)i;
        }
    }
    return -1;
}

/* A room's open edges as a 4-bit mask (bit0 +Z/N, bit1 +X/E, bit2 -Z/S,
 * bit3 -X/W), from its shape and rotation - the same per-shape opening
 * rules the door generator uses. Two grid-adjacent rooms are only
 * traversable when both have an opening on their shared edge, so the
 * pathfinder routes through real doorways instead of any adjacency. */
static int navRot90(int m) {
    int r = 0;
    if (m & 1) r |= 8;   /* N -> W */
    if (m & 2) r |= 1;   /* E -> N */
    if (m & 4) r |= 2;   /* S -> E */
    if (m & 8) r |= 4;   /* W -> S */
    return r;
}

static int roomOpenMask(const RoomPlacement *p) {
    int base;
    switch (tplList.items[p->templateIndex].shape) {
        case SHAPE_ROOM1:  base = 4; break;         /* S (dead end) */
        case SHAPE_ROOM2:  base = 1 | 4; break;     /* N+S corridor */
        case SHAPE_ROOM2C: base = 2 | 4; break;     /* E+S corner */
        case SHAPE_ROOM3:  base = 2 | 4 | 8; break; /* E+S+W tee */
        case SHAPE_ROOM4:  base = 15; break;        /* cross */
        default:           return 15;               /* special: open */
    }
    int a = p->angle & 3;
    for (int i = 0; i < a; i++) base = navRot90(base);
    return base;
}

static int navNextCell(float fromX, float fromZ, float goalX, float goalZ,
                       float out[2]) {
    int start = cellIndexAt(fromX, fromZ);
    int goal = cellIndexAt(goalX, goalZ);
    if (start < 0 || goal < 0 || start == goal) return 0;

    static int prev[1024];
    static int queue[1024];
    int qh = 0, qt = 0;
    uint32_t rc = map.roomCount < 1024 ? map.roomCount : 1024;
    for (uint32_t i = 0; i < rc; i++) prev[i] = -2;
    prev[start] = -1;
    queue[qt++] = start;
    const int dx4[4] = { 1, -1, 0, 0 }, dy4[4] = { 0, 0, 1, -1 };
    /* Bit the current room must have open toward the neighbour, and the
     * bit the neighbour must have open back, for each direction. */
    const int curBit[4] = { 2, 8, 1, 4 };   /* E, W, N, S */
    const int nbrBit[4] = { 8, 2, 4, 1 };   /* W, E, S, N */
    int found = 0;
    while (qh < qt) {
        int cur = queue[qh++];
        if (cur == goal) { found = 1; break; }
        int cx = map.rooms[cur].gridX, cy = map.rooms[cur].gridY;
        int curMask = roomOpenMask(&map.rooms[cur]);
        for (int d = 0; d < 4; d++) {
            if (!(curMask & curBit[d])) continue;
            for (uint32_t j = 0; j < rc; j++) {
                if (prev[j] != -2) continue;
                if (map.rooms[j].gridX == cx + dx4[d]
                    && map.rooms[j].gridY == cy + dy4[d]
                    && (roomOpenMask(&map.rooms[j]) & nbrBit[d])) {
                    prev[j] = cur;
                    if (qt < 1024) queue[qt++] = j;
                }
            }
        }
    }
    if (!found) return 0;
    /* Walk back to the cell right after start. */
    int step = goal;
    while (prev[step] != start && prev[step] >= 0) step = prev[step];
    out[0] = map.rooms[step].gridX * ROOM_SPACING;
    out[1] = map.rooms[step].gridY * ROOM_SPACING;
    return 1;
}

/* Direction toward `goal`, routed through the next doorway cell when
 * they are in different rooms. */
static void navDir(float goalX, float goalZ, float *dirX, float *dirZ) {
    float wp[2];
    float tx = goalX, tz = goalZ;
    if (navNextCell(npc173Pos[0], npc173Pos[2], goalX, goalZ, wp)) {
        tx = wp[0];
        tz = wp[1];
    }
    float ddx = tx - npc173Pos[0], ddz = tz - npc173Pos[2];
    float d = sqrtf(ddx * ddx + ddz * ddz);
    if (d < 1.0f) { *dirX = 0.0f; *dirZ = 0.0f; return; }
    *dirX = ddx / d;
    *dirZ = ddz / d;
}

/* Assign per-room events (a port of Loading_Core's CreateEvent calls
 * for the self-contained events the facility can run without the
 * unported SCPs). Deterministic from the map seed so saves match. */
static void spawnRoomEvents(void) {
    arm682Active = 0;
    struct { const char *room; int ev; int pct; } TABLE[] = {
        { "room2c_gw_lcz", EV_173_APPEAR, 80 },
        { "room2_6_lcz",   EV_173_APPEAR, 90 },
        { "room2_4_lcz",   EV_173_APPEAR, 60 },
        { "room2_4_hcz",   EV_173_APPEAR, 60 },
        { "room2_6_hcz",   EV_173_APPEAR, 50 },
        { "room3_2_ez",    EV_173_APPEAR, 80 },
        { "room3_3_ez",    EV_173_APPEAR, 60 },
        { "room2_lcz",     EV_TRICK,      15 },
        { "room2_3_lcz",   EV_TRICK,      15 },
        { "room2_5_hcz",   EV_682_ROAR,   50 },
        { "room3_hcz",     EV_682_ROAR,   50 },
        { "room2_5_ez",    EV_682_ROAR,   50 },
    };
    int n = (int)(sizeof(TABLE) / sizeof(TABLE[0]));
    uint32_t rng = mapSeed ^ 0x51ED2C0Bu;
    for (uint32_t r = 0; r < map.roomCount && r < MAX_EVENT_ROOMS; r++) {
        roomEventId[r] = EV_NONE;
        roomEventState[r] = 0.0f;
        const char *nm = tplList.items[map.rooms[r].templateIndex].name;
        for (int i = 0; i < n; i++) {
            if (strcmp(TABLE[i].room, nm) != 0) continue;
            rng = rng * 1664525u + 1013904223u;
            if ((int)((rng >> 16) % 100u) < TABLE[i].pct) {
                roomEventId[r] = TABLE[i].ev;
            }
            break;
        }
    }
}

/* The scripted SCP-173 ambush spot for e_173_appearing, per room
 * template (room-local raw units from Events_Core). */
static int event173Spot(const char *nm, uint32_t *rng, float out[3]) {
    if (!strcmp(nm, "room2_4_lcz") || !strcmp(nm, "room2_4_hcz")
        || !strcmp(nm, "room2_6_hcz")) {
        out[0] = 640.0f; out[1] = 100.0f; out[2] = -896.0f; return 1;
    }
    if (!strcmp(nm, "room2_6_lcz")) {
        out[0] = -832.0f; out[1] = 100.0f; out[2] = 0.0f; return 1;
    }
    if (!strcmp(nm, "room2c_gw_lcz")) {
        out[0] = -410.0f; out[1] = 100.0f; out[2] = 410.0f; return 1;
    }
    if (!strcmp(nm, "room3_2_ez") || !strcmp(nm, "room3_3_ez")) {
        *rng = *rng * 1664525u + 1013904223u;
        int k = (int)((*rng >> 18) % 3u);
        static const float S[3][3] = {
            { 736.0f, -512.0f, -400.0f },
            { -552.0f, -512.0f, -528.0f },
            { 736.0f, -512.0f, 272.0f },
        };
        out[0] = S[k][0]; out[1] = S[k][1]; out[2] = S[k][2];
        return 1;
    }
    return 0;
}

/* Run the event of the room the player is in or approaching. */
static void updateRoomEvents(void) {
    if (introPhase >= 0 || deathTimer > 0) return;
    int pcell = cellIndexAt(camPos[0], camPos[2]);
    for (uint32_t r = 0; r < map.roomCount && r < MAX_EVENT_ROOMS; r++) {
        int ev = roomEventId[r];
        if (ev == EV_NONE) continue;
        const RoomPlacement *p = &map.rooms[r];
        float cx = p->gridX * ROOM_SPACING, cz = p->gridY * ROOM_SPACING;
        float dx = camPos[0] - cx, dz = camPos[2] - cz;
        float dist = sqrtf(dx * dx + dz * dz);
        int inRoom = (pcell == (int)r);

        if (ev == EV_173_APPEAR) {
            /* Player nearing the room but not yet in it: pop 173 into
             * the scripted spot if it is idle, unseen and far. */
            if (dist < 1536.0f && !inRoom && npc173Active
                && !playerSees173()) {
                float pdx = camPos[0] - npc173Pos[0];
                float pdz = camPos[2] - npc173Pos[2];
                if (pdx * pdx + pdz * pdz > 1536.0f * 1536.0f) {
                    const char *nm =
                        tplList.items[p->templateIndex].name;
                    float local[3], w[3];
                    if (event173Spot(nm, &eventRng, local)) {
                        localToWorld(p, local, w);
                        npc173Pos[0] = w[0];
                        npc173Pos[2] = w[2];
                        float o[3] = { w[0], camPos[1] + 200.0f, w[2] };
                        float hy;
                        npc173Pos[1] = rayDownWorld(o, 3000.0f, &hy)
                                     ? hy : camPos[1] - EYE_HEIGHT;
                        roomEventId[r] = EV_NONE; /* one-shot */
                    }
                }
            }
        } else if (ev == EV_TRICK) {
            /* Deep in the room: a blink, a horror sting, and a shove
             * back the way you came - once. */
            if (inRoom && dist < 512.0f) {
                blinkFrames = 18;
                blinkTimer = 100.0f;
                audioPlay(sndHorror11, 1.0f, 0.0f);
                float b = sqrtf(dx * dx + dz * dz);
                if (b > 1.0f) {
                    camPos[0] += dx / b * 256.0f;
                    camPos[2] += dz / b * 256.0f;
                }
                camYaw += 3.14159265f;
                roomEventId[r] = EV_NONE;
            }
        } else if (ev == EV_682_ROAR) {
            /* e_682_roar is a pure audio + camera scare: 682 is never
             * seen and deals NO damage. A distant tremor builds, it
             * roars partway through, the shake peaks, then it fades. The
             * countdown only advances while the player is in the room.
             * (The arm bursting through belongs to the Gate B ending,
             * not this event; it is not triggered here.) */
            if (roomEventState[r] == 0.0f) {
                if (inRoom) {
                    eventRng = eventRng * 1664525u + 1013904223u;
                    roomEventState[r] =
                        600.0f + (float)((eventRng >> 16) % 900u);
                }
            } else if (inRoom) {
                float prev = roomEventState[r];
                roomEventState[r] -= 1.0f;
                if (prev > 300.0f && roomEventState[r] <= 300.0f) {
                    audioPlay(snd682Roar, 1.0f, 0.0f);
                }
                if (roomEventState[r] < 300.0f && roomEventState[r] > 120.0f) {
                    camShake = 2.5f;   /* the roar's aftermath */
                } else if (roomEventState[r] < 520.0f) {
                    camShake = 0.6f;   /* a low, distant tremor first */
                }
                if (roomEventState[r] <= 0.0f) roomEventId[r] = EV_NONE;
            }
        }
    }
    /* The arm sweep (reserved for the Gate B ending; dormant during the
     * roar event). It rolls in through an arc, then retracts. */
    if (arm682Active) {
        arm682Roll += 5.0f;
        camShake = 2.5f;
        if (arm682Roll >= 360.0f) arm682Active = 0;
    }
    if (camShake > 0.0f) camShake -= 0.05f;
}

static void update173(void) {
    if (!npc173Active || deathTimer > 0 || !walkMode || inPocket || inMask)
        return;
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
                /* During the intro the breach is scripted (the source
                 * sets the player non-playable and 173 only snaps the
                 * scripted NPCs, then leaves through the vent) - it must
                 * never kill the player here. */
                if (introPhase >= 0) return;
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
                float ndx, ndz;
                navDir(camPos[0], camPos[2], &ndx, &ndz);
                if (ndx == 0.0f && ndz == 0.0f) { /* same room */
                    ndx = dx / dist;
                    ndz = dz / dist;
                }
                move173(ndx, ndz, speed);
                npc173YawDeg = atan2f(ndx, ndz) * 180.0f / 3.14159265f;
                moving = 1;
            }
        } else if (npc173EnemyX != 0.0f || npc173EnemyZ != 0.0f) {
            /* Move to where the player was last seen. */
            float ex = npc173EnemyX - npc173Pos[0];
            float ez = npc173EnemyZ - npc173Pos[2];
            float ed = sqrtf(ex * ex + ez * ez);
            if (ed > 128.0f && rand() % 500 != 0) {
                float ndx, ndz;
                navDir(npc173EnemyX, npc173EnemyZ, &ndx, &ndz);
                if (ndx == 0.0f && ndz == 0.0f) {
                    ndx = ex / ed;
                    ndz = ez / ed;
                }
                move173(ndx, ndz, 97.0f);
                npc173YawDeg = atan2f(ndx, ndz) * 180.0f / 3.14159265f;
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

static IntroHuman INTRO_HUMANS[14];
static int introHumanCount;
static int introDIdx[2] = { -1, -1 }; /* the chamber-front Class-Ds */

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

        /* Ulgrin holds his post until the player actually steps out
         * of the cell (the door needs its moment to slide open). */
        int holdPost = !escortWalking && escortWp == 0
                    && camPos[2] - INTRO_GY * ROOM_SPACING < 560.0f;
        if (holdPost) {
            ulgrin->yawDeg = -atan2f(pdx, pdz) * 180.0f / 3.14159265f;
        }
        int walking = 0;
        if (!holdPost && escortWp < ESCORT_WP_COUNT && pdist < 1400.0f) {
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
                    injuries += 1.0f;
                    bloodloss += 28.0f;
                    damageFlash = 0.7f;
                    audioPlay(sndDamage[rand() % 4], 1.0f, 0.0f);
                    if (bloodloss >= 100.0f) {
                        bloodloss = 100.0f;
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

    /* Ulgrin's partner (the source's NPC[4]) trails behind the player
     * instead of standing at the cell: he walks to keep within a short
     * distance once the player is out and moving through the block. */
    IntroHuman *rear = introHumanCount > 1
                           && INTRO_HUMANS[1].rt == &introGuardRT
                       ? &INTRO_HUMANS[1]
                       : NULL;
    if (rear && introPhase >= 1 && introPhase <= 2) {
        float wx = INTRO_GX * ROOM_SPACING + rear->x;
        float wz = INTRO_GY * ROOM_SPACING + rear->z;
        float dx = camPos[0] - wx, dz = camPos[2] - wz;
        float d = sqrtf(dx * dx + dz * dz);
        int walking = 0;
        if (d > 360.0f) {
            float sp = 4.4f;
            rear->x += dx / d * sp;
            rear->z += dz / d * sp;
            rear->yawDeg = -atan2f(dx, dz) * 180.0f / 3.14159265f;
            float o[3] = { INTRO_GX * ROOM_SPACING + rear->x, 200.0f,
                           INTRO_GY * ROOM_SPACING + rear->z };
            float hy;
            if (rayDownWorld(o, 600.0f, &hy)) rear->y = hy;
            walking = 1;
        }
        if (walking && rear->animStart != 1.0f) {
            rear->animStart = 1.0f;
            rear->animEnd = 38.0f;
            rear->animSpeed = 0.66f;
            rear->frame = 1.0f;
        } else if (!walking && rear->animStart != 77.0f) {
            rear->animStart = 77.0f;
            rear->animEnd = 201.0f;
            rear->animSpeed = 0.4f;
            rear->frame = 77.0f;
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
            /* Only the escort opens doors (the source scripts each
             * one); opening every door near the player let side doors
             * open that never should. */
            int nearDoor = 0;
            if (ulgrin) {
                float ux = INTRO_GX * ROOM_SPACING + ulgrin->x - d->x;
                float uz = INTRO_GY * ROOM_SPACING + ulgrin->z - d->z;
                nearDoor = ux * ux + uz * uz < 380.0f * 380.0f;
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
                float bedX = -4240.0f, bedZ = 110.0f, bedY = 152.0f;
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
        case 2: /* the two Class-Ds are ordered in first, then the
                 * player follows them through the gate */
            for (int k = 0; k < 2; k++) {
                if (introDIdx[k] < 0) continue;
                IntroHuman *d = &INTRO_HUMANS[introDIdx[k]];
                float tx = k == 0 ? 1450.0f : 1350.0f;
                float tz = k == 0 ? 620.0f : 980.0f;
                float ddx = tx - d->x, ddz = tz - d->z;
                float dd = sqrtf(ddx * ddx + ddz * ddz);
                if (dd > 24.0f) {
                    d->x += ddx / dd * 3.6f;
                    d->z += ddz / dd * 3.6f;
                    d->yawDeg = -atan2f(ddx, ddz) * 180.0f / 3.14159265f;
                    if (d->animStart != 39.0f) {
                        /* Class-D walk cycle (the intro event's own
                         * AnimateNPC 39-76). */
                        d->animStart = 39.0f;
                        d->animEnd = 76.0f;
                        d->animSpeed = 0.55f;
                        d->frame = 39.0f;
                    }
                } else if (d->animStart == 39.0f) {
                    /* Arrived: settle into a standing idle (not the
                     * cowering pose, which read as dead). */
                    d->animStart = 210.0f;
                    d->animEnd = 235.0f;
                    d->animSpeed = 0.1f;
                    d->frame = 210.0f;
                }
            }
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
    /* The roster matches UpdateIntro's CreateNPC list: Ulgrin and his
     * partner at the cell, the radio guard, the neighboring inmate,
     * the chamber balcony guard, Franklin and the scientist in the
     * observation room, the two Class-Ds posted in front of SCP-173's
     * chamber (they get sent in ahead of the player), and the south
     * balcony group. Standing yaws face as the original does. */
    IntroHuman defs[13] = {
        { &introGuardRT, -4130.0f, 0.0f, 830.0f, 0.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 77 },                /* Ulgrin */
        { &introGuardRT, -3985.0f, 0.0f, 786.0f, 315.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 120 },
        { &introGuardRT, -8064.0f, 0.0f, 1096.0f, 0.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 160 },               /* radio guy */
        { &introGuardRT, 328.0f, 480.0f, 1072.0f, 0.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 40 },                /* balcony */
        { &introFranklinRT, -3424.0f, -100.0f, -2208.0f, 180.0f,
          NULL, 1, "Franklin.png", 357, 381, 0.12f, 366 },
        { &introScientistRT, -3073.0f, -315.0f, -2165.0f, 225.0f,
          NULL, 1, "scientist.png", 182, 182, 0.0f, 182 },
        { &introClassDRT, 208.0f, 0.0f, 480.0f, 270.0f,
          NULL, 1, NULL, 210, 235, 0.1f, 210 },    /* chamber D #1 */
        { &introClassDRT, 160.0f, 0.0f, 320.0f, 270.0f,
          NULL, 1, "class_d(2).png", 210, 235, 0.1f, 220 }, /* D #2 */
        { &introGuardRT, -3800.0f, 250.0f, -4088.0f, 0.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 90 },      /* south balcony */
        { &introGuardRT, -4200.0f, 250.0f, -4088.0f, 0.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 150 },
        { &introClassDRT, -4000.0f, 250.0f, -4088.0f, 0.0f,
          NULL, 1, "D_9341.png", 357, 381, 0.12f, 364 },
        { &introGuardRT, -7208.0f, -600.0f, -3104.0f, 0.0f,
          NULL, 1, NULL, 77, 201, 0.4f, 60 },      /* lower level */
        { &introClassDRT, -5675.0f, -1020.0f, -3717.0f, 0.0f,
          NULL, 1, NULL, 357, 381, 0.12f, 368 },
    };
    for (int i = 0; i < 13; i++) {
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
    static GLuint slotVbo[14];
    introHumanCount = 0;
    introDIdx[0] = introDIdx[1] = -1;
    for (int i = 0; i < 13; i++) {
        if (!defs[i].skin && !defs[i].rt->ok) continue;
        if (defs[i].skin) {
            if (!slotVbo[i]) glGenBuffers(1, &slotVbo[i]);
            defs[i].vbo = slotVbo[i];
            defs[i].posed = 0;
        }
        if (i == 6) introDIdx[0] = introHumanCount;
        if (i == 7) introDIdx[1] = introHumanCount;
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

/* ---- decals: ground/wall splats (Map_Core CreateDecal) ----
 * A decal is a textured, alpha-blended quad laid on a surface (pitch 90
 * lies it flat on the floor). SCP-106 leaves a corrosion trail; the
 * player bleeds droplets; the pocket dimension is strewn with pd
 * decals. Textures live in GFX/Decals. World size: the Blitz quad spans
 * -1..1 (2 units) scaled by Size, so a port half-extent = Size * 256
 * raw units. Held in a fixed ring buffer - the oldest is overwritten. */
enum {
    DECAL_CORROSIVE_1 = 0, DECAL_CORROSIVE_2 = 1,
    DECAL_BLOOD_1 = 2, DECAL_BLOOD_6 = 7,
    DECAL_PD_1 = 8, DECAL_PD_6 = 13,
    DECAL_BULLET_HOLE_1 = 14, DECAL_BULLET_HOLE_2 = 15,
    DECAL_BLOOD_DROP_1 = 16, DECAL_BLOOD_DROP_2 = 17,
    DECAL_427 = 18, DECAL_409 = 19, DECAL_WATER = 20,
    /* KETER/APOLLYON are achievement-gated: slots exist so IDs line up
     * with the source, but their textures are never loaded (decalSpawn
     * skips an id with no texture). */
    DECAL_KETER = 21, DECAL_APOLLYON = 22,
    DECAL_ID_MAX = 23
};

typedef struct {
    int used;
    int id;
    float pos[3];
    float pitch, yaw, roll;   /* Blitz euler degrees */
    float size, sizeChange, maxSize;
    float alpha, alphaChange;
    int blend;                /* 1 alpha, 2 multiply, 3 additive */
    float timer;              /* corrosive: child-spawn cadence */
    float life;               /* -1 = forever, else frames remaining */
} Decal;

#define MAX_DECALS 256
static Decal decals[MAX_DECALS];
static int decalHead;
static GLuint decalTex[DECAL_ID_MAX];
static int decalsLoaded;

/* FillRoom's static splats live apart from the transient ring so the
 * corrosion/blood effects can never recycle the room scatter. */
#define MAX_ROOM_DECALS 256
static Decal roomDecals[MAX_ROOM_DECALS];
static int roomDecalCount;

static void decalsInit(void) {
    if (decalsLoaded) return;
    decalsLoaded = 1;
    char name[64];
    for (int i = 0; i < 2; i++) {
        snprintf(name, sizeof(name), "corrosive_decal(%d).png", i);
        decalTex[DECAL_CORROSIVE_1 + i] = textureGet(name);
    }
    for (int i = 0; i < 6; i++) {
        snprintf(name, sizeof(name), "blood_decal(%d).png", i);
        decalTex[DECAL_BLOOD_1 + i] = textureGet(name);
    }
    for (int i = 0; i < 6; i++) {
        snprintf(name, sizeof(name), "pd_decal(%d).png", i);
        decalTex[DECAL_PD_1 + i] = textureGet(name);
    }
    for (int i = 0; i < 2; i++) {
        snprintf(name, sizeof(name), "bullet_hole_decal(%d).png", i);
        decalTex[DECAL_BULLET_HOLE_1 + i] = textureGet(name);
    }
    for (int i = 0; i < 2; i++) {
        snprintf(name, sizeof(name), "blood_drop_decal(%d).png", i);
        decalTex[DECAL_BLOOD_DROP_1 + i] = textureGet(name);
    }
    decalTex[DECAL_427] = textureGet("scp_427_decal.png");
    decalTex[DECAL_409] = textureGet("scp_409_decal.png");
    decalTex[DECAL_WATER] = textureGet("water_decal.png");
    /* DECAL_KETER / DECAL_APOLLYON: achievement-gated, left unloaded. */
}

/* Full form; call sites tune sizeChange/life afterward. */
static Decal *decalSpawn(int id, float x, float y, float z, float pitch,
                         float yaw, float roll, float size, float alpha,
                         int blend) {
    if (id < 0 || id >= DECAL_ID_MAX || !decalTex[id]) return NULL;
    Decal *d = &decals[decalHead];
    decalHead = (decalHead + 1) % MAX_DECALS;
    d->used = 1;
    d->id = id;
    d->pos[0] = x; d->pos[1] = y; d->pos[2] = z;
    d->pitch = pitch; d->yaw = yaw; d->roll = roll;
    d->size = size; d->sizeChange = 0.0f; d->maxSize = size > 1.0f ? size : 1.0f;
    d->alpha = alpha; d->alphaChange = 0.0f;
    d->blend = blend;
    d->timer = 0.0f;
    d->life = -1.0f;
    return d;
}

/* Lay FillRoom's static splats (room_decals.h) into every placed room,
 * rotated with the room, in the persistent room-decal array. Runs once
 * per map, after the map is generated and decal textures are loaded. */
static void spawnRoomDecals(void) {
    roomDecalCount = 0;
    const int N = (int)(sizeof(ROOM_DECALS) / sizeof(ROOM_DECALS[0]));
    for (uint32_t r = 0;
         r < map.roomCount && roomDecalCount < MAX_ROOM_DECALS; r++) {
        const RoomPlacement *p = &map.rooms[r];
        const char *nm = tplList.items[p->templateIndex].name;
        for (int i = 0; i < N && roomDecalCount < MAX_ROOM_DECALS; i++) {
            const RoomDecalDef *rd = &ROOM_DECALS[i];
            if (strcmp(rd->room, nm) != 0) continue;
            float local[3] = { rd->x, rd->y, rd->z };
            float w[3];
            localToWorld(p, local, w);
            Decal *d = &roomDecals[roomDecalCount++];
            memset(d, 0, sizeof(*d));
            d->used = 1;
            d->id = rd->id;
            d->pos[0] = w[0]; d->pos[1] = w[1]; d->pos[2] = w[2];
            d->pitch = rd->pitch;
            /* Yaw follows the room's quarter-turn placement. */
            d->yaw = rd->yaw + (float)(p->angle * 90);
            d->roll = rd->roll;
            d->size = rd->size;
            d->maxSize = rd->size;
            d->alpha = rd->alpha;
            d->blend = rd->blend;
            d->life = -1.0f;
        }
    }
}

/* A flat corrosion splat on the floor at (x,z), random spin. */
static void decalCorrosion(float x, float y, float z, float size,
                           float alpha) {
    Decal *d = decalSpawn(DECAL_CORROSIVE_1, x, y + 0.5f, z, 90.0f,
                          (float)(rand() % 360), 0.0f, size, alpha, 1);
    if (d) {
        d->sizeChange = -0.00002f * 256.0f; /* very slow shrink */
        d->life = 5400.0f;
    }
}

static void decalsUpdate(void) {
    for (int i = 0; i < MAX_DECALS; i++) {
        Decal *d = &decals[i];
        if (!d->used) continue;
        if (d->sizeChange != 0.0f) {
            d->size += d->sizeChange;
            if (d->sizeChange > 0.0f && d->size >= d->maxSize) {
                d->size = d->maxSize;
                d->sizeChange = 0.0f;
            }
        }
        if (d->alphaChange != 0.0f) {
            d->alpha += d->alphaChange;
            if (d->alpha > 1.0f) d->alpha = 1.0f;
        }
        if (d->life > 0.0f) {
            d->life -= 1.0f;
            /* fade out the last second of life */
            if (d->life < 60.0f) d->alpha = d->life / 60.0f;
        }
        if (d->size <= 0.0f || d->alpha <= 0.0f || d->life == 0.0f) {
            d->used = 0;
        }
    }
}

/* Emit one decal quad (blend/rotation/size); the GL passes are set up by
 * decalsDraw. Distance-culled and skipped if its texture never loaded. */
static void decalEmit(const Decal *d) {
    if (!d->used) return;
    float ddx = d->pos[0] - camPos[0], ddz = d->pos[2] - camPos[2];
    if (ddx * ddx + ddz * ddz > VIEW_RANGE * VIEW_RANGE) return;
    GLuint tex = decalTex[d->id];
    if (!tex) return;
    switch (d->blend) {
        case 2: glBlendFunc(GL_DST_COLOR, GL_ZERO); break;      /* mul */
        case 3: glBlendFunc(GL_SRC_ALPHA, GL_ONE); break;       /* add */
        default: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glColor4f(1.0f, 1.0f, 1.0f, d->alpha);
    float h = d->size * 256.0f;
    GLfloat v[18] = {
        -h,  h, 0.0f,   h,  h, 0.0f,   h, -h, 0.0f,
        -h,  h, 0.0f,   h, -h, 0.0f,  -h, -h, 0.0f,
    };
    glPushMatrix();
    glTranslatef(d->pos[0], d->pos[1], d->pos[2]);
    glRotatef(-d->yaw, 0.0f, 1.0f, 0.0f);
    glRotatef(d->pitch, 1.0f, 0.0f, 0.0f);
    glRotatef(d->roll, 0.0f, 0.0f, 1.0f);
    glVertexPointer(3, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glPopMatrix();
}

static void decalsDraw(void) {
    int any = roomDecalCount > 0;
    for (int i = 0; !any && i < MAX_DECALS; i++) if (decals[i].used) any = 1;
    if (!any) return;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableClientState(GL_COLOR_ARRAY);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.02f);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    static const GLfloat uvs[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    for (int i = 0; i < roomDecalCount; i++) decalEmit(&roomDecals[i]);
    for (int i = 0; i < MAX_DECALS; i++) decalEmit(&decals[i]);
    glColor4f(1, 1, 1, 1);
    glDepthMask(GL_TRUE);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_BLEND);
    glEnableClientState(GL_COLOR_ARRAY);
}

/* ---- SCP-106 ---- */
static void draw106(const float viewPos[3]);
static void draw096(const float viewPos[3]);
static void draw049(const float viewPos[3]);
static void draw939(const float viewPos[3]);
static void draw966(const float viewPos[3]);
static void draw1499(const float viewPos[3]);
static void draw860(const float viewPos[3]);

static void spawn106Near(void) {
    /* Behind the player, out of the cell if possible, on the floor. */
    float fx = sinf(camYaw), fz = -cosf(camYaw);
    float sx = camPos[0] - fx * 700.0f;
    float sz = camPos[2] - fz * 700.0f;
    if (!roomExistsAt(sx, sz)) { sx = camPos[0]; sz = camPos[2]; }
    npc106Pos[0] = sx;
    npc106Pos[2] = sz;
    float o[3] = { sx, camPos[1] + 200.0f, sz };
    float hy;
    npc106Pos[1] = rayDownWorld(o, 3000.0f, &hy) ? hy : camPos[1] - EYE_HEIGHT;
    npc106State = N106_SPAWNING;
    npc106Frame = 111.0f;
    audioPlay(snd106Decay[rand() % 4], 1.0f, 0.0f);
    /* A corrosion pool wells up under it as it materializes (source
     * State 2: CreateDecal CORROSIVE_1, size 0.05, growing to full). */
    Decal *pool = decalSpawn(DECAL_CORROSIVE_1, sx, npc106Pos[1] + 0.5f, sz,
                             90.0f, (float)(rand() % 360), 0.0f, 0.05f, 0.85f,
                             1);
    if (pool) {
        pool->sizeChange = 0.0015f;
        pool->maxSize = 1.0f;
        pool->life = 7200.0f;
    }
}

static void reset106(void) {
    npc106Active = skin106 != NULL && !npc106Contained;
    npc106State = N106_DORMANT;
    /* First appearance a while into the run (source idles ~22000+
     * frames; scaled down so it turns up within a session). */
    npc106Timer = 3600.0f + (float)(rand() % 3000);
    npc106Cool = 0;
    npc106GrabTimer = 0.0f;
    posed106 = 0;
    npc106Pos[1] = -5000.0f;
}

/* ---- pocket dimension escape ---- */
static void enterPocketDimension(void) {
    if (pdRoomIdx < 0) return;
    pdReturn[0] = camPos[0];
    pdReturn[1] = camPos[1];
    pdReturn[2] = camPos[2];
    pdReturn[3] = camYaw;
    float ox = PD_GX * ROOM_SPACING, oz = PD_GY * ROOM_SPACING;
    camPos[0] = ox - 400.0f;
    camPos[2] = oz - 400.0f;
    float o[3] = { camPos[0], 1500.0f, camPos[2] };
    float hy;
    camPos[1] = (rayDownWorld(o, 3000.0f, &hy) ? hy : 0.0f) + EYE_HEIGHT;
    camYaw = 2.35f; /* face into the start room */
    camPitch = 0.0f;
    velY = 0.0f;
    inPocket = 1;
    pocketTimer = 18000.0f; /* ~5 min overall safety collapse */
    pdCircle = 0.0f;
    pdLunging = 0;
    pdState = PD_START;
    pdEventState = 0.0f;
    pdStateTimer = 0.0f;
    pd106Hidden = 0;
    npc106Active = skin106 != NULL;
    npc106State = N106_CHASING; /* so draw106 shows it */
    blinkFrames = 20;
    blinkTimer = 100.0f;
    audioPlay(sndPdEnter, 1.0f, 0.0f);
    audioLoopAmbience(sndPdRumble, 0.6f);
}

static void leavePocketDimension(int escaped) {
    inPocket = 0;
    pdLunging = 0;
    npc106State = N106_DORMANT;
    npc106Pos[1] = -5000.0f;
    audioLoopAmbience(-1, 0.0f);
    if (escaped) {
        audioPlay(sndPdExit, 1.0f, 0.0f);
        camPos[0] = pdReturn[0];
        camPos[1] = pdReturn[1];
        camPos[2] = pdReturn[2];
        camYaw = pdReturn[3];
        velY = 0.0f;
        snprintf(toastMsg, sizeof(toastMsg), "YOU ESCAPED THE POCKET"
                 " DIMENSION");
        toastTimer = 240;
        gameMusicStart();
    } else {
        audioPlay(sndPdExplode, 1.0f, 0.0f);
        snprintf(deathCause, sizeof(deathCause), "THE POCKET DIMENSION");
        deathTimer = 180;
    }
}

/* Origin of each sub-room within the composite (raw world). The base
 * room + tunnels sit at the PD origin; dim_3 (four-way / throne /
 * trenches / exit) at z+8192; dim_4 (tower / labyrinth) at z+16384. */
static void pdRegionOrigin(int state, float *ox, float *oz) {
    float bx = PD_GX * ROOM_SPACING, bz = PD_GY * ROOM_SPACING;
    *ox = bx;
    switch (state) {
        case PD_FOURWAY: case PD_THRONE:
        case PD_TRENCHES: case PD_EXIT:
            *oz = bz + 8192.0f; break;
        case PD_TOWER: case PD_LABYRINTH:
            *oz = bz + 16384.0f; break;
        default:
            *oz = bz; break;
    }
}

/* Drop the player onto the floor at (x,z), blinking (the game blinks
 * on every pocket-dimension teleport). */
static void pdPlacePlayer(float x, float z) {
    camPos[0] = x;
    camPos[2] = z;
    float o[3] = { x, 2500.0f, z };
    float hy;
    camPos[1] = (rayDownWorld(o, 8000.0f, &hy) ? hy : 0.0f) + EYE_HEIGHT;
    velY = 0.0f;
    blinkFrames = 18;
    blinkTimer = 100.0f;
}

/* The teleport table (UpdateDimension106's Select Random): each bucket
 * lands the player in a specific sub-room, or exits the dimension. */
static void pdTeleportRandom(int r) {
    float ox, oz;
    pdLunging = 0;
    pdStateTimer = 0.0f;
    pdEventState = 0.0f;
    audioPlay(snd106Corr[rand() % 3], 0.9f, 0.0f);
    if (r >= 1 && r <= 5) {              /* rotate into the start room */
        pdState = PD_START;
        pdPlacePlayer(PD_GX * ROOM_SPACING + 300.0f,
                      PD_GY * ROOM_SPACING);
    } else if (r >= 6 && r <= 13) {      /* the four-way room */
        pdState = PD_FOURWAY;
        pdRegionOrigin(PD_FOURWAY, &ox, &oz);
        pdPlacePlayer(ox, oz);
    } else if (r >= 14 && r <= 17) {     /* middle of the start room */
        pdState = PD_START;
        pdPlacePlayer(PD_GX * ROOM_SPACING, PD_GY * ROOM_SPACING);
    } else if (r == 18) {                /* the exit room */
        pdState = PD_EXIT;
        pdRegionOrigin(PD_EXIT, &ox, &oz);
        pdPlacePlayer(ox - 400.0f, oz);
    } else if (r >= 19 && r <= 23) {     /* random exit to the facility */
        leavePocketDimension(1);
    } else if (r >= 24 && r <= 26) {     /* the fake HCZ tunnel */
        pdState = PD_FAKETUNNEL;
        pdRegionOrigin(PD_FOURWAY, &ox, &oz);
        pdPlacePlayer(ox, oz + 1000.0f);
    } else if (r >= 27 && r <= 30) {     /* the tower room */
        pdState = PD_TOWER;
        pdEventState = 15.0f;
        pdRegionOrigin(PD_TOWER, &ox, &oz);
        pdPlacePlayer(ox, oz);
    } else if (r == 31) {                /* the labyrinth */
        pdState = PD_LABYRINTH;
        pdRegionOrigin(PD_LABYRINTH, &ox, &oz);
        pdPlacePlayer(ox - 500.0f, oz - 500.0f);
        npc106Pos[0] = ox + 500.0f;
        npc106Pos[2] = oz + 500.0f;
    } else {                             /* 32: exit back to SCP-005 */
        leavePocketDimension(1);
    }
}

/* SCP-106's orbit-and-lunge in the start room (shared by PD_StartRoom
 * and used as its threat). Returns 1 if it caught the player. */
static int pdOrbit106(float cx, float cz) {
    if (!npc106Active || !skin106) return 0;
    if (!pdLunging) {
        pdCircle += 0.6f;
        float a = pdCircle * 3.14159265f / 180.0f;
        npc106Pos[0] = cx + sinf(a) * 950.0f;
        npc106Pos[2] = cz + cosf(a) * 950.0f;
        float o[3] = { npc106Pos[0], camPos[1] + 200.0f, npc106Pos[2] };
        float hy;
        npc106Pos[1] = rayDownWorld(o, 3000.0f, &hy) ? hy
                                                     : camPos[1] - EYE_HEIGHT;
        npc106Frame += 0.7f;
        if (npc106Frame > 333.0f || npc106Frame < 284.0f) {
            npc106Frame = 284.0f;
        }
        /* Store the raw heading; draw106 applies the -yaw+180 render
         * transform (baking it in here double-rotated the model). */
        npc106YawDeg = atan2f(camPos[0] - npc106Pos[0],
                              camPos[2] - npc106Pos[2])
                     * 180.0f / 3.14159265f;
        /* After a long dwell (~70*65 frames) it may pounce. */
        if (pdEventState > 4550.0f && rand() % 800 == 0) {
            pdLunging = 1;
            audioPlay(snd106Laugh, 1.0f, 0.0f);
        }
        return 0;
    }
    float dx = camPos[0] - npc106Pos[0];
    float dz = camPos[2] - npc106Pos[2];
    float d = sqrtf(dx * dx + dz * dz);
    if (d < 150.0f) return 1;
    if (d > 1.0f) {
        npc106Pos[0] += dx / d * 40.0f;
        npc106Pos[2] += dz / d * 40.0f;
        npc106YawDeg = atan2f(dx, dz) * 180.0f / 3.14159265f;
    }
    npc106Frame += 0.7f;
    if (npc106Frame > 333.0f) npc106Frame = 284.0f;
    return 0;
}

/* A slow relentless homing chase (tower / labyrinth), 106 walking
 * toward the player at the given speed. */
static void pdChase106(float speed) {
    if (!npc106Active || !skin106) return;
    float dx = camPos[0] - npc106Pos[0];
    float dz = camPos[2] - npc106Pos[2];
    float d = sqrtf(dx * dx + dz * dz);
    if (d > 1.0f) {
        npc106Pos[0] += dx / d * speed;
        npc106Pos[2] += dz / d * speed;
        npc106YawDeg = atan2f(dx, dz) * 180.0f / 3.14159265f;
    }
    float o[3] = { npc106Pos[0], npc106Pos[1] + 250.0f, npc106Pos[2] };
    float hy;
    if (rayDownWorld(o, 6000.0f, &hy)) npc106Pos[1] = hy;
    float prevFrame = npc106Frame;
    npc106Frame += 0.7f;
    if (npc106Frame > 333.0f) npc106Frame = 284.0f;
    if ((prevFrame <= 286.0f && npc106Frame > 286.0f)
        || (prevFrame <= 311.0f && npc106Frame > 311.0f)) {
        decalCorrosion(npc106Pos[0], npc106Pos[1], npc106Pos[2], 0.12f, 0.8f);
    }
}

static void pdKill(const char *cause) {
    snprintf(deathCause, sizeof(deathCause), "%s", cause);
    audioPlay(sndPdExplode, 1.0f, 0.0f);
    deathTimer = 180;
    inPocket = 0;
    audioLoopAmbience(-1, 0.0f);
}

/* Camera-pull toward a world point (the throne kneel): ease the yaw and
 * pitch to look at it, harder as sanity falls. */
static void pdLookAt(const float target[3], float rate) {
    float dx = target[0] - camPos[0];
    float dy = target[1] - camPos[1];
    float dz = target[2] - camPos[2];
    float wantYaw = -atan2f(dx, dz);
    float horiz = sqrtf(dx * dx + dz * dz);
    float wantPitch = -atan2f(dy, horiz);
    float dyaw = wantYaw - camYaw;
    while (dyaw > 3.14159265f) dyaw -= 6.2831853f;
    while (dyaw < -3.14159265f) dyaw += 6.2831853f;
    camYaw += dyaw * rate;
    camPitch += (wantPitch - camPitch) * rate;
}

static void updatePocketDimension(void) {
    if (!inPocket || deathTimer > 0) return;
    pdStateTimer += 1.0f;
    pocketTimer -= 1.0f;
    if (pocketTimer <= 0.0f) {
        leavePocketDimension(0);
        return;
    }
    float bx = PD_GX * ROOM_SPACING, bz = PD_GY * ROOM_SPACING;
    pdPillarsOn = 0;
    pd106Hidden = !(pdState == PD_START || pdState == PD_TOWER
                    || pdState == PD_LABYRINTH);

    switch (pdState) {
        case PD_START: {
            pdEventState += 1.0f;
            if (pdOrbit106(bx, bz)) { pdKill("SCP-106"); return; }
            /* Walking out past the tunnel ring shuffles the player to a
             * random sub-room (EntityDistanceSquared > 1200*RoomScale). */
            float dx = camPos[0] - bx, dz = camPos[2] - bz;
            if (dx * dx + dz * dz > 1100.0f * 1100.0f) {
                pdTeleportRandom(1 + rand() % 30);
            }
            break;
        }
        case PD_FOURWAY: {
            pdEventState += 1.0f;
            float ox, oz;
            pdRegionOrigin(PD_FOURWAY, &ox, &oz);
            /* Two flying pillars orbit and crush on contact. */
            pdPillarsOn = pdPillarRT.ok;
            if (pdPillarsOn) {
                float t = pdEventState * 0.03f;
                float sv = sinf(t * 1.6f) * 200.0f;
                float cv = cosf(t * 0.8f) * 260.0f;
                pdPillar[0][0] = ox + 700.0f + cv;
                pdPillar[0][1] = 0.0f;
                pdPillar[0][2] = oz + sv;
                pdPillar[1][0] = ox + sv;
                pdPillar[1][1] = 0.0f;
                pdPillar[1][2] = oz + 700.0f + cv;
                for (int i = 0; i < 2; i++) {
                    float px = camPos[0] - pdPillar[i][0];
                    float pz = camPos[2] - pdPillar[i][2];
                    if (px * px + pz * pz < 220.0f * 220.0f) {
                        float b = sqrtf(px * px + pz * pz);
                        if (b > 1.0f) {
                            camPos[0] += px / b * 200.0f;
                            camPos[2] += pz / b * 200.0f;
                        }
                        pdKill("A FLYING PILLAR");
                        return;
                    }
                }
            }
            /* Near the throne of eyes: kneel before it (throne room). */
            float tx = bx, tz = bz + 8192.0f - 2848.0f;
            float ddx = camPos[0] - tx, ddz = camPos[2] - tz;
            if (ddx * ddx + ddz * ddz < 2000.0f * 2000.0f) {
                pdState = PD_THRONE;
                pdEventState = 0.0f;
                pdStateTimer = 0.0f;
                audioPlay(snd106Breath, 0.8f, 0.0f);
                break;
            }
            /* Fell into a pit: to a random room if far from the centre,
             * else killed by the fall (death 106_2). */
            if (camPos[1] < -1600.0f) {
                float fdx = camPos[0] - ox, fdz = camPos[2] - oz;
                if (fdx * fdx + fdz * fdz > 4750.0f * 4750.0f) {
                    pdTeleportRandom(14 + rand() % 17);
                } else {
                    pdKill("THE POCKET DIMENSION");
                    return;
                }
            }
            break;
        }
        case PD_THRONE: {
            pdEventState += 1.0f;
            float tx = bx, ty = 1376.0f, tz = bz + 8192.0f - 2848.0f;
            float target[3] = { tx, ty, tz };
            /* The throne drags the camera to it and drains sanity, the
             * lens pulsing inward the nearer you kneel. */
            pdLookAt(target, 0.06f);
            injuries += 0.00025f;
            sanity -= 0.25f;
            if (sanity < -1000.0f) sanity = -1000.0f;
            float pz = (sinf(gTick * 0.05f) + 1.0f) * 12.0f;
            if (cameraZoom < pz) cameraZoom = pz;
            if (blurAmount < 0.4f) blurAmount = 0.4f;
            float ddx = camPos[0] - tx, ddz = camPos[2] - tz;
            if (ddx * ddx + ddz * ddz >= 2000.0f * 2000.0f) {
                pdState = PD_FOURWAY;
                pdEventState = 0.0f;
                break;
            }
            /* Crouch to kneel: dropped into the trenches. */
            if (inputHit(ACTION_CROUCH)) {
                float ox, oz;
                pdRegionOrigin(PD_TRENCHES, &ox, &oz);
                pdPlacePlayer(ox - 1344.0f, oz - 1184.0f);
                pdState = PD_TRENCHES;
                pdEventState = 0.0f;
                pdStateTimer = 0.0f;
                audioPlay(sndPdExplode, 0.8f, 0.0f);
            }
            break;
        }
        case PD_TRENCHES: {
            /* The trench plane hunts the player across the flats. The
             * plane mesh and its line-of-sight texture swap are not
             * ported; approximated as a mounting dread that injures over
             * time and shakes the view, with the sinkhole exit reached
             * by walking to the low ground (or after a grace period). */
            pdEventState += 1.0f;
            injuries += 0.0003f;
            camShake = 2.0f;
            if (rand() % 200 == 0) {
                audioPlay(snd106Corr[rand() % 3], 0.7f, 0.0f);
            }
            if (camPos[1] < -1600.0f || pdStateTimer > 2400.0f) {
                camShake = 0.0f;
                pdTeleportRandom(18);
            }
            break;
        }
        case PD_EXIT: {
            pdEventState += 1.0f;
            float ox, oz;
            pdRegionOrigin(PD_EXIT, &ox, &oz);
            /* The sinkhole exit: nearing it blurs the view, reaching it
             * spits the player back into the facility (escape). */
            float ex = ox + 1024.0f, ez = oz;
            float dx = camPos[0] - ex, dz = camPos[2] - ez;
            float d2 = dx * dx + dz * dz;
            if (d2 < 640.0f * 640.0f) {
                blinkTimer = 100.0f * (d2 / (640.0f * 640.0f));
                float b = 1.0f - d2 / (640.0f * 640.0f);
                if (blurAmount < b * 0.9f) blurAmount = b * 0.9f;
                if (d2 < 130.0f * 130.0f) {
                    leavePocketDimension(1);
                    return;
                }
            }
            /* Wander off and it collapses into another shuffle. */
            if (pdStateTimer > 3000.0f) pdTeleportRandom(19 + rand() % 5);
            break;
        }
        case PD_FAKETUNNEL: {
            /* A convincing fake HCZ corridor; opening a door or standing
             * too long drops the floor away, back to the four-way room. */
            pdEventState += 1.0f;
            if (pdStateTimer == 1.0f) audioPlay(sndHorror11, 0.8f, 0.0f);
            if (camPos[1] < -1600.0f || pdStateTimer > 1200.0f) {
                pdTeleportRandom(6 + rand() % 8);
            }
            break;
        }
        case PD_TOWER: {
            /* SCP-106 idols ring the tower and wake one by one; when the
             * countdown runs out the real 106 attacks. Reaching the low
             * exit shuffles the player onward. */
            pdEventState -= 0.02f;
            if (pdEventState > 12.0f) {
                /* Idols dormant: 106 stands watch nearby. */
                if (rand() % 750 == 0) {
                    blinkFrames = 18;
                    blinkTimer = 100.0f;
                    audioPlay(snd106Laugh, 0.8f, 0.0f);
                    pdEventState -= 1.0f;
                }
            } else {
                pdChase106(46.0f);
                float dx = camPos[0] - npc106Pos[0];
                float dz = camPos[2] - npc106Pos[2];
                if (dx * dx + dz * dz < 150.0f * 150.0f) {
                    pdKill("SCP-106");
                    return;
                }
            }
            if (camPos[1] < -1600.0f || pdStateTimer > 2700.0f) {
                pdTeleportRandom(12 + rand() % 17);
            }
            break;
        }
        case PD_LABYRINTH: {
            /* The rockmoss maze: 106 hunts at triple speed. Break far
             * enough from the centre and the dimension spits you out. */
            pdEventState += 1.0f;
            injuries += 0.0001f;
            pdChase106(52.0f * 3.0f * 0.4f);
            float dx = camPos[0] - npc106Pos[0];
            float dz = camPos[2] - npc106Pos[2];
            if (dx * dx + dz * dz < 150.0f * 150.0f) {
                pdKill("SCP-106");
                return;
            }
            float ox, oz;
            pdRegionOrigin(PD_LABYRINTH, &ox, &oz);
            float cdx = camPos[0] - ox, cdz = camPos[2] - oz;
            if (cdx * cdx + cdz * cdz > 3678.0f * 3678.0f) {
                pdTeleportRandom(32);
            }
            break;
        }
        default:
            break;
    }
    /* Fall clean out of the world: collapse. */
    if (inPocket && camPos[1] < -6000.0f) leavePocketDimension(0);
}

static void update106(void) {
    if (!npc106Active || deathTimer > 0 || !walkMode || introPhase >= 0
        || inPocket || femurTimer > 0.0f) {
        return;
    }
    float dx = camPos[0] - npc106Pos[0];
    float dz = camPos[2] - npc106Pos[2];
    float dist = sqrtf(dx * dx + dz * dz);
    float feetDy = fabsf((camPos[1] - EYE_HEIGHT) - npc106Pos[1]);
    int sameLevel = feetDy < 260.0f;

    switch (npc106State) {
        case N106_DORMANT: {
            /* SCP-268 hides you; time still passes but no spawn. On the
             * aggressive difficulties (Keter/Apollyon) the spawn timer
             * counts down twice as fast (source: fps\Factor *
             * (1 + AggressiveNPCs)). The room's DisableDecals slows or
             * halts it: 0 full, 1 half, 2 quarter (floored so it never
             * spawns while you stay put), 3 no countdown at all. */
            float countDown = (wear268 ? 0.3f : 1.0f) * (1.0f + npcAggressive);
            int dd = roomDisableDecalsAt(camPos);
            if (dd < 3) {
                if (dd == 1) countDown *= 0.5f;
                else if (dd == 2) countDown *= 0.25f;
                npc106Timer -= countDown;
                if (dd == 2 && npc106Timer < 600.0f) npc106Timer = 600.0f;
                if (npc106Timer <= 0.0f) spawn106Near();
            }
            break;
        }
        case N106_SPAWNING:
            npc106Frame += 2.2f; /* rise anim 111..259 */
            npc106YawDeg = atan2f(dx, dz) * 180.0f / 3.14159265f;
            if (npc106Frame >= 259.0f) {
                npc106State = N106_CHASING;
                npc106Frame = 284.0f;
                npc106ChaseTimer = 3000.0f + (float)(rand() % 500);
                audioPlay(snd106Laugh, 0.9f, 0.0f);
            }
            break;
        case N106_CHASING: {
            /* Walk cycle 284..333. */
            float prevFrame = npc106Frame;
            npc106Frame += 0.7f;
            if (npc106Frame > 333.0f) npc106Frame = 284.0f;
            /* Corrosive footprints on each footfall (source frames
             * 286 / 311), leaving 106's trademark trail. */
            if (dist < 2500.0f
                && ((prevFrame <= 286.0f && npc106Frame > 286.0f)
                    || (prevFrame <= 311.0f && npc106Frame > 311.0f))) {
                decalCorrosion(npc106Pos[0], npc106Pos[1], npc106Pos[2],
                               0.1f, 0.8f);
            }
            if (dist < 150.0f && sameLevel) {
                /* Caught: it seizes the player and wrenches (frames
                 * 105..110), then drags them under (source's grab +
                 * FallTimer, MoveToPocketDimension at FallTimer<-250). */
                npc106State = N106_GRABBING;
                npc106Frame = 105.0f;
                npc106GrabTimer = 0.0f;
                audioPlay(snd106Laugh, 1.0f, 0.0f);
                return;
            }
            float ndx, ndz;
            navDir(camPos[0], camPos[2], &ndx, &ndz);
            if (ndx == 0.0f && ndz == 0.0f && dist > 1.0f) {
                ndx = dx / dist;
                ndz = dz / dist;
            }
            /* 106 is slow but relentless; it phases (no wall block). */
            npc106Pos[0] += ndx * 52.0f;
            npc106Pos[2] += ndz * 52.0f;
            npc106YawDeg = atan2f(ndx, ndz) * 180.0f / 3.14159265f;
            float o[3] = { npc106Pos[0], npc106Pos[1] + 250.0f, npc106Pos[2] };
            float hy;
            if (rayDownWorld(o, 600.0f, &hy)) npc106Pos[1] = hy;

            /* Dread-zoom: close and facing the player, the lens pulses
             * inward (source me\CurrCameraZoom, Sin(MilliSec/20)). */
            if (dist < 2100.0f) {
                float vx = sinf(camYaw), vz = -cosf(camYaw);
                float inFront = dist > 1.0f
                              ? (-dx * vx - dz * vz) / dist : 1.0f;
                if (inFront > 0.25f) {
                    float prox = (2100.0f - dist) / 2100.0f;
                    float pulse = (sinf(gTick * 0.06f) + 1.0f) * 9.0f
                                * prox;
                    if (cameraZoom < pulse) cameraZoom = pulse;
                    /* ...and the view blurs as it fills the frame
                     * (source BlurVolume, clamped 0.1..0.9). */
                    float b = 0.1f + prox * 0.8f;
                    if (blurAmount < b) blurAmount = b;
                }
            }

            /* Corrode the doors it passes (source: within 144 of the
             * player, a closed door frame within ~0.5 of 106; office /
             * wooden / big / 914 are immune). */
            if (dist < 3072.0f) {
                for (uint32_t di = 0; di < doors.count; di++) {
                    Door *dr = &doors.items[di];
                    if (dr->corroded || dr->openState > 1.0f) continue;
                    if (dr->type == 3 || dr->type == 4 || dr->type == 5
                        || dr->type == 7) {
                        continue;
                    }
                    float ex = dr->x - npc106Pos[0];
                    float ez = dr->z - npc106Pos[2];
                    if (ex * ex + ez * ez < 256.0f * 256.0f) {
                        dr->corroded = 1;
                        /* Beyond the texture swap, 106's rot pools at the
                         * door's foot and eats into the panel face - the
                         * door-surface corrosion. */
                        decalCorrosion(dr->x, dr->y, dr->z, 0.7f, 0.7f);
                        Decal *fd = decalSpawn(DECAL_CORROSIVE_2,
                                               dr->x, dr->y + 150.0f, dr->z,
                                               0.0f, (float)dr->angle, 0.0f,
                                               0.6f, 0.75f, 1);
                        if (fd) fd->life = 3600.0f;
                        audioPlay3D(snd106Corr[rand() % 3], npc106Pos,
                                    camPos, camYaw, 1500.0f);
                        break;
                    }
                }
            }

            /* Occasional corrosion/breathing cues. */
            if (rand() % 180 == 0) {
                audioPlay3D(snd106Corr[rand() % 3], npc106Pos, camPos,
                            camYaw, 2200.0f);
            }
            if (dist < 500.0f && rand() % 120 == 0) {
                audioPlay(snd106Breath, 0.6f, 0.0f);
            }

            /* Phase-behind: when far and unseen, jump to a cell nearer
             * the player (the signature "it's suddenly right there"). */
            if (npc106Cool > 0) npc106Cool--;
            if (npc106Cool == 0 && dist > ROOM_SPACING * 1.8f) {
                float wp[2];
                if (navNextCell(npc106Pos[0], npc106Pos[2], camPos[0],
                                camPos[2], wp)) {
                    npc106Pos[0] = wp[0];
                    npc106Pos[2] = wp[1];
                    npc106Cool = 120;
                    audioPlay3D(snd106Wall[rand() % 3], npc106Pos, camPos,
                                camYaw, 2500.0f);
                }
            }
            /* Sink away (State3): the chase timer only runs down while
             * 106 is unseen; when it empties and 106 is off-screen and
             * not right on top of the player, it slips back into the
             * walls to re-idle - the source's "it's suddenly gone". */
            {
                float vx = sinf(camYaw), vz = -cosf(camYaw);
                float inFront = dist > 1.0f
                              ? (-dx * vx - dz * vz) / dist : 1.0f;
                float head[3] = { npc106Pos[0], npc106Pos[1] + 200.0f,
                                  npc106Pos[2] };
                int visible = inFront > 0.25f && blinkFrames == 0
                            && lineOfSight(camPos, head);
                if (!visible) npc106ChaseTimer -= 1.0f;
                if (npc106ChaseTimer <= 0.0f && !visible && dist > 500.0f) {
                    reset106();
                    break;
                }
            }
            /* Give up if the player breaks away for good. */
            if (dist > ROOM_SPACING * 4.0f) reset106();
            break;
        }
        case N106_GRABBING: {
            /* It holds the player fast; movement/look are frozen (the
             * input gate checks N106_GRABBING). Frames 105..110 are the
             * grab; at 110 it wrenches - a violent head twist (source
             * shows me\Head rotated +-45 with DamageSFX/HorrorSFX) - and
             * then drags them down (FallTimer) into the pocket
             * dimension. */
            npc106GrabTimer += 1.0f;
            npc106YawDeg = atan2f(dx, dz) * 180.0f / 3.14159265f;
            /* Cling to the player's position. */
            float toX = camPos[0] - dx * 0.15f;
            float toZ = camPos[2] - dz * 0.15f;
            npc106Pos[0] = toX;
            npc106Pos[2] = toZ;
            float o[3] = { npc106Pos[0], npc106Pos[1] + 250.0f, npc106Pos[2] };
            float hy;
            if (rayDownWorld(o, 600.0f, &hy)) npc106Pos[1] = hy;
            if (npc106Frame < 110.0f) {
                npc106Frame += 0.7f; /* grab */
                if (npc106Frame >= 110.0f) {
                    npc106Frame = 110.0f;
                    audioPlay(sndHorror11, 1.0f, 0.0f);
                    audioPlay(snd106Laugh, 0.9f, 0.0f);
                    blinkFrames = 8;
                    blinkTimer = 100.0f;
                }
            } else {
                /* The wrench: twist the view harder over the drag. */
                float g = npc106GrabTimer;
                camYaw += sinf(g * 0.8f) * 0.06f;
                camPitch += cosf(g * 1.1f) * 0.04f;
                if (camPitch > 1.5f) camPitch = 1.5f;
                if (camPitch < -1.5f) camPitch = -1.5f;
                camShake = 3.0f;
                injuries += 0.01f;
            }
            /* FallTimer < -250: dragged under. */
            if (npc106GrabTimer > 190.0f) {
                camShake = 0.0f;
                if (pdRoomIdx >= 0) {
                    enterPocketDimension();
                } else {
                    snprintf(deathCause, sizeof(deathCause), "SCP-106");
                    deathTimer = 180;
                }
                reset106();
                return;
            }
            break;
        }
        default:
            break;
    }
}

/* ---- SCP-096 ---- */

static void reset096(void) {
    /* Calm it back to sitting where it stands (a respawn / new run). */
    npc096State = S096_IDLE;
    npc096Frame = 936.0f;
    npc096ScreamTimer = 0.0f;
}

static void spawn096(void) {
    npc096Active = 0;
    if (!skin096) return;
    /* Its containment / HCZ rooms (e_096_spawn). Fall back to any large
     * room3/room4 if the zone split hasn't placed an HCZ variant. */
    static const char *SPAWN_ROOMS[] = {
        "room2_3_hcz", "room2_4_hcz", "room2_5_hcz", "room2_hcz",
        "room3_hcz", "room3_2_hcz", "room3_3_hcz", "room4_hcz",
        "room4_2_hcz",
    };
    int best = -1;
    for (uint32_t r = 0; r < map.roomCount && best < 0; r++) {
        const char *nm = tplList.items[map.rooms[r].templateIndex].name;
        for (unsigned s = 0; s < sizeof(SPAWN_ROOMS) / sizeof(SPAWN_ROOMS[0]);
             s++) {
            if (strcmp(nm, SPAWN_ROOMS[s]) == 0) { best = (int)r; break; }
        }
    }
    if (best < 0) {
        for (uint32_t r = 0; r < map.roomCount && best < 0; r++) {
            const char *nm = tplList.items[map.rooms[r].templateIndex].name;
            if (strncmp(nm, "room3", 5) == 0 || strncmp(nm, "room4", 5) == 0) {
                best = (int)r;
            }
        }
    }
    if (best < 0) return;
    npc096Pos[0] = map.rooms[best].gridX * ROOM_SPACING;
    npc096Pos[2] = map.rooms[best].gridY * ROOM_SPACING;
    npc096Pos[1] = 0.0f;
    npc096State = S096_IDLE;
    npc096Frame = 936.0f;
    npc096YawDeg = 0.0f;
    npc096ScreamTimer = 0.0f;
    npc096Active = 1;
}

/* UpdateNPCType096: sits harmless until the player looks at its face,
 * then gets up, screams, and sprints down the player, killing on
 * contact. Looking away after the trigger does not stop it. */
static void update096(void) {
    if (!npc096Active || !skin096 || deathTimer > 0 || !walkMode
        || introPhase >= 0 || inPocket || inMask) {
        return;
    }
    float dx = camPos[0] - npc096Pos[0];
    float dz = camPos[2] - npc096Pos[2];
    float dist = sqrtf(dx * dx + dz * dz);

    /* IsLooking: 096's face on screen (in the view cone), within sight
     * range, with a clear line to it, and the player not mid-blink
     * (source EntityVisible(Camera, n\OBJ2) + EntityInView). */
    int looking = 0;
    if (dist < 4200.0f && blinkFrames == 0) {
        float vx = sinf(camYaw), vz = -cosf(camYaw);
        float facing = dist > 1.0f ? (-dx * vx - dz * vz) / dist : 1.0f;
        float head[3] = { npc096Pos[0], npc096Pos[1] + 300.0f, npc096Pos[2] };
        if (facing > 0.55f && lineOfSight(camPos, head)) looking = 1;
    }

    switch (npc096State) {
        case S096_IDLE:
            /* Sit and cover its face (936..1263), harmless. */
            npc096Frame += 0.1f;
            if (npc096Frame > 1263.0f) npc096Frame = 936.0f;
            if (looking && dist < 3400.0f) {
                npc096State = S096_TRIGGERED;
                npc096Frame = 193.0f;
                npc096ScreamTimer = 0.0f;
                npc096YawDeg = atan2f(dx, dz) * 180.0f / 3.14159265f;
                audioPlay(snd096Trigger, 1.0f, 0.0f);
                cameraZoom = 10.0f;
            }
            break;
        case S096_TRIGGERED:
            /* Get up (193..311), then scream in place (1471..1556) for a
             * while before it charges. Source screams ~26 s; compressed
             * to ~8 s so the port stays playable. */
            npc096ScreamTimer += 1.0f;
            npc096YawDeg = atan2f(dx, dz) * 180.0f / 3.14159265f;
            if (npc096Frame < 311.0f) {
                npc096Frame += 0.5f;
            } else {
                npc096Frame += 0.4f;
                if (npc096Frame > 1556.0f) npc096Frame = 1471.0f;
                if ((int)npc096ScreamTimer == 40) {
                    audioPlay(snd096Scream, 1.0f, 0.0f);
                }
            }
            if (npc096ScreamTimer > 480.0f) {
                npc096State = S096_CHASE;
                npc096Frame = 737.0f;
                audioPlay(snd096Scream, 0.9f, 0.0f);
            }
            break;
        case S096_CHASE: {
            /* Run cycle 737..935; sprint toward the player. */
            npc096Frame += 0.8f;
            if (npc096Frame > 935.0f) npc096Frame = 737.0f;
            if (dist < 190.0f) {
                /* Caught: a savage kill (source: shake 30, blur, blood
                 * decals, death "096"). */
                camShake = 5.0f;
                blurAmount = 0.9f;
                float feet = camPos[1] - EYE_HEIGHT;
                for (int i = 0; i < 8; i++) {
                    float a = (float)(rand() % 628) / 100.0f;
                    float rr = 40.0f + (float)(rand() % 160);
                    decalSpawn(DECAL_BLOOD_DROP_1 + rand() % 2,
                               camPos[0] + cosf(a) * rr, feet + 0.5f,
                               camPos[2] + sinf(a) * rr, 90.0f,
                               (float)(rand() % 360), 0.0f,
                               0.1f + (float)(rand() % 25) / 100.0f, 0.9f, 1);
                }
                audioPlay(snd096Scream, 1.0f, 0.0f);
                snprintf(deathCause, sizeof(deathCause), "SCP-096");
                deathTimer = 180;
                return;
            }
            float ndx, ndz, wp[2];
            if (navNextCell(npc096Pos[0], npc096Pos[2], camPos[0], camPos[2],
                            wp)) {
                ndx = wp[0] - npc096Pos[0];
                ndz = wp[1] - npc096Pos[2];
            } else {
                ndx = dx;
                ndz = dz;
            }
            float d = sqrtf(ndx * ndx + ndz * ndz);
            if (d > 1.0f) { ndx /= d; ndz /= d; }
            npc096Pos[0] += ndx * 78.0f;   /* Speed 6.0 -> a fast sprint */
            npc096Pos[2] += ndz * 78.0f;
            npc096YawDeg = atan2f(ndx, ndz) * 180.0f / 3.14159265f;
            float o[3] = { npc096Pos[0], npc096Pos[1] + 250.0f, npc096Pos[2] };
            float hy;
            if (rayDownWorld(o, 600.0f, &hy)) npc096Pos[1] = hy;
            /* It smashes shut doors out of its way (source force-opens +
             * locks the door in its path with a slam + big shake). */
            for (uint32_t di = 0; di < doors.count; di++) {
                Door *dr = &doors.items[di];
                if (dr->open || dr->openState > 1.0f) continue;
                float ex = dr->x - npc096Pos[0], ez = dr->z - npc096Pos[2];
                if (ex * ex + ez * ez < 300.0f * 300.0f) {
                    dr->open = 1;
                    if (dist < 2048.0f) camShake = 3.0f;
                    audioPlay3D(sndDoorOpen[rand() % 3], npc096Pos, camPos,
                                camYaw, 2500.0f);
                    break;
                }
            }
            /* The dread-zoom pulses while it bears down (me\CurrCameraZoom,
             * Sin(MilliSec/20)*10). */
            if (dist < 3000.0f) {
                float pz = (sinf(gTick * 0.05f) + 1.0f) * 10.0f;
                if (cameraZoom < pz) cameraZoom = pz;
            }
            break;
        }
        default:
            break;
    }
}

/* ---- SCP-049 + SCP-049-2 ---- */

static void reset049(void) {
    npc049State = S049_IDLE;
    npc049Frame = 269.0f;
    npc049Timer = 1800.0f + (float)(rand() % 2400);
    npc049Cool = 0;
}

static void spawn049(void) {
    npc049Active = 0;
    npc0492Count = 0;
    for (int z = 0; z < MAX_0492; z++) npc0492Active[z] = 0;
    if (!skin049) return;
    /* Its containment (cont2_049) if generated, else any large room far
     * from the player's start cell. */
    int best = -1;
    for (uint32_t r = 0; r < map.roomCount; r++) {
        if (strcmp(tplList.items[map.rooms[r].templateIndex].name,
                   "cont2_049") == 0) { best = (int)r; break; }
    }
    if (best < 0) {
        for (uint32_t r = 0; r < map.roomCount && best < 0; r++) {
            const char *nm = tplList.items[map.rooms[r].templateIndex].name;
            int gx = map.rooms[r].gridX, gy = map.rooms[r].gridY;
            if ((strncmp(nm, "room2", 5) == 0 || strncmp(nm, "room3", 5) == 0)
                && (gx * gx + gy * gy) > 9) {
                best = (int)r;
            }
        }
    }
    if (best < 0) return;
    npc049Pos[0] = map.rooms[best].gridX * ROOM_SPACING;
    npc049Pos[2] = map.rooms[best].gridY * ROOM_SPACING;
    npc049Pos[1] = 0.0f;
    npc049YawDeg = 0.0f;
    reset049();
    npc049Active = 1;
    /* A small retinue of reanimated 049-2 shambling nearby. */
    if (skin0492) {
        npc0492Count = 2;
        npc0492Frame = 705.0f;
        for (int z = 0; z < npc0492Count; z++) {
            npc0492Pos[z][0] = npc049Pos[0] + (z == 0 ? 300.0f : -300.0f);
            npc0492Pos[z][1] = 0.0f;
            npc0492Pos[z][2] = npc049Pos[2] + 200.0f;
            npc0492Yaw[z] = 0.0f;
            npc0492Active[z] = 1;
        }
    }
}

/* A slow relentless walk toward the player, cell-pathed; returns the
 * step direction (0,0 if already on top of the player). */
static void doctorStep(const float from[3], float speed, float out[3]) {
    float dx = camPos[0] - from[0], dz = camPos[2] - from[2];
    float wp[2], ndx = dx, ndz = dz;
    if (navNextCell(from[0], from[2], camPos[0], camPos[2], wp)) {
        ndx = wp[0] - from[0];
        ndz = wp[1] - from[2];
    }
    float d = sqrtf(ndx * ndx + ndz * ndz);
    if (d > 1.0f) { ndx /= d; ndz /= d; }
    out[0] = ndx * speed;
    out[2] = ndz * speed;
    out[1] = 0.0f;
}

/* UpdateNPCType049: idles until active, then walks the player down and
 * "cures" on contact; a hazmat suit or SCP-714 delays that (and can be
 * torn off). Phases closer when far, like 173/106. */
static void update049(void) {
    if (!npc049Active || !skin049 || deathTimer > 0 || !walkMode
        || introPhase >= 0 || inPocket || inMask) {
        return;
    }
    float dx = camPos[0] - npc049Pos[0];
    float dz = camPos[2] - npc049Pos[2];
    float dist = sqrtf(dx * dx + dz * dz);

    /* Away from it, the protective timers recover (source: both climb
     * back to 500 while Dist >= 0.25). */
    if (dist > 250.0f) {
        if (rmHazmatTimer < 500.0f) rmHazmatTimer += 1.0f;
        if (rm714Timer < 500.0f) rm714Timer += 1.0f;
    }

    switch (npc049State) {
        case S049_IDLE:
            npc049Frame += 0.2f;
            if (npc049Frame > 345.0f) npc049Frame = 269.0f;
            npc049Timer -= 1.0f + (float)npcAggressive;
            if (npc049Timer <= 0.0f) {
                npc049State = S049_PURSUE;
                npc049Frame = 346.0f;
                audioPlay3D(snd049Spot[rand() % 3], npc049Pos, camPos,
                            camYaw, 3500.0f);
            }
            break;
        case S049_PURSUE: {
            npc049Frame += 0.6f;
            if (npc049Frame > 463.0f) npc049Frame = 346.0f;
            if (dist < 200.0f) {
                /* It seizes the player. A hazmat suit or SCP-714 buys a
                 * few seconds - and can be escaped - before the "cure";
                 * once torn off (timer hits 0) the next grip is fatal
                 * (source RemoveHazmatTimer / Remove714Timer). */
                if (wearHazmat && rmHazmatTimer > 0.0f) {
                    rmHazmatTimer -= 1.5f;
                    camShake = 1.5f;
                    if (rmHazmatTimer <= 0.0f) {
                        wearHazmat = 0;
                        removeInventoryByName("Hazmat Suit");
                        snprintf(toastMsg, sizeof(toastMsg),
                                 "SCP-049 TEARS YOUR HAZMAT SUIT APART");
                        toastTimer = 200;
                    }
                    npc049Frame = 537.0f; /* leaning in */
                    return;
                }
                if (using714 && rm714Timer > 0.0f) {
                    rm714Timer -= 2.0f;
                    blurAmount = 0.7f;
                    if (rm714Timer <= 0.0f) {
                        using714 = 0;
                        removeInventoryByName("SCP-714");
                        snprintf(toastMsg, sizeof(toastMsg),
                                 "THE JADE RING IS WRENCHED FROM YOUR HAND");
                        toastTimer = 200;
                    }
                    npc049Frame = 537.0f;
                    return;
                }
                /* Unprotected (or protection exhausted): the cure. */
                audioPlay(snd049Horror, 1.0f, 0.0f);
                snprintf(deathCause, sizeof(deathCause), "SCP-049");
                deathTimer = 180;
                npc049State = S049_KILL;
                npc049Frame = 537.0f;
                return;
            }
            float step[3];
            doctorStep(npc049Pos, 32.0f, step);
            npc049Pos[0] += step[0];
            npc049Pos[2] += step[2];
            if (step[0] != 0.0f || step[2] != 0.0f) {
                npc049YawDeg = atan2f(step[0], step[2]) * 180.0f / 3.14159265f;
            }
            float o[3] = { npc049Pos[0], npc049Pos[1] + 250.0f, npc049Pos[2] };
            float hy;
            if (rayDownWorld(o, 600.0f, &hy)) npc049Pos[1] = hy;
            if (rand() % 260 == 0) {
                audioPlay3D(snd049Search[rand() % 3], npc049Pos, camPos,
                            camYaw, 3000.0f);
            }
            if (rand() % 200 == 0) {
                audioPlay3D(snd049Breath, npc049Pos, camPos, camYaw, 2000.0f);
            }
            /* Phase closer if the player breaks away (TeleportCloser). */
            if (npc049Cool > 0) npc049Cool--;
            if (npc049Cool == 0 && dist > ROOM_SPACING * 2.5f) {
                float wp[2];
                if (navNextCell(npc049Pos[0], npc049Pos[2], camPos[0],
                                camPos[2], wp)) {
                    npc049Pos[0] = wp[0];
                    npc049Pos[2] = wp[1];
                    npc049Cool = 180;
                }
            }
            break;
        }
        case S049_KILL:
            npc049Frame += 0.6f;
            if (npc049Frame > 660.0f) npc049Frame = 660.0f;
            break;
        default:
            break;
    }
}

/* The 049-2 retinue: slow shamblers that also kill on a touch. They
 * share one animation phase (skinned once) and each walk the player
 * down independently. */
static void update0492(void) {
    if (!npc0492Count || !skin0492 || deathTimer > 0 || !walkMode
        || introPhase >= 0 || inPocket || inMask) {
        return;
    }
    npc0492Frame += 0.5f;
    if (npc0492Frame > 794.0f) npc0492Frame = 705.0f;
    if (npc0492Cool > 0.0f) npc0492Cool -= 1.0f;
    /* Only shamble once 049 itself is on the hunt. */
    if (npc049State == S049_IDLE) return;
    for (int z = 0; z < npc0492Count; z++) {
        if (!npc0492Active[z]) continue;
        float dx = camPos[0] - npc0492Pos[z][0];
        float dz = camPos[2] - npc0492Pos[z][2];
        if (dx * dx + dz * dz < 200.0f * 200.0f) {
            /* They maul rather than instakill (source injures on each
             * bite; enough of them bleed you out). */
            if (npc0492Cool <= 0.0f) {
                injuries += 1.6f;
                damageFlash = 0.6f;
                camShake = 1.5f;
                audioPlay(snd049Horror, 0.9f, 0.0f);
                npc0492Cool = 45.0f;
            }
            continue;
        }
        float step[3];
        doctorStep(npc0492Pos[z], 24.0f, step);
        npc0492Pos[z][0] += step[0];
        npc0492Pos[z][2] += step[2];
        if (step[0] != 0.0f || step[2] != 0.0f) {
            npc0492Yaw[z] = atan2f(step[0], step[2]) * 180.0f / 3.14159265f;
        }
        float o[3] = { npc0492Pos[z][0], npc0492Pos[z][1] + 250.0f,
                       npc0492Pos[z][2] };
        float hy;
        if (rayDownWorld(o, 600.0f, &hy)) npc0492Pos[z][1] = hy;
    }
}

/* ---- SCP-939 ---- */

static void reset939(void) {
    npc939State = S939_PATROL;
    npc939Frame = 290.0f;
    npc939Cool = 0.0f;
}

static void spawn939(void) {
    npc939Active = 0;
    if (!skin939) return;
    int best = -1;
    for (uint32_t r = 0; r < map.roomCount; r++) {
        if (strcmp(tplList.items[map.rooms[r].templateIndex].name,
                   "room3_storage") == 0) { best = (int)r; break; }
    }
    if (best < 0) return;
    npc939Pos[0] = map.rooms[best].gridX * ROOM_SPACING + 300.0f;
    npc939Pos[2] = map.rooms[best].gridY * ROOM_SPACING + 300.0f;
    npc939Pos[1] = 0.0f;
    npc939Home[0] = (float)map.rooms[best].gridX * ROOM_SPACING;
    npc939Home[1] = (float)map.rooms[best].gridY * ROOM_SPACING;
    npc939Wander[0] = npc939Pos[0];
    npc939Wander[1] = npc939Pos[2];
    npc939YawDeg = 0.0f;
    reset939();
    npc939Active = 1;
}

/* UpdateNPCType939: blind, it locates the player by the noise they make
 * (playerNoise). Loud + near -> it charges and bites; a still, crouched
 * player barely registers. Enough bites kill. */
static void update939(void) {
    if (!npc939Active || !skin939 || deathTimer > 0 || !walkMode
        || introPhase >= 0 || inPocket || inMask) {
        return;
    }
    /* Only prowls its storage room. */
    if (strcmp(roomNameAt(camPos), "room3_storage") != 0) {
        npc939State = S939_PATROL;
        return;
    }
    float dx = camPos[0] - npc939Pos[0];
    float dz = camPos[2] - npc939Pos[2];
    float dist = sqrtf(dx * dx + dz * dz);
    if (npc939Cool > 0.0f) npc939Cool -= 1.0f;
    /* Keep it on the floor in every state (it used to only settle while
     * attacking, so it floated when idle/alert). */
    float go[3] = { npc939Pos[0], npc939Pos[1] + 250.0f, npc939Pos[2] };
    float ghy;
    if (rayDownWorld(go, 600.0f, &ghy)) npc939Pos[1] = ghy;

    /* Heard-range grows with the player's noise (SndVolume). */
    float heard = 400.0f + playerNoise * 3200.0f;
    int knows = dist < heard || dist < 500.0f;

    switch (npc939State) {
        case S939_PATROL:
            npc939Frame += 0.15f;
            if (npc939Frame > 405.0f) npc939Frame = 290.0f;
            /* Patrol: drift around the room toward a wander point, picking
             * a fresh one on arrival or now and then (source waypoints). */
            {
                float wdx = npc939Wander[0] - npc939Pos[0];
                float wdz = npc939Wander[1] - npc939Pos[2];
                float wd = sqrtf(wdx * wdx + wdz * wdz);
                if (wd < 140.0f || (rand() % 600) == 0) {
                    npc939Wander[0] = npc939Home[0]
                                    + (float)((rand() % 1600) - 800);
                    npc939Wander[1] = npc939Home[1]
                                    + (float)((rand() % 1600) - 800);
                } else if (wd > 1.0f) {
                    npc939Pos[0] += wdx / wd * 4.0f;
                    npc939Pos[2] += wdz / wd * 4.0f;
                    npc939YawDeg = atan2f(wdx, wdz) * 180.0f / 3.14159265f;
                }
            }
            /* It mimics human voices to draw the curious closer. */
            if (dist < 3500.0f && rand() % 900 == 0) {
                audioPlay3D(snd939Lure[rand() % 3], npc939Pos, camPos,
                            camYaw, 3500.0f);
            }
            if (knows) {
                npc939State = S939_ALERT;
                npc939Frame = 175.0f;
                audioPlay3D(snd939Attack[rand() % 3], npc939Pos, camPos,
                            camYaw, 3000.0f);
            }
            break;
        case S939_ALERT:
            /* Orients and stalks toward the sound; escalates if still
             * heard, relaxes if the player goes silent. */
            npc939Frame += 0.4f;
            if (npc939Frame > 297.0f) npc939Frame = 175.0f;
            npc939YawDeg = atan2f(dx, dz) * 180.0f / 3.14159265f;
            if (dist > 1.0f) {
                npc939Pos[0] += dx / dist * 22.0f;
                npc939Pos[2] += dz / dist * 22.0f;
            }
            if (dist < 900.0f && playerNoise > 0.2f) {
                npc939State = S939_ATTACK;
                npc939Frame = 449.0f;
                audioPlay(snd939Horror, 1.0f, 0.0f);
            } else if (!knows) {
                npc939State = S939_PATROL;
            }
            break;
        case S939_ATTACK: {
            /* Charge and bite (source lunge 449..464, bite 18..68). */
            npc939Frame += 0.7f;
            if (npc939Frame > 464.0f) npc939Frame = 449.0f;
            npc939YawDeg = atan2f(dx, dz) * 180.0f / 3.14159265f;
            float bhead[3] = { npc939Pos[0], npc939Pos[1] + 150.0f,
                               npc939Pos[2] };
            if (dist > 220.0f || !lineOfSight(camPos, bhead)) {
                /* Close on the sound (blind, so it presses on even
                 * through walls) but only bite once actually in reach. */
                if (dist > 1.0f) {
                    npc939Pos[0] += dx / dist * 60.0f;
                    npc939Pos[2] += dz / dist * 60.0f;
                }
            } else if (npc939Cool <= 0.0f) {
                /* A bite: injures; enough of them kill (Injuries > 4). */
                injuries += 2.0f;
                damageFlash = 0.7f;
                camShake = 2.0f;
                audioPlay(snd939Attack[rand() % 3], 1.0f, 0.0f);
                npc939Cool = 45.0f;
                if (injuries > 4.0f) {
                    snprintf(deathCause, sizeof(deathCause), "SCP-939");
                    deathTimer = 180;
                    return;
                }
            }
            float o[3] = { npc939Pos[0], npc939Pos[1] + 250.0f, npc939Pos[2] };
            float hy;
            if (rayDownWorld(o, 600.0f, &hy)) npc939Pos[1] = hy;
            /* Lose interest if the player goes silent and backs off. */
            if (dist > heard && dist > 1200.0f && playerNoise < 0.15f) {
                npc939State = S939_ALERT;
            }
            break;
        }
        default:
            break;
    }
}

/* ---- SCP-966 ---- */

static void spawn966(void) {
    npc966Active = 0;
    npc966Drowsy = 0.0f;
    npc966Frame = 2.0f;
    if (!skin966) return;
    static const char *ROOMS[] = {
        "room2_3_hcz", "room2_4_hcz", "room2_hcz", "room3_hcz",
        "room3_2_hcz", "room4_hcz",
    };
    int best = -1;
    for (uint32_t r = 0; r < map.roomCount && best < 0; r++) {
        const char *nm = tplList.items[map.rooms[r].templateIndex].name;
        for (unsigned s = 0; s < sizeof(ROOMS) / sizeof(ROOMS[0]); s++) {
            if (strcmp(nm, ROOMS[s]) == 0) { best = (int)r; break; }
        }
    }
    if (best < 0) {
        for (uint32_t r = 0; r < map.roomCount && best < 0; r++) {
            const char *nm = tplList.items[map.rooms[r].templateIndex].name;
            if (strncmp(nm, "room2", 5) == 0
                && (map.rooms[r].gridX * map.rooms[r].gridX
                    + map.rooms[r].gridY * map.rooms[r].gridY) > 16) {
                best = (int)r;
            }
        }
    }
    if (best < 0) return;
    npc966Pos[0] = map.rooms[best].gridX * ROOM_SPACING;
    npc966Pos[2] = map.rooms[best].gridY * ROOM_SPACING;
    npc966Pos[1] = 0.0f;
    npc966YawDeg = 0.0f;
    npc966Active = 1;
}

/* UpdateNPCType966: unseen without night vision, it creeps toward the
 * player and steals their rest (stamina/sanity). Exhausted and caught,
 * it kills. The goggles are the counter - watched, it holds back. */
static void update966(void) {
    if (!npc966Active || !skin966 || deathTimer > 0 || !walkMode
        || introPhase >= 0 || inPocket || inMask) {
        return;
    }
    float dx = camPos[0] - npc966Pos[0];
    float dz = camPos[2] - npc966Pos[2];
    float dist = sqrtf(dx * dx + dz * dz);
    npc966Frame += 0.2f;
    if (npc966Frame > 214.0f) npc966Frame = 2.0f;

    /* Faithful to the source: 966 does NOT tire the player - a *tired*
     * player rouses IT. Aggression builds while the player is exhausted
     * and unwatched, and eases when rested or watched through the
     * goggles (source: me\Stamina < 10 raises n\State3 toward attack). */
    int seen = wearNVG != 0;
    /* A sealed head - gas mask or hazmat - or SCP-714 dulls the sleep
     * pull (source I_966\HasInsomnia is halved by 714 and blocked by a
     * mask/hazmat), so it rouses far slower. */
    int shielded = wearGasMask || wearHazmat || using714;
    if (!seen && stamina < 15.0f) {
        npc966Drowsy += shielded ? 0.35f : 1.0f;
    } else if (npc966Drowsy > 0.0f) {
        npc966Drowsy -= shielded ? 0.5f : 0.2f;
    }
    int aggro = npc966Drowsy > 300.0f;

    /* Watched, it backs away; unseen it creeps closer - and rushes once
     * roused. Moves every frame (it used to freeze at the kill boundary
     * and hover there). */
    float speed = seen ? -8.0f : (aggro ? 26.0f : 10.0f);
    if (dist > 1.0f) {
        npc966Pos[0] += dx / dist * speed;
        npc966Pos[2] += dz / dist * speed;
        npc966YawDeg = atan2f(dx, dz) * 180.0f / 3.14159265f;
    }
    float o[3] = { npc966Pos[0], npc966Pos[1] + 250.0f, npc966Pos[2] };
    float hy;
    if (rayDownWorld(o, 600.0f, &hy)) npc966Pos[1] = hy;

    /* Near and unwatched, it steals your rest - draining sanity and
     * blurring the view - but it never touches your stamina. */
    if (dist < 900.0f && !seen) {
        sanity -= shielded ? 0.04f : 0.12f;
        if (sanity < 0.0f) sanity = 0.0f;
        if (blurAmount < (shielded ? 0.15f : 0.3f)) {
            blurAmount = shielded ? 0.15f : 0.3f;
        }
        if (aggro && (int)npc966Drowsy % 360 == 0) {
            snprintf(toastMsg, sizeof(toastMsg),
                     "YOUR EYELIDS ARE SO HEAVY...");
            toastTimer = 150;
        }
    }
    /* Caught, unwatched, while exhausted or fully roused: it takes you
     * in your sleep. */
    if (dist < 200.0f && !seen && (aggro || stamina < 5.0f)) {
        snprintf(deathCause, sizeof(deathCause), "SCP-966");
        deathTimer = 180;
    }
}

/* ---- SCP-1499 mask dimension ---- */

/* Drop a congregation member on the floor at (x,z), recording its post. */
static void mask1499Place(int k, float x, float z, int type, float yaw) {
    npc1499Pos[k][0] = x;
    npc1499Pos[k][2] = z;
    float o[3] = { x, 3000.0f, z };
    float hy;
    npc1499Pos[k][1] = rayDownWorld(o, 6000.0f, &hy) ? hy : 0.0f;
    npc1499Home[k][0] = npc1499Pos[k][0];
    npc1499Home[k][1] = npc1499Pos[k][1];
    npc1499Home[k][2] = npc1499Pos[k][2];
    npc1499Type[k] = type;
    npc1499Yaw[k] = yaw;
    npc1499Aggro[k] = 0;
    npc1499Chat[k] = -1;
    npc1499Frame[k] = type == 2 ? 509.0f : 296.0f;
    npc1499Active[k] = 1;
}

static void enterMaskDimension(void) {
    if (maskRoomIdx < 0 || !skin1499) return;
    maskReturn[0] = camPos[0];
    maskReturn[1] = camPos[1];
    maskReturn[2] = camPos[2];
    maskReturn[3] = camYaw;
    float ox = MASK_GX * ROOM_SPACING, oz = MASK_GY * ROOM_SPACING;
    /* Arrive in the nave, facing the altar (-x). */
    camPos[0] = ox + 2200.0f;
    camPos[2] = oz + 2380.0f;
    float o[3] = { camPos[0], 2600.0f, camPos[2] };
    float hy;
    camPos[1] = (rayDownWorld(o, 5000.0f, &hy) ? hy : 0.0f) + EYE_HEIGHT;
    camYaw = -1.5708f;
    camPitch = 0.0f;
    velY = 0.0f;
    inMask = 1;
    blinkFrames = 20;
    blinkTimer = 100.0f;
    npc1499Count = 0;
    /* The congregation, at the source's church coordinates: the king on
     * the altar, a guard beside him, two guards at the entrance. */
    mask1499Place(npc1499Count++, ox - 1917.0f, oz + 2308.0f, 2, 90.0f);
    mask1499Place(npc1499Count++, ox - 1917.0f, oz + 2052.0f, 1, 90.0f);
    mask1499Place(npc1499Count++, ox + 4055.0f, oz + 1884.0f, 3, -90.0f);
    mask1499Place(npc1499Count++, ox + 4055.0f, oz + 2876.0f, 3, -90.0f);
    /* Wandering citizens throng the nave around the player. */
    for (int c = 0; c < 4 && npc1499Count < MAX_1499; c++) {
        float a = (float)c * 1.5708f + 0.4f;
        mask1499Place(npc1499Count++, camPos[0] + cosf(a) * 750.0f,
                      camPos[2] + sinf(a) * 750.0f, 0, 0.0f);
    }
    /* Re-entry ambush: the dimension tires of visitors - every third
     * donning, the citizens ring the arrival point already hostile
     * (source: EventState2 hits Rand(2,3)/4 and spawns a State-2 ring). */
    maskEntries++;
    if (maskEntries % 3 == 0) {
        for (int k = 0; k < npc1499Count; k++) {
            if (npc1499Type[k] == 0) {
                npc1499Aggro[k] = 1;
                npc1499Frame[k] = (k & 1) ? 100.0f : 1.0f;
            }
        }
        audioPlay(snd1499Trig, 1.0f, 0.0f);
    }
    audioPlay(snd1499Enter, 1.0f, 0.0f);
}

static void leaveMaskDimension(void) {
    inMask = 0;
    npc1499Count = 0;
    for (int k = 0; k < MAX_1499; k++) npc1499Active[k] = 0;
    camPos[0] = maskReturn[0];
    camPos[1] = maskReturn[1];
    camPos[2] = maskReturn[2];
    camYaw = maskReturn[3];
    velY = 0.0f;
    blinkFrames = 16;
    blinkTimer = 100.0f;
    audioPlay(snd1499Exit, 1.0f, 0.0f);
}

/* Rouse a member: it screams and, if a citizen, its cry flips the
 * nearby citizens too (source: a screaming citizen sets every other
 * PrevState-0 member to State 1). */
static void mask1499Rouse(int k) {
    if (npc1499Aggro[k]) return;
    npc1499Aggro[k] = 1;
    npc1499Frame[k] = (k & 1) ? 100.0f : 1.0f;
    audioPlay3D(snd1499Trig, npc1499Pos[k], camPos, camYaw, 4000.0f);
    if (npc1499Type[k] == 0) {
        for (int j = 0; j < npc1499Count; j++) {
            if (j == k || !npc1499Active[j] || npc1499Type[j] != 0) continue;
            float ex = npc1499Pos[j][0] - npc1499Pos[k][0];
            float ez = npc1499Pos[j][2] - npc1499Pos[k][2];
            if (ex * ex + ez * ez < 1600.0f * 1600.0f && !npc1499Aggro[j]) {
                npc1499Aggro[j] = 1;
                npc1499Frame[j] = (j & 1) ? 100.0f : 1.0f;
            }
        }
    }
}

/* The congregation starts peaceful - the king enthroned, guards on post,
 * citizens milling and murmuring - and each member turns on its own when
 * the player strays too near (a citizen's scream cascades to its
 * neighbours). Roused members converge and kill on contact; the king
 * only watches from the altar. Sanity bleeds; the mask off is the escape. */
static void update1499(void) {
    if (!inMask || deathTimer > 0) return;
    int anyAggro = 0;
    for (int k = 0; k < npc1499Count; k++) {
        if (!npc1499Active[k]) continue;
        float dx = camPos[0] - npc1499Pos[k][0];
        float dz = camPos[2] - npc1499Pos[k][2];
        float d = sqrtf(dx * dx + dz * dz);
        int type = npc1499Type[k];

        /* Rouse when the player strays within this member's tolerance
         * AND it can actually see them (source EntityVisible). The king
         * only stirs to a direct approach and never leaves the altar;
         * guards hold a tight line; citizens spook widest. */
        if (!npc1499Aggro[k]) {
            float thr = type == 0 ? 650.0f : type == 2 ? 380.0f : 480.0f;
            float head[3] = { npc1499Pos[k][0], npc1499Pos[k][1] + 200.0f,
                              npc1499Pos[k][2] };
            if (d < thr && lineOfSight(camPos, head)) mask1499Rouse(k);
        }

        if (npc1499Aggro[k]) {
            anyAggro = 1;
            npc1499Yaw[k] = d > 1.0f
                          ? atan2f(dx, dz) * 180.0f / 3.14159265f
                          : npc1499Yaw[k];
            /* Run cycle (source alternates 1..62 / 100..167 by parity). */
            npc1499Frame[k] += 0.7f;
            if ((k & 1) == 0) {
                if (npc1499Frame[k] > 62.0f) npc1499Frame[k] = 1.0f;
            } else {
                if (npc1499Frame[k] > 167.0f) npc1499Frame[k] = 100.0f;
            }
            if (type == 2) {
                /* The king stays enthroned; he only glares. */
            } else {
                if (d < 200.0f) {
                    snprintf(deathCause, sizeof(deathCause),
                             "THE PEOPLE OF SCP-1499");
                    deathTimer = 180;
                    return;
                }
                float sp = (type == 1 || type == 3) ? 30.0f : 22.0f;
                if (d > 1.0f) {
                    npc1499Pos[k][0] += dx / d * sp;
                    npc1499Pos[k][2] += dz / d * sp;
                }
            }
        } else if (type == 2) {
            /* The king: seated idle (509..601), watching. */
            npc1499Frame[k] += 0.15f;
            if (npc1499Frame[k] > 601.0f) npc1499Frame[k] = 509.0f;
            if (d > 1.0f) npc1499Yaw[k] = atan2f(dx, dz) * 180.0f / 3.14159265f;
        } else if (type == 0) {
            /* Citizens shuffle-idle and murmur; now and then a pair drifts
             * together to converse, else each settles near its home post. */
            npc1499Frame[k] += 0.25f;
            if (npc1499Frame[k] > 320.0f) npc1499Frame[k] = 296.0f;
            if (npc1499Chat[k] < 0 || (rand() % 700) == 0) {
                npc1499Chat[k] = npc1499Count > 1
                               ? (int)((unsigned)rand() % npc1499Count) : -1;
            }
            float tx = npc1499Home[k][0], tz = npc1499Home[k][2];
            int c = npc1499Chat[k];
            if (c >= 0 && c != k && npc1499Type[c] == 0 && !npc1499Aggro[c]) {
                tx = (npc1499Home[k][0] + npc1499Pos[c][0]) * 0.5f;
                tz = (npc1499Home[k][2] + npc1499Pos[c][2]) * 0.5f;
                float cdx = npc1499Pos[c][0] - npc1499Pos[k][0];
                float cdz = npc1499Pos[c][2] - npc1499Pos[k][2];
                if (cdx * cdx + cdz * cdz < 320.0f * 320.0f) {
                    npc1499Yaw[k] = atan2f(cdx, cdz) * 180.0f / 3.14159265f;
                    if (rand() % 1200 == 0) {
                        audioPlay3D(snd1499Idle[rand() % 4], npc1499Pos[k],
                                    camPos, camYaw, 2500.0f);
                    }
                }
            }
            npc1499Pos[k][0] += sinf(gTick * 0.01f + (float)k) * 0.5f;
            npc1499Pos[k][2] += cosf(gTick * 0.013f + (float)k) * 0.5f;
            npc1499Pos[k][0] += (tx - npc1499Pos[k][0]) * 0.008f;
            npc1499Pos[k][2] += (tz - npc1499Pos[k][2]) * 0.008f;
            if (rand() % 1500 == 0) {
                audioPlay3D(snd1499Idle[rand() % 4], npc1499Pos[k], camPos,
                            camYaw, 3000.0f);
            }
        } else {
            /* Guards on post: idle, tracking the intruder. */
            npc1499Frame[k] += 0.2f;
            if (npc1499Frame[k] > 320.0f) npc1499Frame[k] = 296.0f;
            if (d > 1.0f) npc1499Yaw[k] = atan2f(dx, dz) * 180.0f / 3.14159265f;
        }
        /* Settle onto the floor (ray from high so the altar is found). */
        float o[3] = { npc1499Pos[k][0], 3000.0f, npc1499Pos[k][2] };
        float hy;
        if (rayDownWorld(o, 6000.0f, &hy)) npc1499Pos[k][1] = hy;
    }
    sanity -= anyAggro ? 0.15f : 0.05f;
    if (sanity < 0.0f) sanity = 0.0f;
}

/* ---- SCP-860-1 (the forest room cont2_860_1) ---- */

static void reset860(void) {
    npc860Frame = 2.0f;
    npc860Cool = 0;
}

static void spawn860(void) {
    npc860Active = 0;
    tree860Count = 0;
    if (!skin860) return;
    int best = -1;
    for (uint32_t r = 0; r < map.roomCount; r++) {
        if (strcmp(tplList.items[map.rooms[r].templateIndex].name,
                   "cont2_860_1") == 0) { best = (int)r; break; }
    }
    if (best < 0) return;
    float ox = map.rooms[best].gridX * ROOM_SPACING;
    float oz = map.rooms[best].gridY * ROOM_SPACING;
    npc860YawDeg = 0.0f;
    reset860();
    npc860Active = 1;
    /* Carve a winding path down a 10x10 forest grid (source ForestGrid:
     * the path starts at a top column and walks down, deviating sideways
     * within a couple of columns of centre, then a tree stands on every
     * off-path cell). Deterministic from the seed so the maze is stable. */
    enum { FGRID = 10 };
    unsigned char pathGrid[FGRID][FGRID];
    memset(pathGrid, 0, sizeof(pathGrid));
    uint32_t rng = mapSeed ^ 0x8601860Bu;
    int cen = FGRID / 2;
    int px = cen, py = 0;
    pathGrid[py][px] = 1;
    int guard = 0;
    while (py < FGRID - 1 && guard++ < 400) {
        rng = rng * 1664525u + 1013904223u;
        if (((rng >> 8) % 100u) < 35u) {          /* deviate sideways */
            rng = rng * 1664525u + 1013904223u;
            int nx = px + (((rng >> 8) & 1u) ? 1 : -1);
            int off = nx - cen; if (off < 0) off = -off;
            if (nx >= 1 && nx <= FGRID - 2 && off <= 2) {
                px = nx;
                pathGrid[py][px] = 1;
                continue;
            }
        }
        py++;                                      /* else step down */
        pathGrid[py][px] = 1;
    }
    /* SCP-860-1 lurks at the far (top) end of the path. */
    npc860Pos[0] = ox - 850.0f + ((float)px + 0.5f) * (1700.0f / FGRID);
    npc860Pos[2] = oz - 850.0f + 0.5f * (1700.0f / FGRID);
    npc860Pos[1] = 0.0f;
    /* Stand a tree on every off-path cell. */
    float cell = 1700.0f / FGRID;
    for (int j = 0; j < FGRID && tree860Count < MAX_TREES; j++) {
        for (int i = 0; i < FGRID && tree860Count < MAX_TREES; i++) {
            if (pathGrid[j][i]) continue;
            rng = rng * 1664525u + 1013904223u;
            float jx = (float)((int)((rng >> 8) % 40u) - 20);
            rng = rng * 1664525u + 1013904223u;
            float jz = (float)((int)((rng >> 8) % 40u) - 20);
            tree860Pos[tree860Count][0] = ox - 850.0f + (i + 0.5f) * cell + jx;
            tree860Pos[tree860Count][1] = 0.0f;
            tree860Pos[tree860Count][2] = oz - 850.0f + (j + 0.5f) * cell + jz;
            tree860Count++;
        }
    }
}

/* Push the player out of any nearby SCP-860 tree trunk so the forest
 * grid actually constrains movement to the carved path. */
static void collide860Trees(float pos[3]) {
    if (!npc860Active) return;
    const float R = 68.0f; /* below half the ~170 cell so the path stays open */
    for (int t = 0; t < tree860Count; t++) {
        float dx = pos[0] - tree860Pos[t][0];
        float dz = pos[2] - tree860Pos[t][2];
        float d2 = dx * dx + dz * dz;
        if (d2 >= R * R || d2 < 0.01f) continue;
        float d = sqrtf(d2);
        float push = (R - d) / d;
        pos[0] += dx * push;
        pos[2] += dz * push;
    }
}

/* UpdateNPCType860_2: it advances only while the player is not looking
 * at it (or is mid-blink) and freezes the instant it is watched - the
 * forest stalker. Contact in the dark is fatal. */
static void update860(void) {
    if (!npc860Active || !skin860 || deathTimer > 0 || !walkMode
        || introPhase >= 0 || inPocket || inMask) {
        return;
    }
    if (strcmp(roomNameAt(camPos), "cont2_860_1") != 0) return;
    float dx = camPos[0] - npc860Pos[0];
    float dz = camPos[2] - npc860Pos[2];
    float dist = sqrtf(dx * dx + dz * dz);
    npc860Frame += 0.3f;
    if (npc860Frame > 40.0f) npc860Frame = 2.0f;
    if (dist > 1.0f) {
        npc860YawDeg = atan2f(dx, dz) * 180.0f / 3.14159265f;
    }
    /* Watched? (in the view cone, in range, eyes open). Like the source,
     * it never moves or harms the player while it is being looked at -
     * only in the dark, once it has actually reached you, does it kill. */
    int seen = 0;
    if (blinkFrames == 0 && dist < 4500.0f) {
        float vx = sinf(camYaw), vz = -cosf(camYaw);
        float facing = dist > 1.0f ? (-dx * vx - dz * vz) / dist : 1.0f;
        if (facing > 0.4f) seen = 1;
        /* A tree between the player and it hides it (the source's
         * peek-from-behind-the-trees): project each trunk onto the
         * sightline and block if it straddles the segment closely. */
        if (seen && dist > 1.0f) {
            float lx = -dx / dist, lz = -dz / dist; /* player -> 860 */
            for (int t = 0; t < tree860Count; t++) {
                float tx = tree860Pos[t][0] - camPos[0];
                float tz = tree860Pos[t][2] - camPos[2];
                float proj = tx * lx + tz * lz;
                if (proj < 100.0f || proj > dist - 100.0f) continue;
                float perp2 = (tx * tx + tz * tz) - proj * proj;
                if (perp2 < 150.0f * 150.0f) { seen = 0; break; }
            }
        }
    }
    if (npc860Cool > 0) npc860Cool--;
    if (seen) return; /* frozen while watched - safe */
    if (dist < 180.0f) {
        snprintf(deathCause, sizeof(deathCause), "SCP-860-1");
        audioPlay(sndHorror11, 1.0f, 0.0f);
        deathTimer = 180;
        return;
    }
    /* Unwatched: it closes fast, and now and then lurches much nearer -
     * always landing behind the player, out of view. */
    if (dist > 1.0f) {
        npc860Pos[0] += dx / dist * 44.0f;
        npc860Pos[2] += dz / dist * 44.0f;
    }
    if (npc860Cool == 0 && dist > 1400.0f) {
        /* Recompute from the post-step position so the lurch lands the
         * intended distance behind the player. */
        float bx = camPos[0] - npc860Pos[0], bz = camPos[2] - npc860Pos[2];
        float bd = sqrtf(bx * bx + bz * bz);
        if (bd > 1.0f) {
            npc860Pos[0] = camPos[0] - bx / bd * 900.0f;
            npc860Pos[2] = camPos[2] - bz / bd * 900.0f;
        }
        npc860Cool = 120;
        audioPlay3D(sndStep[rand() % 4], npc860Pos, camPos, camYaw, 3000.0f);
    }
    float o[3] = { npc860Pos[0], npc860Pos[1] + 250.0f, npc860Pos[2] };
    float hy;
    if (rayDownWorld(o, 600.0f, &hy)) npc860Pos[1] = hy;
}

/* ---- elevators ---- */

/* Ride the elevator whose button was just pressed: pick the next
 * elevator door in the map as the destination and start the sequence. */
static void elevatorStart(const Door *d) {
    if (elevState != ELEV_IDLE || d->type != 1) return;
    int di = (int)(d - doors.items);
    const char *here = roomNameAt(camPos);
    elevRayFromY = 1500.0f;
    /* A vertical car (cont1_079's descent to SCP-079's chamber): its
     * paired elevator door sits at the same X/Z but a very different Y.
     * Ride to that level, dropping the arrival floor ray from just above
     * the destination door so it lands in the lower chamber. */
    int vpair = -1;
    for (uint32_t j = 0; j < doors.count; j++) {
        if ((int)j == di || doors.items[j].type != 1) continue;
        float vx = doors.items[j].x - d->x, vz = doors.items[j].z - d->z;
        if (vx * vx + vz * vz < 64.0f * 64.0f
            && fabsf(doors.items[j].y - d->y) > 2000.0f) {
            vpair = (int)j;
            break;
        }
    }
    /* The gate elevators travel to the real surfaces (and back); every
     * other car fast-travels to the next elevator in the map. */
    if (vpair >= 0) {
        elevDest[0] = doors.items[vpair].x;
        elevDest[2] = doors.items[vpair].z;
        elevRayFromY = doors.items[vpair].y + 400.0f;
        elevDoorB = vpair;
    } else if (strcmp(here, "gate_a_entrance") == 0 && gateARoomIdx >= 0) {
        elevDest[0] = GATEA_GX * ROOM_SPACING;
        elevDest[2] = GATEA_GY * ROOM_SPACING;
        elevDoorB = gateADoor;
    } else if (strcmp(here, "gate_b_entrance") == 0 && gateBRoomIdx >= 0) {
        elevDest[0] = GATEB_GX * ROOM_SPACING;
        elevDest[2] = GATEB_GY * ROOM_SPACING;
        elevDoorB = gateBDoor;
    } else if (strcmp(here, "gate_a") == 0 && gateAEntrOK) {
        elevDest[0] = gateAEntr[0];
        elevDest[2] = gateAEntr[2];
        elevDoorB = di;
    } else if (strcmp(here, "gate_b") == 0 && gateBEntrOK) {
        elevDest[0] = gateBEntr[0];
        elevDest[2] = gateBEntr[2];
        elevDoorB = di;
    } else if (strcmp(here, "room2_mt") == 0 && mtInstCount) {
        elevDest[0] = mtArrive[0];
        elevDest[2] = mtArrive[2];
        elevDoorB = mtElevDoorA;
    } else if (inMtBounds(camPos[0], camPos[2]) && mtEntrOK) {
        elevDest[0] = mtEntr[0];
        elevDest[2] = mtEntr[2];
        elevDoorB = di;
    } else {
        int dest = -1;
        for (uint32_t off = 1; off <= doors.count; off++) {
            int j = (int)(((uint32_t)di + off) % doors.count);
            if (j != di && doors.items[j].type == 1) { dest = j; break; }
        }
        const Door *dd = (dest >= 0) ? &doors.items[dest] : d;
        int gx = (int)floorf(dd->x / ROOM_SPACING + 0.5f);
        int gy = (int)floorf(dd->z / ROOM_SPACING + 0.5f);
        elevDest[0] = gx * ROOM_SPACING;
        elevDest[2] = gy * ROOM_SPACING;
        elevDoorB = (dest >= 0) ? dest : di;
    }
    elevDest[1] = camPos[1];
    elevDoorA = di;
    doors.items[di].open = 0; /* shut for the ride */
    elevState = ELEV_CLOSE;
    elevTimer = 0.0f;
    float dp[3] = { d->x, camPos[1], d->z };
    audioPlay3D(sndElevClose[rand() % 3], dp, camPos, camYaw, 2500.0f);
}

static void updateElevator(void) {
    if (elevState == ELEV_IDLE) return;
    elevTimer += 1.0f;
    /* Black out once the doors have shut, through the ride. */
    if ((elevState == ELEV_CLOSE && elevTimer > 30.0f)
        || elevState == ELEV_TRAVEL) {
        blinkFrames = 30;
    }
    switch (elevState) {
        case ELEV_CLOSE:
            if (elevTimer > 90.0f) {
                elevState = ELEV_TRAVEL;
                elevTimer = 0.0f;
                audioPlay(sndElevMove, 0.8f, 0.0f);
            }
            break;
        case ELEV_TRAVEL:
            if (elevTimer > 150.0f) {
                camPos[0] = elevDest[0];
                camPos[2] = elevDest[2];
                float o[3] = { camPos[0], elevRayFromY, camPos[2] };
                float hy;
                camPos[1] = (rayDownWorld(o, 3000.0f, &hy) ? hy : 0.0f)
                          + EYE_HEIGHT;
                velY = 0.0f;
                if (elevDoorB >= 0 && elevDoorB < (int)doors.count) {
                    doors.items[elevDoorB].open = 1;
                }
                audioPlay(sndElevOpen[rand() % 3], 0.9f, 0.0f);
                elevState = ELEV_ARRIVE;
                elevTimer = 0.0f;
            }
            break;
        case ELEV_ARRIVE:
            if (elevTimer > 30.0f) elevState = ELEV_IDLE; /* view fades in */
            break;
        default:
            break;
    }
}

/* ---- Tesla gates ---- */

static void teslaSpawn(void) {
    teslaCount = 0;
    teslaFlash = 0.0f;
    for (uint32_t r = 0; r < map.roomCount && teslaCount < MAX_TESLA; r++) {
        const char *nm = tplList.items[map.rooms[r].templateIndex].name;
        if (strncmp(nm, "room2_tesla", 11) == 0) {
            TeslaGate *g = &teslaGates[teslaCount++];
            g->x = map.rooms[r].gridX * ROOM_SPACING;
            g->z = map.rooms[r].gridY * ROOM_SPACING;
            g->state = 0;
            g->timer = 0.0f;
        }
    }
}

/* The gate cycle (Events_Core e_tesla EventState 0..3): idle until the
 * player steps into the plane, wind up (~35 frames), zap (fatal in the
 * inner box, repels 106), then recharge (~70 frames). Kill/activation
 * boxes are the source's |dx|,|dz| < 1.0 / 0.75 blitz units (x256). */
static void teslaUpdate(void) {
    if (deathTimer > 0) return;
    if (teslaFlash > 0.0f) teslaFlash -= 0.04f;
    for (int i = 0; i < teslaCount; i++) {
        TeslaGate *g = &teslaGates[i];
        float gp[3] = { g->x, EYE_HEIGHT, g->z };
        float pdx = fabsf(camPos[0] - g->x);
        float pdz = fabsf(camPos[2] - g->z);
        float pdy = fabsf(camPos[1] - EYE_HEIGHT);
        int nearGate = pdx < 2400.0f && pdz < 2400.0f;
        switch (g->state) {
            case 0: /* idle: arm when the player enters the plane */
                if (pdx < 256.0f && pdz < 256.0f && pdy < 333.0f) {
                    g->state = 1;
                    g->timer = 0.0f;
                    if (nearGate) {
                        audioPlay3D(sndTeslaWind, gp, camPos, camYaw, 4000.0f);
                    }
                }
                break;
            case 1: /* charge */
                g->timer += 1.0f;
                if (g->timer >= 35.0f) {
                    g->state = 2;
                    if (nearGate) {
                        audioPlay3D(sndTeslaShock, gp, camPos, camYaw,
                                    4000.0f);
                    }
                }
                break;
            case 2: /* zap */
                if (pdx < 192.0f && pdz < 192.0f && pdy < 333.0f) {
                    teslaFlash = 0.85f;
                    camShake = 1.5f;
                    snprintf(deathCause, sizeof(deathCause), "A TESLA GATE");
                    deathTimer = 180;
                    return;
                }
                if (nearGate && teslaFlash < 0.35f) teslaFlash = 0.35f;
                /* Repel SCP-106 (its State 4 retreat): sent dormant. */
                if (npc106Active && npc106State != N106_DORMANT
                    && fabsf(npc106Pos[0] - g->x) < 220.0f
                    && fabsf(npc106Pos[2] - g->z) < 220.0f) {
                    audioPlay(sndTeslaShock, 0.8f, 0.0f);
                    reset106();
                    snprintf(toastMsg, sizeof(toastMsg),
                             "SCP-106 WAS REPELLED BY THE TESLA GATE");
                    toastTimer = 220;
                }
                g->timer -= 1.5f;
                if (g->timer <= 0.0f) {
                    g->state = 3;
                    g->timer = -70.0f;
                    if (nearGate) {
                        audioPlay3D(sndTeslaPower, gp, camPos, camYaw,
                                    4000.0f);
                    }
                }
                break;
            case 3: /* recharge */
                g->timer += 1.0f;
                if (g->timer >= 0.0f) g->state = 0;
                break;
        }
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

/* A camera-facing (Y-up / cylindrical) textured sprite in the world,
 * alpha-blended - the port's billboard primitive, e.g. the pocket
 * dimension's throne of glowing eyes (CreateSprite). Called inside
 * the 3D pass with the modelview already set to the camera. */
static void drawBillboard(const float pos[3], float w, float h, GLuint tex,
                          float alpha) {
    if (!tex) return;
    /* Right vector from the camera yaw; up is world +Y. */
    float rx = cosf(camYaw), rz = sinf(camYaw);
    float hw = w * 0.5f;
    GLfloat verts[18] = {
        pos[0] - rx * hw, pos[1],     pos[2] - rz * hw,
        pos[0] + rx * hw, pos[1],     pos[2] + rz * hw,
        pos[0] + rx * hw, pos[1] + h, pos[2] + rz * hw,
        pos[0] - rx * hw, pos[1],     pos[2] - rz * hw,
        pos[0] + rx * hw, pos[1] + h, pos[2] + rz * hw,
        pos[0] - rx * hw, pos[1] + h, pos[2] - rz * hw,
    };
    GLfloat uvs[12] = { 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0 };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableClientState(GL_COLOR_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, alpha);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glVertexPointer(3, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glColor4f(1, 1, 1, 1);
    glEnableClientState(GL_COLOR_ARRAY);
}

/* The arc of a Tesla gate, drawn additively across the gate while it
 * zaps (charge flickers faintly too). Approximates the game's overlay
 * sprite + red light. */
static void drawTeslaArcs(const float viewPos[3]) {
    if (!teslaArcTex) return;
    for (int i = 0; i < teslaCount; i++) {
        const TeslaGate *g = &teslaGates[i];
        if (g->state != 1 && g->state != 2) continue;
        float dx = g->x - viewPos[0], dz = g->z - viewPos[2];
        if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;
        float a = g->state == 2
                ? 0.6f + 0.4f * sinf(gTick * 1.7f)
                : 0.15f * (0.5f + 0.5f * sinf(gTick * 3.0f));
        if (a < 0.02f) continue;
        /* Additive so the arc glows; a tall quad across the corridor. */
        float rx = cosf(camYaw), rz = sinf(camYaw);
        float hw = 360.0f;
        float base = 40.0f, h = 620.0f;
        GLfloat verts[18] = {
            g->x - rx * hw, base,     g->z - rz * hw,
            g->x + rx * hw, base,     g->z + rz * hw,
            g->x + rx * hw, base + h, g->z + rz * hw,
            g->x - rx * hw, base,     g->z - rz * hw,
            g->x + rx * hw, base + h, g->z + rz * hw,
            g->x - rx * hw, base + h, g->z - rz * hw,
        };
        GLfloat uvs[12] = { 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0 };
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4f(0.7f, 0.85f, 1.0f, a);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, teslaArcTex);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        glVertexPointer(3, GL_FLOAT, 0, verts);
        glTexCoordPointer(2, GL_FLOAT, 0, uvs);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glColor4f(1, 1, 1, 1);
        glEnableClientState(GL_COLOR_ARRAY);
    }
}

/* The pocket dimension's throne of eyes, billboarded. */
static GLuint pdThroneTex;

/* The multi-mesh assembly: the eight tunnels ringing the start room
 * plus the far throne and pillar rooms, each instance overlaid on the
 * base dimension_106 room. Split into opaque/alpha passes to match the
 * room batches. */
static void drawPocketComposite(int alphaPass) {
    if (!inPocket || !pdMeshCount) return;
    float ox = PD_GX * ROOM_SPACING, oz = PD_GY * ROOM_SPACING;
    for (int m = 0; m < pdInstCount; m++) {
        const PdInstance *in = &pdInst[m];
        const PdMesh *pm = &pdMesh[in->mesh];
        glPushMatrix();
        glTranslatef(ox + in->off[0], in->off[1], oz + in->off[2]);
        glRotatef(-in->yawDeg, 0.0f, 1.0f, 0.0f);
        drawBatchSet(pm->scene, pm->gl, alphaPass);
        glPopMatrix();
    }
}

static void drawMtMaze(int alphaPass) {
    if (!mtInstCount || !inMtBounds(camPos[0], camPos[2])) return;
    for (int m = 0; m < mtInstCount; m++) {
        const MtInstance *in = &mtInst[m];
        float dx = in->wx - camPos[0], dz = in->wz - camPos[2];
        if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;
        const MtMesh *mm = &mtMesh[in->mesh];
        if (!mm->scene) continue;
        glPushMatrix();
        glTranslatef(in->wx, 0.0f, in->wz);
        glRotatef(-(float)(in->yaw * 90), 0.0f, 1.0f, 0.0f);
        drawBatchSet(mm->scene, mm->gl, alphaPass);
        glPopMatrix();
    }
}

static void drawPocketPillars(void) {
    if (!inPocket || !pdPillarsOn || !pdPillarRT.ok) return;
    float t = pdEventState * 0.03f;
    for (int i = 0; i < 2; i++) {
        glPushMatrix();
        glTranslatef(pdPillar[i][0], pdPillar[i][1], pdPillar[i][2]);
        glRotatef(t * 2.0f * 57.29578f, 0.0f, 1.0f, 0.0f);
        drawModelRT(&pdPillarRT);
        glPopMatrix();
    }
}

static void drawArm682(void) {
    if (!arm682Active || !arm682RT.ok) return;
    glPushMatrix();
    glTranslatef(arm682Pos[0], arm682Pos[1], arm682Pos[2]);
    glRotatef(-arm682Yaw, 0.0f, 1.0f, 0.0f);
    glRotatef(arm682Roll, 0.0f, 0.0f, 1.0f);
    drawModelRT(&arm682RT);
    glPopMatrix();
}

static void drawPocketThrone(void) {
    if (!inPocket || !pdThroneTex) return;
    /* Throne of eyes at the head of the throne room (Objects[17]):
     * dim_3 sits at z + 8192 raw, the sprite 2848 nearer and 1376 up
     * (r\y + 1376*RoomScale, EntityZ(dim_3) - 2848*RoomScale). */
    float t = sinf(pdCircle * 0.05f) * 0.15f + 0.7f;
    float pos[3] = { PD_GX * ROOM_SPACING, 1376.0f,
                     PD_GY * ROOM_SPACING + 8192.0f - 2848.0f };
    drawBillboard(pos, 900.0f, 900.0f, pdThroneTex, t);
}

static void draw106(const float viewPos[3]) {
    if (!npc106Active || !skin106 || npc106State == N106_DORMANT) return;
    if (inPocket && pd106Hidden) return; /* absent from this sub-room */
    float dx = npc106Pos[0] - viewPos[0], dz = npc106Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    GLuint *ibos = skinIBOsFor(skin106);
    if (!ibos) return;
    if (!vbo106) glGenBuffers(1, &vbo106);
    /* Re-skin at ~30 Hz. */
    static int poseTick;
    if (!posed106 || ((poseTick++) & 1) == 0) {
        skinnedEval(skin106, npc106Frame);
        uint32_t vc;
        const SceneVertex *verts = skinnedVertices(skin106, &vc);
        glBindBuffer(GL_ARRAY_BUFFER, vbo106);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vc * sizeof(SceneVertex)),
                     verts, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        posed106 = 1;
    }
    glPushMatrix();
    glTranslatef(npc106Pos[0], npc106Pos[1], npc106Pos[2]);
    glRotatef(-npc106YawDeg + 180.0f, 0.0f, 1.0f, 0.0f);
    glScalef(skin106Scale, skin106Scale, skin106Scale);
    glBindBuffer(GL_ARRAY_BUFFER, vbo106);
    glVertexPointer(3, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(x));
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(SceneVertex), VTX_OFF(r));
    glTexCoordPointer(2, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(du));
    for (uint32_t b = 0; b < skinnedBatchCount(skin106); b++) {
        const SkinBatch *batch = skinnedBatch(skin106, b);
        GLuint tex = textureGet(batch->textureName);
        if (tex) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex); }
        else glDisable(GL_TEXTURE_2D);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibos[b]);
        glDrawElements(GL_TRIANGLES, (GLsizei)batch->indexCount,
                       GL_UNSIGNED_SHORT, NULL);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glPopMatrix();

    /* The trailing "second body" (n\OBJ2): a faded duplicate that wells
     * up behind it at distance / in fog (source shows OBJ2 beyond
     * CameraFogDist*0.6, rotated yaw-180, up 0.946 back -0.165, alpha by
     * distance). Reuses the posed VBO with a flat, dim, alpha color. */
    float distSq = dx * dx + dz * dz;
    if (distSq > 1300.0f * 1300.0f) {
        float a = (sqrtf(distSq) - 1300.0f) / 2200.0f;
        if (a > 0.55f) a = 0.55f;
        if (a > 0.02f) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glDisableClientState(GL_COLOR_ARRAY);
            glColor4f(0.55f, 0.5f, 0.55f, a);
            glPushMatrix();
            glTranslatef(npc106Pos[0], npc106Pos[1] + 242.0f, npc106Pos[2]);
            glRotatef(-npc106YawDeg, 0.0f, 1.0f, 0.0f); /* yaw-180 vs body */
            glTranslatef(0.0f, 0.0f, -42.0f);
            glScalef(skin106Scale, skin106Scale, skin106Scale);
            glBindBuffer(GL_ARRAY_BUFFER, vbo106);
            glVertexPointer(3, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(x));
            glTexCoordPointer(2, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(du));
            for (uint32_t b = 0; b < skinnedBatchCount(skin106); b++) {
                const SkinBatch *batch = skinnedBatch(skin106, b);
                GLuint tex = textureGet(batch->textureName);
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
            glPopMatrix();
            glColor4f(1, 1, 1, 1);
            glEnableClientState(GL_COLOR_ARRAY);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
    }
}

static void draw096(const float viewPos[3]) {
    if (!npc096Active || !skin096) return;
    float dx = npc096Pos[0] - viewPos[0], dz = npc096Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    GLuint *ibos = skinIBOsFor(skin096);
    if (!ibos) return;
    if (!vbo096) glGenBuffers(1, &vbo096);
    static int poseTick;
    if (!posed096 || ((poseTick++) & 1) == 0) {
        skinnedEval(skin096, npc096Frame);
        uint32_t vc;
        const SceneVertex *verts = skinnedVertices(skin096, &vc);
        glBindBuffer(GL_ARRAY_BUFFER, vbo096);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vc * sizeof(SceneVertex)),
                     verts, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        posed096 = 1;
    }
    glPushMatrix();
    glTranslatef(npc096Pos[0], npc096Pos[1], npc096Pos[2]);
    glRotatef(-npc096YawDeg + 180.0f, 0.0f, 1.0f, 0.0f);
    glScalef(skin096Scale, skin096Scale, skin096Scale);
    glBindBuffer(GL_ARRAY_BUFFER, vbo096);
    glVertexPointer(3, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(x));
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(SceneVertex), VTX_OFF(r));
    glTexCoordPointer(2, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(du));
    for (uint32_t b = 0; b < skinnedBatchCount(skin096); b++) {
        const SkinBatch *batch = skinnedBatch(skin096, b);
        GLuint tex = textureGet(batch->textureName);
        if (tex) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex); }
        else glDisable(GL_TEXTURE_2D);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibos[b]);
        glDrawElements(GL_TRIANGLES, (GLsizei)batch->indexCount,
                       GL_UNSIGNED_SHORT, NULL);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glPopMatrix();
}

/* Draw a posed skinned mesh (its VBO already filled) at a world
 * position/yaw - shared by 049 and each 049-2. */
static void drawSkinnedAtTex(SkinnedMesh *skin, GLuint vbo, GLuint *ibos,
                             float scale, const float pos[3], float yawDeg,
                             GLuint texOverride) {
    glPushMatrix();
    glTranslatef(pos[0], pos[1], pos[2]);
    glRotatef(-yawDeg + 180.0f, 0.0f, 1.0f, 0.0f);
    glScalef(scale, scale, scale);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexPointer(3, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(x));
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(SceneVertex), VTX_OFF(r));
    glTexCoordPointer(2, GL_FLOAT, sizeof(SceneVertex), VTX_OFF(du));
    for (uint32_t b = 0; b < skinnedBatchCount(skin); b++) {
        const SkinBatch *batch = skinnedBatch(skin, b);
        GLuint tex = texOverride ? texOverride : textureGet(batch->textureName);
        if (tex) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex); }
        else glDisable(GL_TEXTURE_2D);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibos[b]);
        glDrawElements(GL_TRIANGLES, (GLsizei)batch->indexCount,
                       GL_UNSIGNED_SHORT, NULL);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glPopMatrix();
}

static void drawSkinnedAt(SkinnedMesh *skin, GLuint vbo, GLuint *ibos,
                          float scale, const float pos[3], float yawDeg) {
    drawSkinnedAtTex(skin, vbo, ibos, scale, pos, yawDeg, 0);
}

static void draw049(const float viewPos[3]) {
    if (npc049Active && skin049) {
        float dx = npc049Pos[0] - viewPos[0], dz = npc049Pos[2] - viewPos[2];
        GLuint *ibos = skinIBOsFor(skin049);
        if (ibos && dx * dx + dz * dz <= VIEW_RANGE * VIEW_RANGE) {
            if (!vbo049) glGenBuffers(1, &vbo049);
            static int poseTick;
            if (!posed049 || ((poseTick++) & 1) == 0) {
                skinnedEval(skin049, npc049Frame);
                uint32_t vc;
                const SceneVertex *v = skinnedVertices(skin049, &vc);
                glBindBuffer(GL_ARRAY_BUFFER, vbo049);
                glBufferData(GL_ARRAY_BUFFER,
                             (GLsizeiptr)(vc * sizeof(SceneVertex)), v,
                             GL_DYNAMIC_DRAW);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                posed049 = 1;
            }
            drawSkinnedAt(skin049, vbo049, ibos, skin049Scale, npc049Pos,
                          npc049YawDeg);
        }
    }
    /* 049-2: skin once at the shared phase, draw each shambler. */
    if (npc0492Count && skin0492) {
        GLuint *ibos = skinIBOsFor(skin0492);
        if (!ibos) return;
        if (!vbo0492) glGenBuffers(1, &vbo0492);
        static int poseTick2;
        if (!posed0492 || ((poseTick2++) & 1) == 0) {
            skinnedEval(skin0492, npc0492Frame);
            uint32_t vc;
            const SceneVertex *v = skinnedVertices(skin0492, &vc);
            glBindBuffer(GL_ARRAY_BUFFER, vbo0492);
            glBufferData(GL_ARRAY_BUFFER,
                         (GLsizeiptr)(vc * sizeof(SceneVertex)), v,
                         GL_DYNAMIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            posed0492 = 1;
        }
        for (int z = 0; z < npc0492Count; z++) {
            if (!npc0492Active[z]) continue;
            float dx = npc0492Pos[z][0] - viewPos[0];
            float dz = npc0492Pos[z][2] - viewPos[2];
            if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;
            drawSkinnedAt(skin0492, vbo0492, ibos, skin0492Scale,
                          npc0492Pos[z], npc0492Yaw[z]);
        }
    }
}

static void draw939(const float viewPos[3]) {
    if (!npc939Active || !skin939) return;
    float dx = npc939Pos[0] - viewPos[0], dz = npc939Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    GLuint *ibos = skinIBOsFor(skin939);
    if (!ibos) return;
    if (!vbo939) glGenBuffers(1, &vbo939);
    static int poseTick;
    if (!posed939 || ((poseTick++) & 1) == 0) {
        skinnedEval(skin939, npc939Frame);
        uint32_t vc;
        const SceneVertex *v = skinnedVertices(skin939, &vc);
        glBindBuffer(GL_ARRAY_BUFFER, vbo939);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vc * sizeof(SceneVertex)),
                     v, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        posed939 = 1;
    }
    drawSkinnedAt(skin939, vbo939, ibos, skin939Scale, npc939Pos,
                  npc939YawDeg);
}

static void draw966(const float viewPos[3]) {
    /* Only the night-vision goggles reveal it. */
    if (!npc966Active || !skin966 || !wearNVG) return;
    float dx = npc966Pos[0] - viewPos[0], dz = npc966Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    GLuint *ibos = skinIBOsFor(skin966);
    if (!ibos) return;
    if (!vbo966) glGenBuffers(1, &vbo966);
    static int poseTick;
    if (!posed966 || ((poseTick++) & 1) == 0) {
        skinnedEval(skin966, npc966Frame);
        uint32_t vc;
        const SceneVertex *v = skinnedVertices(skin966, &vc);
        glBindBuffer(GL_ARRAY_BUFFER, vbo966);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vc * sizeof(SceneVertex)),
                     v, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        posed966 = 1;
    }
    drawSkinnedAt(skin966, vbo966, ibos, skin966Scale, npc966Pos,
                  npc966YawDeg);
}

static void draw1499(const float viewPos[3]) {
    if (!inMask || !npc1499Count || !skin1499) return;
    GLuint *ibos = skinIBOsFor(skin1499);
    if (!ibos) return;
    if (!vbo1499) glGenBuffers(1, &vbo1499);
    /* Each member has its own animation phase, so it is skinned per
     * instance; the king wears his crowned texture. */
    for (int k = 0; k < npc1499Count; k++) {
        if (!npc1499Active[k]) continue;
        float dx = npc1499Pos[k][0] - viewPos[0];
        float dz = npc1499Pos[k][2] - viewPos[2];
        if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;
        uint32_t vc;
        skinnedEval(skin1499, npc1499Frame[k]);
        const SceneVertex *v = skinnedVertices(skin1499, &vc);
        glBindBuffer(GL_ARRAY_BUFFER, vbo1499);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vc * sizeof(SceneVertex)),
                     v, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        drawSkinnedAtTex(skin1499, vbo1499, ibos, skin1499Scale,
                         npc1499Pos[k], npc1499Yaw[k],
                         npc1499Type[k] == 2 ? tex1499King : 0);
    }
}

static void draw860(const float viewPos[3]) {
    if (!npc860Active) return;
    if (strcmp(roomNameAt(camPos), "cont2_860_1") != 0) return;
    /* The copse of trees. */
    if (tree860RT.ok) {
        for (int t = 0; t < tree860Count; t++) {
            float dx = tree860Pos[t][0] - viewPos[0];
            float dz = tree860Pos[t][2] - viewPos[2];
            if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;
            glPushMatrix();
            glTranslatef(tree860Pos[t][0], tree860Pos[t][1], tree860Pos[t][2]);
            glRotatef((float)(t * 47 % 360), 0.0f, 1.0f, 0.0f);
            drawModelRT(&tree860RT);
            glPopMatrix();
        }
    }
    /* The stalker itself. */
    if (!skin860) return;
    float dx = npc860Pos[0] - viewPos[0], dz = npc860Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    GLuint *ibos = skinIBOsFor(skin860);
    if (!ibos) return;
    if (!vbo860) glGenBuffers(1, &vbo860);
    static int poseTick;
    if (!posed860 || ((poseTick++) & 1) == 0) {
        skinnedEval(skin860, npc860Frame);
        uint32_t vc;
        const SceneVertex *v = skinnedVertices(skin860, &vc);
        glBindBuffer(GL_ARRAY_BUFFER, vbo860);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vc * sizeof(SceneVertex)),
                     v, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        posed860 = 1;
    }
    drawSkinnedAt(skin860, vbo860, ibos, skin860Scale, npc860Pos,
                  npc860YawDeg);
}

/* ---- SCP-372: the peripheral jumper (cont3_372) ---- */

static void spawn372(void) {
    npc372Ok = 0;
    npc372Idle = 1;
    npc372State = 0.0f;
    npc372Frame = 0.0f;
    if (!skin372) return;
    for (uint32_t r = 0; r < map.roomCount; r++) {
        if (strcmp(tplList.items[map.rooms[r].templateIndex].name,
                   "cont3_372") == 0) {
            npc372Ok = 1;
            break;
        }
    }
    npc372Pos[0] = npc372Pos[1] = npc372Pos[2] = 0.0f;
}

/* UpdateNPCType372: idle and hidden, it now and then flits to a spot
 * ~450 raw off to the player's side and lingers; while active it bobs and
 * spins, but the instant you centre it in your gaze it darts back toward
 * the edge of sight. Purely a startle - it never touches you. */
static void update372(void) {
    if (!npc372Ok || !skin372) return;
    if (strcmp(roomNameAt(camPos), "cont3_372") != 0) {
        npc372Idle = 1;
        return;
    }
    if (npc372Idle) {
        /* Source reveals it during a blink (BlinkTimer window): you blink
         * and it is suddenly there at the edge of sight. A rare timer
         * fallback keeps it appearing even if you barely blink. */
        if ((blinkFrames > 0 && (rand() % 4) == 0) || (rand() % 240) == 0) {
            float ang = camYaw + (float)((rand() % 180) - 90) * 0.0174533f;
            float dist = 400.0f + (float)(rand() % 130);
            npc372Pos[0] = camPos[0] + sinf(ang) * dist;
            npc372Pos[2] = camPos[2] - cosf(ang) * dist;
            npc372Pos[1] = camPos[1] - EYE_HEIGHT * 0.4f;
            npc372Idle = 0;
            npc372State = 60.0f + (float)(rand() % 120);
            if ((rand() % 4) == 0) {
                audioPlay3D(sndRattle[rand() % 3], npc372Pos, camPos, camYaw,
                            900.0f);
            }
        }
        return;
    }
    /* Active: bob and advance the anim. */
    npc372Frame += 3.0f;
    if (npc372Frame > 300.0f) npc372Frame = 1.0f;
    /* If the player centres it in view it darts to the periphery: cos of
     * the angle between the view forward and the direction to it - beyond
     * ~22 deg off-centre it stops fleeing and lingers at the edge of
     * sight. */
    float dx = npc372Pos[0] - camPos[0], dz = npc372Pos[2] - camPos[2];
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 1.0f) len = 1.0f;
    float fx = sinf(camYaw), fz = -cosf(camYaw);   /* view forward */
    float cosang = (dx * fx + dz * fz) / len;
    if (cosang > 0.927f) {
        float rx = -fz, rz = fx;                    /* view right */
        float s = (dx * rx + dz * rz >= 0.0f) ? 1.0f : -1.0f;
        npc372Pos[0] += rx * s * 16.0f;             /* dart to that side */
        npc372Pos[2] += rz * s * 16.0f;
        if ((rand() % 30) == 0) {
            audioPlay3D(sndRattle[rand() % 3], npc372Pos, camPos, camYaw,
                        900.0f);
        }
    }
    npc372State -= 0.8f;
    if (npc372State <= 0.0f) npc372Idle = 1; /* vanishes */
}

static void draw372(const float viewPos[3]) {
    if (!npc372Ok || npc372Idle || !skin372) return;
    float dx = npc372Pos[0] - viewPos[0], dz = npc372Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    GLuint *ibos = skinIBOsFor(skin372);
    if (!ibos) return;
    if (!vbo372) glGenBuffers(1, &vbo372);
    static int poseTick;
    if (!posed372 || ((poseTick++) & 1) == 0) {
        skinnedEval(skin372, npc372Frame);
        uint32_t vc;
        const SceneVertex *v = skinnedVertices(skin372, &vc);
        glBindBuffer(GL_ARRAY_BUFFER, vbo372);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vc * sizeof(SceneVertex)),
                     v, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        posed372 = 1;
    }
    /* A slow spin, source-style (RotateEntity roll = MilliSec/5). */
    float bob = sinf(gTick * 0.08f) * 12.0f;
    float p[3] = { npc372Pos[0], npc372Pos[1] + 76.0f + bob, npc372Pos[2] };
    drawSkinnedAt(skin372, vbo372, ibos, skin372Scale, p,
                  (float)((gTick * 2) % 360));
}

/* ---- SCP-205: the shadow demon (cont1_205) ---- */

static void spawn205(void) {
    npc205Ok = 0;
    npc205Rise = 0.0f;
    npc205Frame = 0.0f;
    npc205Horror = 0.0f;
    if (!skin205) return;
    for (uint32_t r = 0; r < map.roomCount; r++) {
        const RoomPlacement *p = &map.rooms[r];
        if (strcmp(tplList.items[p->templateIndex].name, "cont1_205") != 0) {
            continue;
        }
        /* Demons' spawnpoint (source local -1536,730,192). */
        float local[3] = { -1536.0f, 730.0f, 192.0f }, w[3];
        localToWorld(p, local, w);
        npc205Pos[0] = w[0]; npc205Pos[1] = w[1]; npc205Pos[2] = w[2];
        npc205Home[0] = w[0]; npc205Home[1] = w[1]; npc205Home[2] = w[2];
        npc205Yaw = (float)(p->angle * 90) - 90.0f;
        npc205Ok = 1;
        break;
    }
}

/* e_cont1_205: while the player is in the chamber the shadow demon rises
 * and looms, moaning (Horror.ogg) and gnawing at sanity. Seen mainly on
 * the observation monitor the camera feeds. The rising-woman set-piece
 * and the lethal grab are approximated as a dread swell. */
static void update205(void) {
    if (!npc205Ok || !skin205) return;
    int inChamber = strcmp(roomNameAt(camPos), "cont1_205") == 0;
    npc205Frame += 1.5f;
    if (npc205Frame > 300.0f) npc205Frame = 1.0f;
    if (inChamber) {
        if (npc205Rise < 1.0f) npc205Rise += 0.0015f;
        sanity -= 0.03f;
        if (sanity < -1000.0f) sanity = -1000.0f;
        npc205Horror += 1.0f;
        if (npc205Horror > 360.0f) {
            npc205Horror = 0.0f;
            if (snd205Horror >= 0) {
                audioPlay3D(snd205Horror, npc205Pos, camPos, camYaw, 2000.0f);
            }
        }
        /* Fully risen, the shadow lunges - it closes on the player and
         * grabs on contact (source teleports you into the demon). */
        if (npc205Rise >= 1.0f && deathTimer == 0 && walkMode
            && introPhase < 0) {
            float dx = camPos[0] - npc205Pos[0];
            float dz = camPos[2] - npc205Pos[2];
            float dist = sqrtf(dx * dx + dz * dz);
            if (dist > 1.0f) {
                float speed = 7.0f + (float)npcAggressive * 3.0f;
                npc205Pos[0] += dx / dist * speed;
                npc205Pos[2] += dz / dist * speed;
                npc205Yaw = atan2f(dx, dz) * 180.0f / 3.14159265f;
            }
            float o[3] = { npc205Pos[0], npc205Pos[1] + 250.0f, npc205Pos[2] };
            float hy;
            if (rayDownWorld(o, 600.0f, &hy)) npc205Pos[1] = hy;
            if (dist < 200.0f) {
                snprintf(deathCause, sizeof(deathCause), "SCP-205");
                deathTimer = 180;
            }
        }
    } else if (npc205Rise > 0.0f) {
        npc205Rise -= 0.004f;
        if (npc205Rise < 0.0f) {
            npc205Rise = 0.0f;
            /* Sank back; return to its lair for next time. */
            npc205Pos[0] = npc205Home[0];
            npc205Pos[1] = npc205Home[1];
            npc205Pos[2] = npc205Home[2];
        }
    }
}

static void draw205(const float viewPos[3]) {
    if (!npc205Ok || npc205Rise <= 0.01f || !skin205) return;
    float dx = npc205Pos[0] - viewPos[0], dz = npc205Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    GLuint *ibos = skinIBOsFor(skin205);
    if (!ibos) return;
    if (!vbo205) glGenBuffers(1, &vbo205);
    static int poseTick;
    if (!posed205 || ((poseTick++) & 1) == 0) {
        skinnedEval(skin205, npc205Frame);
        uint32_t vc;
        const SceneVertex *v = skinnedVertices(skin205, &vc);
        glBindBuffer(GL_ARRAY_BUFFER, vbo205);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vc * sizeof(SceneVertex)),
                     v, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        posed205 = 1;
    }
    /* Rises up out of the floor as npc205Rise grows. */
    float p[3] = { npc205Pos[0], npc205Pos[1] - 260.0f + npc205Rise * 260.0f,
                   npc205Pos[2] };
    drawSkinnedAt(skin205, vbo205, ibos, skin205Scale, p, npc205Yaw);
}

/* ---- SCP-914's hostile output (SCP-008 zombie / hostile SCP-1499) ---- */

static void spawnRefineHostile(int type) {
    if (type == 0 && !skin008) return;
    if (type == 1 && !skin1499) return;
    rhActive = 1;
    rhType = type;
    rhPos[0] = scp914Out[0];
    rhPos[2] = scp914Out[2];
    float o[3] = { rhPos[0], scp914Out[1] + 200.0f, rhPos[2] };
    float hy;
    rhPos[1] = rayDownWorld(o, 800.0f, &hy) ? hy : scp914Out[1];
    rhFrame = 0.0f;
    rhYaw = 0.0f;
    if (type == 0 && snd939Horror >= 0) audioPlay(snd939Horror, 0.8f, 0.0f);
    if (type == 1 && snd1499Trig >= 0) audioPlay(snd1499Trig, 0.9f, 0.0f);
}

static void updateRefineHostile(void) {
    if (!rhActive || deathTimer > 0 || !walkMode || introPhase >= 0
        || inPocket || inMask) {
        return;
    }
    float dx = camPos[0] - rhPos[0], dz = camPos[2] - rhPos[2];
    float dist = sqrtf(dx * dx + dz * dz);
    rhFrame += 0.25f;
    if (rhFrame > 200.0f) rhFrame = 2.0f;
    float speed = 9.0f + (float)npcAggressive * 3.0f;
    if (dist > 1.0f) {
        rhPos[0] += dx / dist * speed;
        rhPos[2] += dz / dist * speed;
        rhYaw = atan2f(dx, dz) * 180.0f / 3.14159265f;
    }
    float o[3] = { rhPos[0], rhPos[1] + 250.0f, rhPos[2] };
    float hy;
    if (rayDownWorld(o, 600.0f, &hy)) rhPos[1] = hy;
    if (dist < 190.0f) {
        snprintf(deathCause, sizeof(deathCause),
                 rhType == 0 ? "SCP-008" : "SCP-1499");
        deathTimer = 180;
    }
}

static void drawRefineHostile(const float viewPos[3]) {
    if (!rhActive) return;
    SkinnedMesh *sk = rhType == 0 ? skin008 : skin1499;
    float scale = rhType == 0 ? skin008Scale : skin1499Scale;
    if (!sk) return;
    float dx = rhPos[0] - viewPos[0], dz = rhPos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    GLuint *ibos = skinIBOsFor(sk);
    if (!ibos) return;
    if (!rhVbo) glGenBuffers(1, &rhVbo);
    skinnedEval(sk, rhFrame);
    uint32_t vc;
    const SceneVertex *v = skinnedVertices(sk, &vc);
    glBindBuffer(GL_ARRAY_BUFFER, rhVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vc * sizeof(SceneVertex)),
                 v, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    drawSkinnedAt(sk, rhVbo, ibos, scale, rhPos, rhYaw);
}

/* ---- SCP-513-1: the bell-summoned haunt ---- */

static void spawn513(void) {
    if (!skin513) return;
    npc513Active = 1;
    npc513Timer = 0.0f;
    npc513Bell = 0.0f;
    npc513Frame = 0.0f;
    float ang = camYaw + 1.2f;
    npc513Pos[0] = camPos[0] + sinf(ang) * 450.0f;
    npc513Pos[2] = camPos[2] - cosf(ang) * 450.0f;
    npc513Pos[1] = camPos[1] - EYE_HEIGHT * 0.3f;
    npc513Yaw = 0.0f;
    if (snd513Bell[0] >= 0) audioPlay(snd513Bell[rand() % 3], 0.8f, 0.0f);
}

/* UpdateNPCType513_1: it lurks at the edge of sight; centring it makes it
 * flit elsewhere, it tolls its bells now and then, and its presence eats
 * at sanity - but it never touches you. */
static void update513(void) {
    if (!npc513Active || !skin513 || deathTimer > 0 || !walkMode
        || introPhase >= 0 || inPocket || inMask) {
        return;
    }
    npc513Frame += 0.3f;
    if (npc513Frame > 200.0f) npc513Frame = 2.0f;
    float dx = camPos[0] - npc513Pos[0], dz = camPos[2] - npc513Pos[2];
    float len = sqrtf(dx * dx + dz * dz);
    if (len < 1.0f) len = 1.0f;
    float fx = sinf(camYaw), fz = -cosf(camYaw);
    if ((-dx * fx - dz * fz) / len > 0.9f) {
        npc513Timer = 999.0f; /* stared at: flit away now */
    }
    npc513Timer += 1.0f;
    if (npc513Timer > 180.0f) {
        npc513Timer = 0.0f;
        float ang = camYaw + ((rand() % 2) ? 1.2f : -1.2f)
                  + (float)((rand() % 40) - 20) * 0.01f;
        float dist = 380.0f + (float)(rand() % 160);
        npc513Pos[0] = camPos[0] + sinf(ang) * dist;
        npc513Pos[2] = camPos[2] - cosf(ang) * dist;
        npc513Pos[1] = camPos[1] - EYE_HEIGHT * 0.3f;
        len = 1.0f;
    }
    npc513Yaw = atan2f(dx, dz) * 180.0f / 3.14159265f;
    npc513Bell += 1.0f;
    if (npc513Bell > 300.0f) {
        npc513Bell = 0.0f;
        if (snd513Bell[0] >= 0) {
            audioPlay3D(snd513Bell[rand() % 3], npc513Pos, camPos, camYaw,
                        1500.0f);
        }
    }
    if (len < 700.0f) {
        sanity -= 0.03f;
        if (sanity < -1000.0f) sanity = -1000.0f;
        if (blurAmount < 0.12f) blurAmount = 0.12f;
    }
}

static void draw513(const float viewPos[3]) {
    if (!npc513Active || !skin513) return;
    float dx = npc513Pos[0] - viewPos[0], dz = npc513Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    GLuint *ibos = skinIBOsFor(skin513);
    if (!ibos) return;
    if (!vbo513) glGenBuffers(1, &vbo513);
    static int poseTick;
    if (!posed513 || ((poseTick++) & 1) == 0) {
        skinnedEval(skin513, npc513Frame);
        uint32_t vc;
        const SceneVertex *v = skinnedVertices(skin513, &vc);
        glBindBuffer(GL_ARRAY_BUFFER, vbo513);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vc * sizeof(SceneVertex)),
                     v, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        posed513 = 1;
    }
    float bob = sinf(gTick * 0.06f) * 10.0f;
    float p[3] = { npc513Pos[0], npc513Pos[1] + 90.0f + bob, npc513Pos[2] };
    drawSkinnedAt(skin513, vbo513, ibos, skin513Scale, p, npc513Yaw);
}

/* ---- SCP-035: the possessed host (cont1_035) ---- */

static void spawn035(void) {
    npc035Ok = 0;
    npc035State = 0;
    npc035Frame = 0.0f;
    if (!skinClassD) return;
    for (uint32_t r = 0; r < map.roomCount; r++) {
        const RoomPlacement *p = &map.rooms[r];
        if (strcmp(tplList.items[p->templateIndex].name, "cont1_035") != 0) {
            continue;
        }
        float local[3] = { -576.0f, 0.5f, 640.0f }, w[3];
        localToWorld(p, local, w);
        npc035Pos[0] = w[0]; npc035Pos[1] = w[1]; npc035Pos[2] = w[2];
        npc035Home[0] = w[0]; npc035Home[1] = w[1]; npc035Home[2] = w[2];
        npc035Yaw = (float)(p->angle * 90) + 270.0f;
        npc035Ok = 1;
        break;
    }
}

/* e_cont1_035: the possessed Class-D slumps in the chamber until you look
 * at it up close, then it gets up (GetUp.ogg) and hunts you down, killing
 * on contact. The gas-lever containment puzzle and the corrosive
 * tentacles are not reproduced. */
static void update035(void) {
    if (!npc035Ok || !skinClassD || deathTimer > 0 || !walkMode
        || introPhase >= 0) {
        return;
    }
    npc035Frame += npc035State ? 0.5f : 0.1f;
    if (npc035Frame > 300.0f) npc035Frame = 2.0f;
    float dx = camPos[0] - npc035Pos[0], dz = camPos[2] - npc035Pos[2];
    float dist = sqrtf(dx * dx + dz * dz);
    if (npc035State == 0) {
        if (strcmp(roomNameAt(camPos), "cont1_035") != 0) return;
        if (dist < 900.0f) {
            float len = dist < 1.0f ? 1.0f : dist;
            float fx = sinf(camYaw), fz = -cosf(camYaw);
            if ((dx * fx + dz * fz) / len > 0.6f) { /* you look at it */
                npc035State = 1;
                if (snd035GetUp >= 0) audioPlay(snd035GetUp, 0.9f, 0.0f);
                snprintf(toastMsg, sizeof(toastMsg),
                         "IT WEARS A SMILING FACE...");
                toastTimer = 200;
            }
        }
    } else {
        if (dist > 1.0f) {
            float speed = 8.0f + (float)npcAggressive * 3.0f;
            npc035Pos[0] += dx / dist * speed;
            npc035Pos[2] += dz / dist * speed;
            npc035Yaw = atan2f(dx, dz) * 180.0f / 3.14159265f;
        }
        float o[3] = { npc035Pos[0], npc035Pos[1] + 250.0f, npc035Pos[2] };
        float hy;
        if (rayDownWorld(o, 600.0f, &hy)) npc035Pos[1] = hy;
        if (dist < 190.0f) {
            snprintf(deathCause, sizeof(deathCause), "SCP-035");
            deathTimer = 180;
        }
        if (dist > 3400.0f) { /* lost you: sit back down */
            npc035State = 0;
            npc035Pos[0] = npc035Home[0];
            npc035Pos[1] = npc035Home[1];
            npc035Pos[2] = npc035Home[2];
        }
    }
}

static void draw035(const float viewPos[3]) {
    if (!npc035Ok || !skinClassD) return;
    float dx = npc035Pos[0] - viewPos[0], dz = npc035Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    GLuint *ibos = skinIBOsFor(skinClassD);
    if (!ibos) return;
    if (!vbo035) glGenBuffers(1, &vbo035);
    skinnedEval(skinClassD, npc035Frame);
    uint32_t vc;
    const SceneVertex *v = skinnedVertices(skinClassD, &vc);
    glBindBuffer(GL_ARRAY_BUFFER, vbo035);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(vc * sizeof(SceneVertex)),
                 v, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    drawSkinnedAtTex(skinClassD, vbo035, ibos, skinClassDScale, npc035Pos,
                     npc035Yaw, tex035);
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

/* ---- SCP-914: the refinement machine (cont1_914) ---- */

static const char *setting914Name(int s) {
    switch (s) {
        case -2: return "ROUGH";
        case -1: return "COARSE";
        case 0:  return "1:1";
        case 1:  return "FINE";
        default: return "VERY FINE";
    }
}

/* A slice of Use914: keycards step up/down by setting (1:1 famously spits
 * out a Playing Card), and everything else is destroyed on Rough/Coarse
 * and passed through otherwise. Returns the output template, or -1 if the
 * item is consumed to nothing. */
/* The Use914 transformation table, one row per input item, outputs in
 * setting order { ROUGH, COARSE, 1:1, FINE, VERYFINE }. NULL = the item
 * is destroyed. Multi-output / random / NPC / difficulty branches in the
 * source are collapsed to their primary deterministic outcome. Any output
 * whose template the port lacks falls back to passing the item through. */
typedef struct {
    const char *in;
    const char *out[5];
} Refine914Row;

static const Refine914Row REFINE914[] = {
    { "Gas Mask",            { NULL, NULL, "Gas Mask", "Fine Gas Mask", "Very Fine Gas Mask" } },
    { "Fine Gas Mask",       { NULL, NULL, "Gas Mask", "Fine Gas Mask", "Very Fine Gas Mask" } },
    { "Very Fine Gas Mask",  { NULL, NULL, "Gas Mask", "Fine Gas Mask", "Very Fine Gas Mask" } },
    { "Heavy Gas Mask",      { NULL, NULL, "Gas Mask", "Fine Gas Mask", "Very Fine Gas Mask" } },
    { "SCP-1499",            { NULL, NULL, "Gas Mask", "Fine SCP-1499", "Fine SCP-1499" } },
    { "Fine SCP-1499",       { NULL, NULL, "Gas Mask", "Fine SCP-1499", "Fine SCP-1499" } },
    { "Night Vision Goggles",{ NULL, "Electronical Components", "SCRAMBLE Gear", "Fine Night Vision Goggles", "Very Fine Night Vision Goggles" } },
    { "Fine Night Vision Goggles",{ NULL, "Electronical Components", "SCRAMBLE Gear", "Fine Night Vision Goggles", "Very Fine Night Vision Goggles" } },
    { "Very Fine Night Vision Goggles",{ NULL, "Electronical Components", "SCRAMBLE Gear", "Fine Night Vision Goggles", "Very Fine Night Vision Goggles" } },
    { "SCRAMBLE Gear",       { NULL, "Electronical Components", "Night Vision Goggles", "Fine SCRAMBLE Gear", "Fine SCRAMBLE Gear" } },
    { "Fine SCRAMBLE Gear",  { NULL, "Electronical Components", "Night Vision Goggles", "Fine SCRAMBLE Gear", "Fine SCRAMBLE Gear" } },
    { "Ballistic Helmet",    { NULL, NULL, "Ballistic Vest", "Heavy Ballistic Vest", "Heavy Ballistic Vest" } },
    { "Newsboy Cap",         { NULL, "Newsboy Cap", NULL, "Fine SCP-268", "Fine SCP-268" } },
    { "SCP-268",             { NULL, "Newsboy Cap", NULL, "Fine SCP-268", "Fine SCP-268" } },
    { "Fine SCP-268",        { NULL, "Newsboy Cap", NULL, "Fine SCP-268", "Fine SCP-268" } },
    { "Ballistic Vest",      { NULL, "Corrosive Ballistic Vest", "Ballistic Helmet", "Heavy Ballistic Vest", "Bulky Ballistic Vest" } },
    { "Heavy Ballistic Vest",{ NULL, "Corrosive Ballistic Vest", "Ballistic Helmet", "Heavy Ballistic Vest", "Bulky Ballistic Vest" } },
    { "Hazmat Suit",         { NULL, NULL, "Hazmat Suit", "Fine Hazmat Suit", "Very Fine Hazmat Suit" } },
    { "Fine Hazmat Suit",    { NULL, NULL, "Hazmat Suit", "Fine Hazmat Suit", "Very Fine Hazmat Suit" } },
    { "Heavy Hazmat Suit",   { NULL, NULL, "Hazmat Suit", "Fine Hazmat Suit", "Very Fine Hazmat Suit" } },
    { "Very Fine Hazmat Suit",{ NULL, NULL, "Hazmat Suit", "Infected Syringe", "Infected Syringe" } },
    { "Electronical Components",{ NULL, NULL, NULL, "Night Vision Goggles", "Fine Night Vision Goggles" } },
    { "E-Reader",            { NULL, "Clipboard", NULL, "E-Reader 20", "E-Reader 30" } },
    { "E-Reader 20",         { NULL, "Clipboard", NULL, "E-Reader 20", "E-Reader 30" } },
    { "E-Reader 30",         { NULL, "Clipboard", NULL, "E-Reader 20", "E-Reader 30" } },
    { "Radio Transceiver",   { NULL, "Electronical Components", "18V Radio Transceiver", "Fine Radio Transceiver", "Very Fine Radio Transceiver" } },
    { "18V Radio Transceiver",{ NULL, "Electronical Components", "18V Radio Transceiver", "Fine Radio Transceiver", "Very Fine Radio Transceiver" } },
    { "Fine Radio Transceiver",{ NULL, "Electronical Components", "18V Radio Transceiver", "Fine Radio Transceiver", "Very Fine Radio Transceiver" } },
    { "S-NAV Navigator",     { NULL, "Electronical Components", "S-NAV Navigator", "S-NAV 310 Navigator", "S-NAV 300 Navigator" } },
    { "S-NAV 300 Navigator", { NULL, "Electronical Components", "S-NAV Navigator", "S-NAV 310 Navigator", "S-NAV 300 Navigator" } },
    { "S-NAV 310 Navigator", { NULL, "Electronical Components", "S-NAV Navigator", "S-NAV 310 Navigator", "S-NAV 300 Navigator" } },
    { "SCP-148 Ingot",       { "SCP-148 Ingot", "SCP-148 Ingot", NULL, NULL, NULL } },
    { "White Severed Hand",  { NULL, NULL, "Black Severed Hand", NULL, NULL } },
    { "Black Severed Hand",  { NULL, NULL, "Yellow Severed Hand", NULL, NULL } },
    { "Yellow Severed Hand", { NULL, NULL, "White Severed Hand", NULL, NULL } },
    { "First Aid Kit",       { NULL, NULL, "Blue First Aid Kit", "Compact First Aid Kit", "Strange Bottle" } },
    { "Blue First Aid Kit",  { NULL, NULL, "First Aid Kit", "Compact First Aid Kit", "Strange Bottle" } },
    { "Compact First Aid Kit",{ NULL, NULL, "First Aid Kit", "Compact First Aid Kit", "Strange Bottle" } },
    { "Playing Card",        { NULL, NULL, "Level 1 key Card", "Level 1 key Card", "Level 2 key Card" } },
    { "Coin",                { NULL, NULL, "Level 1 key Card", "Level 1 key Card", "Level 2 key Card" } },
    { "Quarter",             { NULL, NULL, "Level 1 key Card", "Level 1 key Card", "Level 2 key Card" } },
    { "Mastercard",          { NULL, "Quarter", "Level 1 key Card", "Level 1 key Card", "Level 2 key Card" } },
    { "SCP-005",             { NULL, "Level 3 key Card", "Level 5 key Card", NULL, NULL } },
    { "SCP-860",             { NULL, NULL, "SCP-860", "Fine SCP-860", "Fine SCP-860" } },
    { "Fine SCP-860",        { NULL, NULL, "SCP-860", "Fine SCP-860", "Fine SCP-860" } },
    { "SCP-513",             { NULL, NULL, "SCP-513", "SCP-513", "SCP-513" } },
    { "Cigarette",           { NULL, NULL, "Cigarette", "Joint", "Smelly Joint" } },
    { "Joint",               { NULL, NULL, "Cigarette", "Joint", "Smelly Joint" } },
    { "SCP-714",             { NULL, "Coarse SCP-714", "SCP-714", "Fine SCP-714", "Fine SCP-714" } },
    { "Coarse SCP-714",      { NULL, "Coarse SCP-714", "SCP-714", "Fine SCP-714", "Fine SCP-714" } },
    { "Fine SCP-714",        { NULL, "Coarse SCP-714", "SCP-714", "Fine SCP-714", "Fine SCP-714" } },
    { "4.5V Battery",        { NULL, NULL, NULL, "9V Battery", "18V Battery" } },
    { "9V Battery",          { NULL, "4.5V Battery", NULL, "18V Battery", "999V Battery" } },
    { "18V Battery",         { "4.5V Battery", "9V Battery", NULL, "18V Battery", "999V Battery" } },
    { "999V Battery",        { "4.5V Battery", "9V Battery", "Strange Battery", "Strange Battery", "Strange Battery" } },
    { "ReVision Eyedrops",   { NULL, NULL, "ReVision Eyedrops", "Fine Eyedrops", "Very Fine Eyedrops" } },
    { "RedVision Eyedrops",  { NULL, NULL, "ReVision Eyedrops", "Fine Eyedrops", "Very Fine Eyedrops" } },
    { "Fine Eyedrops",       { NULL, NULL, "ReVision Eyedrops", "Fine Eyedrops", "Very Fine Eyedrops" } },
    { "Syringe",             { NULL, NULL, "Compact First Aid Kit", "Fine Syringe", "Very Fine Syringe" } },
    { "Fine Syringe",        { NULL, "First Aid Kit", "Blue First Aid Kit", "Very Fine Syringe", "Very Fine Syringe" } },
    { "Very Fine Syringe",   { "Electronical Components", "Electronical Components", "Electronical Components", "Infected Syringe", "Infected Syringe" } },
    { "Infected Syringe",    { NULL, NULL, NULL, "Syringe", "Blue First Aid Kit" } },
    { "Pill",                { NULL, NULL, "Pill", "Upgraded Pill", "Upgraded Pill" } },
    { "SCP-500-01",          { NULL, NULL, "Pill", "Upgraded Pill", "Upgraded Pill" } },
    { "Blank Paper",         { NULL, "Blank Paper", NULL, "Origami", "Origami" } },
    { "Origami",             { NULL, "Blank Paper", NULL, "SCP-085", "SCP-085" } },
    { "SCP-1025",            { NULL, "Blank Paper", "SCP-1025", "Fine SCP-1025", "Fine SCP-1025" } },
    { "Book",                { NULL, "Blank Paper", "SCP-1025", "Fine SCP-1025", "Fine SCP-1025" } },
};

/* The deterministic primary output for one (item, setting) - a single
 * template, or -1 if the item is destroyed. The random / multi-output
 * layers wrap this. */
static int r914Base(int tpl, int setting) {
    if (tpl < 0) return -1;
    int si = setting + 2; /* -2..2 -> 0..4 */
    if (si < 0) si = 0;
    if (si > 4) si = 4;
    const char *in = ITEM_TEMPLATES[tpl].name;
    for (unsigned r = 0; r < sizeof(REFINE914) / sizeof(REFINE914[0]); r++) {
        if (strcmp(REFINE914[r].in, in) != 0) continue;
        const char *out = REFINE914[r].out[si];
        if (!out) return -1;                  /* destroyed */
        int t = itemTplFind(out);
        return t >= 0 ? t : tpl;              /* missing template: pass through */
    }
    /* Keycards step by setting: Rough shreds, Coarse a level down, 1:1 the
     * famous Playing Card, Fine up one, Very Fine up two, past L5 a
     * Mastercard. */
    int lvl = itemKeycardLevel(tpl);
    if (lvl > 0) {
        int target = lvl;
        switch (setting) {
            case -2: return -1;
            case -1: target = lvl - 1; break;
            case 0: {
                int pc = itemTplFind("Playing Card");
                return pc >= 0 ? pc : tpl;
            }
            case 1: target = lvl + 1; break;
            default: target = lvl + 2; break;
        }
        if (target < 1) return -1;
        if (target > 5) {
            int mc = itemTplFind("Mastercard");
            if (mc >= 0) return mc;
            target = 5;
        }
        char nm[32];
        snprintf(nm, sizeof(nm), "Level %d key Card", target);
        int t = itemTplFind(nm);
        return t >= 0 ? t : tpl;
    }
    /* Default (source): Rough/Coarse ruin the item, the rest pass it
     * through untouched. */
    if (setting <= -1) return -1;
    return tpl;
}

/* Source Use914 has random "jackpot" rolls that replace the base output
 * (e.g. a gas mask very rarely refines into SCP-1499). in/setting match;
 * on a 1-in-oneIn roll the base is swapped for `item`. */
typedef struct {
    const char *in;
    int setting;
    const char *item;
    int oneIn;
} R914Rare;

static const R914Rare R914_RARE[] = {
    { "Gas Mask", 0, "Hazmat Suit", 4 },
    { "Fine Gas Mask", 0, "Hazmat Suit", 4 },
    { "Very Fine Gas Mask", 0, "Hazmat Suit", 4 },
    { "Heavy Gas Mask", 0, "Hazmat Suit", 4 },
    { "Gas Mask", 1, "SCP-1499", 50 },
    { "Fine Gas Mask", 1, "SCP-1499", 50 },
    { "Very Fine Gas Mask", 1, "SCP-1499", 50 },
    { "Heavy Gas Mask", 1, "SCP-1499", 50 },
    { "Gas Mask", 2, "SCP-1499", 100 },
    { "Fine Gas Mask", 2, "SCP-1499", 100 },
    { "Very Fine Gas Mask", 2, "SCP-1499", 100 },
    { "Heavy Gas Mask", 2, "SCP-1499", 100 },
    { "Night Vision Goggles", 1, "Fine SCRAMBLE Gear", 5 },
    { "Fine Night Vision Goggles", 1, "Fine SCRAMBLE Gear", 5 },
    { "Very Fine Night Vision Goggles", 1, "Fine SCRAMBLE Gear", 5 },
};

/* Source cases that spit out a SECOND item alongside the base. */
typedef struct {
    const char *in;
    int setting;
    const char *item;
} R914Extra;

static const R914Extra R914_EXTRA[] = {
    { "Syringe", 2, "Infected Syringe" },
    { "Fine Syringe", 2, "Infected Syringe" },
    { "9V Battery", 2, "Strange Battery" },
    { "18V Battery", 2, "Strange Battery" },
    { "S-NAV Navigator", 2, "S-NAV Navigator Ultimate" },
    { "S-NAV 300 Navigator", 2, "S-NAV Navigator Ultimate" },
    { "S-NAV 310 Navigator", 2, "S-NAV Navigator Ultimate" },
    { "E-Reader", 1, "Clipboard" },
    { "E-Reader", 2, "Clipboard" },
    { "E-Reader 20", 1, "Clipboard" },
    { "E-Reader 30", 2, "Clipboard" },
    { "Infected Syringe", 2, "Fine Syringe" },
    { "SCP-005", 0, "White Severed Hand" },
    { "SCP-005", 0, "Yellow Key" },
    { "Mastercard", -1, "Quarter" },
    { "Mastercard", -1, "Quarter" },
    { "Mastercard", -1, "Quarter" },
};

static void r914Add(int out[6], int *n, const char *name) {
    if (*n >= 6 || !name) return;
    int t = itemTplFind(name);
    if (t >= 0) out[(*n)++] = t;
}

/* Produce the SCP-914 output list for (item, setting): the primary output
 * plus any deterministic second items, or a random jackpot replacement.
 * Returns the number of items produced (0 = the item is destroyed). */
static int refine914(int tpl, int setting, int out[6]) {
    refineHostilePending = -1;
    if (tpl < 0) return 0;
    int n = 0;
    const char *in = ITEM_TEMPLATES[tpl].name;
    /* The two NPC-producing outcomes (Use914): Very Fine SCP-1499 breeds a
     * hostile SCP-1499, and a refined severed hand (Fine/Very Fine) breeds
     * an SCP-008 zombie. They emit no item. */
    if (setting >= 2 && (strcmp(in, "SCP-1499") == 0
                         || strcmp(in, "Fine SCP-1499") == 0)) {
        refineHostilePending = 1;
        return 0;
    }
    if (setting >= 1 && (strcmp(in, "White Severed Hand") == 0
                         || strcmp(in, "Black Severed Hand") == 0
                         || strcmp(in, "Yellow Severed Hand") == 0)) {
        refineHostilePending = 0;
        return 0;
    }
    /* Keycard Fine/Very Fine can jackpot a Mastercard, with odds that
     * climb with difficulty (source SelectedDifficulty\OtherFactors:
     * Easy never, up to ~2/3 on Extreme Very Fine). */
    if (setting >= 1 && itemKeycardLevel(tpl) > 0) {
        static const int MC_FINE[4] = { 0, 17, 20, 25 };
        static const int MC_VF[4]   = { 0, 33, 50, 67 };
        int f = gDiffFactor;
        if (f < 0 || f > 3) f = 1;
        int pct = (setting >= 2 ? MC_VF : MC_FINE)[f];
        if (pct > 0 && (rand() % 100) < pct) {
            r914Add(out, &n, "Mastercard");
            return n;
        }
    }
    /* Random replacements first. */
    for (unsigned r = 0; r < sizeof(R914_RARE) / sizeof(R914_RARE[0]); r++) {
        if (R914_RARE[r].setting != setting
            || strcmp(R914_RARE[r].in, in) != 0) {
            continue;
        }
        if ((rand() % R914_RARE[r].oneIn) == 0) {
            r914Add(out, &n, R914_RARE[r].item);
            return n;
        }
        break;
    }
    /* Electronics Fine/Very Fine roll one of three tech outputs. */
    if (strcmp(in, "Electronical Components") == 0 && setting >= 1) {
        static const char *E_FINE[3] = { "Radio Transceiver",
            "S-NAV 300 Navigator", "Night Vision Goggles" };
        static const char *E_VF[3] = { "Fine Radio Transceiver",
            "S-NAV 310 Navigator", "Very Fine Night Vision Goggles" };
        r914Add(out, &n, (setting >= 2 ? E_VF : E_FINE)[rand() % 3]);
        return n;
    }
    int base = r914Base(tpl, setting);
    if (base >= 0) out[n++] = base;
    /* Deterministic extra items. */
    for (unsigned e = 0; e < sizeof(R914_EXTRA) / sizeof(R914_EXTRA[0]); e++) {
        if (R914_EXTRA[e].setting == setting
            && strcmp(R914_EXTRA[e].in, in) == 0) {
            r914Add(out, &n, R914_EXTRA[e].item);
        }
    }
    return n;
}

static void spawn914(void) {
    scp914Ok = 0;
    rhActive = 0;
    refineHostilePending = -1;
    scp914State = 0;
    scp914Setting = 0;
    scp914Timer = 0.0f;
    scp914RefineTpl = -1;
    for (uint32_t r = 0; r < map.roomCount; r++) {
        const RoomPlacement *p = &map.rooms[r];
        if (strcmp(tplList.items[p->templateIndex].name, "cont1_914") != 0) {
            continue;
        }
        float in[3] = { -1128.0f, 0.5f, 640.0f }, w[3];
        localToWorld(p, in, w);
        scp914In[0] = w[0]; scp914In[1] = w[1]; scp914In[2] = w[2];
        float out[3] = { 308.0f, 0.5f, 640.0f };
        localToWorld(p, out, w);
        scp914Out[0] = w[0]; scp914Out[1] = w[1]; scp914Out[2] = w[2];
        float kn[3] = { -416.0f, 230.0f, 374.0f };
        localToWorld(p, kn, w);
        scp914Knob[0] = w[0]; scp914Knob[1] = w[1]; scp914Knob[2] = w[2];
        scp914Yaw = (float)(p->angle * 90);
        scp914Ok = 1;
        break;
    }
}

/* Interaction near the machine: the knob cycles the setting, the input
 * booth runs the currently-selected inventory item through. Returns 1 if
 * it handled the press. */
/* Shut (open=0) or reopen the two SCP_914_DOOR booth doors while the
 * machine runs (they are type 7, near the machine). */
static void set914Doors(int open) {
    for (uint32_t di = 0; di < doors.count; di++) {
        Door *d = &doors.items[di];
        if (d->type != 7) continue;
        float ddx = d->x - scp914Knob[0], ddz = d->z - scp914Knob[2];
        if (ddx * ddx + ddz * ddz < 2400.0f * 2400.0f) d->open = open;
    }
}

static int try914Interact(void) {
    if (!scp914Ok || scp914State != 0) return 0;
    float kdx = camPos[0] - scp914Knob[0], kdz = camPos[2] - scp914Knob[2];
    if (kdx * kdx + kdz * kdz < 280.0f * 280.0f) {
        scp914Setting++;
        if (scp914Setting > 2) scp914Setting = -2;
        snprintf(toastMsg, sizeof(toastMsg), "SCP-914: %s",
                 setting914Name(scp914Setting));
        toastTimer = 160;
        audioPlay(sndButton[rand() % 2], 0.8f, 0.0f);
        return 1;
    }
    float idx = camPos[0] - scp914In[0], idz = camPos[2] - scp914In[2];
    if (idx * idx + idz * idz < 420.0f * 420.0f) {
        if (invSel < 0 || invSel >= (int)inventoryCount) {
            snprintf(toastMsg, sizeof(toastMsg),
                     "SELECT AN ITEM TO REFINE");
            toastTimer = 160;
            return 1;
        }
        scp914RefineTpl = inventory[invSel];
        consumeSlot(invSel);
        scp914State = 1;
        scp914Timer = 0.0f;
        set914Doors(0); /* the booths seal for the run */
        if (snd914 >= 0) audioPlay(snd914, 0.9f, 0.0f);
        snprintf(toastMsg, sizeof(toastMsg), "SCP-914 WHIRS TO LIFE...");
        toastTimer = 180;
        return 1;
    }
    return 0;
}

static void update914(void) {
    if (!scp914Ok || scp914State != 1) return;
    scp914Timer += 1.0f;
    if (scp914Timer > 180.0f) {  /* the doors reopen after ~3 s */
        scp914State = 0;
        set914Doors(1); /* the booths open on the finished output */
        int out[6];
        int n = refine914(scp914RefineTpl, scp914Setting, out);
        scp914RefineTpl = -1;
        if (refineHostilePending >= 0) {
            spawnRefineHostile(refineHostilePending);
            snprintf(toastMsg, sizeof(toastMsg),
                     refineHostilePending == 0
                         ? "SCP-914 BIRTHED SOMETHING WRONG..."
                         : "IT CAME THROUGH WITH YOU...");
            toastTimer = 240;
            refineHostilePending = -1;
        } else if (n > 0) {
            for (int i = 0; i < n; i++) {
                worldItemAdd(out[i], scp914Out[0] + (float)(i * 60),
                             scp914Out[1] + 20.0f, scp914Out[2]);
            }
            if (n > 1) {
                snprintf(toastMsg, sizeof(toastMsg),
                         "SCP-914 OUTPUT: %s (+%d more)",
                         ITEM_TEMPLATES[out[0]].name, n - 1);
            } else {
                snprintf(toastMsg, sizeof(toastMsg), "SCP-914 OUTPUT: %s",
                         ITEM_TEMPLATES[out[0]].name);
            }
        } else {
            snprintf(toastMsg, sizeof(toastMsg), "SCP-914 DESTROYED IT");
        }
        toastTimer = 220;
    }
}

static void draw914(const float viewPos[3]) {
    if (!scp914Ok) return;
    float dx = scp914Knob[0] - viewPos[0], dz = scp914Knob[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    /* The knob, rotated to its setting (-2..2 -> +-90 deg). */
    glPushMatrix();
    glTranslatef(scp914Knob[0], scp914Knob[1], scp914Knob[2]);
    glRotatef(-scp914Yaw, 0.0f, 1.0f, 0.0f);
    glRotatef((float)scp914Setting * 45.0f, 0.0f, 0.0f, 1.0f);
    drawModelRT(&knob914RT);
    glPopMatrix();
    /* The control key just below it. */
    glPushMatrix();
    glTranslatef(scp914Knob[0], scp914Knob[1] - 40.0f, scp914Knob[2]);
    glRotatef(-scp914Yaw, 0.0f, 1.0f, 0.0f);
    drawModelRT(&key914RT);
    glPopMatrix();
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
            if (mapZoneOf(p->gridY) != k + 1) continue;
            worldItemAdd(itemTplFind(zoneCard[k]),
                         p->gridX * ROOM_SPACING, 60.0f,
                         p->gridY * ROOM_SPACING);
            break;
        }
    }

    /* Maintenance-tunnel generator room stash (source MT_GENERATOR:
     * SCP-500-01 + Night Vision Goggles on the generator dead-end cell).
     * Added last because spawnItems() resets worldItemCount and runs
     * after setupMaintenanceTunnel(). */
    if (mtGenOK) {
        worldItemAdd(itemTplFind("SCP-500-01"),
                     mtGen[0], mtGen[1] + 30.0f, mtGen[2]);
        worldItemAdd(itemTplFind("Night Vision Goggles"),
                     mtGen[0], mtGen[1] + 30.0f, mtGen[2] + 120.0f);
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

static void removeInventoryByName(const char *name) {
    for (unsigned i = 0; i < inventoryCount; i++) {
        if (strcmp(ITEM_TEMPLATES[inventory[i]].name, name) == 0) {
            consumeSlot((int)i);
            return;
        }
    }
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
        if (injuries <= 0.0f && bloodloss <= 0.0f) {
            return "YOU ARE NOT INJURED";
        }
        int fine = strstr(name, "Fine") || strstr(name, "Compact");
        injuries = 0.0f;
        bloodloss = fine ? 0.0f
                         : (bloodloss > 40.0f ? bloodloss - 40.0f : 0.0f);
        consumeSlot(slot);
        return "YOU BANDAGE YOUR WOUNDS";
    }
    if (strcmp(name, "SCP-500-01") == 0) {
        injuries = 0.0f;
        bloodloss = 0.0f;
        sanity = 100.0f;
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
        injuries = 0.0f;
        sanity = 100.0f;
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
    if (strcmp(name, "SCP-513") == 0) {
        /* Ringing the bell summons SCP-513-1 (it keeps the bell). */
        consumeSlot(slot);
        spawn513();
        return "YOU RING THE BELL. SOMETHING ANSWERS...";
    }
    if (strcmp(name, "Ballistic Vest") == 0) {
        wearVest = !wearVest;
        return wearVest ? "YOU PUT ON THE BALLISTIC VEST"
                        : "YOU TAKE OFF THE BALLISTIC VEST";
    }
    if (strcmp(name, "Hazmat Suit") == 0) {
        wearHazmat = !wearHazmat;
        return wearHazmat ? "YOU PUT ON THE HAZMAT SUIT"
                          : "YOU TAKE OFF THE HAZMAT SUIT";
    }
    if (strcmp(name, "Radio Transceiver") == 0) {
        radioSetChannel(radioChannel >= 3 ? -1 : radioChannel + 1);
        if (radioChannel < 0) return "YOU SWITCH OFF THE RADIO";
        snprintf(msg, sizeof(msg), "RADIO: CHANNEL %d", radioChannel + 1);
        return msg;
    }
    if (strcmp(name, "Pizza Slice") == 0) {
        bloodloss = bloodloss > 10.0f ? bloodloss - 10.0f : 0.0f;
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
        using714 = !using714;
        return using714 ? "YOU SLIP ON THE JADE RING. IT WARDS OFF SICKNESS"
                        : "YOU TAKE OFF THE JADE RING";
    }
    if (strcmp(name, "SCP-1499") == 0) {
        if (inMask) {
            leaveMaskDimension();
            return "YOU TEAR THE MASK OFF. THE FACILITY RETURNS";
        }
        if (maskRoomIdx < 0 || !skin1499) return "AN OLD SOVIET GAS MASK";
        enterMaskDimension();
        return "YOU PULL ON THE MASK. THE WORLD DISSOLVES...";
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
/* Keypad code entry for keypad doors. */
static int keypadOpen;
static int keypadDoor = -1;   /* index into doors.items */
static int keypadDigits[4];
static int keypadPos;
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
    gDiffFactor = DIFFICULTIES[d].factors;
}

/* ---- new game form state ---- */

static char newName[16];
static char newSeedStr[16];
static int introEnabled = 1;
static int startupVideosEnabled = 1; /* opt\PlayStartup: boot splash clips */
static int pendingIntroVideo;        /* play startup_Intro on 1st game frame */
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
    fprintf(f, "music=%f\nsfx=%f\nsens=%f\ninverty=%d\nintro=%d\nstartup=%d\n",
            optMusicVol, optSfxVol, optLookSens, optInvertY, introEnabled,
            startupVideosEnabled);
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
        } else if (strncmp(line, "startup=", 8) == 0) {
            startupVideosEnabled = (int)strtol(line + 8, NULL, 10);
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

/* Per-zone music (Loading_Core Music[0..2]): the streamed track
 * follows the player's current zone. */
static const char *ZONE_MUSIC[4] = {
    NULL,
    DATA_ROOT "/SFX/Music/LightContainmentZone.ogg",
    DATA_ROOT "/SFX/Music/HeavyContainmentZone.ogg",
    DATA_ROOT "/SFX/Music/EntranceZone.ogg",
};

/* Per-SCP / per-room chamber music (Main_Core's ShouldPlay overrides).
 * The base track is the room's zone music; entering a signature chamber
 * or a monster starting its chase swaps in that encounter's track, and
 * chase music outranks chamber music the way the source's ShouldPlay
 * assignments do (a later, higher-priority event wins the frame).
 * Returns a static path literal so callers can compare by pointer. */
static const char *desiredMusicPath(void) {
    if (!worldReady) return ZONE_MUSIC[1];
    /* Chase music - highest priority, matches Music[10/12/19] and the
     * 096 angered/chase streams. */
    if (npc096Active && npc096State == S096_CHASE)
        return DATA_ROOT "/SFX/Music/096Chase.ogg";
    if (npc096Active && npc096State == S096_TRIGGERED)
        return DATA_ROOT "/SFX/Music/096Angered.ogg";
    if (npc106Active &&
        (npc106State == N106_CHASING || npc106State == N106_GRABBING))
        return DATA_ROOT "/SFX/Music/106Chase.ogg";
    if (npc049Active &&
        (npc049State == S049_PURSUE || npc049State == S049_KILL))
        return DATA_ROOT "/SFX/Music/049Chase.ogg";
    /* Signature chamber music while the player is in the room. */
    const char *r = roomNameAt(camPos);
    if (!strcmp(r, "cont1_079")) return DATA_ROOT "/SFX/Music/079Chamber.ogg";
    if (!strcmp(r, "cont1_914")) return DATA_ROOT "/SFX/Music/914Chamber.ogg";
    if (!strcmp(r, "cont2_012")) return DATA_ROOT "/SFX/Music/012Chamber.ogg";
    if (!strcmp(r, "cont1_035")) return DATA_ROOT "/SFX/Music/035Chamber.ogg";
    if (!strcmp(r, "cont2_049")) return DATA_ROOT "/SFX/Music/049Chamber.ogg";
    if (!strcmp(r, "cont1_205")) return DATA_ROOT "/SFX/Music/205Chamber.ogg";
    if (!strcmp(r, "cont1_106")) return DATA_ROOT "/SFX/Music/106Chamber.ogg";
    if (!strcmp(r, "room3_storage"))
        return DATA_ROOT "/SFX/Music/Room3_Storage.ogg";
    if (!strcmp(r, "cont2_860_1"))
        return DATA_ROOT "/SFX/Music/860_1_Blue.ogg";
    /* Default: the room's zone track. */
    return ZONE_MUSIC[zoneAt(camPos)];
}

static const char *currentMusicPath = NULL;

static void gameMusicStart(void) {
    if (radioChannel >= 0) return; /* the radio owns the music path */
    const char *path = desiredMusicPath();
    currentMusicPath = path;
    currentMusicZone = worldReady ? zoneAt(camPos) : 1;
    audioStreamMusic(path, 0.55f, 1);
}

/* Bleeding, healing and sanity, run each gameplay frame
 * (Main_Core.bb's injury model). */
static void updatePlayerCondition(void) {
    if (introPhase >= 0) { health = 100.0f - bloodloss; return; }
    if (deathTimer == 0) {
        /* Injuries above 1 bleed into bloodloss; the vest already cut
         * the incoming injury. */
        if (injuries > 1.0f) {
            float r = (injuries < 3.5f ? injuries : 3.5f) / 300.0f;
            bloodloss += r;
            if (bloodloss > 100.0f) bloodloss = 100.0f;
        }
        /* Wounds clot slowly on their own. */
        injuries -= 0.0025f;
        if (injuries < 0.0f) injuries = 0.0f;
        /* Bleeding leaves droplets underfoot as the player moves. */
        if (bloodloss > 4.0f && walkMode && rand() % 40 == 0) {
            float fy;
            float o[3] = { camPos[0], camPos[1], camPos[2] };
            float gy = rayDownWorld(o, 3000.0f, &fy) ? fy
                                                     : camPos[1] - EYE_HEIGHT;
            decalSpawn(DECAL_BLOOD_DROP_1 + rand() % 2, camPos[0], gy + 0.5f,
                       camPos[2], 90.0f, (float)(rand() % 360), 0.0f,
                       0.05f + 0.03f * (bloodloss / 100.0f), 0.9f, 1);
        }
        if (bloodloss >= 100.0f) {
            snprintf(deathCause, sizeof(deathCause), "BLOOD LOSS");
            deathTimer = 180;
        }

        /* Sanity drains in the dark, in the pocket dimension and near
         * SCP-106; it recovers in the light. */
        float drain = 0.0f;
        if (inPocket) {
            drain = 0.14f;
        } else if (introDark) {
            drain = 0.05f;
        } else if (npc106Active && npc106State != N106_DORMANT) {
            float dx = camPos[0] - npc106Pos[0];
            float dz = camPos[2] - npc106Pos[2];
            if (dx * dx + dz * dz < 1400.0f * 1400.0f) drain = 0.09f;
        }
        sanity += drain > 0.0f ? -drain : 0.04f;
        if (sanity < 0.0f) sanity = 0.0f;
        if (sanity > 100.0f) sanity = 100.0f;
        if (sanity < 30.0f) {
            float sv = (30.0f - sanity) / 30.0f * 1.8f;
            if (camShake < sv) camShake = sv;
        }
    }
    health = 100.0f - bloodloss;
}

/* Called each gameplay frame: swap the track when the zone changes. */
static void updateZoneMusic(void) {
    if (radioChannel >= 0 || !worldReady) return;
    const char *path = desiredMusicPath();
    if (path != currentMusicPath) {
        currentMusicPath = path;
        currentMusicZone = zoneAt(camPos);
        audioStreamMusic(path, 0.55f, 1);
    }
}

/* Room ambience emitters (RMESH soundemitter entities): loop the
 * ambience of the nearest in-range emitter in the active rooms on the
 * dedicated ambience channel. Sounds load lazily by path. */
static int ambienceSound(int id) {
    static const char *AMB[12] = {
        "/Ambient/Room ambience/rumble.ogg",
        "/Ambient/Room ambience/lowdrone.ogg",
        "/Ambient/Room ambience/pulsing.ogg",
        "/Ambient/Room ambience/ventilation.ogg",
        "/Ambient/Room ambience/drip.ogg",
        "/Alarm/Alarm0.ogg",
        "/Ambient/Room ambience/895.ogg",
        "/Ambient/Room ambience/fuelpump.ogg",
        "/Ambient/Room ambience/Fan.ogg",
        "/Ambient/Room ambience/servers1.ogg",
        "/Ambient/Room ambience/173chamber.ogg",
        "/Ambient/Room ambience/372Cell.ogg",
    };
    if (id < 1 || id > 12) return -1;
    char path[256];
    snprintf(path, sizeof(path), SFX_DIR "%s", AMB[id - 1]);
    return audioLoad(path);
}

static void updateRoomAmbience(void) {
    int bestId = 0;
    float bestVol = 0.0f;
    for (int i = 0; i < activeCount; i++) {
        const RoomPlacement *rp = activeRooms[i];
        const TemplateRT *rt = &tplRT[rp->templateIndex];
        for (int e = 0; e < rt->emitterCount; e++) {
            const RoomEmitter *em = &rt->emitters[e];
            float local[3] = { em->x, em->y, em->z }, w[3];
            localToWorld(rp, local, w);
            float dx = w[0] - camPos[0], dz = w[2] - camPos[2];
            float d = sqrtf(dx * dx + dz * dz);
            if (d >= em->range) continue;
            float vol = (1.0f - d / em->range) * 0.7f;
            if (vol > bestVol) {
                bestVol = vol;
                bestId = em->id;
            }
        }
    }
    if (bestId != currentAmbienceId) {
        currentAmbienceId = bestId;
        audioLoopAmbience(bestId ? ambienceSound(bestId) : -1, bestVol);
    }
}

/* Ambient one-shots (source AmbientSFX): every so often a random distant
 * sound from the current zone's set drifts in from the dark - a scream,
 * a groan, dripping - positioned off to one side of the player. Loaded on
 * demand (audioLoad caches by path). */
static float ambSfxTimer = 300.0f;

static void updateAmbientSfx(void) {
    if (!worldReady || deathTimer > 0 || introPhase >= 0 || inPocket
        || inMask) {
        return;
    }
    if (ambSfxTimer > 0.0f) { ambSfxTimer -= 1.0f; return; }
    const char *folder;
    int count;
    if (strcmp(roomNameAt(camPos), "cont2_860_1") == 0) {
        folder = "Forest"; count = 10;      /* the SCP-860 forest */
    } else {
        int z = zoneAt(camPos);             /* 1 LCZ, 2 HCZ, 3 EZ */
        if (z <= 1) { folder = "Zone1"; count = 11; }
        else if (z == 2) { folder = "Zone2"; count = 11; }
        else { folder = "Zone3"; count = 12; }
    }
    char path[256];
    snprintf(path, sizeof(path), SFX_DIR "/Ambient/%s/Ambient%d.ogg",
             folder, rand() % count);
    int snd = audioLoad(path);
    if (snd >= 0) {
        float ang = (float)(rand() % 628) * 0.01f;
        float dist = 500.0f + (float)(rand() % 1500);
        float pos[3] = { camPos[0] + sinf(ang) * dist, camPos[1],
                         camPos[2] - cosf(ang) * dist };
        audioPlay3D(snd, pos, camPos, camYaw, dist * 2.2f);
    }
    ambSfxTimer = 900.0f + (float)(rand() % 1800); /* ~15-45 s at 60 fps */
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
    fprintf(f, "condx=%f %f %f\n", injuries, bloodloss, sanity);
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
    float condInjuries = 0.0f, condBloodloss = 0.0f, condSanity = 100.0f;
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
        } else if (strncmp(line, "condx=", 6) == 0) {
            p = line + 6;
            condInjuries = strtof(p, &p);
            condBloodloss = strtof(p, &p);
            condSanity = strtof(p, &p);
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
    injuries = condInjuries;
    bloodloss = condBloodloss;
    sanity = condSanity;
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
    prewarmSpawnArea();
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

/* Draw a model with every batch's diffuse swapped for texOverride
 * (the locked button's red panel), restoring the originals after. */
static void drawModelRTTinted(const ModelRT *rt, GLuint texOverride) {
    if (!rt->ok) return;
    GLuint saved[16];
    uint32_t n = rt->scene->batchCount;
    if (n > 16) n = 16;
    for (uint32_t i = 0; i < n; i++) {
        saved[i] = rt->gl[i].diffuse;
        rt->gl[i].diffuse = texOverride;
    }
    glPushMatrix();
    glScalef(rt->scale[0], rt->scale[1], rt->scale[2]);
    drawBatchSet(rt->scene, rt->gl, 0);
    glPopMatrix();
    for (uint32_t i = 0; i < n; i++) rt->gl[i].diffuse = saved[i];
}

/* A door frame/panel, swapped to a corroded texture when SCP-106 has
 * rotted it. */
static void drawDoorPart(const ModelRT *rt, GLuint corr) {
    if (corr) drawModelRTTinted(rt, corr);
    else drawModelRT(rt);
}

static void drawDoors(const float viewPos[3]) {
    for (uint32_t i = 0; i < doors.count; i++) {
        const Door *d = &doors.items[i];
        float dx = d->x - viewPos[0], dz = d->z - viewPos[2];
        if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;

        GLuint corr = d->corroded
                    ? (d->type == 2 ? doorCorrHeavyTex : doorCorrTex) : 0;

        glPushMatrix();
        glTranslatef(d->x, d->y, d->z);
        glRotatef(-(float)d->angle, 0.0f, 1.0f, 0.0f);

        /* Frame and panels by door type (Loading_Core.bb models). */
        const ModelRT *frame = &doorFrameRT;
        const ModelRT *p1 = &doorPanelRT, *p2 = &doorPanelRT;
        int hinged = 0, single = 0, bigLeaves = 0;
        switch (d->type) {
            case 1: p1 = p2 = &elevatorRT; break;
            case 2: p1 = &heavy1RT; p2 = &heavy2RT; break;
            case 3: frame = &bigFrameRT; p1 = &big1RT; p2 = &big2RT;
                    bigLeaves = 1; break;
            case 4: frame = &officeFrameRT; p1 = &officeRT; hinged = 1; break;
            case 5: frame = &woodenFrameRT; p1 = &woodenRT; hinged = 1; break;
            case 6: p1 = &oneSidedRT; single = 1; break;
            case 7: p1 = &door914RT; single = 1; break;
            default: break;
        }
        drawDoorPart(frame, corr);

        float slide = doorSlide(d);
        if (hinged) {
            /* Office/wooden doors swing on a hinge instead of sliding. */
            glPushMatrix();
            glTranslatef(-92.0f, 0.0f, 0.0f);
            glRotatef(-d->openState * 0.5f, 0.0f, 1.0f, 0.0f);
            glTranslatef(92.0f, 0.0f, 0.0f);
            drawDoorPart(p1, corr);
            glPopMatrix();
        } else {
            glPushMatrix();
            glTranslatef(slide, 0.0f, 0.0f);
            drawDoorPart(p1, corr);
            glPopMatrix();

            if (!single) {
                glPushMatrix();
                if (bigLeaves) {
                    /* The big containment gate is two distinct halves
                     * (contdoorleft/right) that meet at the centre and
                     * slide apart along the same axis - the source does
                     * NOT mirror OBJ2 (Angle + (Not BIG_DOOR)*180) and
                     * moves it -SinValue. Flipping it 180 would pile both
                     * halves on one side and leave the other half open. */
                    glTranslatef(-slide, 0.0f, 0.0f);
                } else {
                    glRotatef(180.0f, 0.0f, 1.0f, 0.0f);
                    glTranslatef(slide, 0.0f, 0.0f);
                }
                drawDoorPart(p2, corr);
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

/* Shortest signed difference a-b, in (-180, 180]. */
static float yawDelta(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

/* Swivel/track the security-camera heads near the player (UpdateSecurity
 * Cams): follow-cams point at the player, sweep-cams oscillate within
 * +-Turn (0.2 deg/frame, reversing at +-Turn*1.3), the rest sit still. */
static void camerasUpdate(void) {
    for (int i = 0; i < worldCameraCount; i++) {
        WorldCamera *c = &worldCameras[i];
        float dx = camPos[0] - c->x, dz = camPos[2] - c->z;
        if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;
        if (c->follow) {
            float dy = camPos[1] - (c->y - 21.0f);
            float dist = sqrtf(dx * dx + dz * dz);
            float tYaw = atan2f(dx, dz) * (180.0f / 3.14159265f);
            float tPitch = -atan2f(dy, dist) * (180.0f / 3.14159265f);
            c->headYaw += yawDelta(tYaw, c->headYaw) * 0.12f;
            c->headPitch += (tPitch - c->headPitch) * 0.12f;
        } else if (c->turn > 0.0f) {
            if (!c->dir) {
                c->currAngle += 0.2f;
                if (c->currAngle > c->turn * 1.3f) c->dir = 1;
            } else {
                c->currAngle -= 0.2f;
                if (c->currAngle < -c->turn * 1.3f) c->dir = 0;
            }
            float cl = c->currAngle;
            if (cl > c->turn) cl = c->turn;
            if (cl < -c->turn) cl = -c->turn;
            c->headYaw = c->yawBase + cl;
            c->headPitch = c->pitch;
        } else {
            c->headYaw = c->yawBase;
            c->headPitch = c->pitch;
        }
    }
}

static void camerasDraw(const float viewPos[3]) {
    /* Red light blinks: default texture for the first ~800 of every
     * ~1350 ms (source MilliSec Mod 1350), red for the rest. At ~60 fps
     * that is 48 of every 81 frames. */
    int red = (int)(gTick % 81) >= 48;
    for (int i = 0; i < worldCameraCount; i++) {
        const WorldCamera *c = &worldCameras[i];
        float dx = c->x - viewPos[0], dz = c->z - viewPos[2];
        if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;

        glPushMatrix();
        glTranslatef(c->x, c->y, c->z);
        glRotatef(-c->roomYaw, 0.0f, 1.0f, 0.0f);
        drawModelRT(&camBaseRT);
        glPopMatrix();

        glPushMatrix();
        glTranslatef(c->x, c->y - 21.0f, c->z); /* head sits below base */
        glRotatef(-c->headYaw, 0.0f, 1.0f, 0.0f);
        glRotatef(c->headPitch, 1.0f, 0.0f, 0.0f);
        if (red) drawModelRTTinted(&camHeadRT, camHeadRedTex);
        else drawModelRT(&camHeadRT);
        glPopMatrix();
    }
}

/* MTF camera-detection subplot (UpdateCameraCheck): now and then the
 * MTF run a camera sweep - an announcement, then a window in which any
 * security camera that sees the player flags it, and a second
 * announcement (found / not found) when the window closes. The source
 * kicks this off when an MTF unit loses its target; the port has no MTF,
 * so it fires on a rare timer for the same tension, and "detected" is a
 * scare cue (toast + horror sting) rather than a dispatch, since there
 * is no squad to send. */
#define CAM_CHECK_DURATION 5400.0f   /* ~90 s at 60 fps (source 70*90) */

static void cameraCheckUpdate(void) {
    if (!worldReady) return;
    /* SCP-079 begins broadcasting SCP-895's feed once you clear the LCZ. */
    if (zoneAt(camPos) >= 2) leftFirstZone = 1;
    if (camCheckTimer <= 0.0f) {
        if ((rand() % 10800) == 0) {  /* ~1 sweep per 3 min */
            camCheckTimer = 1.0f;
            camCheckSpotted = 0;
            if (sndCamCheck >= 0) audioPlay(sndCamCheck, 0.9f, 0.0f);
        }
        return;
    }
    if (!camCheckSpotted) {
        for (int i = 0; i < worldCameraCount; i++) {
            const WorldCamera *c = &worldCameras[i];
            float dx = camPos[0] - c->x, dz = camPos[2] - c->z;
            if (dx * dx + dz * dz > 2200.0f * 2200.0f) continue;
            /* Inside the head's ~60 deg cone (source DeltaYaw < 60) and
             * not occluded. */
            float toPlayer = atan2f(dx, dz) * (180.0f / 3.14159265f);
            if (fabsf(yawDelta(toPlayer, c->headYaw)) > 60.0f) continue;
            float eye[3] = { c->x, c->y - 21.0f, c->z };
            if (!lineOfSight(eye, camPos)) continue;
            camCheckSpotted = 1;
            break;
        }
    }
    camCheckTimer += 1.0f;
    if (camCheckTimer >= CAM_CHECK_DURATION) {
        camCheckTimer = 0.0f;
        if (camCheckSpotted) {
            if (sndCamFound[0] >= 0) audioPlay(sndCamFound[rand() % 2],
                                               0.95f, 0.0f);
            if (sndHorror11 >= 0) audioPlay(sndHorror11, 0.6f, 0.0f);
            snprintf(toastMsg, sizeof(toastMsg),
                     "A SECURITY CAMERA CAUGHT YOU");
            toastTimer = 220;
        } else if (sndCamNoFound >= 0) {
            audioPlay(sndCamNoFound, 0.9f, 0.0f);
        }
    }
}

/* Render the nearest Screen camera's view into monFeedTex. Draws the
 * scene into a MON_FEED_SIZE corner of the backbuffer (pointed from the
 * camera toward the player, a security-cam-watching-the-occupant view),
 * then copies it out with glCopyTexImage2D - the same no-FBO trick as the
 * blur. Refreshed every few frames (source RenderInterval). Call before
 * the main scene clear. */
static void renderCameraFeed(void) {
    monFeedActive = 0;
    if (!worldReady || activeCount == 0 || !monitorRT.ok) {
        monFeedCam = -1;
        return;
    }
    int best = -1;
    float bestD2 = 2600.0f * 2600.0f;
    for (int i = 0; i < worldCameraCount; i++) {
        if (!worldCameras[i].screen) continue;
        float dx = worldCameras[i].mx - camPos[0];
        float dz = worldCameras[i].mz - camPos[2];
        float d2 = dx * dx + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    monFeedCam = best;
    if (best < 0) return;
    monFeedTick += 1.0f;
    if (monFeedTex && monFeedTick < 6.0f) { monFeedActive = 1; return; }
    monFeedTick = 0.0f;

    const WorldCamera *c = &worldCameras[best];
    float eyeY = c->y - 21.0f;
    /* Aim at the player, except the cont1_205 observation camera aims at
     * the shadow demon so it shows on the monitor. */
    float tx = camPos[0], ty = camPos[1], tz = camPos[2];
    if (npc205Ok) {
        float cx = c->x - npc205Pos[0], cz = c->z - npc205Pos[2];
        if (cx * cx + cz * cz < 1600.0f * 1600.0f) {
            tx = npc205Pos[0]; ty = npc205Pos[1] + 90.0f; tz = npc205Pos[2];
        }
    }
    float dx = tx - c->x, dz = tz - c->z, dy = ty - eyeY;
    float horiz = sqrtf(dx * dx + dz * dz);
    if (horiz < 1.0f) horiz = 1.0f;
    float yaw = atan2f(dx, -dz) * (180.0f / 3.14159265f);
    float pitch = atan2f(-dy, horiz) * (180.0f / 3.14159265f);

    glViewport(0, 0, MON_FEED_SIZE, MON_FEED_SIZE);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, MON_FEED_SIZE, MON_FEED_SIZE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    float zn = 4.0f, zf = VIEW_RANGE * 3.0f;
    float t = zn * tanf(35.0f * 3.14159265f / 180.0f);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-t, t, -t, t, zn, zf);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glRotatef(pitch, 1, 0, 0);
    glRotatef(yaw, 0, 1, 0);
    glTranslatef(-c->x, -eyeY, -c->z);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);
    glColor4f(1, 1, 1, 1);
    for (int i = 0; i < activeCount; i++) drawRoomBatches(activeRooms[i], 0);
    /* SCP-205's shadow demon is seen chiefly here, on the monitor. */
    float feedEye[3] = { c->x, eyeY, c->z };
    draw205(feedEye);

    if (!monFeedTex) {
        glGenTextures(1, &monFeedTex);
        glBindTexture(GL_TEXTURE_2D, monFeedTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, monFeedTex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, MON_FEED_SIZE,
                     MON_FEED_SIZE, 0);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    monFeedActive = 1;
}

/* Draw the monitor props; the active one shows the live feed, the rest a
 * dark screen. */
static void drawMonitors(const float viewPos[3]) {
    for (int i = 0; i < worldCameraCount; i++) {
        const WorldCamera *c = &worldCameras[i];
        if (!c->screen) continue;
        float dx = c->mx - viewPos[0], dz = c->mz - viewPos[2];
        if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;

        glPushMatrix();
        glTranslatef(c->mx, c->my, c->mz);
        glRotatef(-c->myaw, 0.0f, 1.0f, 0.0f);
        glRotatef(c->mpitch, 1.0f, 0.0f, 0.0f);
        drawModelRT(&monitorRT);
        glPopMatrix();

        int lit = (i == monFeedCam && monFeedActive && monFeedTex);
        int broadcast = leftFirstZone && scp895Ok;
        glPushMatrix();
        glTranslatef(c->mx, c->my, c->mz);
        glRotatef(-c->myaw, 0.0f, 1.0f, 0.0f);
        glTranslatef(0.0f, 0.0f, 34.0f); /* onto the screen face */
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glDisableClientState(GL_COLOR_ARRAY);
        GLfloat sv[18] = {
            -62.0f,  46.0f, 0.0f,  62.0f,  46.0f, 0.0f,  62.0f, -46.0f, 0.0f,
            -62.0f,  46.0f, 0.0f,  62.0f, -46.0f, 0.0f, -62.0f, -46.0f, 0.0f,
        };
        if (lit) {
            /* The capture is bottom-up; flip V. Under the SCP-895/079
             * broadcast the feed jitters and bleeds red. */
            float j = broadcast ? ((rand() % 20) - 10) / 100.0f : 0.0f;
            GLfloat uv[12] = { 0, 1 + j, 1, 1 + j, 1, j,
                               0, 1 + j, 1, j, 0, j };
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, monFeedTex);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, uv);
            if (broadcast) glColor4f(1.0f, 0.35f, 0.3f, 1.0f);
            else glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        } else if (broadcast) {
            /* No live feed here, but the broadcast still scrambles it:
             * a flickering red static. */
            glDisable(GL_TEXTURE_2D);
            float f = 0.15f + (rand() % 40) / 100.0f;
            glColor4f(f, 0.02f, 0.02f, 1.0f);
        } else {
            glDisable(GL_TEXTURE_2D);
            glColor4f(0.02f, 0.03f, 0.05f, 1.0f);
        }
        glVertexPointer(3, GL_FLOAT, 0, sv);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glColor4f(1, 1, 1, 1);
        glEnableClientState(GL_COLOR_ARRAY);
        glPopMatrix();
    }
}

/* ---- SCP-079 (cont1_079): the sentient computer ---- */

static void spawn079(void) {
    scp079Ok = 0;
    scp079State = 0;
    scp079Timer = 0.0f;
    scp079OvFrame = 0;
    scp079FlickT = 0.0f;
    scp079DocDone = 0;
    for (uint32_t r = 0; r < map.roomCount; r++) {
        const RoomPlacement *p = &map.rooms[r];
        if (strcmp(tplList.items[p->templateIndex].name, "cont1_079") != 0) {
            continue;
        }
        /* Computer position, source-local (PositionEntity 166,-10800,1606
         * * RoomScale) with the room's quarter-turn; RotateEntity yaw -90. */
        float local[3] = { 166.0f, -10800.0f, 1606.0f }, w[3];
        localToWorld(p, local, w);
        scp079Pos[0] = w[0]; scp079Pos[1] = w[1]; scp079Pos[2] = w[2];
        scp079Yaw = (float)(p->angle * 90) - 90.0f;
        scp079Ok = 1;
        break;
    }
}

/* e_cont1_079: reaching the terminal in the lower chamber wakes SCP-079 -
 * it speaks (Speech.ogg), its screen flickers overlay frames, and it
 * drops the SCP-079 document; approaching again once it has finished
 * makes it refuse (Refuse.ogg). The Gate B broadcast tie-in is out of
 * scope (no gate endings ported). */
static void update079(void) {
    if (!scp079Ok) return;
    float dx = camPos[0] - scp079Pos[0];
    float dy = camPos[1] - scp079Pos[1];
    float dz = camPos[2] - scp079Pos[2];
    int atTerminal = (dx * dx + dy * dy + dz * dz) < 768.0f * 768.0f;

    if (scp079State == 1) {
        scp079Timer += 1.0f;
        scp079FlickT += 1.0f;
        if (scp079FlickT >= 8.0f) {
            scp079FlickT = 0.0f;
            scp079OvFrame = 1 + (rand() % 6); /* overlays 2..7 while talking */
        }
        if (scp079Timer > 1800.0f) {          /* ~30 s, then settle */
            scp079State = 2;
            scp079Timer = 0.0f;
            scp079OvFrame = 0;                /* overlay 1: idle screen */
        }
    } else if (scp079State == 2) {
        scp079Timer += 1.0f;
    }

    if (atTerminal && scp079State == 0) {
        if (snd079Speech >= 0) audioPlay(snd079Speech, 1.0f, 0.0f);
        scp079State = 1;
        scp079Timer = 0.0f;
        snprintf(toastMsg, sizeof(toastMsg), "SCP-079 STIRS...");
        toastTimer = 200;
        if (!scp079DocDone) {
            worldItemAdd(itemTplFind("Document SCP-079"), scp079Pos[0],
                         scp079Pos[1] + 20.0f, scp079Pos[2] + 120.0f);
            scp079DocDone = 1;
        }
    } else if (atTerminal && scp079State == 2 && scp079Timer > 300.0f) {
        if (snd079Refuse >= 0) audioPlay(snd079Refuse, 1.0f, 0.0f);
        scp079State = 1;
        scp079Timer = 0.0f;
    }
}

static void draw079(const float viewPos[3]) {
    if (!scp079Ok) return;
    float dx = scp079Pos[0] - viewPos[0], dz = scp079Pos[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;

    glPushMatrix();
    glTranslatef(scp079Pos[0], scp079Pos[1], scp079Pos[2]);
    glRotatef(-scp079Yaw, 0.0f, 1.0f, 0.0f);
    drawModelRT(&scp079RT);
    glPopMatrix();

    /* The screen: a small glowing quad on the console's front face,
     * showing the current overlay frame (flickers while it speaks). */
    int f = scp079OvFrame;
    if (f < 0 || f > 6) f = 0;
    GLuint ov = scp079Ov[f];
    if (!ov) return;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableClientState(GL_COLOR_ARRAY);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, ov);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);   /* additive glow */
    glDepthMask(GL_FALSE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    static const GLfloat uv[12] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
    GLfloat v[18] = {
        -45.0f,  36.0f, 0.0f,   45.0f,  36.0f, 0.0f,   45.0f, -36.0f, 0.0f,
        -45.0f,  36.0f, 0.0f,   45.0f, -36.0f, 0.0f,  -45.0f, -36.0f, 0.0f,
    };
    glPushMatrix();
    glTranslatef(scp079Pos[0], scp079Pos[1] + 44.0f, scp079Pos[2]);
    glRotatef(-scp079Yaw, 0.0f, 1.0f, 0.0f);
    glTranslatef(0.0f, 0.0f, 26.0f);     /* onto the console's face */
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glVertexPointer(3, GL_FLOAT, 0, v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glPopMatrix();
    glColor4f(1, 1, 1, 1);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnableClientState(GL_COLOR_ARRAY);
}

/* ---- SCP-895 (cont1_895): the camera coffin ---- */

static void spawn895(void) {
    scp895Ok = 0;
    scp895State = 0;
    scp895Timer = 0.0f;
    scp895IdleT = 0.0f;
    for (uint32_t r = 0; r < map.roomCount; r++) {
        const RoomPlacement *p = &map.rooms[r];
        if (strcmp(tplList.items[p->templateIndex].name, "cont1_895") != 0) {
            continue;
        }
        float local[3] = { 0.0f, -1532.0f, 2508.0f }, w[3];
        localToWorld(p, local, w);
        scp895Coffin[0] = w[0]; scp895Coffin[1] = w[1]; scp895Coffin[2] = w[2];
        scp895GuardYaw = (float)(p->angle * 90) + 90.0f;
        scp895Ok = 1;
        break;
    }
}

/* e_cont1_895: the coffin fills its chamber with dread (sanity bleeds,
 * the view blurs, the trapped guard murmurs), and a close approach makes
 * the slumped guard corpse lurch into view with a scream. The source's
 * SCP-106 lure and the SCP-895 camera-feed amplification (CoffinEffect)
 * need subsystems the port lacks and are left out. */
static void update895(void) {
    if (!scp895Ok) return;
    float dx = camPos[0] - scp895Coffin[0];
    float dy = camPos[1] - scp895Coffin[1];
    float dz = camPos[2] - scp895Coffin[2];
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > 2200.0f * 2200.0f) return;   /* only inside the chamber */
    float dist = sqrtf(d2);
    float prox = 1.0f - dist / 2200.0f;
    sanity -= 0.04f * prox;
    if (sanity < -1000.0f) sanity = -1000.0f;
    if (blurAmount < 0.28f * prox) blurAmount = 0.28f * prox;

    if (scp895State == 0) {
        scp895IdleT += 1.0f;
        if (scp895IdleT > 280.0f) {
            scp895IdleT = 0.0f;
            audioPlay3D(snd895Idle[rand() % 3], scp895Coffin, camPos, camYaw,
                        1800.0f);
        }
        if (dist < 520.0f) {
            scp895State = 1;
            scp895Timer = 0.0f;
            audioPlay3D(snd895Scream[rand() % 3], scp895Coffin, camPos, camYaw,
                        3000.0f);
            if (sndHorror11 >= 0) audioPlay(sndHorror11, 0.7f, 0.0f);
            if (camShake < 8.0f) camShake = 8.0f;
            blurAmount = 0.7f;
            snprintf(toastMsg, sizeof(toastMsg), "SCP-895");
            toastTimer = 200;
        }
    } else {
        scp895Timer += 1.0f;
        if (blurAmount < 0.45f) blurAmount = 0.45f;
        if (dist > 900.0f) scp895State = 0; /* re-arms if you back off */
    }
}

static void draw895(const float viewPos[3]) {
    if (!scp895Ok || scp895State == 0) return;
    float dx = scp895Coffin[0] - viewPos[0], dz = scp895Coffin[2] - viewPos[2];
    if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) return;
    glPushMatrix();
    glTranslatef(scp895Coffin[0], scp895Coffin[1], scp895Coffin[2]);
    glRotatef(-scp895GuardYaw, 0.0f, 1.0f, 0.0f);
    glRotatef(80.0f, 1.0f, 0.0f, 0.0f); /* slumped over */
    drawModelRT(&introGuardRT);
    glPopMatrix();
}

/* ---- SCP-012 (cont2_012): "A Bad Composition" ---- */

static void spawn012(void) {
    scp012Ok = 0;
    scp012Comp = 0.0f;
    for (uint32_t r = 0; r < map.roomCount; r++) {
        const RoomPlacement *p = &map.rooms[r];
        if (strcmp(tplList.items[p->templateIndex].name, "cont2_012") != 0) {
            continue;
        }
        /* The score's display box (Objects[0]) - source local
         * -360,-130,456. */
        float local[3] = { -360.0f, -130.0f, 456.0f }, w[3];
        localToWorld(p, local, w);
        scp012Pos[0] = w[0]; scp012Pos[1] = w[1]; scp012Pos[2] = w[2];
        scp012Ok = 1;
        break;
    }
}

/* Main_Core scribe_event: standing at the score (DistanceSquared < 0.36
 * world = 0.6 world -> 153 raw) pins the eyes open and paints the bloody
 * compulsion overlay, with a horror sting. Sanity bleeds while it holds
 * you. The overlay itself is drawn in the HUD pass from scp012Comp. */
static void update012(void) {
    if (!scp012Ok) return;
    float dx = camPos[0] - scp012Pos[0];
    float dz = camPos[2] - scp012Pos[2];
    int atScore = (dx * dx + dz * dz) < 153.0f * 153.0f;
    if (atScore) {
        if (scp012Comp < 0.02f && sndHorror11 >= 0) {
            audioPlay(sndHorror11, 0.7f, 0.0f); /* HorrorSFX[11] */
        }
        blinkFrames = 0;               /* it forces the eyes open */
        blinkTimer = 100.0f;
        scp012Comp += 0.05f;
        if (scp012Comp > 1.0f) scp012Comp = 1.0f;
        sanity -= 0.15f;
        if (sanity < -1000.0f) sanity = -1000.0f;
    } else if (scp012Comp > 0.0f) {
        scp012Comp -= 0.04f;
        if (scp012Comp < 0.0f) scp012Comp = 0.0f;
    }
}

static const ModelRT *buttonModelFor(int btnId) {
    switch (btnId) {
        case 1: return &buttonKeycardRT;
        case 2: return &buttonKeypadRT;
        case 3: return &buttonScannerRT;
        case 4: return &buttonElevatorRT;
        default: return &buttonRT;
    }
}

static void drawFixtures(const float viewPos[3]) {
    for (int i = 0; i < worldLeverCount; i++) {
        WorldLever *lv = &worldLevers[i];
        float dx = lv->x - viewPos[0], dz = lv->z - viewPos[2];
        if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;
        /* Ease the handle toward its target pitch. */
        float target = lv->on ? -80.0f : 80.0f;
        lv->pitch += (target - lv->pitch) * 0.25f;
        glPushMatrix();
        glTranslatef(lv->x, lv->y, lv->z);
        glPushMatrix();
        glRotatef(-lv->yawDeg, 0.0f, 1.0f, 0.0f);
        drawModelRT(&leverBaseRT);
        glPopMatrix();
        glRotatef(-(lv->yawDeg - 180.0f), 0.0f, 1.0f, 0.0f);
        glRotatef(lv->pitch, 1.0f, 0.0f, 0.0f);
        drawModelRT(&leverHandleRT);
        glPopMatrix();
    }
    for (int i = 0; i < worldButtonCount; i++) {
        WorldButton *bt = &worldButtons[i];
        float dx = bt->x - viewPos[0], dz = bt->z - viewPos[2];
        if (dx * dx + dz * dz > VIEW_RANGE * VIEW_RANGE) continue;
        if (bt->press > 0.0f) bt->press -= 0.08f;
        glPushMatrix();
        glTranslatef(bt->x, bt->y, bt->z);
        glRotatef(-bt->yaw, 0.0f, 1.0f, 0.0f);
        glRotatef(bt->pitch, 1.0f, 0.0f, 0.0f);
        glRotatef(bt->roll, 0.0f, 0.0f, 1.0f);
        /* Depress into the panel a touch when pressed. */
        if (bt->press > 0.0f) glTranslatef(0.0f, 0.0f, -bt->press * 3.0f);
        const ModelRT *m = buttonModelFor(bt->btnId);
        if (bt->locked && texButtonRed) {
            drawModelRTTinted(m, texButtonRed);
        } else {
            drawModelRT(m);
        }
        glPopMatrix();
    }
}

/* Nearest lever/button to the player within reach; type 1 = lever,
 * 2 = button. Returns the index or -1. */
static int fixtureNearest(const float pos[3], int *type) {
    float best = 200.0f * 200.0f;
    int bi = -1, bt = 0;
    for (int i = 0; i < worldLeverCount; i++) {
        float dx = worldLevers[i].x - pos[0];
        float dy = worldLevers[i].y - (pos[1] - EYE_HEIGHT * 0.4f);
        float dz = worldLevers[i].z - pos[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < best) { best = d; bi = i; bt = 1; }
    }
    for (int i = 0; i < worldButtonCount; i++) {
        float dx = worldButtons[i].x - pos[0];
        float dy = worldButtons[i].y - (pos[1] - EYE_HEIGHT * 0.4f);
        float dz = worldButtons[i].z - pos[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d < best) { best = d; bi = i; bt = 2; }
    }
    if (type) *type = bt;
    return bi;
}

/* Rooms whose lever/button drives a bespoke, unported subsystem rather
 * than a door - they must not fall through to the generic door control:
 *   cont1_106  the femur breaker + magnet (its own handler),
 *   room2_nuke the Omega Warhead arming,
 *   cont1_914  the SCP-914 refinement knob,
 *   gate_a/b   the Gate A/B endings.
 * Everything else (containment chambers, checkpoints, guard/server/
 * storage rooms) uses its fixtures as door controls, which the port
 * has. */
static int fixtureDrivesDoor(const char *room) {
    return strcmp(room, "cont1_106") != 0
        && strcmp(room, "room2_nuke") != 0
        && strcmp(room, "cont1_914") != 0
        && strcmp(room, "gate_a") != 0
        && strcmp(room, "gate_b") != 0;
}

/* Open/close the nearest door to a just-operated fixture within the same
 * room (a wall control panel authorises the door, so any lock/keycard
 * gives way). Returns the door, or NULL if none is in reach. */
static Door *fixtureOperateDoor(const float fxPos[3], const float ear[3]) {
    Door *best = NULL;
    float bestD2 = 1400.0f * 1400.0f; /* within the room */
    for (uint32_t i = 0; i < doors.count; i++) {
        Door *d = &doors.items[i];
        if (d->type == 1) continue; /* elevator cars ride, not toggle */
        float dx = d->x - fxPos[0], dz = d->z - fxPos[2];
        float dy = d->y - fxPos[1];
        float d2 = dx * dx + dz * dz + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = d; }
    }
    if (!best) return NULL;
    best->locked = 0;
    best->open = !best->open;
    float dpos[3] = { best->x, ear[1], best->z };
    int v = rand() % 3;
    int snd = best->heavy ? (best->open ? sndBigOpen[v] : sndBigClose[v])
                          : (best->open ? sndDoorOpen[v] : sndDoorClose[v]);
    audioPlay3D(snd, dpos, ear, camYaw, 2500.0f);
    return best;
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

/* Screen-space blur (source me\BlurVolume): SCP-106 in view - and the
 * pocket dimension - smear the view. The rendered frame is copied to a
 * texture, then drawn back four times at growing offsets so the
 * overlapping copies soften it. Real post-process, no FBO needed. */
static GLuint blurTex;
static void drawScreenBlur(void) {
    if (blurAmount <= 0.02f) return;
    float amt = blurAmount > 0.9f ? 0.9f : blurAmount;
    if (!blurTex) {
        glGenTextures(1, &blurTex);
        glBindTexture(GL_TEXTURE_2D, blurTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, blurTex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, SCREEN_W, SCREEN_H, 0);

    beginHud2D();
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, blurTex);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float off = 1.5f + amt * 6.0f;   /* offset in pixels grows with blur */
    static const float dir[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
    /* The capture is bottom-up; flip V so screen top maps to V=1. */
    GLfloat uv[8] = { 0,1, 1,1, 1,0, 0,0 };
    for (int t = 0; t < 4; t++) {
        float ox = dir[t][0] * off, oy = dir[t][1] * off;
        glColor4f(1.0f, 1.0f, 1.0f, amt * 0.4f);
        GLfloat v[8] = {
            ox,             oy,
            SCREEN_W + ox,  oy,
            SCREEN_W + ox,  SCREEN_H + oy,
            ox,             SCREEN_H + oy,
        };
        glVertexPointer(2, GL_FLOAT, 0, v);
        glTexCoordPointer(2, GL_FLOAT, 0, uv);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    }
    glDisable(GL_BLEND);
    glColor4f(1, 1, 1, 1);
    endHud2D();
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
    /* The pre-rendered intro cutscene plays before the wake-up, gated
     * by both the intro and the startup-videos options (like the
     * source's PlayMovie, which honours opt\PlayStartup). */
    pendingIntroVideo = introEnabled && startupVideosEnabled;
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
    prewarmSpawnArea();
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

#define OPT_ROWS 5

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
            } else if (i == 4) {
                startupVideosEnabled = !startupVideosEnabled;
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
            case 4:
                startupVideosEnabled = !startupVideosEnabled;
                break;
        }
    }
    if (inputHit(ACTION_INTERACT)) {
        if (optSel == 3) optInvertY = !optInvertY;
        if (optSel == 4) startupVideosEnabled = !startupVideosEnabled;
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
    drawFrame(x, y, 580.0f * SX, 310.0f * SY, 0);

    const char *labels[OPT_ROWS] = {
        "Music volume", "Sound volume", "Look sensitivity", "Invert Y axis",
        "Startup videos",
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
        } else if (i == 3) {
            snprintf(val, sizeof(val), "< %s >", optInvertY ? "ON" : "OFF");
        } else {
            snprintf(val, sizeof(val), "< %s >",
                     startupVideosEnabled ? "ON" : "OFF");
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
    mtext(x + 24.0f * SX, y + 310.0f * SY + 10.0f, 1.5f, 0.5f, 0.5f, 0.5f,
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

/* Startup splash videos (Graphics_Core PlayStartupVideos): the studio
 * idents and the content warning play in order before the title menu.
 * Each source .wmv (WMV3, undecodable on the Vita) ships re-encoded to
 * H.264 MP4 (startup_<name>.mp4) and plays as real video through
 * sceAvPlayer. Gated by the "Startup videos" option (opt\PlayStartup);
 * like the source's PlayMovie, a clip that will not open is simply
 * skipped - there is no title-card substitute. */
static void playStartupVideos(void) {
#if DIAG_DISABLE_VIDEOS
    return;
#endif
    if (!startupVideosEnabled) return;
    static const char *CLIPS[4] = {
        "startup_Undertow", "startup_TSS", "startup_UET", "startup_Warning",
    };
    for (int i = 0; i < 4; i++) {
        char vp[256];
        snprintf(vp, sizeof(vp), MENU_DIR "/%s.mp4", CLIPS[i]);
        videoPlayFile(vp);
    }
}

/* ---- loading screen (Menu_Core RenderLoading) ----
 * A random SCP render over loading_back.png with its SCP title and the
 * current load step + percent at the bottom, shown while the heavy
 * asset/sound load runs (otherwise the player just sees black). */
#define LOADING_DIR DATA_ROOT "/GFX/LoadingScreens"

static GLuint loadImageTex(const char *dir, const char *name) {
    const char *dirs[1] = { dir };
    char path[1024];
    if (!textureResolve(name, dirs, 1, path, sizeof(path))) return 0;
    char err[128];
    TextureImage *img = textureLoadFile(path, 1024, err, sizeof(err));
    if (!img) return 0;
    GLuint h = 0;
    glGenTextures(1, &h);
    glBindTexture(GL_TEXTURE_2D, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)img->width,
                 (GLsizei)img->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 img->pixels);
    textureFree(img);
    return h;
}

static GLuint loadBackTex, loadImgTex;
static char loadTitle[24];

static void loadingScreenInit(void) {
    /* Renders in GFX/LoadingScreens named by SCP number. */
    static const char *IMGS[] = {
        "173", "106", "096", "049", "079", "205", "895", "914", "012",
        "035", "372", "939", "966", "513", "682", "008", "1123", "1499",
        "409", "427", "714", "005", "268", "294",
    };
    loadBackTex = loadImageTex(LOADING_DIR, "loading_back.png");
    const char *pick = IMGS[(unsigned)rand() % (sizeof(IMGS) / sizeof(IMGS[0]))];
    char fn[32];
    snprintf(fn, sizeof(fn), "%s.png", pick);
    loadImgTex = loadImageTex(LOADING_DIR, fn);
    snprintf(loadTitle, sizeof(loadTitle), "SCP-%s", pick);
}

static void renderLoadingScreen(const char *step, int percent) {
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    beginHud2D();
    if (loadBackTex) {
        drawQuad(0, 0, SCREEN_W, SCREEN_H, loadBackTex, 1, 1, 1, 1);
    } else {
        drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0.02f, 0.02f, 0.03f, 1);
    }
    if (loadImgTex) {
        float s = 300.0f;
        drawQuad((SCREEN_W - s) * 0.5f, (SCREEN_H - s) * 0.5f - 24.0f, s, s,
                 loadImgTex, 1, 1, 1, 1);
        mtextC(SCREEN_W * 0.5f, SCREEN_H * 0.5f - 190.0f, 2.5f, 1, 1, 1,
               loadTitle);
    }
    char line[96];
    snprintf(line, sizeof(line), "%s   %d%%", step, percent);
    mtextC(SCREEN_W * 0.5f, SCREEN_H - 40.0f, 1.6f, 0.85f, 0.85f, 0.85f, line);
    endHud2D();
    vglSwapBuffers(GL_FALSE);
}

/* Load the player's cell and its ring-1 neighbors to completion behind
 * the loading screen: the synchronous spawn-room load measured 5.4 s on
 * device, which used to freeze the first gameplay frame. */
static void prewarmSpawnArea(void) {
    int px = (int)floorf(camPos[0] / ROOM_SPACING + 0.5f);
    int py = (int)floorf(camPos[2] / ROOM_SPACING + 0.5f);
    int total = 0, done = 0;
    for (uint32_t i = 0; i < map.roomCount; i++) {
        const RoomPlacement *p = &map.rooms[i];
        int adx = abs(p->gridX - px), ady = abs(p->gridY - py);
        const TemplateRT *rt = &tplRT[p->templateIndex];
        if ((adx > ady ? adx : ady) <= 1 && rt->state != 1
            && rt->state != -1) {
            total++;
        }
    }
    if (!total) return;
    uint64_t t0 = sceKernelGetProcessTimeWide();
    for (uint32_t i = 0; i < map.roomCount; i++) {
        const RoomPlacement *p = &map.rooms[i];
        int adx = abs(p->gridX - px), ady = abs(p->gridY - py);
        const TemplateRT *rt = &tplRT[p->templateIndex];
        if ((adx > ady ? adx : ady) > 1 || rt->state == 1
            || rt->state == -1) {
            continue;
        }
        renderLoadingScreen("LOADING AREA", 100 * done / total);
        templateEnsure(p->templateIndex);
        done++;
    }
    renderLoadingScreen("LOADING AREA", 100);
    plog("prewarm: %d rooms in %ums", done,
         (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000));
}

/* The startup_Intro cutscene (Menu_Core PlayMovie("startup_Intro")):
 * played when a new game begins with the intro enabled. The source .wmv
 * is undecodable on the Vita, but the same cutscene ships as H.264 MP4
 * (three sequential fragments), which sceAvPlayer hardware-decodes.
 * Deferred to the first game frame via this flag so it runs outside the
 * menu's HUD 2D scope. */
static void playIntroVideo(void) {
#if DIAG_DISABLE_VIDEOS
    return;
#endif
    if (!videoInit()) return;
    audioStopMusic();
    videoPlayFile(MENU_DIR "/startup_Intro1.mp4");
    videoPlayFile(MENU_DIR "/startup_Intro2.mp4");
    videoPlayFile(MENU_DIR "/startup_Intro3.mp4");
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
    /* vglInit reserves the ENTIRE CDRAM and PHYCONT partitions for
     * vitaGL, leaving nothing for sceAvPlayer's decoder frame buffers
     * (the device log showed both partitions 100% full). Leave a slice
     * of each unreserved so the startup/intro videos can decode; the
     * game's 256px-capped textures still fit in what remains. Thresholds
     * are the bytes vitaGL leaves FREE per pool. */
    vglInitWithCustomThreshold(0x800000, SCREEN_W, SCREEN_H,
                               0x1000000 /* RAM: 16 MB */,
                               0x1000000 /* CDRAM: 16 MB */,
                               0x800000 /* PHYCONT: 8 MB */,
                               0 /* CDLG */, SCE_GXM_MULTISAMPLE_NONE);

    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    inputInit();

    /* Asset loader worker: decodes/parses off the render thread. */
    loaderRunning = 1;
    SceUID loaderThread = sceKernelCreateThread("asset_loader", loaderLoop,
                                                160, 0x40000, 0, 0, NULL);
    if (loaderThread >= 0) {
        sceKernelStartThread(loaderThread, 0, NULL);
        loaderOk = 1;
    }

    int haveData = templatesLoad(ROOMS_INI, &tplList) && tplList.count > 0;
    if (haveData) {
        tplRT = (TemplateRT *)calloc(tplList.count, sizeof(TemplateRT));
        optionsLoad();
        mkdir(SAVES_DIR, 0777);
        int audioOk = audioInit();
        if (audioOk) optionsApply();
        srand((unsigned)sceKernelGetProcessTimeWide());
        pendingSeed = (uint32_t)(sceKernelGetProcessTimeWide() & 0xFFFFFF) | 1u;
        /* Play the boot videos now, before the door/NPC/texture/sound
         * load fills the heap: sceAvPlayer needs a few MB of contiguous
         * CPU RAM for its demux/decode buffers, so give it the emptiest
         * heap of the session (sounds now decode lazily, but the door/
         * NPC/texture load below still costs real memory). */
        playStartupVideos();
        /* Heavy asset + audio load, behind the loading screen so the
         * player sees progress instead of a black screen. */
        loadingScreenInit();
        renderLoadingScreen("LOADING MODELS", 15);
        buildDoorAssets();
        buildNpcAssets();
        renderLoadingScreen("LOADING TEXTURES", 45);
        loadHudTextures();
        loadMenuTextures();
        texButtonRed = textureGet("keypad_locked.png");
        pdThroneTex = textureGet("scp_106_eyes.png");
        renderLoadingScreen("LOADING SOUNDS", 70);
        if (audioOk) {
            loadSounds();
            /* Warm the small, hot SFX so first plays are instant; big
             * files (voice lines, stingers, ambience) decode on the
             * decoder thread when first triggered. */
            audioPredecodeSmall(160 * 1024, 24 * 1024 * 1024);
        }
        renderLoadingScreen("LOADING DECALS", 90);
        decalsInit();
        renderLoadingScreen("DONE", 100);
        /* The loading art is never drawn again; return its VRAM (the
         * world fills the pool completely, so every MB counts). */
        if (loadBackTex) {
            glDeleteTextures(1, &loadBackTex);
            loadBackTex = 0;
        }
        if (loadImgTex) {
            glDeleteTextures(1, &loadImgTex);
            loadImgTex = 0;
        }
        /* Boot to the title menu; the world is generated when the
         * player starts or loads a game. */
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
        /* Worst frame in the last fps window: the smoothed fps hides
         * single-frame stalls, this exposes them. */
        static uint64_t framePrevUs;
        static uint64_t frameMaxUs;
        static unsigned worstMs;
        if (framePrevUs && now - framePrevUs > frameMaxUs) {
            frameMaxUs = now - framePrevUs;
        }
        framePrevUs = now;
        if (now - fpsLast >= 500000) {
            fps = fpsFrames * 1000000.0f / (float)(now - fpsLast);
            fpsFrames = 0;
            fpsLast = now;
            worstMs = (unsigned)(frameMaxUs / 1000);
            frameMaxUs = 0;
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

        /* First frame of a fresh game: play the intro cutscene (it takes
         * over the screen with its own render loop) before gameplay. */
        if (haveData && pendingIntroVideo) {
            pendingIntroVideo = 0;
            playIntroVideo();
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
        if (keypadOpen && !pauseOpen) {
            menuPollTap();
            /* D-pad: left/right pick a digit slot, up/down change it;
             * X submits, Circle cancels; touch taps the number pad. */
            if (inputHit(ACTION_LEAN_LEFT) && keypadPos > 0) keypadPos--;
            if (inputHit(ACTION_LEAN_RIGHT) && keypadPos < 3) keypadPos++;
            if (inputHit(ACTION_SAVE)) {
                keypadDigits[keypadPos] = (keypadDigits[keypadPos] + 1) % 10;
            }
            if (inputDpadDownHit()) {
                keypadDigits[keypadPos] = (keypadDigits[keypadPos] + 9) % 10;
            }
            if (menuTapped) {
                float kx = SCREEN_W / 2.0f - 150.0f, ky = 160.0f;
                for (int n = 0; n < 10; n++) {
                    float bx = kx + (n % 5) * 60.0f, by = ky + 130.0f
                             + (n / 5) * 58.0f;
                    if (tapIn(bx, by, 52.0f, 50.0f)) {
                        keypadDigits[keypadPos] = n;
                        if (keypadPos < 3) keypadPos++;
                    }
                }
                if (!tapIn(kx - 20.0f, ky - 20.0f, 340.0f, 320.0f)) {
                    keypadOpen = 0;
                }
            }
            if (inputHit(ACTION_INTERACT) && keypadDoor >= 0
                && keypadDoor < (int)doors.count) {
                Door *kd = &doors.items[keypadDoor];
                int entered = keypadDigits[0] * 1000 + keypadDigits[1] * 100
                            + keypadDigits[2] * 10 + keypadDigits[3];
                if (entered == kd->code) {
                    kd->code = 0; /* unlocked for good */
                    kd->open = 1;
                    keypadOpen = 0;
                    audioPlay(sndKeycardUse[rand() % 2], 0.9f, 0.0f);
                    snprintf(toastMsg, sizeof(toastMsg), "ACCESS GRANTED");
                    toastTimer = 120;
                } else {
                    audioPlay(sndDoorLock, 0.9f, 0.0f);
                    snprintf(toastMsg, sizeof(toastMsg), "WRONG CODE");
                    toastTimer = 120;
                }
            }
            if (inputHit(ACTION_CROUCH)) keypadOpen = 0;
        }
        if (!pauseOpen && !keypadOpen && inputHit(ACTION_INVENTORY)) {
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
            && !keypadOpen && !introCameraLocked()
            && inputHit(ACTION_INTERACT) && try914Interact()) {
            /* SCP-914 handled the press (knob cycle or refine run). */
        } else if (haveData && !invOpen && !pauseOpen && !pausedAtFrameStart
            && !keypadOpen && !introCameraLocked()
            && inputHit(ACTION_INTERACT)) {
            int fxType = 0;
            int fx = fixtureNearest(camPos, &fxType);
            int picked = fx < 0 ? itemPickupNearest(camPos) : -1;
            if (fx >= 0) {
                if (fxType == 1) {
                    WorldLever *lv = &worldLevers[fx];
                    lv->on = !lv->on;
                    float lp[3] = { lv->x, camPos[1], lv->z };
                    audioPlay3D(sndLever, lp, camPos, camYaw, 1200.0f);
                    const char *rm = roomNameAt(camPos);
                    if (strcmp(rm, "cont1_106") == 0) {
                        audioPlay(lv->on ? sndMagnetUp : sndMagnetDown,
                                  0.8f, 0.0f);
                    } else if (fixtureDrivesDoor(rm)) {
                        /* Containment/checkpoint/guard-room levers throw
                         * their room's door (the ported subsystem). */
                        float fp[3] = { lv->x, lv->y, lv->z };
                        if (fixtureOperateDoor(fp, camPos)) {
                            snprintf(toastMsg, sizeof(toastMsg),
                                     lv->on ? "THE DOOR SLIDES OPEN"
                                            : "THE DOOR SLIDES SHUT");
                            toastTimer = 120;
                        }
                    }
                } else {
                    WorldButton *bt = &worldButtons[fx];
                    bt->press = 1.0f;
                    float bp[3] = { bt->x, camPos[1], bt->z };
                    audioPlay3D(bt->locked ? sndDoorLock
                                           : sndButton[rand() % 2],
                                bp, camPos, camYaw, 1200.0f);
                    const char *brm = roomNameAt(camPos);
                    if (bt->locked) {
                        snprintf(toastMsg, sizeof(toastMsg),
                                 "IT WON'T BUDGE");
                        toastTimer = 120;
                    } else if (!bt->btnId && femurTimer <= 0.0f
                               && !npc106Contained
                               && strcmp(brm, "cont1_106") == 0) {
                        /* The femur breaker: the magnet (a chamber
                         * lever) must be engaged first (source: the
                         * button does nothing until EventState2). */
                        int magnetOn = 0;
                        for (int li = 0; li < worldLeverCount; li++) {
                            float lx = worldLevers[li].x - bt->x;
                            float lz = worldLevers[li].z - bt->z;
                            if (lx * lx + lz * lz < 2500.0f * 2500.0f
                                && worldLevers[li].on) {
                                magnetOn = 1;
                                break;
                            }
                        }
                        if (!magnetOn) {
                            snprintf(toastMsg, sizeof(toastMsg),
                                     "ENGAGE THE MAGNET FIRST (THE LEVER)");
                            toastTimer = 200;
                        } else {
                            femurTimer = 1.0f;
                            femurSpot[0] = bt->x;
                            femurSpot[1] = bt->y;
                            femurSpot[2] = bt->z;
                            audioPlay(sndFemur, 1.0f, 0.0f);
                            snprintf(toastMsg, sizeof(toastMsg),
                                     "THE FEMUR BREAKER ACTIVATES...");
                            toastTimer = 200;
                        }
                    } else if (fixtureDrivesDoor(brm)) {
                        /* A plain wall button throws its room's door. */
                        float fp[3] = { bt->x, bt->y, bt->z };
                        Door *od = fixtureOperateDoor(fp, camPos);
                        if (od) {
                            snprintf(toastMsg, sizeof(toastMsg),
                                     od->open ? "THE DOOR SLIDES OPEN"
                                              : "THE DOOR SLIDES SHUT");
                            toastTimer = 120;
                        }
                    }
                }
            } else if (picked >= 0) {
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
                        audioPlay(pressed->keycard > 0
                                      ? sndKeycardUse[rand() % 2]
                                      : sndButton[rand() % 2], 0.9f, 0.0f);
                        /* An elevator call takes the ride, not a plain
                         * open (only when the player is at that car). */
                        float ed0 = camPos[0] - pressed->x;
                        float ed2 = camPos[2] - pressed->z;
                        if (pressed->type == 1 && elevState == ELEV_IDLE
                            && ed0 * ed0 + ed2 * ed2 < 900.0f * 900.0f) {
                            elevatorStart(pressed);
                        } else {
                            float dpos[3] = { pressed->x, camPos[1],
                                              pressed->z };
                            int v = rand() % 3;
                            int snd = pressed->heavy
                                    ? (pressed->open ? sndBigOpen[v]
                                                     : sndBigClose[v])
                                    : (pressed->open ? sndDoorOpen[v]
                                                     : sndDoorClose[v]);
                            audioPlay3D(snd, dpos, camPos, camYaw, 2500.0f);
                        }
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
                case DOOR_PRESS_CODE:
                    keypadOpen = 1;
                    keypadDoor = (int)(pressed - doors.items);
                    keypadDigits[0] = keypadDigits[1] = keypadDigits[2]
                        = keypadDigits[3] = 0;
                    keypadPos = 0;
                    audioPlay(sndButton[rand() % 2], 0.9f, 0.0f);
                    break;
                default:
                    break;
            }
            }
        }
        if (haveData && !pauseOpen) {
            /* The dread-zoom and screen blur ease back each frame; 106
             * (and the throne) re-ramp them while they hold the gaze. */
            gTick++;
            cameraZoom *= 0.9f;
            if (cameraZoom < 0.05f) cameraZoom = 0.0f;
            blurAmount *= 0.88f;
            if (blurAmount < 0.02f) blurAmount = 0.0f;
            doorsUpdate(&doors);
            camerasUpdate();
            cameraCheckUpdate();
            update079();
            update895();
            update012();
            update372();
            update205();
            update914();
            update035();
            updateRefineHostile();
            update513();
            update173();
            update106();
            update096();
            update049();
            update0492();
            update939();
            update966();
            update1499();
            update860();
            updateElevator();
            introUpdate();
            if (introPhase < 0 && !inPocket) {
                updateZoneMusic();
                updateRoomAmbience();
                updateAmbientSfx();
                updateRoomEvents();
            }
            audioService(); /* start ambience whose PCM just finished */
            updatePocketDimension();
            updatePlayerCondition();
            teslaUpdate();
            decalsUpdate();
            if (femurTimer > 0.0f) {
                femurTimer += 1.0f;
                /* Lure phase: 106 rises out of the pit to feed. */
                if (femurTimer > 300.0f && femurTimer < 840.0f) {
                    npc106Active = 1;
                    npc106State = N106_SPAWNING;
                    npc106Pos[0] = femurSpot[0];
                    npc106Pos[2] = femurSpot[2];
                    float rise = (femurTimer - 300.0f) / 540.0f;
                    npc106Pos[1] = femurSpot[1] - 280.0f + rise * 280.0f;
                    npc106Frame = 110.0f; /* feeding pose */
                    if (femurTimer < 320.0f) {
                        audioPlay(snd106Decay[rand() % 4], 1.0f, 0.0f);
                    }
                }
                if (femurTimer >= 900.0f) {
                    femurTimer = 0.0f;
                    npc106Contained = 1;
                    npc106Active = 0;
                    npc106State = N106_DORMANT;
                    npc106Pos[1] = -5000.0f;
                    snprintf(toastMsg, sizeof(toastMsg),
                             "SCP-106 HAS BEEN RECONTAINED");
                    toastTimer = 300;
                }
            }
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
                reset096();
                reset049();
                reset939();
                reset860();
                npc966Drowsy = 0.0f;
                inMask = 0;
                npc1499Count = 0;
                health = 100.0f;
                injuries = 0.0f;
                bloodloss = 0.0f;
                sanity = 100.0f;
                snprintf(toastMsg, sizeof(toastMsg),
                         "YOU WERE KILLED BY %s", deathCause);
                toastTimer = 240;
            }
        }
        if (toastTimer > 0) toastTimer--;

        StickState look = inputLook();
        StickState move = inputMove();
        if (invOpen || pauseOpen || keypadOpen || deathTimer > 0
            || introCameraLocked() || npc106State == N106_GRABBING
            || elevState != ELEV_IDLE) {
            /* Freeze the camera and player while a menu is open, the
             * death screen is playing, the intro wake-up cinematic drives
             * the camera, SCP-106 has the player in its grab, or an
             * elevator ride is under way. */
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
            /* How much noise the player is making (SCP-939 hunts by it):
             * loud when sprinting, hushed crouch-walking, near-silent
             * standing still. */
            playerNoise = !moving ? (crouched ? 0.05f : 0.12f)
                        : sprinting ? 1.0f : crouched ? 0.25f : 0.6f;
            float mvx = (fwdX * -move.y + cosf(camYaw) * move.x) * speed;
            float mvz = (fwdZ * -move.y + sinf(camYaw) * move.x) * speed;
            camPos[0] += mvx;
            camPos[2] += mvz;

            /* Footstep cadence by distance walked (PlayStepSound),
             * with a metal step set on grating/panel floors
             * (GetStepSound). */
            if (velY > -1.0f) {
                stepAccum += sqrtf(mvx * mvx + mvz * mvz);
                float strideLen = 170.0f;
                if (stepAccum >= strideLen) {
                    stepAccum = 0.0f;
                    int metal = floorIsMetal(camPos);
                    if (sprinting) {
                        audioPlay(metal ? sndRunMetal[rand() % 8]
                                        : sndRun[rand() % 7], 0.7f, 0.0f);
                    } else {
                        audioPlay(metal ? sndStepMetal[rand() % 8]
                                        : sndStep[rand() % 8],
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
            collide860Trees(camPos);

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
                        injuries += dmg / 20.0f;
                        bloodloss += dmg * 0.8f;
                        damageFlash = 0.6f;
                        audioPlay(sndDamage[rand() % 4], 1.0f, 0.0f);
                        if (bloodloss >= 100.0f) {
                            bloodloss = 100.0f;
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

        /* Capture the nearest monitor's live feed before clearing the
         * frame (it renders into a scissored corner, then copies out). */
        renderCameraFeed();

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (haveData && activeCount > 0) {
            setPerspective();
            applyDebugState();
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            float shP = 0.0f, shY = 0.0f;
            if (camShake > 0.0f) {
                shP = sinf(fpsFrames * 1.7f) * camShake;
                shY = sinf(fpsFrames * 2.3f) * camShake;
            }
            glRotatef(camPitch * 180.0f / 3.14159265f + shP, 1, 0, 0);
            glRotatef(camYaw * 180.0f / 3.14159265f + shY, 0, 1, 0);
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
            drawMtMaze(0);
            drawPocketComposite(0);
            decalsDraw();
            drawDoors(camPos);
            drawFixtures(camPos);
            camerasDraw(camPos);
            drawMonitors(camPos);
            draw079(camPos);
            draw895(camPos);
            draw372(camPos);
            draw205(camPos);
            draw914(camPos);
            draw035(camPos);
            drawRefineHostile(camPos);
            draw513(camPos);
            drawItems(camPos);
            draw173(camPos);
            draw106(camPos);
            draw096(camPos);
            draw049(camPos);
            draw939(camPos);
            draw966(camPos);
            draw1499(camPos);
            draw860(camPos);
            drawArm682();
            drawTeslaArcs(camPos);
            drawPocketPillars();
            drawPocketThrone();
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
            drawMtMaze(1);
            drawPocketComposite(1);
            drawScreenBlur();
        }

        char line1[320];
        snprintf(line1, sizeof(line1), "%s   [%s]", statusLine,
                 haveData ? roomNameAt(camPos) : "-");
        char line2[256];
        struct mallinfo hmi = mallinfo();
        snprintf(line2, sizeof(line2),
                 "bld=%s fps=%.0f ms=%u dec-fail=%d texfail=%d tpl=%d "
                 "act=%d vram=%uK heap=%uK",
                 PORT_BUILD_TAG, fps, worstMs, audioLoadDecodeFails(),
                 texFailCount, tplFailCount, activeCount,
                 (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024),
                 (unsigned)((unsigned)hmi.fordblks / 1024));
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
        /* Bleeding: a red vignette that deepens with bloodloss, plus
         * a flash when damage lands. */
        if (haveData && deathTimer == 0) {
            float hurt = bloodloss / 100.0f * 0.6f + damageFlash;
            if (damageFlash > 0.0f) damageFlash -= 0.02f;
            if (hurt > 0.01f) {
                drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0.6f, 0.0f, 0.0f,
                         hurt > 0.85f ? 0.85f : hurt);
            }
            /* A Tesla gate discharging nearby whites out the screen. */
            if (teslaFlash > 0.01f) {
                drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0.8f, 0.9f, 1.0f,
                         teslaFlash > 0.85f ? 0.85f : teslaFlash);
            }
            /* Low sanity darkens the edges with a cold, unstable tint. */
            if (sanity < 50.0f) {
                float sv = (50.0f - sanity) / 50.0f;
                float jit = sinf(fpsFrames * 0.9f) * 0.05f * sv;
                drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0.0f, 0.05f, 0.03f,
                         sv * 0.5f + jit);
            }
            /* SCP-012 floods the view with its bloody compulsion overlay,
             * jittering it about like the source (Rand offset per frame). */
            if (scp012Comp > 0.01f && scp012OvTex) {
                float jx = (float)(rand() % 80) - 40.0f;
                float jy = (float)(rand() % 40) - 20.0f;
                drawQuad(jx - 60.0f, jy - 60.0f, SCREEN_W + 120.0f,
                         SCREEN_H + 120.0f, scp012OvTex, 1.0f, 1.0f, 1.0f,
                         scp012Comp);
            }
        }
        if (haveData && walkMode && !invOpen && !pauseOpen) {
            drawMeter(20, SCREEN_H - 60, hudBlinkIcon, hudBlinkBar,
                      blinkTimer / 100.0f);
            drawMeter(20, SCREEN_H - 110, hudSprintIcon, hudStaminaBar,
                      stamina / 100.0f);
            /* Blood and sanity bars above the vitals meters. */
            drawQuad(20, SCREEN_H - 132, 120, 6, 0, 0.15f, 0.02f, 0.02f, 1);
            drawQuad(20, SCREEN_H - 132, 120 * (bloodloss / 100.0f), 6, 0,
                     0.8f, 0.1f, 0.1f, 1);
            drawQuad(20, SCREEN_H - 146, 120, 6, 0, 0.05f, 0.1f, 0.1f, 1);
            drawQuad(20, SCREEN_H - 146, 120 * (sanity / 100.0f), 6, 0,
                     0.3f, 0.7f, 0.8f, 1);
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
        if (keypadOpen && !pauseOpen) {
            /* Keypad panel: code display, digit cursor, number pad. */
            float kx = SCREEN_W / 2.0f - 150.0f, ky = 160.0f;
            drawQuad(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 0, 0.55f);
            drawQuad(kx - 20, ky - 20, 340, 320, 0, 0.05f, 0.05f, 0.06f,
                     0.95f);
            drawQuad(kx - 20, ky - 20, 340, 2, 0, 0.6f, 0.6f, 0.6f, 1);
            drawQuad(kx - 20, ky + 298, 340, 2, 0, 0.6f, 0.6f, 0.6f, 1);
            for (int n = 0; n < 4; n++) {
                float bx = kx + 20.0f + n * 70.0f;
                int cur = n == keypadPos;
                drawQuad(bx, ky, 56, 70, 0, cur ? 0.25f : 0.1f,
                         cur ? 0.3f : 0.12f, cur ? 0.25f : 0.1f, 1.0f);
                char dg[2] = { (char)('0' + keypadDigits[n]), 0 };
                glPushMatrix();
                glScalef(4.0f, 4.0f, 1.0f);
                glColor4f(0.6f, 1.0f, 0.6f, 1.0f);
                drawText((bx + 16.0f) / 4.0f, (ky + 18.0f) / 4.0f, dg);
                glColor4f(1, 1, 1, 1);
                glPopMatrix();
            }
            for (int n = 0; n < 10; n++) {
                float bx = kx + (n % 5) * 60.0f, by = ky + 130.0f
                         + (n / 5) * 58.0f;
                drawQuad(bx, by, 52, 50, 0, 0.12f, 0.12f, 0.14f, 1.0f);
                char dg[2] = { (char)('0' + n), 0 };
                glPushMatrix();
                glScalef(2.5f, 2.5f, 1.0f);
                glColor4f(0.85f, 0.85f, 0.85f, 1.0f);
                drawText((bx + 20.0f) / 2.5f, (by + 17.0f) / 2.5f, dg);
                glColor4f(1, 1, 1, 1);
                glPopMatrix();
            }
            glPushMatrix();
            glScalef(1.5f, 1.5f, 1.0f);
            glColor4f(0.6f, 0.6f, 0.6f, 1.0f);
            drawText((kx - 10.0f) / 1.5f, (ky + 305.0f) / 1.5f,
                     "dpad: digits   X: enter   O: cancel");
            glColor4f(1, 1, 1, 1);
            glPopMatrix();
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
