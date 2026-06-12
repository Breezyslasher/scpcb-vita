/*
 * Asset validation harness for the Vita port's format loaders.
 *
 * Recursively scans the given directories and:
 *   - parses every .rmesh file with the RMesh loader,
 *   - parses every .b3d file with the B3D loader,
 *   - resolves every texture those files reference (room folder first,
 *     then GFX/Map/Textures, case-insensitive — matching Map_Core.bb)
 *     and decodes each unique one to RGBA8,
 * then reports aggregate statistics including the texture memory bill
 * at native resolution vs. capped resolutions (Vita budget planning).
 *
 * Exits non-zero if any file fails to parse or any resolved texture
 * fails to decode. Unresolved references are reported but not fatal:
 * the game itself tolerates missing textures (Tex[j] stays 0).
 *
 * Build (host):
 *   gcc -O2 -Wall -o validate_assets vita/tools/validate_assets.c \
 *       vita/src/formats/rmesh.c vita/src/formats/b3d.c \
 *       vita/src/formats/texture.c -lm
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/formats/b3d.h"
#include "../src/formats/rmesh.h"
#include "../src/formats/texture.h"
#include "../src/game/collision.h"
#include "../src/game/mapgen.h"
#include "../src/render/scene.h"

#define MAP_TEXTURES_DIR "GFX/Map/Textures"

static struct {
    unsigned rmeshOk, rmeshFail;
    unsigned b3dOk, b3dFail;
    unsigned long long surfaces, vertices, triangles, entities;
    unsigned long long b3dMeshNodes, b3dVertices, b3dTriangles;
    unsigned texDecoded, texDecodeFail, texMissing;
    unsigned long long texBytesNative, texBytesCap512, texBytesCap256;
    unsigned sceneOk, sceneFail;
    unsigned long long sceneBatches, sceneVertices, maxBatchVertices;
    unsigned colOk, colFail, colGrounded;
} stats;

static int failures;

/* ---- unique string sets (resolved texture paths / missing names) ---- */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} StrSet;

static int strSetAdd(StrSet *set, const char *s) {
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->items[i], s) == 0) return 0;
    }
    if (set->count == set->cap) {
        size_t ncap = set->cap ? set->cap * 2 : 64;
        char **grown = (char **)realloc(set->items, ncap * sizeof(char *));
        if (!grown) return 0;
        set->items = grown;
        set->cap = ncap;
    }
    set->items[set->count] = strdup(s);
    if (!set->items[set->count]) return 0;
    set->count++;
    return 1;
}

static StrSet texPaths;   /* resolved, to decode */
static StrSet texMissing; /* unresolved names, to report */

/* ---------------------------------------------------------------- */

static const char *extOf(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

static int extEq(const char *name, const char *ext) {
    const char *e = extOf(name);
    while (*e && *ext) {
        char a = (char)((*e >= 'A' && *e <= 'Z') ? *e + 32 : *e);
        if (a != *ext) return 0;
        e++;
        ext++;
    }
    return *e == '\0' && *ext == '\0';
}

static void dirOf(const char *path, char *out, size_t outLen) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, outLen, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= outLen) n = outLen - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

static void collectTexture(const char *name, const char *containingDir) {
    if (!name || name[0] == '\0') return;
    const char *dirs[2] = { containingDir, MAP_TEXTURES_DIR };
    char resolved[4096];
    if (textureResolve(name, dirs, 2, resolved, sizeof(resolved))) {
        strSetAdd(&texPaths, resolved);
    } else {
        if (strSetAdd(&texMissing, name)) {
            stats.texMissing++;
        }
    }
}

static void tallyNode(const B3DNode *n) {
    if (!n) return;
    if (n->mesh) {
        stats.b3dMeshNodes++;
        stats.b3dVertices += n->mesh->vertexCount;
        for (uint32_t i = 0; i < n->mesh->triSetCount; i++) {
            stats.b3dTriangles += n->mesh->triSets[i].triangleCount;
        }
    }
    for (uint32_t i = 0; i < n->childCount; i++) {
        tallyNode(n->children[i]);
    }
}

static void checkFile(const char *path) {
    char err[256];
    char dir[4096];

    if (extEq(path, "rmesh")) {
        RMesh *m = rmeshLoadFile(path, err, sizeof(err));
        if (!m) {
            printf("FAIL rmesh %s: %s\n", path, err);
            stats.rmeshFail++;
            failures = 1;
            return;
        }
        stats.rmeshOk++;
        stats.surfaces += m->surfaceCount;
        stats.entities += m->entityCount;

        Scene *sc = sceneBuild(m);
        if (!sc) {
            printf("FAIL scene %s\n", path);
            stats.sceneFail++;
            failures = 1;
        } else {
            stats.sceneOk++;
            stats.sceneBatches += sc->batchCount;
            for (uint32_t i = 0; i < sc->batchCount; i++) {
                stats.sceneVertices += sc->batches[i].vertexCount;
                if (sc->batches[i].vertexCount > stats.maxBatchVertices) {
                    stats.maxBatchVertices = sc->batches[i].vertexCount;
                }
            }
            CollisionWorld *cw = collisionBuild(sc, m);
            if (!cw) {
                printf("FAIL colw  %s\n", path);
                stats.colFail++;
                failures = 1;
            } else {
                stats.colOk++;
                /* Drop a probe from the room center: most rooms should
                 * report ground below. */
                float origin[3] = {
                    (sc->boundsMin[0] + sc->boundsMax[0]) * 0.5f,
                    sc->boundsMax[1],
                    (sc->boundsMin[2] + sc->boundsMax[2]) * 0.5f,
                };
                float hitY;
                if (collisionRayDown(cw, origin,
                                     sc->boundsMax[1] - sc->boundsMin[1]
                                         + 1.0f, &hitY)) {
                    stats.colGrounded++;
                }
                int up = 0;
                collisionSpherePush(cw, origin, 38.0f, &up);
                collisionFree(cw);
            }
            sceneFree(sc);
        }

        dirOf(path, dir, sizeof(dir));
        for (uint32_t i = 0; i < m->surfaceCount; i++) {
            stats.vertices += m->surfaces[i].vertexCount;
            stats.triangles += m->surfaces[i].triangleCount;
            collectTexture(m->surfaces[i].textures[0].name, dir);
            collectTexture(m->surfaces[i].textures[1].name, dir);
        }
        rmeshFree(m);
    } else if (extEq(path, "b3d")) {
        B3DModel *m = b3dLoadFile(path, err, sizeof(err));
        if (!m) {
            printf("FAIL b3d   %s: %s\n", path, err);
            stats.b3dFail++;
            failures = 1;
            return;
        }
        stats.b3dOk++;
        tallyNode(m->root);

        /* Exercise prop scene-building with an identity placement. */
        RMesh empty;
        memset(&empty, 0, sizeof(empty));
        Scene *sc = sceneBuild(&empty);
        if (sc) {
            float pos[3] = { 0, 0, 0 };
            float euler[3] = { 0, 0, 0 };
            float scl[3] = { 1, 1, 1 };
            if (!sceneAppendB3D(sc, m, pos, euler, scl, NULL)) {
                printf("FAIL scene-b3d %s\n", path);
                failures = 1;
            }
            sceneFree(sc);
        }

        dirOf(path, dir, sizeof(dir));
        for (uint32_t i = 0; i < m->textureCount; i++) {
            collectTexture(m->textures[i].file, dir);
        }
        b3dFree(m);
    }
}

static void walk(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("FAIL stat  %s\n", path);
        failures = 1;
        return;
    }
    if (S_ISREG(st.st_mode)) {
        checkFile(path);
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;

    DIR *d = opendir(path);
    if (!d) {
        printf("FAIL open  %s\n", path);
        failures = 1;
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        walk(child);
    }
    closedir(d);
}

static unsigned long long cappedBytes(uint32_t w, uint32_t h, uint32_t cap) {
    while (w > cap || h > cap) {
        if (w > 1) w /= 2;
        if (h > 1) h /= 2;
    }
    return (unsigned long long)w * h * 4;
}

static void decodeTextures(void) {
    char err[256];
    for (size_t i = 0; i < texPaths.count; i++) {
        TextureImage *img = textureLoadFile(texPaths.items[i], 0, err, sizeof(err));
        if (!img) {
            printf("FAIL tex   %s: %s\n", texPaths.items[i], err);
            stats.texDecodeFail++;
            failures = 1;
            continue;
        }
        stats.texDecoded++;
        stats.texBytesNative += (unsigned long long)img->width * img->height * 4;
        stats.texBytesCap512 += cappedBytes(img->width, img->height, 512);
        stats.texBytesCap256 += cappedBytes(img->width, img->height, 256);
        textureFree(img);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <dir-or-file>...\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        walk(argv[i]);
    }
    decodeTextures();

    printf("\nRMesh: %u ok, %u failed\n", stats.rmeshOk, stats.rmeshFail);
    printf("  surfaces=%llu vertices=%llu triangles=%llu entities=%llu\n",
           stats.surfaces, stats.vertices, stats.triangles, stats.entities);
    printf("B3D:   %u ok, %u failed\n", stats.b3dOk, stats.b3dFail);
    printf("  mesh nodes=%llu vertices=%llu triangles=%llu\n",
           stats.b3dMeshNodes, stats.b3dVertices, stats.b3dTriangles);
    printf("Scenes: %u built, %u failed\n", stats.sceneOk, stats.sceneFail);
    printf("  batches=%llu vertices=%llu maxBatchVertices=%llu\n",
           stats.sceneBatches, stats.sceneVertices, stats.maxBatchVertices);
    printf("Collision: %u worlds built, %u failed, %u rooms grounded\n",
           stats.colOk, stats.colFail, stats.colGrounded);
    printf("Textures: %u decoded, %u decode failures, %u unresolved refs\n",
           stats.texDecoded, stats.texDecodeFail, stats.texMissing);
    printf("  RGBA8 footprint: native=%.1f MB, cap512=%.1f MB, cap256=%.1f MB\n",
           stats.texBytesNative / (1024.0 * 1024.0),
           stats.texBytesCap512 / (1024.0 * 1024.0),
           stats.texBytesCap256 / (1024.0 * 1024.0));
    if (stats.texMissing > 0) {
        printf("  unresolved references:\n");
        for (size_t i = 0; i < texMissing.count; i++) {
            printf("    %s\n", texMissing.items[i]);
        }
    }

    /* Map generation against the shipped template list. */
    RoomTemplateList tpls;
    if (templatesLoad("Data/rooms.ini", &tpls)) {
        unsigned genOk = 0;
        unsigned long long totalRooms = 0;
        for (uint32_t seed = 1; seed <= 50; seed++) {
            GeneratedMap m;
            if (mapGenerate(&tpls, seed, &m)) {
                genOk++;
                totalRooms += m.roomCount;
                mapFree(&m);
            } else {
                printf("FAIL mapgen seed %u\n", seed);
                failures = 1;
            }
        }
        printf("MapGen: %u templates, %u/50 seeds ok, avg rooms=%.1f\n",
               tpls.count, genOk, genOk ? (double)totalRooms / genOk : 0.0);
        templatesFree(&tpls);
    } else {
        printf("MapGen: Data/rooms.ini not found (skipped)\n");
    }

    if (stats.rmeshOk + stats.rmeshFail + stats.b3dOk + stats.b3dFail == 0) {
        printf("no assets found - check the paths\n");
        return 2;
    }
    return failures ? 1 : 0;
}
