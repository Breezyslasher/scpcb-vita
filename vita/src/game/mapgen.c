#include "mapgen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRID MAPGEN_GRID
#define TILE_NONE 0
#define TILE_HALL 1
#define TILE_CHECKPOINT 255

/* ---- Blitz-style RNG: Rand(a,b) inclusive, Rand(n) = Rand(1,n) ---- */

static uint32_t rngState;

static uint32_t rngNext(void) {
    rngState = rngState * 1664525u + 1013904223u;
    return rngState >> 8;
}

static int randRange(int a, int b) {
    if (b < a) {
        int t = a;
        a = b;
        b = t;
    }
    return a + (int)(rngNext() % (uint32_t)(b - a + 1));
}

static int rand1(int n) {
    return randRange(1, n);
}

/* ---------------- rooms.ini parsing ---------------- */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'
                       || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    return s;
}

static int keyEq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (char)((*a >= 'A' && *a <= 'Z') ? *a + 32 : *a);
        char cb = (char)((*b >= 'A' && *b <= 'Z') ? *b + 32 : *b);
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int addTemplate(RoomTemplateList *list, const char *name) {
    RoomTemplateInfo *grown = (RoomTemplateInfo *)realloc(
        list->items, (list->count + 1) * sizeof(RoomTemplateInfo));
    if (!grown) return 0;
    list->items = grown;
    RoomTemplateInfo *t = &list->items[list->count];
    memset(t, 0, sizeof(*t));
    t->name = strdup(name);
    t->shape = -1;
    if (!t->name) return 0;
    /* Lowercase like the game (rt\Name = Lower(Loc)). */
    for (char *p = t->name; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    }
    list->count++;
    return 1;
}

int templatesLoad(const char *iniPath, RoomTemplateList *out) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(iniPath, "rb");
    if (!f) return 0;

    char line[512];
    RoomTemplateInfo *cur = NULL;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (s[0] == ';' || s[0] == '\0') continue;
        if (s[0] == '[') {
            char *end = strchr(s, ']');
            if (!end) continue;
            *end = '\0';
            if (keyEq(s + 1, "room ambience")) {
                cur = NULL;
                continue;
            }
            if (!addTemplate(out, s + 1)) {
                fclose(f);
                return 0;
            }
            cur = &out->items[out->count - 1];
            continue;
        }
        if (!cur) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (keyEq(key, "Mesh Path")) {
            free(cur->meshPath);
            cur->meshPath = strdup(val);
        } else if (keyEq(key, "Shape")) {
            if (keyEq(val, "1")) cur->shape = SHAPE_ROOM1;
            else if (keyEq(val, "2")) cur->shape = SHAPE_ROOM2;
            else if (keyEq(val, "2C")) cur->shape = SHAPE_ROOM2C;
            else if (keyEq(val, "3")) cur->shape = SHAPE_ROOM3;
            else if (keyEq(val, "4")) cur->shape = SHAPE_ROOM4;
        } else if (keyEq(key, "Commonness")) {
            cur->commonness = atoi(val);
            if (cur->commonness < 0) cur->commonness = 0;
            if (cur->commonness > 100) cur->commonness = 100;
        } else if (keyEq(key, "Zone1")) {
            cur->zones[0] = atoi(val);
        } else if (keyEq(key, "Zone2")) {
            cur->zones[1] = atoi(val);
        } else if (keyEq(key, "Zone3")) {
            cur->zones[2] = atoi(val);
        } else if (keyEq(key, "Zone4")) {
            cur->zones[3] = atoi(val);
        } else if (keyEq(key, "Zone5")) {
            cur->zones[4] = atoi(val);
        } else if (keyEq(key, "DisableDecals")) {
            cur->disableDecals = atoi(val);
            if (cur->disableDecals < 0) cur->disableDecals = 0;
            if (cur->disableDecals > 3) cur->disableDecals = 3;
        }
    }
    fclose(f);
    return 1;
}

void templatesFree(RoomTemplateList *list) {
    for (uint32_t i = 0; i < list->count; i++) {
        free(list->items[i].name);
        free(list->items[i].meshPath);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/* ---------------- generation ---------------- */

/* Zone band for a grid row (Map_Core.bb 5620): the top of the grid
 * (high y) is LCZ (1), the bottom EZ (3). */
static int zoneOf(int y) {
    if (y < GRID / 3 + 1) return 3;
    if (y < (int)(GRID * (2.0 / 3.0))) return 2;
    return 1;
}

static int templateInZone(const RoomTemplateInfo *t, int zone) {
    for (int i = 0; i < 5; i++) {
        if (t->zones[i] == zone) return 1;
    }
    return 0;
}

static int findByName(const RoomTemplateList *list, const char *name) {
    for (uint32_t i = 0; i < list->count; i++) {
        if (keyEq(list->items[i].name, name)) return (int)i;
    }
    return -1;
}

/* Weighted template pick, mirroring CreateRoom's commonness loop:
 * random candidates with probability commonness/100, falling back to
 * the first zone/shape match. */
static int pickTemplate(const RoomTemplateList *list, int zone, int shape) {
    int fallback = -1;
    for (int attempt = 0; attempt < 200; attempt++) {
        uint32_t i = rngNext() % list->count;
        const RoomTemplateInfo *t = &list->items[i];
        if (t->shape != shape || !templateInZone(t, zone)) continue;
        if (fallback < 0) fallback = (int)i;
        if (t->commonness > 0 && randRange(0, 99) < t->commonness) {
            return (int)i;
        }
    }
    if (fallback >= 0) return fallback;
    for (uint32_t i = 0; i < list->count; i++) {
        const RoomTemplateInfo *t = &list->items[i];
        if (t->shape == shape && templateInZone(t, zone)) return (int)i;
    }
    return -1;
}

/* ---- forced special rooms (Map_Core.bb SetRoom calls) ----
 * zone: 0 = LCZ, 1 = HCZ, 2 = EZ; weight positions the room within
 * that zone's per-shape queue. */
typedef struct {
    int zone;
    int shape;
    const char *name;
    float weight;
} ForcedRoom;

static const ForcedRoom FORCED_ROOMS[] = {
    { 0, SHAPE_ROOM1, "cont1_173", 0.0f },
    { 0, SHAPE_ROOM1, "cont1_005", 0.15f },
    { 0, SHAPE_ROOM1, "room1_storage", 0.35f },
    { 0, SHAPE_ROOM1, "cont1_914", 0.5f },
    { 0, SHAPE_ROOM1, "cont1_205", 0.65f },
    { 0, SHAPE_ROOM2, "room2_closets", 0.0f },
    { 0, SHAPE_ROOM2, "room2_test_lcz", 0.1f },
    { 0, SHAPE_ROOM2, "cont2_427_714_860_1025", 0.2f },
    { 0, SHAPE_ROOM2, "room2_storage", 0.3f },
    { 0, SHAPE_ROOM2, "room2_gw_2", 0.4f },
    { 0, SHAPE_ROOM2, "cont2_012", 0.5f },
    { 0, SHAPE_ROOM2, "room2_sl", 0.55f },
    { 0, SHAPE_ROOM2, "cont2_500_1499", 0.6f },
    { 0, SHAPE_ROOM2, "cont2_1123", 0.75f },
    { 0, SHAPE_ROOM2, "room2_js", 0.85f },
    { 0, SHAPE_ROOM2, "room2_elevator", 0.9f },
    { 0, SHAPE_ROOM2C, "room2c_gw_lcz", 0.0f },
    { 0, SHAPE_ROOM2C, "cont2c_066_1162_arc", 0.5f },
    { 0, SHAPE_ROOM3, "room3_storage", 0.4f }, /* Rnd(0.2,0.6) */
    { 0, SHAPE_ROOM3, "cont3_372", 0.8f },
    { 0, SHAPE_ROOM4, "room4_ic", 0.3f },
    { 1, SHAPE_ROOM1, "cont1_079", 0.15f },
    { 1, SHAPE_ROOM1, "cont1_106", 0.3f },
    { 1, SHAPE_ROOM1, "cont1_035", 0.45f },
    { 1, SHAPE_ROOM1, "cont1_895", 0.7f },
    { 1, SHAPE_ROOM2, "room2_nuke", 0.1f },
    { 1, SHAPE_ROOM2, "cont2_409", 0.15f },
    { 1, SHAPE_ROOM2, "room2_mt", 0.25f },
    { 1, SHAPE_ROOM2, "cont2_008", 0.4f },
    { 1, SHAPE_ROOM2, "room2_shaft", 0.5f },
    { 1, SHAPE_ROOM2, "cont2_049", 0.6f },
    { 1, SHAPE_ROOM2, "room2_test_hcz", 0.7f },
    { 1, SHAPE_ROOM2, "room2_servers_hcz", 0.9f },
    { 1, SHAPE_ROOM2C, "cont2c_096", 0.5f },
    { 1, SHAPE_ROOM3, "cont3_513", 0.5f },
    { 1, SHAPE_ROOM3, "cont3_966", 0.8f },
    { 2, SHAPE_ROOM1, "gate_b_entrance", 1.0f },
    { 2, SHAPE_ROOM1, "gate_a_entrance", 1.0f },
    { 2, SHAPE_ROOM1, "room1_o5", 1.0f },
    { 2, SHAPE_ROOM1, "room1_lifts", 0.0f },
    { 2, SHAPE_ROOM2, "room2_scientists", 0.1f },
    { 2, SHAPE_ROOM2, "room2_cafeteria", 0.2f },
    { 2, SHAPE_ROOM2, "room2_6_ez", 0.25f },
    { 2, SHAPE_ROOM2, "room2_office_3", 0.3f },
    { 2, SHAPE_ROOM2, "room2_servers_ez", 0.4f },
    { 2, SHAPE_ROOM2, "room2_office", 0.5f },
    { 2, SHAPE_ROOM2, "room2_office_2", 0.55f },
    { 2, SHAPE_ROOM2, "cont2_860_1", 0.6f },
    { 2, SHAPE_ROOM2, "room2_medibay", 0.7f },
    { 2, SHAPE_ROOM2, "room2_scientists_2", 0.8f },
    { 2, SHAPE_ROOM2, "room2_ic", 0.9f },
    { 2, SHAPE_ROOM2C, "room2c_ec", 0.0f },
    { 2, SHAPE_ROOM2C, "room2c_2_ez", 0.0f },
    { 2, SHAPE_ROOM3, "room3_2_ez", 0.3f },
    { 2, SHAPE_ROOM3, "room3_office", 0.5f },
    { 2, SHAPE_ROOM3, "room3_3_ez", 0.7f },
};

#define MAX_FORCED 64

/* SetRoom (Map_Core.bb 4990): place into the per-shape queue at the
 * weighted position within the zone's slice, searching outward for a
 * free slot. */
static void setRoom(int mapRoom[SHAPE_COUNT][MAX_FORCED],
                    const int roomAmount[SHAPE_COUNT][3],
                    int zone, int shape, int tplIndex, float weight) {
    if (tplIndex < 0) return;
    int minPos = 0;
    for (int z = 0; z < zone; z++) minPos += roomAmount[shape][z];
    int maxPos = minPos + roomAmount[shape][zone] - 1;
    if (maxPos < minPos || maxPos >= MAX_FORCED) return;
    if (weight < 0) weight = 0;
    if (weight > 1) weight = 1;
    int pos = minPos + (int)(weight * (maxPos - minPos));
    if (mapRoom[shape][pos] < 0) {
        mapRoom[shape][pos] = tplIndex;
        return;
    }
    int span = (maxPos - pos) > (pos - minPos) ? (maxPos - pos)
                                               : (pos - minPos);
    for (int off = 1; off <= span; off++) {
        if (pos + off <= maxPos && mapRoom[shape][pos + off] < 0) {
            mapRoom[shape][pos + off] = tplIndex;
            return;
        }
        if (pos - off >= minPos && mapRoom[shape][pos - off] < 0) {
            mapRoom[shape][pos - off] = tplIndex;
            return;
        }
    }
}

static int addRoom(GeneratedMap *map, int tpl, int x, int y, int angle) {
    RoomPlacement *grown = (RoomPlacement *)realloc(
        map->rooms, (map->roomCount + 1) * sizeof(RoomPlacement));
    if (!grown) return 0;
    map->rooms = grown;
    RoomPlacement *p = &map->rooms[map->roomCount++];
    p->templateIndex = tpl;
    p->gridX = x;
    p->gridY = y;
    p->angle = angle;
    return 1;
}

#define AT(g, x, y) ((g)[(x) + (y) * GRID])
#define OCC(g, x, y) (AT(g, x, y) != TILE_NONE ? 1 : 0)

int mapGenerate(const RoomTemplateList *templates, uint32_t seed,
                GeneratedMap *out) {
    memset(out, 0, sizeof(*out));
    rngState = seed ? seed : 1;

    uint8_t grid[GRID * GRID];
    memset(grid, TILE_NONE, sizeof(grid));

    int transition0 = (int)(GRID * (2.0 / 3.0)) + 1;
    int transition1 = (int)(GRID * (1.0 / 3.0)) + 1;
    (void)transition0;
    (void)transition1;

    /* Hallway layout (Map_Core.bb 5208..5276). */
    int x = GRID / 2;
    int y = GRID - 2;
    for (int i = y; i <= GRID - 1; i++) {
        AT(grid, x, i) = TILE_HALL;
    }
    out->startX = x;
    out->startY = GRID - 1;

    do {
        int x2 = (int)(GRID * 0.6);
        int width = randRange(x2, (int)(GRID * 0.85));
        if (x > x2) {
            width = -width;
        } else if (x > (int)(GRID * 0.4)) {
            x = x - width / 2;
        }
        if (x + width > GRID - 3) {
            width = GRID - 3 - x;
        } else if (x + width < 2) {
            width = -x + 2;
        }
        if (x + width < x) x = x + width;
        if (width < 0) width = -width;
        for (int i = x; i <= x + width && i < GRID; i++) {
            AT(grid, i, y) = TILE_HALL;
        }

        int height = randRange(3, 4);
        if (y - height < 1) height = y - 1;
        int yHallways = randRange(4, 5);
        if (zoneOf(y - height) != zoneOf(y - height + 1)) height--;

        int temp = x;
        for (int i = 1; i <= yHallways; i++) {
            int hx = randRange(x, x + width - 1);
            if (hx < 2) hx = 2;
            if (hx > GRID - 2) hx = GRID - 2;
            while (hx + 1 < GRID
                   && (OCC(grid, hx, y - 1) || OCC(grid, hx - 1, y - 1)
                       || OCC(grid, hx + 1, y - 1))) {
                hx++;
            }
            if (hx < x + width) {
                int tempHeight;
                if (i == 1) {
                    tempHeight = height;
                    hx = (rand1(2) == 1) ? x : x + width;
                } else {
                    tempHeight = rand1(height > 0 ? height : 1);
                }
                for (int y2 = y - tempHeight; y2 <= y; y2++) {
                    if (y2 < 0) continue;
                    if (zoneOf(y2) != zoneOf(y2 + 1)) {
                        AT(grid, hx, y2) = TILE_CHECKPOINT;
                    } else {
                        AT(grid, hx, y2) = TILE_HALL;
                    }
                }
                if (tempHeight == height) temp = hx;
            }
        }
        x = temp;
        y = y - height;
    } while (y >= 2);

    /* Force more ROOM1 (Map_Core.bb 5320..): each zone needs at least
     * five dead-ends so the forced special rooms get slots; grow one
     * from an empty cell whose single neighbor is a 2- or 3-connection
     * room (which upgrades to a 3 or 4). */
    {
        int trans0 = (int)(GRID * (2.0 / 3.0)) + 1;
        int trans1 = (int)(GRID * (1.0 / 3.0)) + 1;
        for (int zi = 0; zi < 3; zi++) {
            int ymin = (zi == 2) ? 1 : (zi == 0 ? trans0 : trans1);
            int ymax = (zi == 0) ? GRID - 2
                     : (zi == 1 ? trans0 - 1 : trans1 - 1);
            int count = 0;
            for (y = ymin; y <= ymax; y++) {
                for (x = 1; x <= GRID - 2; x++) {
                    if (AT(grid, x, y) == TILE_NONE
                        || AT(grid, x, y) == TILE_CHECKPOINT) continue;
                    int conn = OCC(grid, x + 1, y) + OCC(grid, x - 1, y)
                             + OCC(grid, x, y + 1) + OCC(grid, x, y - 1);
                    if (conn == 1) count++;
                }
            }
            int need = 5 - count;
            for (y = ymin; y <= ymax && need > 0; y++) {
                for (x = 1; x <= GRID - 2 && need > 0; x++) {
                    if (AT(grid, x, y) != TILE_NONE) continue;
                    int conn = OCC(grid, x + 1, y) + OCC(grid, x - 1, y)
                             + OCC(grid, x, y + 1) + OCC(grid, x, y - 1);
                    if (conn != 1) continue;
                    int nx = x, ny = y;
                    if (OCC(grid, x + 1, y)) nx = x + 1;
                    else if (OCC(grid, x - 1, y)) nx = x - 1;
                    else if (OCC(grid, x, y + 1)) ny = y + 1;
                    else ny = y - 1;
                    if (AT(grid, nx, ny) == TILE_CHECKPOINT) continue;
                    int nconn = OCC(grid, nx + 1, ny) + OCC(grid, nx - 1, ny)
                              + OCC(grid, nx, ny + 1) + OCC(grid, nx, ny - 1);
                    if (nconn < 2 || nconn > 3) continue;
                    if (!(y < ymax || ny < y || zi == 0)) continue;
                    AT(grid, x, y) = TILE_HALL;
                    need--;
                }
            }
        }
    }

    /* Count rooms per shape and zone slice (Map_Core.bb 5280..),
     * LCZ first to match assignment order, then build the forced-room
     * queues. Zone index: 0 = LCZ (high y), 1 = HCZ, 2 = EZ. */
    int roomAmount[SHAPE_COUNT][3];
    memset(roomAmount, 0, sizeof(roomAmount));
    for (y = GRID - 1; y >= 1; y--) {
        int zi = 3 - zoneOf(y);
        for (x = 1; x <= GRID - 2; x++) {
            if (AT(grid, x, y) == TILE_NONE
                || AT(grid, x, y) == TILE_CHECKPOINT) {
                continue;
            }
            int right = OCC(grid, x + 1, y), left = OCC(grid, x - 1, y);
            int up = y + 1 < GRID ? OCC(grid, x, y + 1) : 0;
            int down = y - 1 >= 0 ? OCC(grid, x, y - 1) : 0;
            int conn = right + left + up + down;
            if (conn == 1) roomAmount[SHAPE_ROOM1][zi]++;
            else if (conn == 2 && ((left && right) || (up && down)))
                roomAmount[SHAPE_ROOM2][zi]++;
            else if (conn == 2) roomAmount[SHAPE_ROOM2C][zi]++;
            else if (conn == 3) roomAmount[SHAPE_ROOM3][zi]++;
            else if (conn == 4) roomAmount[SHAPE_ROOM4][zi]++;
        }
    }

    int mapRoom[SHAPE_COUNT][MAX_FORCED];
    for (int s = 0; s < SHAPE_COUNT; s++) {
        for (int p = 0; p < MAX_FORCED; p++) mapRoom[s][p] = -1;
    }
    for (unsigned f = 0; f < sizeof(FORCED_ROOMS) / sizeof(FORCED_ROOMS[0]);
         f++) {
        const ForcedRoom *fr = &FORCED_ROOMS[f];
        setRoom(mapRoom, roomAmount, fr->zone, fr->shape,
                findByName(templates, fr->name), fr->weight);
    }
    int roomID[SHAPE_COUNT];
    memset(roomID, 0, sizeof(roomID));

    /* Room typing + template assignment (Map_Core.bb 5620..5737). */
    int chkLczHcz = findByName(templates, "room2_checkpoint_lcz_hcz");
    int chkHczEz = findByName(templates, "room2_checkpoint_hcz_ez");

    for (y = GRID - 1; y >= 1; y--) {
        int zone = zoneOf(y);
        for (x = 1; x <= GRID - 2; x++) {
            if (AT(grid, x, y) == TILE_NONE) continue;

            int right = OCC(grid, x + 1, y), left = OCC(grid, x - 1, y);
            int up = y + 1 < GRID ? OCC(grid, x, y + 1) : 0;
            int down = y - 1 >= 0 ? OCC(grid, x, y - 1) : 0;
            int conn = right + left + up + down;
            int tpl = -1, angle = 0;

            if (AT(grid, x, y) == TILE_CHECKPOINT) {
                tpl = (y > GRID / 2) ? chkLczHcz : chkHczEz;
                if (tpl < 0) tpl = pickTemplate(templates, zone, SHAPE_ROOM2);
                angle = 0;
            } else if (conn == 1) {
                if (up) angle = 2;
                else if (left) angle = 3;
                else if (right) angle = 1;
                else angle = 0;
                tpl = roomID[SHAPE_ROOM1] < MAX_FORCED ? mapRoom[SHAPE_ROOM1][roomID[SHAPE_ROOM1]] : -1;
                if (tpl < 0) tpl = pickTemplate(templates, zone, SHAPE_ROOM1);
                roomID[SHAPE_ROOM1]++;
            } else if (conn == 2) {
                if ((left && right) || (up && down)) {
                    if (left && right) {
                        angle = (rand1(2) == 1) ? 1 : 3;
                    } else {
                        angle = (rand1(2) == 1) ? 2 : 0;
                    }
                    tpl = roomID[SHAPE_ROOM2] < MAX_FORCED
                        ? mapRoom[SHAPE_ROOM2][roomID[SHAPE_ROOM2]] : -1;
                    if (tpl < 0) {
                        tpl = pickTemplate(templates, zone, SHAPE_ROOM2);
                    }
                    roomID[SHAPE_ROOM2]++;
                } else {
                    if (left && up) angle = 2;
                    else if (right && up) angle = 1;
                    else if (left && down) angle = 3;
                    else angle = 0;
                    tpl = roomID[SHAPE_ROOM2C] < MAX_FORCED ? mapRoom[SHAPE_ROOM2C][roomID[SHAPE_ROOM2C]] : -1;
                if (tpl < 0) tpl = pickTemplate(templates, zone, SHAPE_ROOM2C);
                roomID[SHAPE_ROOM2C]++;
                }
            } else if (conn == 3) {
                if (!down) angle = 2;
                else if (!left) angle = 1;
                else if (!right) angle = 3;
                else angle = 0;
                tpl = roomID[SHAPE_ROOM3] < MAX_FORCED ? mapRoom[SHAPE_ROOM3][roomID[SHAPE_ROOM3]] : -1;
                if (tpl < 0) tpl = pickTemplate(templates, zone, SHAPE_ROOM3);
                roomID[SHAPE_ROOM3]++;
            } else {
                angle = rand1(4);
                tpl = roomID[SHAPE_ROOM4] < MAX_FORCED ? mapRoom[SHAPE_ROOM4][roomID[SHAPE_ROOM4]] : -1;
                if (tpl < 0) tpl = pickTemplate(templates, zone, SHAPE_ROOM4);
                roomID[SHAPE_ROOM4]++;
            }

            if (tpl < 0 || !addRoom(out, tpl, x, y, angle)) {
                mapFree(out);
                return 0;
            }
        }
    }
    return out->roomCount > 0;
}

void mapFree(GeneratedMap *map) {
    free(map->rooms);
    memset(map, 0, sizeof(*map));
}
