#ifndef VITA_FORMATS_RMESH_H
#define VITA_FORMATS_RMESH_H

#include <stddef.h>
#include <stdint.h>

/*
 * Loader for the game's .rmesh room format, matching exactly what
 * LoadRMesh() in "Source Code/Map_Core.bb" reads. All strings are
 * Blitz3D ReadString format (int32 length prefix, no terminator) and
 * are returned NUL-terminated. Little-endian throughout.
 */

typedef struct {
    float x, y, z;
    float u0, v0;   /* texture layer 0 (lightmap or diffuse) */
    float u1, v1;   /* texture layer 1 */
    uint8_t r, g, b;
} RMeshVertex;

typedef struct {
    uint8_t flags;  /* 0 = no texture; 1 = multiply blend; 3 = alpha-clipped */
    char *name;     /* NULL when flags == 0 */
} RMeshTextureRef;

typedef struct {
    RMeshTextureRef textures[2];
    RMeshVertex *vertices;
    uint32_t vertexCount;
    uint32_t *indices;       /* 3 * triangleCount entries */
    uint32_t triangleCount;
} RMeshSurface;

typedef struct {
    float x, y, z;
} RMeshVec3;

typedef struct {
    RMeshVec3 *vertices;
    uint32_t vertexCount;
    uint32_t *indices;       /* 3 * triangleCount entries */
    uint32_t triangleCount;
} RMeshCollisionSurface;

typedef enum {
    RMESH_ENTITY_SCREEN = 0,
    RMESH_ENTITY_WAYPOINT,
    RMESH_ENTITY_LIGHT,
    RMESH_ENTITY_LIGHT_FIX,
    RMESH_ENTITY_SPOTLIGHT,
    RMESH_ENTITY_SOUND_EMITTER,
    RMESH_ENTITY_MODEL,
    RMESH_ENTITY_PROP        /* "mesh" entity: a .b3d prop placement */
} RMeshEntityType;

typedef struct {
    RMeshEntityType type;
    float x, y, z;           /* raw file coords; game multiplies by RoomScale */
    union {
        struct {
            char *imgPath;
        } screen;
        struct {
            float range;     /* raw; game divides by 2000 */
            char *color;     /* "R G B" */
            float intensity;
        } light;             /* LIGHT and LIGHT_FIX */
        struct {
            float range;
            char *color;
            float intensity;
            char *angles;    /* "pitch yaw" */
            int32_t innerConeAngle;
            int32_t outerConeAngle;
        } spotlight;
        struct {
            int32_t id;
            float range;
        } soundEmitter;
        struct {
            char *file;
        } model;
        struct {
            char *file;      /* name without extension, relative to GFX/Map/Props */
            float pitch, yaw, roll;
            float scaleX, scaleY, scaleZ;
            uint8_t hasCollision;
            int32_t fx;
            char *texture;
        } prop;
    } u;
} RMeshEntity;

typedef struct {
    RMeshSurface *surfaces;
    uint32_t surfaceCount;
    RMeshCollisionSurface *collisionSurfaces;
    uint32_t collisionSurfaceCount;
    RMeshEntity *entities;
    uint32_t entityCount;
} RMesh;

/* Parse from a memory buffer. Returns NULL on failure and, if err is
 * non-NULL, writes a description into err (errLen bytes). */
RMesh *rmeshLoadMemory(const void *data, size_t size, char *err, size_t errLen);

/* Convenience wrapper: read the whole file then parse. */
RMesh *rmeshLoadFile(const char *path, char *err, size_t errLen);

void rmeshFree(RMesh *mesh);

#endif
