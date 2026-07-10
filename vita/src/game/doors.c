#include "doors.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GRID MAPGEN_GRID

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
    /* The grid doors now come out of mapGenerate itself: the source
     * creates them inside CreateMap from the same seeded RNG stream,
     * so their open states are part of seed parity. */
    (void)templates;
    (void)seed;
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < map->gridDoorCount; i++) {
        const GridDoorGen *g = &map->gridDoors[i];
        if (!addDoor(out, g->x, g->z, g->angle, g->heavy, g->open, 0)) {
            doorsFree(out);
            return 0;
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
