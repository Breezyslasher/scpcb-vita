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
    float y;           /* floor height (internal doors on catwalks) */
    int angle;         /* Blitz yaw 0/90/180/270 (render negates);
                          angle %% 180 gives the span axis */
    int heavy;         /* heavy/big/elevator: slower slide, big sounds */
    int type;          /* 0 default 1 elevator 2 heavy 3 big 4 office
                          5 wooden 6 one-sided 7 SCP-914 */
    int open;          /* target state */
    float openState;   /* 0..180, animated */
    int keycard;       /* 0 = none; >0 = required keycard level */
    int locked;
    int code;          /* >0 = keypad door: the 4-digit code */
    int nobuttons;     /* FillRoom removed the buttons */
    int denials;       /* failed button presses (debug force-open) */
    int corroded;      /* SCP-106 rotted the surface (texture swap) */
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
#define DOOR_SLIDE_BIG 340.0f

int doorsGenerate(const GeneratedMap *map, const RoomTemplateList *templates,
                  uint32_t seed, DoorList *out);

/* Append a room-internal door (FillRoom's CreateDoor). angle is the
 * span axis (0 or 90); y is the door base height; type as in Door. */
int doorsAddInternal(DoorList *list, float x, float y, float z, int angle,
                     int type, int open, int keycard, int locked,
                     int nobuttons, int code);
void doorsFree(DoorList *list);

/* Advance the open/close animation one frame (UpdateDoors: 2 deg). */
void doorsUpdate(DoorList *list);

/* Button positions (CreateDoor: door-local x +-0.6, y 0.7, z -+0.1
 * world units, parented to the rotated frame). side is 0 or 1; the
 * button faces yaw side*180 relative to the door. */
void doorButtonWorldPos(const Door *d, int side, float out[3]);

typedef enum {
    DOOR_PRESS_NONE = 0,   /* no button in reach */
    DOOR_PRESS_TOGGLED,
    DOOR_PRESS_LOCKED,
    DOOR_PRESS_KEYCARD,    /* denied: keycard required */
    DOOR_PRESS_CODE        /* keypad door: open the code entry UI */
} DoorPressResult;

/* Press the nearest button within reach (the game's |dx|,|dz| < 1.0
 * world units). keycardLevel is what the player carries; outDoor (may
 * be NULL) receives the door involved. Three denied presses force a
 * keycard door open — a debug courtesy until items are ported. */
DoorPressResult doorsPressButton(DoorList *list, const float pos[3],
                                 int keycardLevel, Door **outDoor);

/* Current panel slide offset in raw units. */
float doorSlide(const Door *d);

/* Push a sphere at pos out of the door panels (horizontal only). */
void doorsCollide(const DoorList *list, float pos[3], float radius);

#endif
