#ifndef VITA_RENDER_SKIN_H
#define VITA_RENDER_SKIN_H

#include <stdint.h>

#include "../formats/b3d.h"
#include "scene.h"

/*
 * CPU-skinned Blitz3D models: evaluates the KEYS keyframes over the
 * bone NODE hierarchy and skins the mesh vertices with the BONE
 * weights, producing a SceneVertex buffer for the fixed-function
 * renderer. Uses the same left-handed -> right-handed mirroring
 * conventions as scene.c so skinned and static models line up.
 */

typedef struct {
    char *textureName;   /* diffuse texture file (basename), or NULL */
    uint16_t *indices;
    uint32_t indexCount;
} SkinBatch;

typedef struct SkinnedMesh SkinnedMesh;

/* NULL if the model has no bones/keyframes (draw it statically). */
SkinnedMesh *skinnedCreate(const B3DModel *model);
void skinnedFree(SkinnedMesh *s);

int skinnedFrames(const SkinnedMesh *s);
float skinnedFps(const SkinnedMesh *s);

/* Bind-pose bounds (for scaling to a target height). */
void skinnedBounds(const SkinnedMesh *s, float mn[3], float mx[3]);

/* Evaluate the pose at `frame` (fractional, clamped to the range) and
 * re-skin the shared vertex buffer. */
void skinnedEval(SkinnedMesh *s, float frame);

const SceneVertex *skinnedVertices(const SkinnedMesh *s, uint32_t *count);

/* Private per-instance buffer (a copy of the template, so several
 * figures can share one skeleton and re-pose on their own schedule). */
SceneVertex *skinnedNewBuffer(const SkinnedMesh *s);
void skinnedEvalInto(SkinnedMesh *s, float frame, SceneVertex *out);
uint32_t skinnedBatchCount(const SkinnedMesh *s);
const SkinBatch *skinnedBatch(const SkinnedMesh *s, uint32_t i);

#endif
