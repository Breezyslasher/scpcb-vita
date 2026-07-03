#include "b3d.h"
#include "reader.h"

#include <stdio.h>

static void setErr(char *err, size_t errLen, const char *msg) {
    if (err && errLen > 0) {
        snprintf(err, errLen, "%s", msg);
    }
}

#define MAX_COUNT (4 * 1024 * 1024)

typedef struct {
    char tag[5];
    size_t end; /* absolute offset where this chunk's payload ends */
} Chunk;

static bool readChunkHeader(Reader *r, Chunk *c) {
    if (!rdHas(r, 8)) return false;
    memcpy(c->tag, r->data + r->pos, 4);
    c->tag[4] = '\0';
    r->pos += 4;
    int32_t len = rdI32(r);
    if (len < 0 || (size_t)len > r->size - r->pos) {
        r->failed = true;
        return false;
    }
    c->end = r->pos + (size_t)len;
    return true;
}

static bool isTag(const Chunk *c, const char *tag) {
    return memcmp(c->tag, tag, 4) == 0;
}

static void freeNode(B3DNode *n) {
    if (!n) return;
    free(n->name);
    free(n->weights);
    free(n->keys);
    if (n->mesh) {
        free(n->mesh->vertices);
        for (uint32_t i = 0; i < n->mesh->triSetCount; i++) {
            free(n->mesh->triSets[i].indices);
        }
        free(n->mesh->triSets);
        free(n->mesh);
    }
    for (uint32_t i = 0; i < n->childCount; i++) {
        freeNode(n->children[i]);
    }
    free(n->children);
    free(n);
}

static bool parseTexs(Reader *r, const Chunk *c, B3DModel *m) {
    /* Entries are variable-length; collect into a growing array. */
    while (rdOk(r) && r->pos < c->end) {
        B3DTexture *grown = (B3DTexture *)realloc(
            m->textures, (m->textureCount + 1) * sizeof(B3DTexture));
        if (!grown) return false;
        m->textures = grown;
        B3DTexture *t = &m->textures[m->textureCount];
        memset(t, 0, sizeof(*t));
        t->file = rdCString(r);
        t->flags = rdI32(r);
        t->blend = rdI32(r);
        t->posX = rdF32(r);
        t->posY = rdF32(r);
        t->scaleX = rdF32(r);
        t->scaleY = rdF32(r);
        t->rotation = rdF32(r);
        if (!rdOk(r)) {
            free(t->file);
            return false;
        }
        m->textureCount++;
    }
    return rdOk(r) && r->pos == c->end;
}

static bool parseBrus(Reader *r, const Chunk *c, B3DModel *m) {
    int32_t nTexs = rdI32(r);
    if (!rdOk(r) || nTexs < 0 || nTexs > 64) return false;
    while (rdOk(r) && r->pos < c->end) {
        B3DBrush *grown = (B3DBrush *)realloc(
            m->brushes, (m->brushCount + 1) * sizeof(B3DBrush));
        if (!grown) return false;
        m->brushes = grown;
        B3DBrush *b = &m->brushes[m->brushCount];
        memset(b, 0, sizeof(*b));
        for (int i = 0; i < B3D_MAX_BRUSH_TEXTURES; i++) {
            b->textureIds[i] = -1;
        }
        b->name = rdCString(r);
        b->r = rdF32(r);
        b->g = rdF32(r);
        b->b = rdF32(r);
        b->a = rdF32(r);
        b->shininess = rdF32(r);
        b->blend = rdI32(r);
        b->fx = rdI32(r);
        b->textureSlots = nTexs;
        for (int32_t i = 0; i < nTexs; i++) {
            int32_t id = rdI32(r);
            if (i < B3D_MAX_BRUSH_TEXTURES) {
                b->textureIds[i] = id;
            }
        }
        if (!rdOk(r)) {
            free(b->name);
            return false;
        }
        m->brushCount++;
    }
    return rdOk(r) && r->pos == c->end;
}

static bool parseVrts(Reader *r, const Chunk *c, B3DMesh *mesh) {
    int32_t flags = rdI32(r);
    int32_t texSets = rdI32(r);
    int32_t texSetSize = rdI32(r);
    if (!rdOk(r) || texSets < 0 || texSets > 8 || texSetSize < 0 || texSetSize > 4) {
        return false;
    }
    mesh->hasNormals = (flags & 1) != 0;
    mesh->hasColors = (flags & 2) != 0;

    size_t floatsPerVertex = 3
        + (mesh->hasNormals ? 3 : 0)
        + (mesh->hasColors ? 4 : 0)
        + (size_t)texSets * (size_t)texSetSize;
    size_t bytesPerVertex = floatsPerVertex * 4;
    size_t payload = c->end - r->pos;
    if (bytesPerVertex == 0 || payload % bytesPerVertex != 0) return false;
    size_t count = payload / bytesPerVertex;
    if (count > MAX_COUNT) return false;

    mesh->vertexCount = (uint32_t)count;
    mesh->vertices = (B3DVertex *)calloc(count ? count : 1, sizeof(B3DVertex));
    if (!mesh->vertices) return false;

    for (size_t i = 0; i < count; i++) {
        B3DVertex *v = &mesh->vertices[i];
        v->x = rdF32(r); v->y = rdF32(r); v->z = rdF32(r);
        if (mesh->hasNormals) {
            v->nx = rdF32(r); v->ny = rdF32(r); v->nz = rdF32(r);
        }
        if (mesh->hasColors) {
            v->r = rdF32(r); v->g = rdF32(r); v->b = rdF32(r); v->a = rdF32(r);
        } else {
            v->r = v->g = v->b = v->a = 1.0f;
        }
        for (int32_t s = 0; s < texSets; s++) {
            for (int32_t k = 0; k < texSetSize; k++) {
                float coord = rdF32(r);
                if (s == 0 && k == 0) v->u = coord;
                if (s == 0 && k == 1) v->v = coord;
            }
        }
    }
    return rdOk(r);
}

static bool parseTris(Reader *r, const Chunk *c, B3DMesh *mesh) {
    int32_t brushId = rdI32(r);
    if (!rdOk(r)) return false;
    size_t payload = c->end - r->pos;
    if (payload % 12 != 0) return false;
    size_t triCount = payload / 12;
    if (triCount > MAX_COUNT) return false;

    B3DTriSet *grown = (B3DTriSet *)realloc(
        mesh->triSets, (mesh->triSetCount + 1) * sizeof(B3DTriSet));
    if (!grown) return false;
    mesh->triSets = grown;
    B3DTriSet *ts = &mesh->triSets[mesh->triSetCount];
    memset(ts, 0, sizeof(*ts));
    ts->brushId = brushId;
    ts->triangleCount = (uint32_t)triCount;
    ts->indices = (uint32_t *)calloc(triCount ? triCount * 3 : 1, sizeof(uint32_t));
    if (!ts->indices) return false;
    mesh->triSetCount++;

    for (size_t i = 0; i < triCount * 3; i++) {
        int32_t idx = rdI32(r);
        if (idx < 0 || (uint32_t)idx >= mesh->vertexCount) {
            r->failed = true;
        }
        ts->indices[i] = (uint32_t)idx;
    }
    return rdOk(r);
}

/* ANIM appears inside the animated NODE; stashed here and picked up
 * by b3dLoadMemory after the tree parse. */
static int32_t gAnimFrames;
static float gAnimFps;

static B3DMesh *parseMesh(Reader *r, const Chunk *c) {
    B3DMesh *mesh = (B3DMesh *)calloc(1, sizeof(B3DMesh));
    if (!mesh) return NULL;
    mesh->brushId = rdI32(r);

    while (rdOk(r) && r->pos < c->end) {
        Chunk sub;
        if (!readChunkHeader(r, &sub) || sub.end > c->end) goto fail;
        if (isTag(&sub, "VRTS")) {
            if (!parseVrts(r, &sub, mesh)) goto fail;
        } else if (isTag(&sub, "TRIS")) {
            if (!parseTris(r, &sub, mesh)) goto fail;
        }
        r->pos = sub.end; /* skip anything unknown / trailing bytes */
    }
    if (!rdOk(r)) goto fail;
    return mesh;

fail:
    free(mesh->vertices);
    for (uint32_t i = 0; i < mesh->triSetCount; i++) {
        free(mesh->triSets[i].indices);
    }
    free(mesh->triSets);
    free(mesh);
    return NULL;
}

static bool parseKeys(Reader *r, const Chunk *c, B3DNode *n) {
    int32_t flags = rdI32(r);
    if (!rdOk(r) || (flags & ~7) != 0) return false;
    size_t entry = 4 + ((flags & 1) ? 12 : 0) + ((flags & 2) ? 12 : 0)
                 + ((flags & 4) ? 16 : 0);
    size_t payload = c->end - r->pos;
    if (entry == 4 || payload % entry != 0) return false;
    size_t count = payload / entry;
    if (count > MAX_COUNT) return false;

    B3DKey *grown = (B3DKey *)realloc(
        n->keys, (n->keyCount + count) * sizeof(B3DKey));
    if (!grown) return false;
    n->keys = grown;
    n->keyFlags |= flags;
    for (size_t i = 0; i < count; i++) {
        B3DKey *k = &n->keys[n->keyCount + i];
        memset(k, 0, sizeof(*k));
        k->scale[0] = k->scale[1] = k->scale[2] = 1.0f;
        k->rotation[0] = 1.0f;
        k->frame = rdI32(r);
        if (flags & 1) {
            k->position[0] = rdF32(r);
            k->position[1] = rdF32(r);
            k->position[2] = rdF32(r);
        }
        if (flags & 2) {
            k->scale[0] = rdF32(r);
            k->scale[1] = rdF32(r);
            k->scale[2] = rdF32(r);
        }
        if (flags & 4) {
            k->rotation[0] = rdF32(r);
            k->rotation[1] = rdF32(r);
            k->rotation[2] = rdF32(r);
            k->rotation[3] = rdF32(r);
        }
    }
    if (!rdOk(r)) return false;
    n->keyCount += (uint32_t)count;
    return true;
}

static bool parseBone(Reader *r, const Chunk *c, B3DNode *n) {
    n->isBone = 1;
    size_t payload = c->end - r->pos;
    if (payload % 8 != 0) return false;
    size_t count = payload / 8;
    if (count > MAX_COUNT) return false;
    if (count == 0) return true;
    n->weights = (B3DBoneWeight *)calloc(count, sizeof(B3DBoneWeight));
    if (!n->weights) return false;
    for (size_t i = 0; i < count; i++) {
        n->weights[i].vertexId = rdI32(r);
        n->weights[i].weight = rdF32(r);
    }
    if (!rdOk(r)) return false;
    n->weightCount = (uint32_t)count;
    return true;
}

static B3DNode *parseNode(Reader *r, const Chunk *c, int depth) {
    if (depth > 64) {
        r->failed = true;
        return NULL;
    }
    B3DNode *n = (B3DNode *)calloc(1, sizeof(B3DNode));
    if (!n) return NULL;

    n->name = rdCString(r);
    n->position[0] = rdF32(r);
    n->position[1] = rdF32(r);
    n->position[2] = rdF32(r);
    n->scale[0] = rdF32(r);
    n->scale[1] = rdF32(r);
    n->scale[2] = rdF32(r);
    n->rotation[0] = rdF32(r);
    n->rotation[1] = rdF32(r);
    n->rotation[2] = rdF32(r);
    n->rotation[3] = rdF32(r);
    if (!rdOk(r)) goto fail;

    while (rdOk(r) && r->pos < c->end) {
        Chunk sub;
        if (!readChunkHeader(r, &sub) || sub.end > c->end) goto fail;
        if (isTag(&sub, "MESH")) {
            n->mesh = parseMesh(r, &sub);
            if (!n->mesh) goto fail;
        } else if (isTag(&sub, "KEYS")) {
            if (!parseKeys(r, &sub, n)) goto fail;
        } else if (isTag(&sub, "BONE")) {
            if (!parseBone(r, &sub, n)) goto fail;
        } else if (isTag(&sub, "ANIM")) {
            rdI32(r); /* flags, unused */
            gAnimFrames = rdI32(r);
            gAnimFps = rdF32(r);
            if (gAnimFps <= 0.0f) gAnimFps = 60.0f;
        } else if (isTag(&sub, "NODE")) {
            B3DNode *child = parseNode(r, &sub, depth + 1);
            if (!child) goto fail;
            B3DNode **grown = (B3DNode **)realloc(
                n->children, (n->childCount + 1) * sizeof(B3DNode *));
            if (!grown) {
                freeNode(child);
                goto fail;
            }
            n->children = grown;
            n->children[n->childCount++] = child;
        }
        r->pos = sub.end; /* skip anything unknown / trailing bytes */
    }
    if (!rdOk(r)) goto fail;
    return n;

fail:
    freeNode(n);
    return NULL;
}

B3DModel *b3dLoadMemory(const void *data, size_t size, char *err, size_t errLen) {
    Reader r;
    rdInit(&r, data, size);
    setErr(err, errLen, "");

    Chunk root;
    if (!readChunkHeader(&r, &root) || !isTag(&root, "BB3D")) {
        setErr(err, errLen, "not a BB3D file");
        return NULL;
    }

    B3DModel *m = (B3DModel *)calloc(1, sizeof(B3DModel));
    if (!m) {
        setErr(err, errLen, "out of memory");
        return NULL;
    }
    m->version = rdI32(&r);
    gAnimFrames = 0;
    gAnimFps = 60.0f;

    while (rdOk(&r) && r.pos < root.end) {
        Chunk sub;
        if (!readChunkHeader(&r, &sub) || sub.end > root.end) {
            setErr(err, errLen, "corrupt chunk header");
            b3dFree(m);
            return NULL;
        }
        if (isTag(&sub, "TEXS")) {
            if (!parseTexs(&r, &sub, m)) {
                setErr(err, errLen, "failed parsing TEXS");
                b3dFree(m);
                return NULL;
            }
        } else if (isTag(&sub, "BRUS")) {
            if (!parseBrus(&r, &sub, m)) {
                setErr(err, errLen, "failed parsing BRUS");
                b3dFree(m);
                return NULL;
            }
        } else if (isTag(&sub, "NODE")) {
            m->root = parseNode(&r, &sub, 0);
            if (!m->root) {
                setErr(err, errLen, "failed parsing NODE");
                b3dFree(m);
                return NULL;
            }
        }
        r.pos = sub.end;
    }
    if (!rdOk(&r)) {
        setErr(err, errLen, "unexpected end of file");
        b3dFree(m);
        return NULL;
    }
    m->animFrames = gAnimFrames;
    m->animFps = gAnimFps;
    return m;
}

B3DModel *b3dLoadFile(const char *path, char *err, size_t errLen) {
    size_t size = 0;
    void *data = readWholeFile(path, &size);
    if (!data) {
        setErr(err, errLen, "could not read file");
        return NULL;
    }
    B3DModel *m = b3dLoadMemory(data, size, err, errLen);
    free(data);
    return m;
}

void b3dFree(B3DModel *m) {
    if (!m) return;
    for (uint32_t i = 0; i < m->textureCount; i++) {
        free(m->textures[i].file);
    }
    free(m->textures);
    for (uint32_t i = 0; i < m->brushCount; i++) {
        free(m->brushes[i].name);
    }
    free(m->brushes);
    freeNode(m->root);
    free(m);
}
