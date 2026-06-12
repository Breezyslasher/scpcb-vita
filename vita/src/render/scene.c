#include "scene.h"

#include <stdlib.h>
#include <string.h>

#define CHUNK_VERTEX_LIMIT 65535

static int nameIsLightmap(const char *name) {
    if (!name) return 0;
    for (const char *p = name; *p; p++) {
        char a = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
        if (a == '_' && p[1]) {
            char b = (char)((p[1] >= 'A' && p[1] <= 'Z') ? p[1] + 32 : p[1]);
            char c = (char)((p[2] >= 'A' && p[2] <= 'Z') ? p[2] + 32 : p[2]);
            if (b == 'l' && c == 'm') return 1;
        }
    }
    return 0;
}

static char *dupStr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void growBounds(Scene *sc, const SceneVertex *v) {
    if (v->x < sc->boundsMin[0]) sc->boundsMin[0] = v->x;
    if (v->y < sc->boundsMin[1]) sc->boundsMin[1] = v->y;
    if (v->z < sc->boundsMin[2]) sc->boundsMin[2] = v->z;
    if (v->x > sc->boundsMax[0]) sc->boundsMax[0] = v->x;
    if (v->y > sc->boundsMax[1]) sc->boundsMax[1] = v->y;
    if (v->z > sc->boundsMax[2]) sc->boundsMax[2] = v->z;
}

static SceneVertex convertVertex(const RMeshSurface *surf, uint32_t idx,
                                 int diffuseSlot, int lightmapSlot) {
    const RMeshVertex *rv = &surf->vertices[idx];
    SceneVertex v;
    v.x = rv->x;
    v.y = rv->y;
    v.z = rv->z;
    /* Texture slot j samples UV set (1 - j). */
    float uv[2][2] = { { rv->u0, rv->v0 }, { rv->u1, rv->v1 } };
    int dSet = diffuseSlot >= 0 ? 1 - diffuseSlot : 0;
    int lSet = lightmapSlot >= 0 ? 1 - lightmapSlot : 1;
    v.du = uv[dSet][0];
    v.dv = uv[dSet][1];
    v.lu = uv[lSet][0];
    v.lv = uv[lSet][1];
    v.r = rv->r;
    v.g = rv->g;
    v.b = rv->b;
    v.a = 255;
    return v;
}

/* Append one batch covering triangles [first, last) of surf, remapping
 * vertex indices into a compact local vertex array. Returns the number
 * of triangles consumed, or 0 on allocation failure. */
static uint32_t emitChunk(Scene *sc, const RMeshSurface *surf,
                          uint32_t firstTri,
                          int diffuseSlot, int lightmapSlot, int alphaClip) {
    uint32_t *remap = (uint32_t *)malloc(surf->vertexCount * sizeof(uint32_t));
    if (!remap && surf->vertexCount > 0) return 0;
    memset(remap, 0xFF, surf->vertexCount * sizeof(uint32_t));

    SceneBatch *grown = (SceneBatch *)realloc(
        sc->batches, (sc->batchCount + 1) * sizeof(SceneBatch));
    if (!grown) {
        free(remap);
        return 0;
    }
    sc->batches = grown;
    SceneBatch *b = &sc->batches[sc->batchCount];
    memset(b, 0, sizeof(*b));

    uint32_t maxTris = surf->triangleCount - firstTri;
    b->vertices = (SceneVertex *)malloc(
        (size_t)CHUNK_VERTEX_LIMIT * sizeof(SceneVertex));
    b->indices = (uint16_t *)malloc((size_t)maxTris * 3 * sizeof(uint16_t));
    if (!b->vertices || !b->indices) {
        free(b->vertices);
        free(b->indices);
        free(remap);
        return 0;
    }

    uint32_t tri = firstTri;
    for (; tri < surf->triangleCount; tri++) {
        const uint32_t *src = &surf->indices[tri * 3];
        /* A triangle can add up to 3 new vertices. */
        uint32_t newVerts = 0;
        for (int k = 0; k < 3; k++) {
            if (remap[src[k]] == 0xFFFFFFFFu) newVerts++;
        }
        if (b->vertexCount + newVerts > CHUNK_VERTEX_LIMIT) break;
        for (int k = 0; k < 3; k++) {
            uint32_t idx = src[k];
            if (remap[idx] == 0xFFFFFFFFu) {
                remap[idx] = b->vertexCount;
                b->vertices[b->vertexCount] =
                    convertVertex(surf, idx, diffuseSlot, lightmapSlot);
                growBounds(sc, &b->vertices[b->vertexCount]);
                b->vertexCount++;
            }
            b->indices[b->indexCount++] = (uint16_t)remap[idx];
        }
    }
    free(remap);

    /* Shrink to fit. */
    if (b->vertexCount > 0) {
        SceneVertex *sv = (SceneVertex *)realloc(
            b->vertices, b->vertexCount * sizeof(SceneVertex));
        if (sv) b->vertices = sv;
    }
    if (b->indexCount > 0) {
        uint16_t *si = (uint16_t *)realloc(
            b->indices, b->indexCount * sizeof(uint16_t));
        if (si) b->indices = si;
    }

    int dSlot = diffuseSlot;
    int lSlot = lightmapSlot;
    b->diffuseName = dSlot >= 0 ? dupStr(surf->textures[dSlot].name) : NULL;
    b->lightmapName = lSlot >= 0 ? dupStr(surf->textures[lSlot].name) : NULL;
    b->alphaClip = alphaClip;
    sc->batchCount++;
    return tri - firstTri;
}

Scene *sceneBuild(const RMesh *mesh) {
    Scene *sc = (Scene *)calloc(1, sizeof(Scene));
    if (!sc) return NULL;
    sc->boundsMin[0] = sc->boundsMin[1] = sc->boundsMin[2] = 1e30f;
    sc->boundsMax[0] = sc->boundsMax[1] = sc->boundsMax[2] = -1e30f;

    for (uint32_t i = 0; i < mesh->surfaceCount; i++) {
        const RMeshSurface *surf = &mesh->surfaces[i];
        if (surf->triangleCount == 0) continue;

        int lightmapSlot = -1, diffuseSlot = -1, alphaClip = 0;
        for (int j = 0; j < 2; j++) {
            if (surf->textures[j].flags == 0 || !surf->textures[j].name) {
                continue;
            }
            if (surf->textures[j].flags == 3) alphaClip = 1;
            if (nameIsLightmap(surf->textures[j].name)) {
                lightmapSlot = j;
            } else {
                diffuseSlot = j;
            }
        }

        uint32_t tri = 0;
        while (tri < surf->triangleCount) {
            uint32_t consumed = emitChunk(sc, surf, tri, diffuseSlot,
                                          lightmapSlot, alphaClip);
            if (consumed == 0) {
                sceneFree(sc);
                return NULL;
            }
            tri += consumed;
        }
    }

    if (sc->boundsMin[0] > sc->boundsMax[0]) {
        memset(sc->boundsMin, 0, sizeof(sc->boundsMin));
        memset(sc->boundsMax, 0, sizeof(sc->boundsMax));
    }
    return sc;
}

void sceneFree(Scene *sc) {
    if (!sc) return;
    for (uint32_t i = 0; i < sc->batchCount; i++) {
        free(sc->batches[i].diffuseName);
        free(sc->batches[i].lightmapName);
        free(sc->batches[i].vertices);
        free(sc->batches[i].indices);
    }
    free(sc->batches);
    free(sc);
}
