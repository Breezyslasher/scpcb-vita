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

/* ---- B3D props: 4x4 column-major matrix helpers ---- */

#include <math.h>

static void matIdentity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void matMul(float out[16], const float a[16], const float b[16]) {
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0]
                           + a[1 * 4 + row] * b[c * 4 + 1]
                           + a[2 * 4 + row] * b[c * 4 + 2]
                           + a[3 * 4 + row] * b[c * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

/* Blitz3D rotation: yaw (Y), then pitch (X, sign flipped), then roll (Z). */
static void matFromEulerBlitz(float m[16], const float eulerDeg[3]) {
    float p = -eulerDeg[0] * 3.14159265f / 180.0f;
    float y = eulerDeg[1] * 3.14159265f / 180.0f;
    float r = eulerDeg[2] * 3.14159265f / 180.0f;
    float cy = cosf(y), sy = sinf(y);
    float cp = cosf(p), sp = sinf(p);
    float cr = cosf(r), sr = sinf(r);
    float ry[16], rx[16], rz[16];
    matIdentity(ry);
    ry[0] = cy;  ry[2] = -sy; ry[8] = sy;  ry[10] = cy;
    matIdentity(rx);
    rx[5] = cp;  rx[6] = sp;  rx[9] = -sp; rx[10] = cp;
    matIdentity(rz);
    rz[0] = cr;  rz[1] = sr;  rz[4] = -sr; rz[5] = cr;
    matMul(m, ry, rx);
    matMul(m, m, rz);
}

static void matFromQuat(float m[16], const float q[4]) {
    /* b3d stores w,x,y,z */
    float w = q[0], x = q[1], y = q[2], z = q[3];
    float n = w * w + x * x + y * y + z * z;
    float s = n > 0.0f ? 2.0f / n : 0.0f;
    matIdentity(m);
    m[0] = 1 - s * (y * y + z * z);
    m[1] = s * (x * y + w * z);
    m[2] = s * (x * z - w * y);
    m[4] = s * (x * y - w * z);
    m[5] = 1 - s * (x * x + z * z);
    m[6] = s * (y * z + w * x);
    m[8] = s * (x * z + w * y);
    m[9] = s * (y * z - w * x);
    m[10] = 1 - s * (x * x + y * y);
}

static void matCompose(float out[16], const float pos[3], const float rot[16],
                       const float scale[3]) {
    float s[16], t[16];
    matIdentity(s);
    s[0] = scale[0];
    s[5] = scale[1];
    s[10] = scale[2];
    matIdentity(t);
    t[12] = pos[0];
    t[13] = pos[1];
    t[14] = pos[2];
    matMul(out, rot, s);
    matMul(out, t, out);
}

static void matApply(const float m[16], const float in[3], float out[3]) {
    for (int i = 0; i < 3; i++) {
        out[i] = m[0 + i] * in[0] + m[4 + i] * in[1] + m[8 + i] * in[2]
               + m[12 + i];
    }
}

static uint8_t clamp255(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)v;
}

static int appendB3DNode(Scene *sc, const B3DModel *model, const B3DNode *node,
                         const float parent[16], const char *textureOverride) {
    float rot[16], local[16], world[16];
    matFromQuat(rot, node->rotation);
    matCompose(local, node->position, rot, node->scale);
    matMul(world, parent, local);

    if (node->mesh && node->mesh->vertexCount > 0
        && node->mesh->vertexCount <= CHUNK_VERTEX_LIMIT) {
        const B3DMesh *mesh = node->mesh;
        for (uint32_t t = 0; t < mesh->triSetCount; t++) {
            const B3DTriSet *ts = &mesh->triSets[t];
            if (ts->triangleCount == 0) continue;

            int32_t brushId = ts->brushId >= 0 ? ts->brushId : mesh->brushId;
            const B3DBrush *brush =
                (brushId >= 0 && (uint32_t)brushId < model->brushCount)
                    ? &model->brushes[brushId] : NULL;
            const char *texName = NULL;
            if (textureOverride && textureOverride[0]) {
                texName = textureOverride;
            } else if (brush) {
                for (int i = 0; i < B3D_MAX_BRUSH_TEXTURES; i++) {
                    int32_t id = brush->textureIds[i];
                    if (id >= 0 && (uint32_t)id < model->textureCount) {
                        texName = model->textures[id].file;
                        break;
                    }
                }
            }
            float br = brush ? brush->r : 1.0f;
            float bg = brush ? brush->g : 1.0f;
            float bb = brush ? brush->b : 1.0f;

            SceneBatch *grown = (SceneBatch *)realloc(
                sc->batches, (sc->batchCount + 1) * sizeof(SceneBatch));
            if (!grown) return 0;
            sc->batches = grown;
            SceneBatch *b = &sc->batches[sc->batchCount];
            memset(b, 0, sizeof(*b));

            b->vertices = (SceneVertex *)malloc(
                mesh->vertexCount * sizeof(SceneVertex));
            b->indices = (uint16_t *)malloc(
                (size_t)ts->triangleCount * 3 * sizeof(uint16_t));
            if (!b->vertices || !b->indices) {
                free(b->vertices);
                free(b->indices);
                return 0;
            }
            for (uint32_t i = 0; i < mesh->vertexCount; i++) {
                const B3DVertex *bv = &mesh->vertices[i];
                SceneVertex *v = &b->vertices[i];
                float in[3] = { bv->x, bv->y, bv->z };
                float out[3];
                matApply(world, in, out);
                v->x = out[0];
                v->y = out[1];
                v->z = out[2];
                v->du = bv->u;
                v->dv = bv->v;
                v->lu = 0.0f;
                v->lv = 0.0f;
                v->r = clamp255(bv->r * br * 255.0f);
                v->g = clamp255(bv->g * bg * 255.0f);
                v->b = clamp255(bv->b * bb * 255.0f);
                v->a = 255;
                growBounds(sc, v);
            }
            b->vertexCount = mesh->vertexCount;
            for (uint32_t i = 0; i < ts->triangleCount * 3; i++) {
                b->indices[i] = (uint16_t)ts->indices[i];
            }
            b->indexCount = ts->triangleCount * 3;
            b->diffuseName = dupStr(texName);
            b->lightmapName = NULL;
            b->alphaClip = 0;
            sc->batchCount++;
        }
    }

    for (uint32_t i = 0; i < node->childCount; i++) {
        if (!appendB3DNode(sc, model, node->children[i], world,
                           textureOverride)) {
            return 0;
        }
    }
    return 1;
}

int sceneAppendB3D(Scene *sc, const B3DModel *model, const float pos[3],
                   const float eulerDeg[3], const float scale[3],
                   const char *textureOverride) {
    if (!model->root) return 1;
    float rot[16], base[16];
    matFromEulerBlitz(rot, eulerDeg);
    matCompose(base, pos, rot, scale);
    return appendB3DNode(sc, model, model->root, base, textureOverride);
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
