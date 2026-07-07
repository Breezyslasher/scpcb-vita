#ifndef VITA_RENDER_SCENE_H
#define VITA_RENDER_SCENE_H

#include <stdint.h>

#include "../formats/b3d.h"
#include "../formats/rmesh.h"

/*
 * CPU-side conversion of a parsed RMesh into renderable batches.
 * Contains no GL calls so it can be exercised on the host against the
 * full asset set; the GL layer uploads from these arrays.
 *
 * Layer semantics recovered from LoadRMesh() in Map_Core.bb:
 *   - texture slot j samples UV set (1 - j)  [TextureCoords(Tex[j], 1-j)]
 *   - a texture whose name contains "_lm" is the baked lightmap (slot 0).
 *     Source composites 2 * diffuse * (ambient + lightmap) (the diffuse
 *     layer is Blitz TextureBlend 5, multiply x2), but the GL layer draws
 *     it additively: the modulate2x pass double-applied the baked vertex
 *     colours and, with no ambient floor, multiplied dim rooms to black
 *     on device (hardware-bisected). See drawBatchSet in main.c.
 *   - layer flag 3 marks the surface alpha-clipped
 */

typedef struct {
    float x, y, z;
    float du, dv;            /* diffuse UV */
    float lu, lv;            /* lightmap UV */
    uint8_t r, g, b, a;
} SceneVertex;

typedef struct {
    char *diffuseName;       /* NULL = untextured */
    char *lightmapName;      /* NULL = no lightmap */
    int alphaClip;           /* alpha-test pass (fences, grates, ...) */
    SceneVertex *vertices;
    uint32_t vertexCount;
    uint16_t *indices;       /* triangle list */
    uint32_t indexCount;
} SceneBatch;

typedef struct {
    SceneBatch *batches;
    uint32_t batchCount;
    float boundsMin[3];
    float boundsMax[3];
} Scene;

/* Build a scene from a parsed room. Surfaces with more than 65535
 * vertices are split across batches. Returns NULL on allocation
 * failure only. */
Scene *sceneBuild(const RMesh *mesh);

/* Append a B3D model (a room prop) to the scene, baking the placement
 * transform and the model's node hierarchy into the vertices.
 * Position/rotation/scale are in the same space as the room mesh
 * vertices (Blitz: RotateEntity order yaw-pitch-roll, degrees).
 * textureOverride, when non-NULL/non-empty, replaces every brush
 * texture. Returns 0 on allocation failure. */
int sceneAppendB3D(Scene *scene, const B3DModel *model,
                   const float pos[3], const float eulerDeg[3],
                   const float scale[3], const char *textureOverride);

void sceneFree(Scene *scene);

#endif
