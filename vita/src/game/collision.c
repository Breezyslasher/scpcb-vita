#include "collision.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GRID_DIM 32
#define PUSH_ITERATIONS 3

typedef struct {
    float a[3], b[3], c[3];
} Tri;

typedef struct {
    uint32_t *tris;
    uint32_t count;
    uint32_t cap;
} Cell;

struct CollisionWorld {
    Tri *tris;
    uint32_t triCount;
    float min[3], max[3];
    float cellSize[3];
    Cell cells[GRID_DIM][GRID_DIM][GRID_DIM];
};

static void triBounds(const Tri *t, float mn[3], float mx[3]) {
    for (int i = 0; i < 3; i++) {
        mn[i] = fminf(t->a[i], fminf(t->b[i], t->c[i]));
        mx[i] = fmaxf(t->a[i], fmaxf(t->b[i], t->c[i]));
    }
}

static int cellIndex(const CollisionWorld *w, float v, int axis) {
    if (w->cellSize[axis] <= 0.0f) return 0;
    int i = (int)((v - w->min[axis]) / w->cellSize[axis]);
    if (i < 0) i = 0;
    if (i >= GRID_DIM) i = GRID_DIM - 1;
    return i;
}

static int cellAdd(Cell *cell, uint32_t tri) {
    if (cell->count == cell->cap) {
        uint32_t ncap = cell->cap ? cell->cap * 2 : 8;
        uint32_t *grown = (uint32_t *)realloc(cell->tris,
                                              ncap * sizeof(uint32_t));
        if (!grown) return 0;
        cell->tris = grown;
        cell->cap = ncap;
    }
    cell->tris[cell->count++] = tri;
    return 1;
}

static int addTri(CollisionWorld *w, const float a[3], const float b[3],
                  const float c[3]) {
    Tri *t = &w->tris[w->triCount];
    memcpy(t->a, a, sizeof(t->a));
    memcpy(t->b, b, sizeof(t->b));
    memcpy(t->c, c, sizeof(t->c));
    w->triCount++;
    return 1;
}

CollisionWorld *collisionBuild(const Scene *scene, const RMesh *mesh) {
    CollisionWorld *w = (CollisionWorld *)calloc(1, sizeof(CollisionWorld));
    if (!w) return NULL;

    uint32_t maxTris = 0;
    for (uint32_t i = 0; i < scene->batchCount; i++) {
        if (!scene->batches[i].alphaClip) {
            maxTris += scene->batches[i].indexCount / 3;
        }
    }
    if (mesh) {
        for (uint32_t i = 0; i < mesh->collisionSurfaceCount; i++) {
            maxTris += mesh->collisionSurfaces[i].triangleCount;
        }
    }
    w->tris = (Tri *)malloc((maxTris ? maxTris : 1) * sizeof(Tri));
    if (!w->tris) {
        free(w);
        return NULL;
    }

    for (uint32_t i = 0; i < scene->batchCount; i++) {
        const SceneBatch *b = &scene->batches[i];
        if (b->alphaClip) continue;
        for (uint32_t k = 0; k + 2 < b->indexCount; k += 3) {
            float v[3][3];
            for (int j = 0; j < 3; j++) {
                const SceneVertex *sv = &b->vertices[b->indices[k + j]];
                v[j][0] = sv->x;
                v[j][1] = sv->y;
                v[j][2] = sv->z;
            }
            addTri(w, v[0], v[1], v[2]);
        }
    }
    if (mesh) {
        for (uint32_t i = 0; i < mesh->collisionSurfaceCount; i++) {
            const RMeshCollisionSurface *s = &mesh->collisionSurfaces[i];
            for (uint32_t k = 0; k + 2 < s->triangleCount * 3; k += 3) {
                float v[3][3];
                for (int j = 0; j < 3; j++) {
                    const RMeshVec3 *sv = &s->vertices[s->indices[k + j]];
                    v[j][0] = sv->x;
                    v[j][1] = sv->y;
                    v[j][2] = sv->z;
                }
                addTri(w, v[0], v[1], v[2]);
            }
        }
    }

    /* Grid bounds and cells. */
    w->min[0] = w->min[1] = w->min[2] = 1e30f;
    w->max[0] = w->max[1] = w->max[2] = -1e30f;
    for (uint32_t i = 0; i < w->triCount; i++) {
        float mn[3], mx[3];
        triBounds(&w->tris[i], mn, mx);
        for (int k = 0; k < 3; k++) {
            if (mn[k] < w->min[k]) w->min[k] = mn[k];
            if (mx[k] > w->max[k]) w->max[k] = mx[k];
        }
    }
    if (w->triCount == 0) {
        memset(w->min, 0, sizeof(w->min));
        memset(w->max, 0, sizeof(w->max));
    }
    for (int k = 0; k < 3; k++) {
        w->cellSize[k] = (w->max[k] - w->min[k]) / GRID_DIM;
    }

    for (uint32_t i = 0; i < w->triCount; i++) {
        float mn[3], mx[3];
        triBounds(&w->tris[i], mn, mx);
        int x0 = cellIndex(w, mn[0], 0), x1 = cellIndex(w, mx[0], 0);
        int y0 = cellIndex(w, mn[1], 1), y1 = cellIndex(w, mx[1], 1);
        int z0 = cellIndex(w, mn[2], 2), z1 = cellIndex(w, mx[2], 2);
        for (int x = x0; x <= x1; x++) {
            for (int y = y0; y <= y1; y++) {
                for (int z = z0; z <= z1; z++) {
                    if (!cellAdd(&w->cells[x][y][z], i)) {
                        collisionFree(w);
                        return NULL;
                    }
                }
            }
        }
    }
    return w;
}

void collisionFree(CollisionWorld *w) {
    if (!w) return;
    for (int x = 0; x < GRID_DIM; x++) {
        for (int y = 0; y < GRID_DIM; y++) {
            for (int z = 0; z < GRID_DIM; z++) {
                free(w->cells[x][y][z].tris);
            }
        }
    }
    free(w->tris);
    free(w);
}

static void closestPointOnTri(const Tri *t, const float p[3], float out[3]) {
    float ab[3], ac[3], ap[3];
    for (int i = 0; i < 3; i++) {
        ab[i] = t->b[i] - t->a[i];
        ac[i] = t->c[i] - t->a[i];
        ap[i] = p[i] - t->a[i];
    }
    float d1 = ab[0] * ap[0] + ab[1] * ap[1] + ab[2] * ap[2];
    float d2 = ac[0] * ap[0] + ac[1] * ap[1] + ac[2] * ap[2];
    if (d1 <= 0.0f && d2 <= 0.0f) {
        memcpy(out, t->a, 3 * sizeof(float));
        return;
    }

    float bp[3];
    for (int i = 0; i < 3; i++) bp[i] = p[i] - t->b[i];
    float d3 = ab[0] * bp[0] + ab[1] * bp[1] + ab[2] * bp[2];
    float d4 = ac[0] * bp[0] + ac[1] * bp[1] + ac[2] * bp[2];
    if (d3 >= 0.0f && d4 <= d3) {
        memcpy(out, t->b, 3 * sizeof(float));
        return;
    }

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        for (int i = 0; i < 3; i++) out[i] = t->a[i] + v * ab[i];
        return;
    }

    float cp[3];
    for (int i = 0; i < 3; i++) cp[i] = p[i] - t->c[i];
    float d5 = ab[0] * cp[0] + ab[1] * cp[1] + ab[2] * cp[2];
    float d6 = ac[0] * cp[0] + ac[1] * cp[1] + ac[2] * cp[2];
    if (d6 >= 0.0f && d5 <= d6) {
        memcpy(out, t->c, 3 * sizeof(float));
        return;
    }

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float u = d2 / (d2 - d6);
        for (int i = 0; i < 3; i++) out[i] = t->a[i] + u * ac[i];
        return;
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float u = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        for (int i = 0; i < 3; i++) {
            out[i] = t->b[i] + u * (t->c[i] - t->b[i]);
        }
        return;
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom, u2 = vc * denom;
    for (int i = 0; i < 3; i++) {
        out[i] = t->a[i] + ab[i] * v + ac[i] * u2;
    }
}

int collisionSpherePush(const CollisionWorld *w, float pos[3], float radius,
                        int *pushedUp) {
    int corrections = 0;
    if (pushedUp) *pushedUp = 0;

    for (int iter = 0; iter < PUSH_ITERATIONS; iter++) {
        int any = 0;
        int x0 = cellIndex(w, pos[0] - radius, 0);
        int x1 = cellIndex(w, pos[0] + radius, 0);
        int y0 = cellIndex(w, pos[1] - radius, 1);
        int y1 = cellIndex(w, pos[1] + radius, 1);
        int z0 = cellIndex(w, pos[2] - radius, 2);
        int z1 = cellIndex(w, pos[2] + radius, 2);
        for (int x = x0; x <= x1; x++) {
            for (int y = y0; y <= y1; y++) {
                for (int z = z0; z <= z1; z++) {
                    const Cell *cell = &w->cells[x][y][z];
                    for (uint32_t k = 0; k < cell->count; k++) {
                        const Tri *t = &w->tris[cell->tris[k]];
                        float cp[3];
                        closestPointOnTri(t, pos, cp);
                        float d[3] = { pos[0] - cp[0], pos[1] - cp[1],
                                       pos[2] - cp[2] };
                        float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                        if (dist2 >= radius * radius || dist2 < 1e-12f) {
                            continue;
                        }
                        float dist = sqrtf(dist2);
                        float push = (radius - dist) / dist;
                        pos[0] += d[0] * push;
                        pos[1] += d[1] * push;
                        pos[2] += d[2] * push;
                        if (pushedUp && d[1] / dist > 0.5f) *pushedUp = 1;
                        corrections++;
                        any = 1;
                    }
                }
            }
        }
        if (!any) break;
    }
    return corrections;
}

static int rayTri(const float o[3], const float dir[3], const Tri *t,
                  float *outT) {
    float e1[3], e2[3];
    for (int i = 0; i < 3; i++) {
        e1[i] = t->b[i] - t->a[i];
        e2[i] = t->c[i] - t->a[i];
    }
    float p[3] = { dir[1] * e2[2] - dir[2] * e2[1],
                   dir[2] * e2[0] - dir[0] * e2[2],
                   dir[0] * e2[1] - dir[1] * e2[0] };
    float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (fabsf(det) < 1e-9f) return 0;
    float inv = 1.0f / det;
    float s[3] = { o[0] - t->a[0], o[1] - t->a[1], o[2] - t->a[2] };
    float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0.0f || u > 1.0f) return 0;
    float q[3] = { s[1] * e1[2] - s[2] * e1[1],
                   s[2] * e1[0] - s[0] * e1[2],
                   s[0] * e1[1] - s[1] * e1[0] };
    float v = (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]) * inv;
    if (v < 0.0f || u + v > 1.0f) return 0;
    float tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    if (tt < 0.0f) return 0;
    *outT = tt;
    return 1;
}

int collisionRayDown(const CollisionWorld *w, const float origin[3],
                     float maxDist, float *hitY) {
    static const float dir[3] = { 0.0f, -1.0f, 0.0f };
    float best = maxDist;
    int hit = 0;

    int x = cellIndex(w, origin[0], 0);
    int z = cellIndex(w, origin[2], 2);
    int y0 = cellIndex(w, origin[1] - maxDist, 1);
    int y1 = cellIndex(w, origin[1], 1);
    for (int y = y0; y <= y1; y++) {
        const Cell *cell = &w->cells[x][y][z];
        for (uint32_t k = 0; k < cell->count; k++) {
            float t;
            if (rayTri(origin, dir, &w->tris[cell->tris[k]], &t) && t < best) {
                best = t;
                hit = 1;
            }
        }
    }
    if (hit) *hitY = origin[1] - best;
    return hit;
}

int collisionRayHit(const CollisionWorld *w, const float origin[3],
                    const float dirIn[3], float maxDist) {
    if (w->triCount == 0) return 0;
    float len = sqrtf(dirIn[0] * dirIn[0] + dirIn[1] * dirIn[1]
                      + dirIn[2] * dirIn[2]);
    if (len < 1e-9f) return 0;
    float dir[3] = { dirIn[0] / len, dirIn[1] / len, dirIn[2] / len };

    /* Amanatides-Woo voxel traversal over the uniform grid, testing the
     * triangles in each cell the ray crosses until it hits or leaves. */
    int cell[3] = { cellIndex(w, origin[0], 0), cellIndex(w, origin[1], 1),
                    cellIndex(w, origin[2], 2) };
    int step[3];
    float tMax[3], tDelta[3];
    for (int a = 0; a < 3; a++) {
        float cs = w->cellSize[a];
        if (cs <= 0.0f) cs = 1.0f;
        if (dir[a] > 1e-9f) {
            step[a] = 1;
            tMax[a] = (w->min[a] + (cell[a] + 1) * cs - origin[a]) / dir[a];
            tDelta[a] = cs / dir[a];
        } else if (dir[a] < -1e-9f) {
            step[a] = -1;
            tMax[a] = (w->min[a] + cell[a] * cs - origin[a]) / dir[a];
            tDelta[a] = -cs / dir[a];
        } else {
            step[a] = 0;
            tMax[a] = 1e30f;
            tDelta[a] = 1e30f;
        }
    }
    float t = 0.0f;
    for (int guard = 0; guard < GRID_DIM * 3 && t <= maxDist; guard++) {
        if (cell[0] >= 0 && cell[0] < GRID_DIM && cell[1] >= 0
            && cell[1] < GRID_DIM && cell[2] >= 0 && cell[2] < GRID_DIM) {
            const Cell *c = &w->cells[cell[0]][cell[1]][cell[2]];
            for (uint32_t k = 0; k < c->count; k++) {
                float tt;
                if (rayTri(origin, dir, &w->tris[c->tris[k]], &tt)
                    && tt >= 0.0f && tt <= maxDist) {
                    return 1;
                }
            }
        }
        int axis = 0;
        if (tMax[1] < tMax[0]) axis = 1;
        if (tMax[2] < tMax[axis]) axis = 2;
        if (step[axis] == 0) break;
        cell[axis] += step[axis];
        t = tMax[axis];
        tMax[axis] += tDelta[axis];
        if (cell[axis] < 0 || cell[axis] >= GRID_DIM) break;
    }
    return 0;
}
