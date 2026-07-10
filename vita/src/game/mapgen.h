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

/* A grid door produced during generation (the source creates these
 * inside CreateMap from the same RNG stream, so their open states are
 * part of seed parity). */
typedef struct {
    float x, z;         /* raw world units */
    int angle;          /* 0 or 90 */
    int heavy;          /* HCZ heavy doors */
    int open;
} GridDoorGen;

typedef struct {
    RoomPlacement *rooms;
    uint32_t roomCount;
    int startX, startY; /* player spawn tile */
    GridDoorGen *gridDoors;
    uint32_t gridDoorCount;
    /* The SCP-860-1 forest maze (GenForestGrid), 10x10, generated
     * inside the same stream when cont2_860_1 is filled:
     * 0 empty, 1 path, 3 door. */
    unsigned char forestGrid[100];
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
                int introEnabled, GeneratedMap *out);

/* Blitz3D's exact RNG, exposed for the other seed-tied systems the
 * source drives from the same generator (the 106 spawn timer drawn
 * right after CreateMap, and the maintenance-tunnel maze which the
 * source reseeds with the map seed). mapGenerate leaves the stream at
 * the source's post-CreateMap position. */
void mapSeedRnd(int32_t seed);
int mapRandInt(int from, int to);
float mapRandFloat(float from, float to);
void mapFree(GeneratedMap *map);

#endif
