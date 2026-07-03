#include "skin.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- small matrix kit, matching scene.c's mirroring conventions ---- */

static void mIdentity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mMul(float out[16], const float a[16], const float b[16]) {
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

/* b3d stores w,x,y,z. Unlike scene.c's static path (which flips x,y
 * to mirror Blitz space), skinning uses the full conjugate (x,y,z
 * flipped) for BOTH bind and key quaternions - what matters is that
 * bind and animation agree, and this pairing poses the CB rigs
 * correctly (verified against software renders of guard/class_d). */
static void mFromQuat(float m[16], const float q[4]) {
    float w = q[0], x = -q[1], y = -q[2], z = -q[3];
    float n = w * w + x * x + y * y + z * z;
    float s = n > 0.0f ? 2.0f / n : 0.0f;
    mIdentity(m);
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

static void mCompose(float out[16], const float pos[3], const float rot[16],
                     const float scale[3]) {
    float s[16], t[16];
    mIdentity(s);
    s[0] = scale[0];
    s[5] = scale[1];
    s[10] = scale[2];
    mIdentity(t);
    t[12] = pos[0];
    t[13] = pos[1];
    t[14] = pos[2];
    mMul(out, rot, s);
    mMul(out, t, out);
}

static void mApply(const float m[16], const float in[3], float out[3]) {
    for (int i = 0; i < 3; i++) {
        out[i] = m[0 + i] * in[0] + m[4 + i] * in[1] + m[8 + i] * in[2]
               + m[12 + i];
    }
}

/* General affine inverse (upper-left 3x3 + translation). */
static int mInvertAffine(float out[16], const float m[16]) {
    float a = m[0], b = m[4], c = m[8];
    float d = m[1], e = m[5], f = m[9];
    float g = m[2], h = m[6], i = m[10];
    float A = e * i - f * h, B = c * h - b * i, C = b * f - c * e;
    float D = f * g - d * i, E = a * i - c * g, F = c * d - a * f;
    float G = d * h - e * g, H = b * g - a * h, I = a * e - b * d;
    float det = a * A + d * B + g * C;
    if (fabsf(det) < 1e-12f) return 0;
    float inv = 1.0f / det;
    mIdentity(out);
    out[0] = A * inv;  out[4] = B * inv;  out[8] = C * inv;
    out[1] = D * inv;  out[5] = E * inv;  out[9] = F * inv;
    out[2] = G * inv;  out[6] = H * inv;  out[10] = I * inv;
    float t[3] = { m[12], m[13], m[14] };
    out[12] = -(out[0] * t[0] + out[4] * t[1] + out[8] * t[2]);
    out[13] = -(out[1] * t[0] + out[5] * t[1] + out[9] * t[2]);
    out[14] = -(out[2] * t[0] + out[6] * t[1] + out[10] * t[2]);
    return 1;
}

/* ---- skinned mesh ---- */

#define MAX_INFLUENCES 4

typedef struct {
    const B3DNode *node;
    int parent;              /* index into the flat array, -1 = root */
    float invBind[16];       /* inverse of the bind-pose global matrix */
    float skinMat[16];       /* animGlobal * invBind, per eval */
    /* Frame-sorted key pointers per channel (pos/scale/rot), so pose
     * evaluation is a binary search instead of scanning every key of
     * every merged KEYS chunk three times. */
    const B3DKey **chKeys[3];
    int chCount[3];
} SkinNode;

typedef struct {
    int bone[MAX_INFLUENCES];    /* flat node indices */
    float weight[MAX_INFLUENCES];
} VertexWeights;

struct SkinnedMesh {
    const B3DModel *model;
    const B3DMesh *mesh;     /* the skinned mesh (first mesh node) */
    int meshNodeIdx;

    SkinNode *nodes;
    int nodeCount;

    VertexWeights *weights;  /* per source mesh vertex */
    float *bindPos;          /* xyz per source vertex, model space */

    SceneVertex *verts;      /* output buffer, one per source vertex */
    uint32_t vertCount;

    SkinBatch *batches;
    uint32_t batchCount;

    float bindMin[3], bindMax[3];
    int frames;
    float fps;
};

static void countNodes(const B3DNode *n, int *count) {
    (*count)++;
    for (uint32_t i = 0; i < n->childCount; i++) {
        countNodes(n->children[i], count);
    }
}

static void flatten(SkinnedMesh *s, const B3DNode *n, int parent) {
    int idx = s->nodeCount++;
    s->nodes[idx].node = n;
    s->nodes[idx].parent = parent;
    for (uint32_t i = 0; i < n->childCount; i++) {
        flatten(s, n->children[i], idx);
    }
}

static int keyFrameCmp(const void *a, const void *b) {
    const B3DKey *ka = *(const B3DKey *const *)a;
    const B3DKey *kb = *(const B3DKey *const *)b;
    return ka->frame - kb->frame;
}

static int buildChannelKeys(SkinNode *sn) {
    const B3DNode *n = sn->node;
    for (int c = 0; c < 3; c++) {
        int flag = 1 << c;
        int count = 0;
        for (uint32_t i = 0; i < n->keyCount; i++) {
            if (n->keys[i].flags & flag) count++;
        }
        sn->chCount[c] = count;
        if (!count) continue;
        sn->chKeys[c] = (const B3DKey **)malloc(
            (size_t)count * sizeof(B3DKey *));
        if (!sn->chKeys[c]) return 0;
        int o = 0;
        for (uint32_t i = 0; i < n->keyCount; i++) {
            if (n->keys[i].flags & flag) sn->chKeys[c][o++] = &n->keys[i];
        }
        qsort(sn->chKeys[c], (size_t)count, sizeof(B3DKey *), keyFrameCmp);
    }
    return 1;
}

static void nodeLocal(const B3DNode *n, float out[16]) {
    float rot[16];
    mFromQuat(rot, n->rotation);
    mCompose(out, n->position, rot, n->scale);
}

static const char *brushTexture(const B3DModel *m, int32_t brushId) {
    if (brushId < 0 || (uint32_t)brushId >= m->brushCount) return NULL;
    const B3DBrush *b = &m->brushes[brushId];
    for (int i = 0; i < B3D_MAX_BRUSH_TEXTURES; i++) {
        int32_t t = b->textureIds[i];
        if (t >= 0 && (uint32_t)t < m->textureCount
            && m->textures[t].file) {
            const char *f = m->textures[t].file;
            const char *base = strrchr(f, '\\');
            const char *base2 = strrchr(f, '/');
            if (base2 > base) base = base2;
            return base ? base + 1 : f;
        }
    }
    return NULL;
}

SkinnedMesh *skinnedCreate(const B3DModel *model) {
    if (!model || !model->root || model->animFrames <= 0) return NULL;

    SkinnedMesh *s = (SkinnedMesh *)calloc(1, sizeof(SkinnedMesh));
    if (!s) return NULL;
    s->model = model;
    s->frames = model->animFrames;
    s->fps = model->animFps;

    int total = 0;
    countNodes(model->root, &total);
    s->nodes = (SkinNode *)calloc((size_t)total, sizeof(SkinNode));
    if (!s->nodes) goto fail;
    flatten(s, model->root, -1);

    /* The skinned mesh: the first node carrying one. */
    s->meshNodeIdx = -1;
    int hasBones = 0;
    for (int i = 0; i < s->nodeCount; i++) {
        if (s->nodes[i].node->mesh && s->meshNodeIdx < 0) s->meshNodeIdx = i;
        if (s->nodes[i].node->weightCount > 0) hasBones = 1;
    }
    if (s->meshNodeIdx < 0 || !hasBones) goto fail;
    for (int i = 0; i < s->nodeCount; i++) {
        if (!buildChannelKeys(&s->nodes[i])) goto fail;
    }
    s->mesh = s->nodes[s->meshNodeIdx].node->mesh;
    s->vertCount = s->mesh->vertexCount;
    if (s->vertCount == 0 || s->vertCount > 65535) goto fail;

    /* Bind-pose globals -> inverse bind matrices. Scratch globals
     * are reused for evaluation later. */
    {
        float (*global)[16] = (float (*)[16])malloc(
            (size_t)s->nodeCount * 16 * sizeof(float));
        if (!global) goto fail;
        for (int i = 0; i < s->nodeCount; i++) {
            float local[16];
            nodeLocal(s->nodes[i].node, local);
            if (s->nodes[i].parent >= 0) {
                mMul(global[i], global[s->nodes[i].parent], local);
            } else {
                memcpy(global[i], local, sizeof(local));
            }
        }
        for (int i = 0; i < s->nodeCount; i++) {
            if (!mInvertAffine(s->nodes[i].invBind, global[i])) {
                mIdentity(s->nodes[i].invBind);
            }
        }

        /* Bind positions in model space. */
        s->bindPos = (float *)malloc((size_t)s->vertCount * 3 * sizeof(float));
        if (!s->bindPos) {
            free(global);
            goto fail;
        }
        for (uint32_t v = 0; v < s->vertCount; v++) {
            float in[3] = { s->mesh->vertices[v].x, s->mesh->vertices[v].y,
                            s->mesh->vertices[v].z };
            mApply(global[s->meshNodeIdx], in, &s->bindPos[v * 3]);
        }
        free(global);
    }

    /* Gather up to MAX_INFLUENCES bone weights per vertex. */
    s->weights = (VertexWeights *)calloc(s->vertCount, sizeof(VertexWeights));
    if (!s->weights) goto fail;
    for (uint32_t v = 0; v < s->vertCount; v++) {
        for (int k = 0; k < MAX_INFLUENCES; k++) s->weights[v].bone[k] = -1;
    }
    for (int i = 0; i < s->nodeCount; i++) {
        const B3DNode *n = s->nodes[i].node;
        for (uint32_t w = 0; w < n->weightCount; w++) {
            int32_t v = n->weights[w].vertexId;
            float wt = n->weights[w].weight;
            if (v < 0 || (uint32_t)v >= s->vertCount || wt <= 0.0f) continue;
            VertexWeights *vw = &s->weights[v];
            int slot = -1;
            for (int k = 0; k < MAX_INFLUENCES; k++) {
                if (vw->bone[k] < 0) {
                    slot = k;
                    break;
                }
            }
            if (slot < 0) { /* replace the weakest influence */
                slot = 0;
                for (int k = 1; k < MAX_INFLUENCES; k++) {
                    if (vw->weight[k] < vw->weight[slot]) slot = k;
                }
                if (vw->weight[slot] >= wt) continue;
            }
            vw->bone[slot] = i;
            vw->weight[slot] = wt;
        }
    }
    for (uint32_t v = 0; v < s->vertCount; v++) {
        VertexWeights *vw = &s->weights[v];
        float sum = 0.0f;
        for (int k = 0; k < MAX_INFLUENCES; k++) {
            if (vw->bone[k] >= 0) sum += vw->weight[k];
        }
        if (sum <= 0.0f) { /* unweighted: rigid to the mesh node */
            vw->bone[0] = s->meshNodeIdx;
            vw->weight[0] = 1.0f;
        } else {
            for (int k = 0; k < MAX_INFLUENCES; k++) vw->weight[k] /= sum;
        }
    }

    /* Static vertex attributes (UV, color); positions filled per eval. */
    s->verts = (SceneVertex *)calloc(s->vertCount, sizeof(SceneVertex));
    if (!s->verts) goto fail;
    for (uint32_t v = 0; v < s->vertCount; v++) {
        const B3DVertex *src = &s->mesh->vertices[v];
        SceneVertex *dst = &s->verts[v];
        dst->du = src->u;
        dst->dv = src->v;
        dst->r = (uint8_t)(src->r * 255.0f);
        dst->g = (uint8_t)(src->g * 255.0f);
        dst->b = (uint8_t)(src->b * 255.0f);
        dst->a = (uint8_t)(src->a * 255.0f);
    }

    /* One batch per tri set. */
    s->batches = (SkinBatch *)calloc(
        s->mesh->triSetCount ? s->mesh->triSetCount : 1, sizeof(SkinBatch));
    if (!s->batches) goto fail;
    for (uint32_t t = 0; t < s->mesh->triSetCount; t++) {
        const B3DTriSet *ts = &s->mesh->triSets[t];
        SkinBatch *b = &s->batches[s->batchCount];
        int32_t brush = ts->brushId >= 0 ? ts->brushId : s->mesh->brushId;
        const char *tex = brushTexture(model, brush);
        b->textureName = tex ? strdup(tex) : NULL;
        b->indexCount = ts->triangleCount * 3;
        b->indices = (uint16_t *)malloc(
            (size_t)(b->indexCount ? b->indexCount : 1) * sizeof(uint16_t));
        if (!b->indices) goto fail;
        for (uint32_t k = 0; k < b->indexCount; k++) {
            b->indices[k] = (uint16_t)ts->indices[k];
        }
        s->batchCount++;
    }

    /* Bind-pose bounds via a frame-0 evaluation. */
    skinnedEval(s, 0.0f);
    s->bindMin[0] = s->bindMin[1] = s->bindMin[2] = 1e30f;
    s->bindMax[0] = s->bindMax[1] = s->bindMax[2] = -1e30f;
    for (uint32_t v = 0; v < s->vertCount; v++) {
        float p[3] = { s->verts[v].x, s->verts[v].y, s->verts[v].z };
        for (int k = 0; k < 3; k++) {
            if (p[k] < s->bindMin[k]) s->bindMin[k] = p[k];
            if (p[k] > s->bindMax[k]) s->bindMax[k] = p[k];
        }
    }
    return s;

fail:
    skinnedFree(s);
    return NULL;
}

void skinnedFree(SkinnedMesh *s) {
    if (!s) return;
    for (int i = 0; i < s->nodeCount; i++) {
        for (int c = 0; c < 3; c++) free(s->nodes[i].chKeys[c]);
    }
    free(s->nodes);
    free(s->weights);
    free(s->bindPos);
    free(s->verts);
    for (uint32_t i = 0; i < s->batchCount; i++) {
        free(s->batches[i].textureName);
        free(s->batches[i].indices);
    }
    free(s->batches);
    free(s);
}

int skinnedFrames(const SkinnedMesh *s) { return s ? s->frames : 0; }
float skinnedFps(const SkinnedMesh *s) { return s ? s->fps : 60.0f; }

void skinnedBounds(const SkinnedMesh *s, float mn[3], float mx[3]) {
    memcpy(mn, s->bindMin, sizeof(s->bindMin));
    memcpy(mx, s->bindMax, sizeof(s->bindMax));
}

/* Bracketing keys for one channel via binary search over the
 * frame-sorted per-channel arrays. Returns 0 if the node has no keys
 * for the channel. */
static int chBracket(const SkinNode *sn, float frame, int ch,
                     const B3DKey **prevOut, const B3DKey **nextOut) {
    int count = sn->chCount[ch];
    if (!count) return 0;
    const B3DKey **keys = sn->chKeys[ch];
    /* last key with key->frame <= frame */
    int lo = 0, hi = count - 1, at = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if ((float)keys[mid]->frame <= frame) {
            at = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    const B3DKey *prev = at >= 0 ? keys[at] : keys[0];
    const B3DKey *next = at + 1 < count ? keys[at + 1] : keys[count - 1];
    if (at < 0) next = keys[0];
    *prevOut = prev;
    *nextOut = next;
    return 1;
}

static float chT(const B3DKey *prev, const B3DKey *next, float frame) {
    if (next->frame <= prev->frame) return 0.0f;
    float t = (frame - (float)prev->frame)
            / (float)(next->frame - prev->frame);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

/* Interpolated node transform at `frame`; nodes without keys keep
 * their base transform. */
static void animLocal(const SkinNode *sn, float frame, float out[16]) {
    const B3DNode *n = sn->node;
    if (n->keyCount == 0) {
        nodeLocal(n, out);
        return;
    }

    float pos[3], scl[3], quat[4];
    int quatFromKey = 0;
    memcpy(pos, n->position, sizeof(pos));
    memcpy(scl, n->scale, sizeof(scl));
    memcpy(quat, n->rotation, sizeof(quat));

    const B3DKey *prev, *next;
    if (chBracket(sn, frame, 0, &prev, &next)) {
        float t = chT(prev, next, frame);
        for (int i = 0; i < 3; i++) {
            pos[i] = prev->position[i]
                   + (next->position[i] - prev->position[i]) * t;
        }
    }
    if (chBracket(sn, frame, 1, &prev, &next)) {
        float t = chT(prev, next, frame);
        for (int i = 0; i < 3; i++) {
            scl[i] = prev->scale[i] + (next->scale[i] - prev->scale[i]) * t;
        }
    }
    if (chBracket(sn, frame, 2, &prev, &next)) {
        float t = chT(prev, next, frame);
        /* nlerp with hemisphere correction */
        float dot = prev->rotation[0] * next->rotation[0]
                  + prev->rotation[1] * next->rotation[1]
                  + prev->rotation[2] * next->rotation[2]
                  + prev->rotation[3] * next->rotation[3];
        float sign = dot < 0.0f ? -1.0f : 1.0f;
        float len2 = 0.0f;
        for (int i = 0; i < 4; i++) {
            quat[i] = prev->rotation[i] * (1.0f - t)
                    + next->rotation[i] * sign * t;
            len2 += quat[i] * quat[i];
        }
        if (len2 > 1e-12f) {
            float inv = 1.0f / sqrtf(len2);
            for (int i = 0; i < 4; i++) quat[i] *= inv;
            quatFromKey = 1;
        } else {
            memcpy(quat, n->rotation, sizeof(quat));
        }
    }

    (void)quatFromKey;
    float rot[16];
    mFromQuat(rot, quat);
    mCompose(out, pos, rot, scl);
}

SceneVertex *skinnedNewBuffer(const SkinnedMesh *s) {
    SceneVertex *buf = (SceneVertex *)malloc(
        (size_t)s->vertCount * sizeof(SceneVertex));
    if (buf) memcpy(buf, s->verts, (size_t)s->vertCount * sizeof(SceneVertex));
    return buf;
}

void skinnedEvalInto(SkinnedMesh *s, float frame, SceneVertex *out) {
    skinnedEval(s, frame);
    memcpy(out, s->verts, (size_t)s->vertCount * sizeof(SceneVertex));
}

void skinnedEval(SkinnedMesh *s, float frame) {
    if (!s) return;
    if (frame < 0.0f) frame = 0.0f;
    if (frame > (float)s->frames) frame = (float)s->frames;

    /* Pose the skeleton. */
    static float stackGlobal[256][16];
    float (*global)[16] = stackGlobal;
    float (*heap)[16] = NULL;
    if (s->nodeCount > 256) {
        heap = (float (*)[16])malloc((size_t)s->nodeCount * 16
                                     * sizeof(float));
        if (!heap) return;
        global = heap;
    }
    for (int i = 0; i < s->nodeCount; i++) {
        float local[16];
        animLocal(&s->nodes[i], frame, local);
        if (s->nodes[i].parent >= 0) {
            mMul(global[i], global[s->nodes[i].parent], local);
        } else {
            memcpy(global[i], local, sizeof(local));
        }
        mMul(s->nodes[i].skinMat, global[i], s->nodes[i].invBind);
    }
    free(heap);

    /* Skin the vertices. */
    for (uint32_t v = 0; v < s->vertCount; v++) {
        const VertexWeights *vw = &s->weights[v];
        const float *bp = &s->bindPos[v * 3];
        float out[3] = { 0, 0, 0 };
        for (int k = 0; k < MAX_INFLUENCES; k++) {
            if (vw->bone[k] < 0) continue;
            float p[3];
            mApply(s->nodes[vw->bone[k]].skinMat, bp, p);
            out[0] += p[0] * vw->weight[k];
            out[1] += p[1] * vw->weight[k];
            out[2] += p[2] * vw->weight[k];
        }
        s->verts[v].x = out[0];
        s->verts[v].y = out[1];
        s->verts[v].z = out[2];
    }
}

const SceneVertex *skinnedVertices(const SkinnedMesh *s, uint32_t *count) {
    if (count) *count = s->vertCount;
    return s->verts;
}

uint32_t skinnedBatchCount(const SkinnedMesh *s) { return s->batchCount; }

const SkinBatch *skinnedBatch(const SkinnedMesh *s, uint32_t i) {
    return &s->batches[i];
}
