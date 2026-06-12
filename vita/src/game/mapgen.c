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
                tpl = pickTemplate(templates, zone, SHAPE_ROOM1);
            } else if (conn == 2) {
                if (left && right) {
                    angle = (rand1(2) == 1) ? 1 : 3;
                    tpl = pickTemplate(templates, zone, SHAPE_ROOM2);
                } else if (up && down) {
                    angle = (rand1(2) == 1) ? 2 : 0;
                    tpl = pickTemplate(templates, zone, SHAPE_ROOM2);
                } else {
                    if (left && up) angle = 2;
                    else if (right && up) angle = 1;
                    else if (left && down) angle = 3;
                    else angle = 0;
                    tpl = pickTemplate(templates, zone, SHAPE_ROOM2C);
                }
            } else if (conn == 3) {
                if (!down) angle = 2;
                else if (!left) angle = 1;
                else if (!right) angle = 3;
                else angle = 0;
                tpl = pickTemplate(templates, zone, SHAPE_ROOM3);
            } else {
                angle = rand1(4);
                tpl = pickTemplate(templates, zone, SHAPE_ROOM4);
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
