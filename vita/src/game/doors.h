#ifndef VITA_GAME_DOORS_H
#define VITA_GAME_DOORS_H

#include <stdint.h>

#include "mapgen.h"

/*
 * Doors between rooms, ported from CreateMap's door pass and
 * CreateDoor/UpdateDoors in Map_Core.bb: spawned on +X edges (angle
 * 90) and +Z edges (angle 0) between connected tiles, deduplicated by
 * the original shape/angle rules; ~20% start open; LCZ/EZ use the
 * default sliding door, HCZ the heavy door. Panels slide apart with
 * the sine-eased motion of UpdateDoors.
 */

typedef struct {
    float x, z;        /* world center, raw mesh units */
    int angle;         /* 0 = spans X (faces Z), 90 = spans Z (faces X) */
    int heavy;         /* HCZ heavy door */
    int open;          /* target state */
    float openState;   /* 0..180, animated */
} Door;

typedef struct {
    Door *items;
    uint32_t count;
} DoorList;

/* Door panel dimensions in raw units (CreateDoor: 203 x 313 x 15). */
#define DOOR_PANEL_HALF_W 102.0f
#define DOOR_PANEL_HALF_D 12.0f
#define DOOR_PANEL_H 313.0f
#define DOOR_SLIDE_DEFAULT 183.0f
#define DOOR_SLIDE_HEAVY 90.0f

int doorsGenerate(const GeneratedMap *map, const RoomTemplateList *templates,
                  uint32_t seed, DoorList *out);
void doorsFree(DoorList *list);

/* Advance the open/close animation one frame (UpdateDoors: 2 deg). */
void doorsUpdate(DoorList *list);

/* Toggle the nearest door within maxDist of pos. Returns 1 if one was
 * toggled. */
int doorsToggleNearest(DoorList *list, const float pos[3], float maxDist);

/* Current panel slide offset in raw units. */
float doorSlide(const Door *d);

/* Push a sphere at pos out of the door panels (horizontal only). */
void doorsCollide(const DoorList *list, float pos[3], float radius);

#endif
