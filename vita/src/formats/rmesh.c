#include "rmesh.h"
#include "reader.h"

#include <stdio.h>

static void setErr(char *err, size_t errLen, const char *msg) {
    if (err && errLen > 0) {
        snprintf(err, errLen, "%s", msg);
    }
}

/* Counts come from the file; cap allocations so a corrupt file cannot
 * request gigabytes. Real rooms stay far below these. */
#define MAX_COUNT (4 * 1024 * 1024)

static bool badCount(int32_t n) {
    return n < 0 || n > MAX_COUNT;
}

static bool parseSurface(Reader *r, RMeshSurface *s) {
    for (int j = 0; j < 2; j++) {
        s->textures[j].flags = rdU8(r);
        if (r->failed) return false;
        if (s->textures[j].flags != 0) {
            s->textures[j].name = rdBlitzString(r);
            if (r->failed) return false;
        }
    }

    int32_t vcount = rdI32(r);
    if (r->failed || badCount(vcount)) return false;
    s->vertexCount = (uint32_t)vcount;
    s->vertices = (RMeshVertex *)calloc(s->vertexCount ? s->vertexCount : 1,
                                        sizeof(RMeshVertex));
    if (!s->vertices) return false;
    for (uint32_t j = 0; j < s->vertexCount; j++) {
        RMeshVertex *v = &s->vertices[j];
        v->x = rdF32(r); v->y = rdF32(r); v->z = rdF32(r);
        v->u0 = rdF32(r); v->v0 = rdF32(r);
        v->u1 = rdF32(r); v->v1 = rdF32(r);
        v->r = rdU8(r); v->g = rdU8(r); v->b = rdU8(r);
    }

    int32_t tcount = rdI32(r);
    if (r->failed || badCount(tcount)) return false;
    s->triangleCount = (uint32_t)tcount;
    s->indices = (uint32_t *)calloc(s->triangleCount ? s->triangleCount * 3 : 1,
                                    sizeof(uint32_t));
    if (!s->indices) return false;
    for (uint32_t j = 0; j < s->triangleCount * 3; j++) {
        int32_t idx = rdI32(r);
        if (idx < 0 || (uint32_t)idx >= s->vertexCount) {
            r->failed = true;
        }
        s->indices[j] = (uint32_t)idx;
    }
    return rdOk(r);
}

static bool parseCollisionSurface(Reader *r, RMeshCollisionSurface *s) {
    int32_t vcount = rdI32(r);
    if (r->failed || badCount(vcount)) return false;
    s->vertexCount = (uint32_t)vcount;
    s->vertices = (RMeshVec3 *)calloc(s->vertexCount ? s->vertexCount : 1,
                                      sizeof(RMeshVec3));
    if (!s->vertices) return false;
    for (uint32_t j = 0; j < s->vertexCount; j++) {
        s->vertices[j].x = rdF32(r);
        s->vertices[j].y = rdF32(r);
        s->vertices[j].z = rdF32(r);
    }

    int32_t tcount = rdI32(r);
    if (r->failed || badCount(tcount)) return false;
    s->triangleCount = (uint32_t)tcount;
    s->indices = (uint32_t *)calloc(s->triangleCount ? s->triangleCount * 3 : 1,
                                    sizeof(uint32_t));
    if (!s->indices) return false;
    for (uint32_t j = 0; j < s->triangleCount * 3; j++) {
        int32_t idx = rdI32(r);
        if (idx < 0 || (uint32_t)idx >= s->vertexCount) {
            r->failed = true;
        }
        s->indices[j] = (uint32_t)idx;
    }
    return rdOk(r);
}

static bool parseEntity(Reader *r, RMeshEntity *e, char *err, size_t errLen) {
    char *type = rdBlitzString(r);
    if (!type) return false;

    bool known = true;
    if (strcmp(type, "screen") == 0) {
        e->type = RMESH_ENTITY_SCREEN;
        e->x = rdF32(r); e->y = rdF32(r); e->z = rdF32(r);
        e->u.screen.imgPath = rdBlitzString(r);
    } else if (strcmp(type, "waypoint") == 0) {
        e->type = RMESH_ENTITY_WAYPOINT;
        e->x = rdF32(r); e->y = rdF32(r); e->z = rdF32(r);
    } else if (strcmp(type, "light") == 0) {
        e->type = RMESH_ENTITY_LIGHT;
        e->x = rdF32(r); e->y = rdF32(r); e->z = rdF32(r);
        e->u.light.range = rdF32(r);
        e->u.light.color = rdBlitzString(r);
        e->u.light.intensity = rdF32(r);
    } else if (strcmp(type, "light_fix") == 0) {
        /* Same fields as "light" but serialized in a different order. */
        e->type = RMESH_ENTITY_LIGHT_FIX;
        e->x = rdF32(r); e->y = rdF32(r); e->z = rdF32(r);
        e->u.light.color = rdBlitzString(r);
        e->u.light.intensity = rdF32(r);
        e->u.light.range = rdF32(r);
    } else if (strcmp(type, "spotlight") == 0) {
        e->type = RMESH_ENTITY_SPOTLIGHT;
        e->x = rdF32(r); e->y = rdF32(r); e->z = rdF32(r);
        e->u.spotlight.range = rdF32(r);
        e->u.spotlight.color = rdBlitzString(r);
        e->u.spotlight.intensity = rdF32(r);
        e->u.spotlight.angles = rdBlitzString(r);
        e->u.spotlight.innerConeAngle = rdI32(r);
        e->u.spotlight.outerConeAngle = rdI32(r);
    } else if (strcmp(type, "soundemitter") == 0) {
        e->type = RMESH_ENTITY_SOUND_EMITTER;
        e->x = rdF32(r); e->y = rdF32(r); e->z = rdF32(r);
        e->u.soundEmitter.id = rdI32(r);
        e->u.soundEmitter.range = rdF32(r);
    } else if (strcmp(type, "model") == 0) {
        e->type = RMESH_ENTITY_MODEL;
        e->u.model.file = rdBlitzString(r);
    } else if (strcmp(type, "mesh") == 0) {
        e->type = RMESH_ENTITY_PROP;
        e->x = rdF32(r); e->y = rdF32(r); e->z = rdF32(r);
        e->u.prop.file = rdBlitzString(r);
        e->u.prop.pitch = rdF32(r);
        e->u.prop.yaw = rdF32(r);
        e->u.prop.roll = rdF32(r);
        e->u.prop.scaleX = rdF32(r);
        e->u.prop.scaleY = rdF32(r);
        e->u.prop.scaleZ = rdF32(r);
        e->u.prop.hasCollision = rdU8(r);
        e->u.prop.fx = rdI32(r);
        e->u.prop.texture = rdBlitzString(r);
    } else {
        /* Unknown entity payloads have unknown sizes; the stream cannot
         * be resynchronized past them. */
        known = false;
        if (err && errLen > 0) {
            snprintf(err, errLen, "unknown entity type '%s'", type);
        }
        r->failed = true;
    }
    free(type);
    return known && rdOk(r);
}

RMesh *rmeshLoadMemory(const void *data, size_t size, char *err, size_t errLen) {
    Reader r;
    rdInit(&r, data, size);
    setErr(err, errLen, "");

    RMesh *m = (RMesh *)calloc(1, sizeof(RMesh));
    if (!m) {
        setErr(err, errLen, "out of memory");
        return NULL;
    }

    char *magic = rdBlitzString(&r);
    if (!magic || strcmp(magic, "RoomMesh") != 0) {
        setErr(err, errLen, "not a RoomMesh file");
        free(magic);
        rmeshFree(m);
        return NULL;
    }
    free(magic);

    int32_t count = rdI32(&r);
    if (r.failed || badCount(count)) {
        setErr(err, errLen, "bad drawn mesh count");
        rmeshFree(m);
        return NULL;
    }
    m->surfaceCount = (uint32_t)count;
    m->surfaces = (RMeshSurface *)calloc(m->surfaceCount ? m->surfaceCount : 1,
                                         sizeof(RMeshSurface));
    if (!m->surfaces) {
        rmeshFree(m);
        return NULL;
    }
    for (uint32_t i = 0; i < m->surfaceCount; i++) {
        if (!parseSurface(&r, &m->surfaces[i])) {
            setErr(err, errLen, "failed parsing drawn mesh");
            rmeshFree(m);
            return NULL;
        }
    }

    count = rdI32(&r);
    if (r.failed || badCount(count)) {
        setErr(err, errLen, "bad collision mesh count");
        rmeshFree(m);
        return NULL;
    }
    m->collisionSurfaceCount = (uint32_t)count;
    m->collisionSurfaces = (RMeshCollisionSurface *)calloc(
        m->collisionSurfaceCount ? m->collisionSurfaceCount : 1,
        sizeof(RMeshCollisionSurface));
    if (!m->collisionSurfaces) {
        rmeshFree(m);
        return NULL;
    }
    for (uint32_t i = 0; i < m->collisionSurfaceCount; i++) {
        if (!parseCollisionSurface(&r, &m->collisionSurfaces[i])) {
            setErr(err, errLen, "failed parsing collision mesh");
            rmeshFree(m);
            return NULL;
        }
    }

    count = rdI32(&r);
    if (r.failed || badCount(count)) {
        setErr(err, errLen, "bad entity count");
        rmeshFree(m);
        return NULL;
    }
    m->entityCount = (uint32_t)count;
    m->entities = (RMeshEntity *)calloc(m->entityCount ? m->entityCount : 1,
                                        sizeof(RMeshEntity));
    if (!m->entities) {
        rmeshFree(m);
        return NULL;
    }
    for (uint32_t i = 0; i < m->entityCount; i++) {
        char entErr[128] = "";
        if (!parseEntity(&r, &m->entities[i], entErr, sizeof(entErr))) {
            if (err && errLen > 0) {
                snprintf(err, errLen, "failed parsing entity %u/%u%s%s",
                         i + 1, m->entityCount,
                         entErr[0] ? ": " : "", entErr);
            }
            rmeshFree(m);
            return NULL;
        }
    }

    return m;
}

RMesh *rmeshLoadFile(const char *path, char *err, size_t errLen) {
    size_t size = 0;
    void *data = readWholeFile(path, &size);
    if (!data) {
        setErr(err, errLen, "could not read file");
        return NULL;
    }
    RMesh *m = rmeshLoadMemory(data, size, err, errLen);
    free(data);
    return m;
}

void rmeshFree(RMesh *m) {
    if (!m) return;
    for (uint32_t i = 0; i < m->surfaceCount; i++) {
        RMeshSurface *s = &m->surfaces[i];
        free(s->textures[0].name);
        free(s->textures[1].name);
        free(s->vertices);
        free(s->indices);
    }
    free(m->surfaces);
    for (uint32_t i = 0; i < m->collisionSurfaceCount; i++) {
        free(m->collisionSurfaces[i].vertices);
        free(m->collisionSurfaces[i].indices);
    }
    free(m->collisionSurfaces);
    for (uint32_t i = 0; i < m->entityCount; i++) {
        RMeshEntity *e = &m->entities[i];
        switch (e->type) {
            case RMESH_ENTITY_SCREEN:
                free(e->u.screen.imgPath);
                break;
            case RMESH_ENTITY_LIGHT:
            case RMESH_ENTITY_LIGHT_FIX:
                free(e->u.light.color);
                break;
            case RMESH_ENTITY_SPOTLIGHT:
                free(e->u.spotlight.color);
                free(e->u.spotlight.angles);
                break;
            case RMESH_ENTITY_MODEL:
                free(e->u.model.file);
                break;
            case RMESH_ENTITY_PROP:
                free(e->u.prop.file);
                free(e->u.prop.texture);
                break;
            default:
                break;
        }
    }
    free(m->entities);
    free(m);
}
