#include "doors.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GRID MAPGEN_GRID

static uint32_t rngState;

static uint32_t rngNext(void) {
    rngState = rngState * 1664525u + 1013904223u;
    return rngState >> 8;
}

static int randRange(int a, int b) {
    return a + (int)(rngNext() % (uint32_t)(b - a + 1));
}

static int addDoor(DoorList *list, float x, float z, int angle, int heavy,
                   int open) {
    Door *grown = (Door *)realloc(list->items,
                                  (list->count + 1) * sizeof(Door));
    if (!grown) return 0;
    list->items = grown;
    Door *d = &list->items[list->count++];
    d->x = x;
    d->z = z;
    d->angle = angle;
    d->heavy = heavy;
    d->open = open;
    d->openState = open ? 180.0f : 0.0f;
    return 1;
}

int doorsGenerate(const GeneratedMap *map, const RoomTemplateList *templates,
                  uint32_t seed, DoorList *out) {
    memset(out, 0, sizeof(*out));
    rngState = seed ? seed : 1;

    /* Rebuild occupancy plus shape/angle per tile from placements. */
    uint8_t occ[GRID * GRID];
    int8_t shape[GRID * GRID];
    int16_t angleDeg[GRID * GRID];
    memset(occ, 0, sizeof(occ));
    memset(shape, -1, sizeof(shape));
    memset(angleDeg, 0, sizeof(angleDeg));
    for (uint32_t i = 0; i < map->roomCount; i++) {
        const RoomPlacement *p = &map->rooms[i];
        int idx = p->gridX + p->gridY * GRID;
        occ[idx] = 1;
        shape[idx] = (int8_t)templates->items[p->templateIndex].shape;
        angleDeg[idx] = (int16_t)((p->angle * 90) % 360);
    }

    const float RS = 2048.0f; /* RoomSpacing in raw units */

    /* Map_Core.bb 5844..: zone by row decides door type; +X then +Z
     * edges, deduplicated by the room's shape/angle. */
    for (int y = GRID - 1; y >= 0; y--) {
        int zone = (y < GRID / 3 + 1) ? 3
                 : (y < (int)(GRID * (2.0 / 3.0))) ? 2 : 1;
        int heavy = ((zone - 1) % 2) != 0; /* DoorType 2 = HEAVY in HCZ */
        for (int x = GRID - 1; x >= 0; x--) {
            int idx = x + y * GRID;
            if (!occ[idx]) continue;
            int sh = shape[idx];
            int an = angleDeg[idx];
            int spawn;

            /* +X edge (door angle 90) */
            switch (sh) {
                case SHAPE_ROOM1:  spawn = (an == 90); break;
                case SHAPE_ROOM2:  spawn = (an == 90 || an == 270); break;
                case SHAPE_ROOM2C: spawn = (an == 0 || an == 90); break;
                case SHAPE_ROOM3:  spawn = (an == 0 || an == 180 || an == 90); break;
                default:           spawn = 1; break;
            }
            if (spawn && x + 1 < GRID && occ[(x + 1) + y * GRID]) {
                int open = randRange(-3, 1) > 0;
                if (!addDoor(out, x * RS + RS / 2.0f, y * RS, 90, heavy,
                             open)) {
                    doorsFree(out);
                    return 0;
                }
            }

            /* +Z edge (door angle 0) */
            switch (sh) {
                case SHAPE_ROOM1:  spawn = (an == 180); break;
                case SHAPE_ROOM2:  spawn = (an == 0 || an == 180); break;
                case SHAPE_ROOM2C: spawn = (an == 180 || an == 90); break;
                case SHAPE_ROOM3:  spawn = (an == 180 || an == 90 || an == 270); break;
                default:           spawn = 1; break;
            }
            if (spawn && y + 1 < GRID && occ[x + (y + 1) * GRID]) {
                int open = randRange(-3, 1) > 0;
                if (!addDoor(out, x * RS, y * RS + RS / 2.0f, 0, heavy,
                             open)) {
                    doorsFree(out);
                    return 0;
                }
            }
        }
    }
    return 1;
}

void doorsFree(DoorList *list) {
    free(list->items);
    memset(list, 0, sizeof(*list));
}

void doorsUpdate(DoorList *list) {
    for (uint32_t i = 0; i < list->count; i++) {
        Door *d = &list->items[i];
        float target = d->open ? 180.0f : 0.0f;
        if (d->openState < target) {
            d->openState += 2.0f;
            if (d->openState > target) d->openState = target;
        } else if (d->openState > target) {
            d->openState -= 2.0f;
            if (d->openState < target) d->openState = target;
        }
    }
}

float doorSlide(const Door *d) {
    float max = d->heavy ? DOOR_SLIDE_HEAVY : DOOR_SLIDE_DEFAULT;
    float t = d->openState * 3.14159265f / 180.0f;
    return max * (1.0f - cosf(t)) * 0.5f;
}

int doorsToggleNearest(DoorList *list, const float pos[3], float maxDist) {
    Door *best = NULL;
    float bestD2 = maxDist * maxDist;
    for (uint32_t i = 0; i < list->count; i++) {
        Door *d = &list->items[i];
        float dx = pos[0] - d->x, dz = pos[2] - d->z;
        float d2 = dx * dx + dz * dz;
        if (d2 < bestD2) {
            bestD2 = d2;
            best = d;
        }
    }
    if (!best) return 0;
    best->open = !best->open;
    return 1;
}

void doorsCollide(const DoorList *list, float pos[3], float radius) {
    for (uint32_t i = 0; i < list->count; i++) {
        const Door *d = &list->items[i];
        float dx = pos[0] - d->x, dz = pos[2] - d->z;
        if (dx * dx + dz * dz > 1024.0f * 1024.0f) continue;
        if (pos[1] > DOOR_PANEL_H + 200.0f) continue;

        /* Door-local frame: lx spans the doorway, lz is through it. */
        float lx, lz;
        if (d->angle == 90) {
            lx = dz;
            lz = dx;
        } else {
            lx = dx;
            lz = dz;
        }

        float slide = doorSlide(d);
        for (int panel = 0; panel < 2; panel++) {
            float cx = panel == 0 ? slide : -slide;
            float px = lx - cx;
            if (fabsf(px) >= DOOR_PANEL_HALF_W + radius) continue;
            if (fabsf(lz) >= DOOR_PANEL_HALF_D + radius) continue;
            /* Push out along the through-axis. */
            float push = (DOOR_PANEL_HALF_D + radius - fabsf(lz))
                       * (lz >= 0.0f ? 1.0f : -1.0f);
            lz += push;
            if (d->angle == 90) {
                pos[0] = d->x + lz;
            } else {
                pos[2] = d->z + lz;
            }
            /* Recompute for the second panel. */
            if (d->angle == 90) {
                lz = pos[0] - d->x;
            } else {
                lz = pos[2] - d->z;
            }
        }
    }
}
