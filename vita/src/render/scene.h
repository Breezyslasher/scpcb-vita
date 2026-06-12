#ifndef VITA_RENDER_SCENE_H
#define VITA_RENDER_SCENE_H

#include <stdint.h>

#include "../formats/rmesh.h"

/*
 * CPU-side conversion of a parsed RMesh into renderable batches.
 * Contains no GL calls so it can be exercised on the host against the
 * full asset set; the GL layer uploads from these arrays.
 *
 * Layer semantics recovered from LoadRMesh() in Map_Core.bb:
 *   - texture slot j samples UV set (1 - j)  [TextureCoords(Tex[j], 1-j)]
 *   - a texture whose name contains "_lm" is a lightmap (additive,
 *     Blitz TextureBlend 3)
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

void sceneFree(Scene *scene);

#endif
