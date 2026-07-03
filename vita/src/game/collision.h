#ifndef VITA_GAME_COLLISION_H
#define VITA_GAME_COLLISION_H

#include "../formats/rmesh.h"
#include "../render/scene.h"

/*
 * Triangle-soup collision for walking the rooms: the opaque drawn
 * geometry (incl. props) plus the .rmesh invisible collision surfaces,
 * indexed by a uniform grid. GL-free and host-testable.
 */

typedef struct CollisionWorld CollisionWorld;

/* Build from a scene's non-alpha batches plus the room's invisible
 * collision surfaces (mesh may be NULL). Returns NULL on allocation
 * failure. */
CollisionWorld *collisionBuild(const Scene *scene, const RMesh *mesh);

void collisionFree(CollisionWorld *world);

/* Push a sphere at pos out of the geometry (a few relaxation
 * iterations). Returns the number of corrections applied; *pushedUp is
 * set when any correction had a strong upward component (i.e. the
 * sphere is resting on walkable ground). */
int collisionSpherePush(const CollisionWorld *world, float pos[3],
                        float radius, int *pushedUp);

/* Cast a ray straight down from origin; on hit within maxDist, writes
 * the hit Y and returns 1. */
int collisionRayDown(const CollisionWorld *world, const float origin[3],
                     float maxDist, float *hitY);

/* Cast a ray from origin along dir (need not be normalized) and return 1
 * if any triangle blocks it within maxDist - i.e. the segment is
 * occluded. Used for line-of-sight tests. */
int collisionRayHit(const CollisionWorld *world, const float origin[3],
                    const float dir[3], float maxDist);

#endif
