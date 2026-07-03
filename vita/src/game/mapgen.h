#ifndef VITA_GAME_MAPGEN_H
#define VITA_GAME_MAPGEN_H

#include <stdint.h>

/*
 * Facility map generation, ported from CreateMap() / LoadRoomTemplates()
 * in Map_Core.bb: an 18x18 grid of hallways laid bottom-to-top through
 * three zones (LCZ/HCZ/EZ), each tile typed by its connection count
 * (ROOM1/2/2C/3/4) with the original angle tables, then filled with
 * room templates from Data/rooms.ini by shape + zone + commonness.
 *
 * Not yet ported: the forced special-room lists (MapRoom/SetRoom) and
 * the post-pass that adds extra ROOM1/ROOM4 tiles.
 */

#define MAPGEN_GRID 18

enum {
    SHAPE_ROOM1 = 0,
    SHAPE_ROOM2,
    SHAPE_ROOM2C,
    SHAPE_ROOM3,
    SHAPE_ROOM4,
    SHAPE_COUNT
};

typedef struct {
    char *name;
    char *meshPath;     /* relative to GFX/Map */
    int shape;
    int zones[5];       /* zone numbers this room may appear in (0 unused) */
    int commonness;     /* 0..100 */
    int disableDecals;  /* 0..3: also gates SCP-106's spawn timer while
                           the player is in the room (0 full speed, 1 half,
                           2 quarter, 3 never) */
} RoomTemplateInfo;

typedef struct {
    RoomTemplateInfo *items;
    uint32_t count;
} RoomTemplateList;

typedef struct {
    int templateIndex;
    int gridX, gridY;
    int angle;          /* quarter turns, *90 degrees, Blitz convention */
} RoomPlacement;

typedef struct {
    RoomPlacement *rooms;
    uint32_t roomCount;
    int startX, startY; /* player spawn tile */
} GeneratedMap;

/* Zone band of a grid row, matching Math_Core.bb GetZone(): the top of
 * the grid (high y) is LCZ, the bottom EZ. Returns the port's 1-based
 * convention (1 = LCZ, 2 = HCZ, 3 = EZ) to line up with rooms.ini's
 * Zone fields. This is a room's true zone (source r\Zone = GetZone(y)),
 * not its template's declared Zone list. */
int mapZoneOf(int gridY);

/* Parse Data/rooms.ini. Returns 0 on I/O failure. */
int templatesLoad(const char *iniPath, RoomTemplateList *out);
void templatesFree(RoomTemplateList *list);

/* Generate a facility. Returns 0 on allocation failure or if the
 * template list lacks a shape needed somewhere on the grid. */
int mapGenerate(const RoomTemplateList *templates, uint32_t seed,
                GeneratedMap *out);
void mapFree(GeneratedMap *map);

#endif
