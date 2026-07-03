#ifndef VITA_FORMATS_B3D_H
#define VITA_FORMATS_B3D_H

#include <stddef.h>
#include <stdint.h>

/*
 * Loader for Blitz3D .b3d models (props, NPC meshes). Parses static
 * geometry — TEXS, BRUS and the NODE/MESH/VRTS/TRIS hierarchy — plus
 * the animation chunks (ANIM header, per-node KEYS keyframes and BONE
 * skin weights) used by render/skin.c for skeletal animation.
 * Spec: blitz-research/blitz3d b3dfile_specs.txt.
 */

#define B3D_MAX_BRUSH_TEXTURES 8

typedef struct {
    char *file;
    int32_t flags;
    int32_t blend;
    float posX, posY;
    float scaleX, scaleY;
    float rotation;
} B3DTexture;

typedef struct {
    char *name;
    float r, g, b, a;
    float shininess;
    int32_t blend;
    int32_t fx;
    int32_t textureIds[B3D_MAX_BRUSH_TEXTURES]; /* -1 = none */
    int32_t textureSlots;                       /* n_texs of the BRUS chunk */
} B3DBrush;

typedef struct {
    float x, y, z;
    float nx, ny, nz;    /* valid when mesh->hasNormals */
    float r, g, b, a;    /* valid when mesh->hasColors */
    float u, v;          /* first tex coord set */
} B3DVertex;

typedef struct {
    int32_t brushId;     /* -1 = use mesh brush */
    uint32_t *indices;   /* 3 * triangleCount entries */
    uint32_t triangleCount;
} B3DTriSet;

typedef struct {
    int32_t brushId;
    int hasNormals;
    int hasColors;
    B3DVertex *vertices;
    uint32_t vertexCount;
    B3DTriSet *triSets;
    uint32_t triSetCount;
} B3DMesh;

/* One KEYS entry; flags say which channels this key carries (keys
 * from different KEYS chunks of one node can carry different
 * channels). */
typedef struct {
    int32_t frame;
    int32_t flags;
    float position[3];   /* flags & 1 */
    float scale[3];      /* flags & 2 */
    float rotation[4];   /* flags & 4, quaternion w,x,y,z */
} B3DKey;

/* BONE chunk entry: skin weight of one mesh vertex to this node. */
typedef struct {
    int32_t vertexId;
    float weight;
} B3DBoneWeight;

typedef struct B3DNode {
    char *name;
    float position[3];
    float scale[3];
    float rotation[4];   /* quaternion w,x,y,z */
    B3DMesh *mesh;       /* NULL for pivots/bones */
    struct B3DNode **children;
    uint32_t childCount;
    /* animation */
    int isBone;
    B3DBoneWeight *weights;
    uint32_t weightCount;
    B3DKey *keys;        /* frame-ordered */
    uint32_t keyCount;
    int32_t keyFlags;    /* union of the node's KEYS chunk flags */
} B3DNode;

typedef struct {
    int32_t version;
    B3DTexture *textures;
    uint32_t textureCount;
    B3DBrush *brushes;
    uint32_t brushCount;
    B3DNode *root;       /* may be NULL for texture/brush-only files */
    int32_t animFrames;  /* ANIM chunk; 0 = no animation */
    float animFps;
} B3DModel;

B3DModel *b3dLoadMemory(const void *data, size_t size, char *err, size_t errLen);
B3DModel *b3dLoadFile(const char *path, char *err, size_t errLen);
void b3dFree(B3DModel *model);

#endif
