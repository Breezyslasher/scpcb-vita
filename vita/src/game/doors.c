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
                   int open, int keycard) {
    Door *grown = (Door *)realloc(list->items,
                                  (list->count + 1) * sizeof(Door));
    if (!grown) return 0;
    list->items = grown;
    Door *d = &list->items[list->count++];
    d->x = x;
    d->y = 0.0f;
    d->z = z;
    d->angle = angle;
    d->heavy = heavy;
    d->type = heavy ? 2 : 0;
    d->open = open;
    d->openState = open ? 180.0f : 0.0f;
    d->keycard = keycard;
    d->locked = 0;
    d->code = 0;
    d->nobuttons = 0;
    d->denials = 0;
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
    uint8_t chk[GRID * GRID];
    memset(occ, 0, sizeof(occ));
    memset(shape, -1, sizeof(shape));
    memset(angleDeg, 0, sizeof(angleDeg));
    memset(chk, 0, sizeof(chk));
    for (uint32_t i = 0; i < map->roomCount; i++) {
        const RoomPlacement *p = &map->rooms[i];
        int idx = p->gridX + p->gridY * GRID;
        occ[idx] = 1;
        shape[idx] = (int8_t)templates->items[p->templateIndex].shape;
        angleDeg[idx] = (int16_t)((p->angle * 90) % 360);
        const char *nm = templates->items[p->templateIndex].name;
        if (nm && strstr(nm, "checkpoint")) chk[idx] = 1;
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
                int kc = (chk[idx] || chk[(x + 1) + y * GRID])
                       ? (y > GRID / 2 ? 1 : 3) : 0;
                if (kc) open = 0;
                if (!addDoor(out, x * RS + RS / 2.0f, y * RS, 90, heavy,
                             open, kc)) {
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
                int kc = (chk[idx] || chk[x + (y + 1) * GRID])
                       ? (y > GRID / 2 ? 1 : 3) : 0;
                if (kc) open = 0;
                if (!addDoor(out, x * RS, y * RS + RS / 2.0f, 0, heavy,
                             open, kc)) {
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

int doorsAddInternal(DoorList *list, float x, float y, float z, int angle,
                     int type, int open, int keycard, int locked,
                     int nobuttons, int code) {
    int heavy = type == 1 || type == 2 || type == 3;
    if (!addDoor(list, x, z, angle, heavy, open, keycard)) return 0;
    Door *d = &list->items[list->count - 1];
    d->y = y;
    d->type = type;
    d->locked = locked;
    d->nobuttons = nobuttons;
    d->code = code;
    return 1;
}

float doorSlide(const Door *d) {
    float max = d->type == 3 ? DOOR_SLIDE_BIG
              : (d->type == 2 ? DOOR_SLIDE_HEAVY : DOOR_SLIDE_DEFAULT);
    float t = d->openState * 3.14159265f / 180.0f;
    return max * (1.0f - cosf(t)) * 0.5f;
}

/* Button local offsets (CreateDoor): x 0.6 - i*1.2, y 0.7,
 * z -0.1 + i*0.2 world units -> raw. */
void doorButtonWorldPos(const Door *d, int side, float out[3]) {
    /* Big gates are wider than the default doorway. */
    float span = d->type == 3 ? 264.0f : 153.6f; /* 0.6 world units */
    float lx = span - side * span * 2.0f;
    float ly = 0.7f * 256.0f;
    float lz = (-0.1f + side * 0.2f) * 256.0f;
    /* Same quarter-turn convention as the renderer (rotates by
     * -angle). */
    switch (((d->angle % 360) + 360) % 360) {
        default:  out[0] = d->x + lx; out[2] = d->z + lz; break;
        case 90:  out[0] = d->x + lz; out[2] = d->z - lx; break;
        case 180: out[0] = d->x - lx; out[2] = d->z - lz; break;
        case 270: out[0] = d->x - lz; out[2] = d->z + lx; break;
    }
    out[1] = d->y + ly;
}

DoorPressResult doorsPressButton(DoorList *list, const float pos[3],
                                 int keycardLevel, Door **outDoor) {
    const float REACH = 256.0f; /* |dx|,|dz| < 1.0 world units */
    Door *best = NULL;
    float bestD2 = 1e30f;
    for (uint32_t i = 0; i < list->count; i++) {
        Door *d = &list->items[i];
        if (d->nobuttons) continue;
        float cdx = pos[0] - d->x, cdz = pos[2] - d->z;
        if (cdx * cdx + cdz * cdz > 1024.0f * 1024.0f) continue;
        for (int side = 0; side < 2; side++) {
            float b[3];
            doorButtonWorldPos(d, side, b);
            float dx = fabsf(pos[0] - b[0]);
            float dz = fabsf(pos[2] - b[2]);
            float dy = fabsf((pos[1] - 230.0f + 179.0f) - b[1]);
            if (dx < REACH && dz < REACH && dy < 400.0f) {
                float d2 = dx * dx + dz * dz;
                if (d2 < bestD2) {
                    bestD2 = d2;
                    best = d;
                }
            }
        }
    }
    if (!best) return DOOR_PRESS_NONE;
    if (outDoor) *outDoor = best;

    if (best->locked) return DOOR_PRESS_LOCKED;
    if (best->code > 0) return DOOR_PRESS_CODE;
    if (best->keycard > 0 && keycardLevel < best->keycard) {
        best->denials++;
        if (best->denials >= 3) {
            /* Debug courtesy until keycard items are ported. */
            best->denials = 0;
            best->open = !best->open;
            return DOOR_PRESS_TOGGLED;
        }
        return DOOR_PRESS_KEYCARD;
    }
    best->open = !best->open;
    return DOOR_PRESS_TOGGLED;
}

void doorsCollide(const DoorList *list, float pos[3], float radius) {
    for (uint32_t i = 0; i < list->count; i++) {
        const Door *d = &list->items[i];
        /* Once the panels are mostly retracted the doorway is clear;
         * per-panel boxes never opened a gap for heavy doors whose
         * slide is shorter than the panel width. */
        if (d->openState > 140.0f) continue;

        float dx = pos[0] - d->x, dz = pos[2] - d->z;
        if (dx * dx + dz * dz > 1024.0f * 1024.0f) continue;
        if (pos[1] > d->y + DOOR_PANEL_H + 200.0f) continue;
        if (pos[1] < d->y - 100.0f) continue;

        /* Door-local frame: lx spans the doorway, lz is through it
         * (the sign convention cancels out for the symmetric box). */
        float lx, lz;
        if (((d->angle % 180) + 180) % 180 == 90) {
            lx = dz;
            lz = dx;
        } else {
            lx = dx;
            lz = dz;
        }

        /* One box across the whole doorway. The big containment gate's
         * two halves (contdoorleft/right, scaled 55) each reach ~244 raw
         * from the centre, wider than the default door's ~203, so its
         * closed box has to span further or the player slips past a shut
         * gate's edges. */
        float halfSpan = d->type == 3 ? 264.0f : 220.0f;
        if (fabsf(lx) >= halfSpan + radius) continue;
        if (fabsf(lz) >= DOOR_PANEL_HALF_D + radius) continue;
        float push = (DOOR_PANEL_HALF_D + radius - fabsf(lz))
                   * (lz >= 0.0f ? 1.0f : -1.0f);
        lz += push;
        if (((d->angle % 180) + 180) % 180 == 90) {
            pos[0] = d->x + lz;
        } else {
            pos[2] = d->z + lz;
        }
    }
}
