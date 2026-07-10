#include "mapgen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRID MAPGEN_GRID
#define TILE_NONE 0
#define TILE_HALL 1
#define TILE_CHECKPOINT 255

/* ---- Blitz3D's exact RNG (bbruntime/bbmath.cpp): the Park-Miller
 * minimal standard (A=48271, M=2^31-1) via Schrage's method, with
 * Blitz's float mapping. Verified against the published Blitz3D
 * source. All arithmetic must stay in float32 (no FMA contraction -
 * see the CMake -ffp-contract=off on this file). ---- */

static int32_t bbState = 1;

static float bbRndF(void) {
    bbState = 48271 * (bbState % 44488) - 3399 * (bbState / 44488);
    if (bbState < 0) bbState += 2147483647;
    return (float)(bbState & 65535) / 65536.0f + 0.5f / 65536.0f;
}

static void bbSeedRnd(int32_t seed) {
    seed &= 0x7fffffff;
    bbState = seed ? seed : 1;
}

/* Rand(a,b) inclusive; Rand(n) = Rand(1,n). Swaps like bbRand. */
static int bbRand(int from, int to) {
    if (to < from) {
        int t = from;
        from = to;
        to = t;
    }
    return (int)(bbRndF() * (float)(to - from + 1)) + from;
}

static float bbRnd(float from, float to) {
    return bbRndF() * (to - from) + from;
}

static int rand1(int n) {
    return bbRand(1, n);
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
            /* rooms.ini uses Windows separators; the port loads from a
               POSIX/Vita filesystem (e.g. dimension1499\foo.rmesh). */
            if (cur->meshPath) {
                for (char *s = cur->meshPath; *s; s++) {
                    if (*s == '\\') *s = '/';
                }
            }
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

/* Zone band for a grid row. Exact port of Math_Core.bb GetZone():
 *   GetZone(y) = Min(Floor((GridSize - y) / GridSize * 3), 2)
 * giving 0 = LCZ (high y) .. 2 = EZ (low y). Returned +1 so the port
 * uses 1 = LCZ, 2 = HCZ, 3 = EZ, matching rooms.ini's Zone fields.
 * The integer form (GRID - y) * 3 / GRID floors for y in 1..GRID. */
int mapZoneOf(int y) {
    int z = (GRID - y) * 3 / GRID;
    if (z < 0) z = 0;
    if (z > 2) z = 2;
    return z + 1;
}

static int zoneOf(int y) {
    return mapZoneOf(y);
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

/* =====================================================================
 * Blitz-exact CreateMap transcription (Map_Core.bb 5190-5949).
 * Every Rand/Rnd the source performs between SeedRnd and the end of
 * the door pass is replayed in the same order, including the RNG
 * consumed by each created room's FillRoom (items, doors, decals,
 * room lights, the 860-1 forest) - that is what makes a given seed
 * produce the PC game's map. Globals the source rolls from the
 * wall-clock stream before seeding are pinned: KEY2_SPAWNRATE = 5
 * (the one value with no FillRoom item site) and
 * I_005\ChanceToSpawn = 1; achievements (S-NAV/E-Reader unlocks) are
 * assumed locked. PC runs with other values diverge in filler-room
 * choice past those rooms - the source itself has that property.
 * ===================================================================== */

#include "room_gen_data.h"

#define KEY2_SPAWNRATE 5
#define I005_CHANCE 1

/* FillRoom draw helpers. */
static void drawITEM(void) { bbRnd(0.0f, 360.0f); }           /* CreateItem */
static void drawDOOR(void) { bbRand(1, 10); }                 /* CreateDoor */
static void drawBATTERY(void) { bbRand(1, 10); drawITEM(); }  /* CreateRandomBattery */

/* FillRoom's common tail: AddLight per RMesh light entity of the
 * template ('S' sprite light = 4 draws, '1' = 1 draw). */
static void drawRoomLights(const char *name) {
    const int n = (int)(sizeof(ROOM_LIGHT_SEQS) / sizeof(ROOM_LIGHT_SEQS[0]));
    for (int i = 0; i < n; i++) {
        if (strcmp(ROOM_LIGHT_SEQS[i].room, name) != 0) continue;
        for (const char *p = ROOM_LIGHT_SEQS[i].lights; *p; p++) {
            if (*p == 'S') {
                bbRnd(0.36f, 0.4f);
                bbRnd(0.36f, 0.4f);
                bbRnd(0.0f, 360.0f);
                bbRand(1, 50);
            } else {
                bbRand(1, 50);
            }
        }
        return;
    }
}

/* Math_Core Chance(): one draw. */
static int bbChance(int percent) { return bbRand(0, 100) <= percent; }

/* ---- SCP-860-1 forest (GenForestGrid + PlaceForest draw replay) ---- */
#define FG 10

static int fgMoveForward(int dir, int px, int py, int retY) {
    if (dir == 1) return retY ? py + 1 : px;
    return retY ? py : px - 1 + dir;
}

static int fgTurnIfDeviating(int maxDev, int px, int center, int dir,
                             int retDeviated) {
    int cur = center - px;
    int deviated = (dir == 0 && cur >= maxDev)
                || (dir == 2 && cur <= -maxDev);
    if (deviated) dir = (dir + 2) % 4;
    return retDeviated ? deviated : dir;
}

static void forestGen(unsigned char fgrid[FG * FG]) {
    /* GenForestGrid (Map_Core 861): constants at 851-858. */
    int door1 = bbRand(3, 7);
    int door2 = bbRand(3, 7);
    memset(fgrid, 0, FG * FG);
    fgrid[door1] = 3;
    fgrid[(FG - 1) * FG + door2] = 3;

    int px = door2, py = 1, dir = 1, deviated;
    fgrid[(FG - 1 - py) * FG + px] = 1;
    while (py < FG - 4) {
        if (dir == 1) {
            if (bbChance(40)) {
                dir = 2 * bbRand(0, 1);
                dir = fgTurnIfDeviating(3, px, 5, dir, 0);
                deviated = fgTurnIfDeviating(3, px, 5, dir, 1);
                if (deviated) fgrid[(FG - 1 - py) * FG + px] = 1;
                px = fgMoveForward(dir, px, py, 0);
                py = fgMoveForward(dir, px, py, 1);
            }
        } else {
            dir = fgTurnIfDeviating(3, px, 5, dir, 0);
            deviated = fgTurnIfDeviating(3, px, 5, dir, 1);
            if (deviated || bbChance(27)) dir = 1;
            px = fgMoveForward(dir, px, py, 0);
            py = fgMoveForward(dir, px, py, 1);
            if (dir == 1) {
                fgrid[(FG - 1 - py) * FG + px] = 1;
                px = fgMoveForward(dir, px, py, 0);
                py = fgMoveForward(dir, px, py, 1);
            }
        }
        fgrid[(FG - 1 - py) * FG + px] = 1;
    }
    dir = 1;
    while (py < FG - 2) {
        px = fgMoveForward(dir, px, py, 0);
        py = fgMoveForward(dir, px, py, 1);
        fgrid[(FG - 1 - py) * FG + px] = 1;
    }
    if (px != door1) {
        dir = door1 > px ? 2 : 0;
        while (px != door1) {
            px = fgMoveForward(dir, px, py, 0);
            py = fgMoveForward(dir, px, py, 1);
            fgrid[(FG - 1 - py) * FG + px] = 1;
        }
    }
    /* Branches. Signed scratch: branches write -1 first. */
    signed char g[FG * FG];
    for (int i = 0; i < FG * FG; i++) g[i] = (signed char)fgrid[i];
    int newY = -3;
    while (newY < FG - 6) {
        newY += 4;
        int tempY = newY;
        int newX = 0;
        if (bbChance(65)) {
            int branchPos = 2 * bbRand(0, 1);
            int leftMost = FG - 1, rightMost = 0;
            for (int i = 0; i < FG; i++) {
                if (g[(FG - 1 - newY) * FG + i] == 1) {
                    if (i < leftMost) leftMost = i;
                    if (i > rightMost) rightMost = i;
                }
            }
            newX = branchPos == 0 ? leftMost - 1 : rightMost + 1;
            if (newX >= 0 && newX < FG
                && g[(FG - 1 - tempY - 1) * FG + newX] != 1
                && g[(FG - 1 - tempY + 1) * FG + newX] != 1) {
                g[(FG - 1 - tempY) * FG + newX] = -1;
                newX = branchPos == 0 ? leftMost - 2 : rightMost + 2;
                if (newX >= 0 && newX < FG) {
                    g[(FG - 1 - tempY) * FG + newX] = -1;
                    int i = 2;
                    while (i < 4) {
                        i++;
                        if (bbChance(18)) break;
                        if (bbRand(0, 3) == 0) {
                            newX += 1 - 2 * (branchPos == 0 ? 1 : 0);
                        } else {
                            tempY += 1;
                        }
                        if (newX < 0 || newX >= FG
                            || g[(FG - 1 - tempY - 1) * FG + newX] == 1) {
                            break;
                        }
                        g[(FG - 1 - tempY) * FG + newX] = -1;
                        if (tempY >= FG - 2) break;
                    }
                }
            }
        }
    }
    for (int i = 1; i <= FG - 2; i++) {
        for (int j = 0; j < FG; j++) {
            if (g[i * FG + j] == -1) g[i * FG + j] = 1;
        }
    }
    for (int i = 0; i < FG * FG; i++) fgrid[i] = (unsigned char)(g[i] < 0 ? 0 : g[i]);
}

/* PlaceForest (Map_Core 1060): the tile detail pass draws per
 * heightmap pixel; door wall pass draws one DOOR per forest door. */
static void forestPlaceDraws(const unsigned char fgrid[FG * FG]) {
    int itemPlaced[4] = { 0, 0, 0, 0 };
    for (int tX = 0; tX < FG; tX++) {
        for (int tY = 1; tY <= FG - 2; tY++) {
            if (fgrid[tY * FG + tX] != 1) continue;
            int t = 0;
            if (tX + 1 < FG) t += fgrid[tY * FG + tX + 1] > 0;
            if (tX - 1 >= 0) t += fgrid[tY * FG + tX - 1] > 0;
            if (tY + 1 < FG) t += fgrid[(tY + 1) * FG + tX] > 0;
            if (tY - 1 >= 0) t += fgrid[(tY - 1) * FG + tX] > 0;
            int tileType;               /* 1..5 = ROOM1..ROOM4 + 1 */
            if (t == 1) tileType = 1;
            else if (t == 2) {
                if ((fgrid[(tY - 1) * FG + tX] > 0
                     && fgrid[(tY + 1) * FG + tX] > 0)
                    || (fgrid[tY * FG + tX + 1] > 0
                        && fgrid[tY * FG + tX - 1] > 0)) {
                    tileType = 2;
                } else {
                    tileType = 3;
                }
            } else if (t == 3) tileType = 4;
            else tileType = 5;

            /* Detail pass: lX 3..W-2, lY 3..W-2; GetColor(lX, W-lY). */
            const unsigned char *red = FOREST_HMAP_RED[tileType - 1];
            for (int lX = 3; lX <= FOREST_HMAP_W - 2; lX++) {
                for (int lY = 3; lY <= FOREST_HMAP_W - 2; lY++) {
                    int colorR = red[(FOREST_HMAP_W - lY) * FOREST_HMAP_W
                                     + lX];
                    if (colorR > bbRand(100, 260)) {
                        if (bbRand(0, 7) <= 6) {
                            bbRnd(0.25f, 0.4f);
                            for (int i = 0; i < 4; i++) {
                                bbRnd(-20.0f, 20.0f);
                            }
                            bbRnd(3.0f, 3.2f);
                            bbRnd(-5.0f, 5.0f);
                            bbRnd(0.0f, 360.0f);
                        } else {
                            bbRnd(0.01f, 0.012f);
                            bbRnd(0.0f, 360.0f);
                        }
                    }
                }
            }
            /* One log item per 3-row band. */
            int band = tY / 3;
            if ((tY % 3) == 2 && !itemPlaced[band]) {
                itemPlaced[band] = 1;
                drawITEM();
            }
        }
    }
    /* The two wall doors (CreateDoor each). */
    for (int i = 0; i < 2; i++) {
        int tY = i * (FG - 1);
        for (int tX = 3; tX <= 7; tX++) {
            if (fgrid[tY * FG + tX] == 3) {
                drawDOOR();
                break;
            }
        }
    }
}

/* FillRoom's RNG stream per room template (extracted 1:1 from
 * Rooms_Core.bb FillRoom; see the session's draw ledger). Cases not
 * listed draw nothing. Ends with the common light tail. */
static void fillRoomDraws(const char *nm, unsigned char forestGrid[100],
                          int gateEntrancePlaced, int chkExtraDoor) {
    if (!strcmp(nm, "room1_storage")) {
        drawDOOR();
        if (KEY2_SPAWNRATE == 6) drawITEM();
        drawITEM(); drawITEM(); drawITEM();
        drawBATTERY();
        drawITEM(); drawITEM(); drawITEM(); drawITEM();
    } else if (!strcmp(nm, "room1_dead_end_lcz")) {
        drawDOOR();
    } else if (!strcmp(nm, "room1_dead_end_ez")) {
        drawDOOR(); drawDOOR();
    } else if (!strcmp(nm, "cont1_005")) {
        drawDOOR();
        drawITEM();
        if (I005_CHANCE == 1) { drawITEM(); bbRnd(0.0f, 360.0f); }
        else if (I005_CHANCE == 3) drawITEM();
    } else if (!strcmp(nm, "cont1_173")) {
        for (int i = 0; i < 10; i++) drawDOOR();
        bbRnd(0.0f, 360.0f);
        bbRnd(0.0f, 360.0f);
        for (int i = 0; i < 5; i++) bbRnd(0.8f, 1.0f);
        drawITEM(); drawITEM();
        if (bbRand(1, 3) == 1) drawITEM();
        drawITEM();
        if (KEY2_SPAWNRATE == 1) { bbRand(1, 4); drawITEM(); }
        /* S-NAV / E-Reader achievement unlocks assumed locked. */
    } else if (!strcmp(nm, "cont1_173_intro")) {
        for (int i = 0; i < 16; i++) drawDOOR();
        for (int i = 0; i < 5; i++) bbRnd(0.8f, 1.0f);
        bbRnd(0.0f, 360.0f); bbRnd(0.5f, 0.7f); bbRnd(0.6f, 1.0f);
        drawITEM();
    } else if (!strcmp(nm, "cont1_205")) {
        for (int i = 0; i < 4; i++) drawDOOR();
        drawITEM(); drawITEM(); drawITEM();
        drawBATTERY();
        drawITEM();
    } else if (!strcmp(nm, "cont3_372")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        drawITEM(); drawITEM();
        bbRnd(0.0f, 100.0f);
    } else if (!strcmp(nm, "cont1_914")) {
        for (int i = 0; i < 4; i++) drawDOOR();
        drawITEM(); drawITEM(); drawITEM();
    } else if (!strcmp(nm, "room2_2_lcz")) {
        drawDOOR();
        if (bbRand(1, 2) == 1) drawITEM();
    } else if (!strcmp(nm, "room2_4_lcz")) {
        drawDOOR();
    } else if (!strcmp(nm, "room2_6_lcz")) {
        drawDOOR(); drawDOOR();
    } else if (!strcmp(nm, "room2_closets")) {
        for (int i = 0; i < 7; i++) drawDOOR();
        drawITEM();
        drawBATTERY();
        if (bbRand(1, 2) == 1) drawBATTERY();
        if (bbRand(1, 2) == 1) drawBATTERY();
        if (KEY2_SPAWNRATE == 3) { bbRand(1, 2); drawITEM(); }
        drawITEM(); drawITEM();
    } else if (!strcmp(nm, "room2_elevator")) {
        drawDOOR();
    } else if (!strcmp(nm, "room2_gw")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        drawDOOR();
        bbRand(1, 2);
    } else if (!strcmp(nm, "room2_gw_2")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        if (KEY2_SPAWNRATE == 2) drawITEM();
    } else if (!strcmp(nm, "room2_js")) {
        drawDOOR();
        bbRnd(0.0f, 360.0f); bbRnd(0.8f, 1.0f);
        drawITEM(); drawITEM();
        drawBATTERY();
    } else if (!strcmp(nm, "room2_sl")) {
        drawDOOR(); drawDOOR(); drawDOOR();
    } else if (!strcmp(nm, "room2_storage")) {
        for (int i = 0; i < 6; i++) drawDOOR();
        drawITEM();
        drawBATTERY();
        drawITEM(); drawITEM();
    } else if (!strcmp(nm, "room2_tesla_lcz") || !strcmp(nm, "room2_tesla_hcz")
               || !strcmp(nm, "room2_tesla_ez")) {
        drawDOOR();
    } else if (!strcmp(nm, "room2_test_lcz")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        drawITEM();
        bbRnd(0.0f, 100.0f);
        drawITEM();
    } else if (!strcmp(nm, "cont2_012")) {
        drawDOOR(); drawDOOR();
        drawITEM(); drawITEM();
        bbRnd(0.0f, 360.0f);
    } else if (!strcmp(nm, "cont2_427_714_860_1025")) {
        for (int i = 0; i < 8; i++) drawDOOR();
        for (int i = 0; i < 15; i++) {
            bbRand(16, 17);                 /* Rand(DECAL_BLOOD_DROP_1, _2) */
            bbRnd(0.0f, 360.0f);
            bbRnd(0.2f, 0.25f);
            bbRnd(0.1f, 0.17f);
        }
        for (int i = 0; i < 6; i++) drawITEM();
    } else if (!strcmp(nm, "cont2_500_1499")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        drawITEM(); drawITEM(); drawITEM(); drawITEM();
        if (bbRand(1, 4) == 1) drawITEM();
    } else if (!strcmp(nm, "cont2_1123")) {
        for (int i = 0; i < 10; i++) drawDOOR();
        bbRnd(0.0f, 360.0f); bbRnd(0.4f, 0.5f); bbRnd(0.8f, 1.0f);
        bbRnd(0.0f, 360.0f); bbRnd(0.6f, 0.7f); bbRnd(0.8f, 1.0f);
        drawITEM(); drawITEM(); drawITEM(); drawITEM();
    } else if (!strcmp(nm, "room2c_lcz")) {
        drawDOOR();
    } else if (!strcmp(nm, "room2c_gw_lcz")) {
        for (int i = 0; i < 4; i++) drawDOOR();
    } else if (!strcmp(nm, "room2c_gw_2_lcz")) {
        for (int i = 0; i < 4; i++) drawDOOR();
    } else if (!strcmp(nm, "cont2c_066_1162_arc")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        for (int i = 0; i < 5; i++) drawITEM();
        drawBATTERY();
        if (KEY2_SPAWNRATE == 4) drawITEM();
    } else if (!strcmp(nm, "room3_storage")) {
        for (int i = 0; i < 8; i++) drawDOOR();
        bbRand(1, 3);
        drawITEM(); drawITEM(); drawITEM();
        bbRnd(0.0f, 1000.0f);
        bbRnd(0.0f, 360.0f);
    } else if (!strcmp(nm, "room4_ic")) {
        drawDOOR();
        drawITEM(); drawITEM(); drawITEM();
    } else if (!strcmp(nm, "room2_checkpoint_lcz_hcz")
               || !strcmp(nm, "room2_checkpoint_hcz_ez")) {
        drawDOOR(); drawDOOR();
        /* Extra sealed door when the cell south of the checkpoint is
         * empty (Rooms_Core 1873/2916). */
        if (chkExtraDoor) drawDOOR();
    } else if (!strcmp(nm, "cont1_035")) {
        for (int i = 0; i < 4; i++) drawDOOR();
        for (int i = 0; i < 5; i++) drawITEM();
    } else if (!strcmp(nm, "cont1_079")) {
        for (int i = 0; i < 5; i++) drawDOOR();
    } else if (!strcmp(nm, "cont1_106")) {
        for (int i = 0; i < 7; i++) drawDOOR();
        drawITEM(); drawITEM(); drawITEM();
    } else if (!strcmp(nm, "cont1_895")) {
        drawDOOR(); drawDOOR();
        static const float SC[6][2] = {
            { 0.4f, 0.5f }, { 0.4f, 0.5f }, { 0.6f, 0.7f },
            { 0.1f, 0.2f }, { 0.6f, 0.7f }, { 0.7f, 0.8f },
        };
        for (int i = 0; i < 6; i++) {
            bbRnd(SC[i][0], SC[i][1]);
            bbRnd(0.0f, 360.0f);
            bbRnd(0.6f, 0.8f);
        }
        drawITEM(); drawITEM();
        bbRnd(0.0f, 1000.0f);
    } else if (!strcmp(nm, "room2_4_hcz")) {
        drawDOOR();
    } else if (!strcmp(nm, "room2_mt")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        bbRnd(0.0f, 360.0f);
        drawITEM();
    } else if (!strcmp(nm, "room2_nuke")) {
        for (int i = 0; i < 5; i++) drawDOOR();
        drawITEM(); drawITEM();
    } else if (!strcmp(nm, "room2_servers_hcz")) {
        drawDOOR(); drawDOOR(); drawDOOR();
    } else if (!strcmp(nm, "room2_shaft")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        bbRnd(0.0f, 360.0f);
        drawITEM(); drawITEM();
        drawBATTERY(); drawBATTERY();
        drawITEM();
    } else if (!strcmp(nm, "room2_test_hcz")) {
        drawDOOR(); drawDOOR();
        drawITEM();
    } else if (!strcmp(nm, "cont2_008")) {
        for (int i = 0; i < 7; i++) drawDOOR();
        drawITEM(); drawITEM();
        drawBATTERY();
        drawITEM(); drawITEM();
    } else if (!strcmp(nm, "cont2_049")) {
        for (int i = 0; i < 12; i++) drawDOOR();
        drawITEM(); drawITEM(); drawITEM();
    } else if (!strcmp(nm, "cont2_409")) {
        drawDOOR(); drawDOOR(); drawDOOR();
    } else if (!strcmp(nm, "cont2c_096")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        bbRnd(0.0f, 360.0f);
        bbRnd(0.0f, 360.0f);
        drawITEM();
        drawBATTERY();
        drawITEM(); drawITEM();
        bbRnd(0.0f, 1000.0f);
        drawITEM();
    } else if (!strcmp(nm, "cont3_513")) {
        drawDOOR(); drawDOOR();
        static const float SC[12][2] = {
            { 0.8f, 1.0f }, { 0.1f, 0.2f }, { 0.2f, 0.3f },
            { 0.3f, 0.4f }, { 0.1f, 0.15f }, { 0.2f, 0.3f },
            { 0.1f, 0.2f }, { 0.1f, 0.2f }, { 0.1f, 0.2f },
            { 0.2f, 0.3f }, { 0.8f, 1.0f }, { 0.3f, 0.4f },
        };
        for (int i = 0; i < 12; i++) {
            bbRnd(SC[i][0], SC[i][1]);
            bbRnd(0.0f, 360.0f);
            bbRnd(0.6f, 0.8f);
        }
        for (int i = 0; i < 5; i++) drawITEM();
        bbRand(0, 6);
        drawITEM();
    } else if (!strcmp(nm, "cont3_966")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        drawITEM();
        bbRnd(0.0f, 1000.0f);
    } else if (!strcmp(nm, "gate_a_entrance") || !strcmp(nm, "gate_b_entrance")) {
        drawDOOR(); drawDOOR();
    } else if (!strcmp(nm, "gate_a")) {
        for (int i = 0; i < 6; i++) drawDOOR();
        if (gateEntrancePlaced) drawDOOR();
        drawDOOR(); drawDOOR();
    } else if (!strcmp(nm, "gate_b")) {
        if (gateEntrancePlaced) drawDOOR();
        drawDOOR(); drawDOOR(); drawDOOR(); drawDOOR();
    } else if (!strcmp(nm, "room1_lifts")) {
        drawDOOR(); drawDOOR();
    } else if (!strcmp(nm, "room1_o5")) {
        drawDOOR();
        drawITEM(); drawITEM(); drawITEM();
        drawBATTERY();
        drawITEM(); drawITEM();
    } else if (!strcmp(nm, "room2_ez")) {
        drawITEM(); drawITEM(); drawITEM();
        bbRnd(0.0f, 100.0f);
        drawITEM();
    } else if (!strcmp(nm, "room2_2_ez")) {
        drawITEM(); drawITEM();
        bbRand(1, 2); drawITEM();
        drawITEM();
        bbRnd(0.0f, 100.0f);
    } else if (!strcmp(nm, "room2_3_ez")) {
        drawDOOR(); drawDOOR();
        bbRand(1, 2); drawITEM();
        drawITEM(); drawITEM(); drawITEM(); drawITEM();
        if (bbRand(1, 3) == 1) drawITEM();
        drawBATTERY();
        if (bbRand(1, 2) == 1) drawBATTERY();
        if (bbRand(1, 2) == 1) drawBATTERY();
    } else if (!strcmp(nm, "room2_6_ez")) {
        if (bbRand(1, 3) == 1) bbRand(1, 4);
    } else if (!strcmp(nm, "room2_cafeteria")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        for (int i = 0; i < 8; i++) drawITEM();
    } else if (!strcmp(nm, "room2_ic")) {
        drawDOOR();
        bbRnd(0.0f, 360.0f); bbRnd(0.8f, 1.0f);
        drawITEM(); drawITEM(); drawITEM();
    } else if (!strcmp(nm, "room2_medibay")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        bbRand(1, 2);
        drawITEM();
        bbRand(1, 2);
        drawITEM(); drawITEM();
    } else if (!strcmp(nm, "room2_office")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        drawITEM();
        drawBATTERY();
        drawITEM();
    } else if (!strcmp(nm, "room2_office_2")) {
        bbRand(1, 5); drawDOOR();
        drawBATTERY();
        if (bbRand(1, 2) == 1) drawBATTERY();
        if (bbRand(1, 3) == 1) drawITEM();
    } else if (!strcmp(nm, "room2_office_3")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        for (int i = 0; i < 5; i++) drawITEM();
    } else if (!strcmp(nm, "room2_servers_ez")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        drawITEM();
    } else if (!strcmp(nm, "room2_scientists")) {
        drawDOOR(); drawDOOR(); drawDOOR();
        for (int i = 0; i < 8; i++) drawITEM();
        drawITEM();                        /* one either branch */
    } else if (!strcmp(nm, "room2_scientists_2")) {
        drawDOOR(); drawDOOR();
        bbRnd(0.0f, 360.0f); bbRnd(0.0f, 360.0f); bbRnd(0.0f, 360.0f);
        drawITEM(); drawITEM(); drawITEM();
    } else if (!strcmp(nm, "cont2_860_1")) {
        for (int i = 0; i < 5; i++) drawDOOR();
        forestGen(forestGrid);
        forestPlaceDraws(forestGrid);
        drawITEM();
    } else if (!strcmp(nm, "room2c_2_ez")) {
        bbRand(1, 4); drawDOOR();
        bbRand(1, 3); drawITEM();
        drawITEM();
        drawBATTERY();
    } else if (!strcmp(nm, "room2c_ec")) {
        drawDOOR(); drawDOOR();
        drawITEM();
    } else if (!strcmp(nm, "room3_ez")) {
        bbRand(1, 6); drawDOOR();
        drawDOOR();
        drawBATTERY();
        int temp = bbRand(1, 5);
        if (temp > 3) {
            drawITEM(); bbRnd(0.0f, 100.0f); drawITEM(); drawBATTERY();
        } else if (temp > 1) {
            drawITEM(); bbRnd(0.0f, 100.0f); drawBATTERY();
        }
        if (bbRand(1, 2) == 1) drawITEM();
    } else if (!strcmp(nm, "room3_2_ez")) {
        drawBATTERY();
        if (bbRand(1, 2) == 1) drawBATTERY();
        if (bbRand(1, 2) == 1) drawBATTERY();
        drawITEM();
        bbRnd(0.0f, 100.0f);
    } else if (!strcmp(nm, "room3_3_ez")) {
        drawITEM(); drawITEM();
    } else if (!strcmp(nm, "room3_gw")) {
        for (int i = 0; i < 6; i++) drawDOOR();
    } else if (!strcmp(nm, "room3_office")) {
        drawDOOR();
        drawITEM();
        if (bbRand(1, 2) == 1) drawITEM();
        if (bbRand(1, 2) == 1) drawITEM();
        bbRnd(0.0f, 360.0f); bbRnd(0.5f, 0.7f);
    } else if (!strcmp(nm, "room4_2_ez")) {
        bbRand(1, 5); drawDOOR();
        drawDOOR();
        drawDOOR();
        bbRand(1, 3); drawDOOR();
        int temp = bbRand(1, 5);
        if (temp > 3) {
            drawITEM(); bbRnd(0.0f, 100.0f); drawITEM(); drawBATTERY();
            bbRand(1, 22); drawITEM();     /* GetRandDocument */
            drawITEM(); drawBATTERY();
        } else if (temp > 1) {
            drawITEM(); bbRnd(0.0f, 100.0f); drawITEM(); drawBATTERY();
            drawITEM(); drawITEM();
        }
        if (bbRand(1, 2) == 1) drawITEM();
    } else if (!strcmp(nm, "dimension_106")) {
        for (int i = 0; i < 10; i++) { bbRnd(0.8f, 1.0f); drawDOOR(); }
        drawDOOR(); drawDOOR();
        drawITEM(); drawITEM();
    }
    /* dimension_1499, room3_lcz, room2c_2_lcz, room2c_ez, room3_4_ez,
     * room2_2_hcz, room3_hcz, room4_hcz, room3_2_hcz, room2_5_hcz,
     * room2_6_hcz, room4_2_hcz, room2_4_ez, room2_5_ez, room4_ez,
     * room2_3_lcz, room2_5_lcz, room2_lcz, room3_2_lcz, room4_lcz:
     * no case draws. */
    drawRoomLights(nm);
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

/* CreateRoom's template pick (Map_Core 2482-2496): the cumulative
 * commonness walk over rooms.ini order, one Rand(total) draw. */
static int createRoomPick(const RoomTemplateList *list, int zone,
                          int shape) {
    int total = 0;
    for (uint32_t t = 0; t < list->count; t++) {
        const RoomTemplateInfo *rt = &list->items[t];
        for (int i = 0; i < 5; i++) {
            if (rt->zones[i] == zone) {
                if (rt->shape == shape) {
                    total += rt->commonness;
                    break;             /* source Exit */
                }
            }
        }
    }
    int randomRoom = bbRand(1, total);
    int acc = 0;
    for (uint32_t t = 0; t < list->count; t++) {
        const RoomTemplateInfo *rt = &list->items[t];
        for (int i = 0; i < 5; i++) {
            if (rt->zones[i] == zone && rt->shape == shape) {
                acc += rt->commonness;
                if (randomRoom > acc - rt->commonness && randomRoom <= acc) {
                    return (int)t;
                }
            }
        }
    }
    return -1;
}

int mapGenerate(const RoomTemplateList *templates, uint32_t seed,
                int introEnabled, GeneratedMap *out) {
    memset(out, 0, sizeof(*out));
    bbSeedRnd((int32_t)seed);

    /* Grid: the source dims Grid[PowTwo(MapGridSize + 1)] = [361],
     * i.e. 362 zero-initialised ints, and freely indexes flat past a
     * row end (e.g. x + 18*18) - those reads are valid zeros. Mirror
     * the exact layout. */
    int grid[(GRID + 1) * (GRID + 1) + 1];
    memset(grid, 0, sizeof(grid));
#define G(ix) grid[ix]
#define GXY(x_, y_) grid[(x_) + (y_) * GRID]

    int transition0 = (int)(GRID * (2.0 / 3.0)) + 1;   /* 13 */
    int transition1 = (int)(GRID * (1.0 / 3.0)) + 1;   /* 7 */

    /* ---- hallway walk (5205-5276) ---- */
    int x = GRID / 2;
    int y = GRID - 2;
    for (int i = y; i <= GRID - 1; i++) GXY(x, i) = 1;
    out->startX = x;
    out->startY = GRID - 1;

    int temp = 0;
    do {
        int x2 = (int)(GRID * 0.6);                    /* Floor */
        int width = bbRand(x2, (int)(GRID * 0.85));
        if (x > x2) {
            width = -width;
        } else if (x > (int)(GRID * 0.4)) {
            x = x - width / 2;
        }
        if (x + width > GRID - 3) width = GRID - 3 - x;
        else if (x + width < 2) width = -x + 2;

        if (x + width < x) x = x + width;
        if (width < 0) width = -width;
        for (int i = x; i <= x + width; i++) {
            GXY(i < GRID ? i : GRID, y) = 1;          /* Min(i, GRID) */
        }

        int height = bbRand(3, 4);
        if (y - height < 1) height = y - 1;
        int yHallways = bbRand(4, 5);
        if (mapZoneOf(y - height) != mapZoneOf(y - height + 1)) height--;

        for (int i = 1; i <= yHallways; i++) {
            x2 = bbRand(x, x + width - 1);
            if (x2 < 2) x2 = 2;
            if (x2 > GRID - 2) x2 = GRID - 2;
            while (G(x2 + (y - 1) * GRID) || G((x2 - 1) + (y - 1) * GRID)
                   || G((x2 + 1) + (y - 1) * GRID)) {
                x2++;
                if (x2 + 1 + (y - 1) * GRID > (GRID + 1) * (GRID + 1)) break;
            }
            if (x2 < x + width) {
                int tempHeight;
                if (i == 1) {
                    tempHeight = height;
                    x2 = (bbRand(1, 2) == 1) ? x : x + width;
                } else {
                    tempHeight = bbRand(1, height);
                }
                for (int y2 = y - tempHeight; y2 <= y; y2++) {
                    if (mapZoneOf(y2) != mapZoneOf(y2 + 1)) {
                        GXY(x2, y2) = 255;             /* checkpoint */
                    } else {
                        GXY(x2, y2) = 1;
                    }
                }
                if (tempHeight == height) temp = x2;
            }
        }
        x = temp;
        y = y - height;
    } while (y >= 2);

#define OCC1(ix) (G(ix) != 0 ? 1 : 0)

    /* ---- count pass (5279-5317): mutates tiles to their connection
     * count (checkpoints stay 255). Zone slices via GetZone. ---- */
    int roomAmount[SHAPE_COUNT][3];
    memset(roomAmount, 0, sizeof(roomAmount));
    for (y = 1; y <= GRID - 1; y++) {
        int zi = mapZoneOf(y) - 1;
        for (x = 1; x <= GRID - 1; x++) {
            if (GXY(x, y) == 0) continue;
            int t = OCC1((x + 1) + y * GRID) + OCC1((x - 1) + y * GRID)
                  + OCC1(x + (y + 1) * GRID) + OCC1(x + (y - 1) * GRID);
            if (GXY(x, y) != 255) GXY(x, y) = t;
            switch (GXY(x, y)) {
                case 1: roomAmount[SHAPE_ROOM1][zi]++; break;
                case 2:
                    if (OCC1((x + 1) + y * GRID)
                        + OCC1((x - 1) + y * GRID) == 2) {
                        roomAmount[SHAPE_ROOM2][zi]++;
                    } else if (OCC1(x + (y + 1) * GRID)
                               + OCC1(x + (y - 1) * GRID) == 2) {
                        roomAmount[SHAPE_ROOM2][zi]++;
                    } else {
                        roomAmount[SHAPE_ROOM2C][zi]++;
                    }
                    break;
                case 3: roomAmount[SHAPE_ROOM3][zi]++; break;
                case 4: roomAmount[SHAPE_ROOM4][zi]++; break;
                default: break;
            }
        }
    }

    /* ---- force more ROOM1 (5320-5392) ---- */
    for (int i = 0; i < 3; i++) {
        int need = -roomAmount[SHAPE_ROOM1][i] + 5;
        if (need <= 0) continue;
        int yMin = (i == 2) ? 1 : (i == 0 ? transition0 : transition1);
        int yMax = (i == 0) ? GRID - 2 : ((i == 1 ? transition0
                                                  : transition1) - 1);
        for (y = yMin; y <= yMax; y++) {
            for (x = 1; x <= GRID - 2; x++) {
                if (GXY(x, y) != 0) { if (need == 0) break; continue; }
                if ((OCC1((x + 1) + y * GRID) + OCC1((x - 1) + y * GRID)
                     + OCC1(x + (y + 1) * GRID)
                     + OCC1(x + (y - 1) * GRID)) == 1) {
                    int x2 = x, y2 = y;
                    if (G((x + 1) + y * GRID)) { x2 = x + 1; y2 = y; }
                    else if (G((x - 1) + y * GRID)) { x2 = x - 1; y2 = y; }
                    else if (G(x + (y + 1) * GRID)) { x2 = x; y2 = y + 1; }
                    else if (G(x + (y - 1) * GRID)) { x2 = x; y2 = y - 1; }
                    int placed = 0;
                    if (GXY(x2, y2) > 1 && GXY(x2, y2) < 4
                        && (y < yMax || y2 < y || i == 0)) {
                        if (GXY(x2, y2) == 2) {
                            if (OCC1((x2 + 1) + y2 * GRID)
                                + OCC1((x2 - 1) + y2 * GRID) == 2) {
                                roomAmount[SHAPE_ROOM2][i]--;
                                roomAmount[SHAPE_ROOM3][i]++;
                                placed = 1;
                            } else if (OCC1(x2 + (y2 + 1) * GRID)
                                       + OCC1(x2 + (y2 - 1) * GRID) == 2) {
                                roomAmount[SHAPE_ROOM2][i]--;
                                roomAmount[SHAPE_ROOM3][i]++;
                                placed = 1;
                            }
                        } else if (GXY(x2, y2) == 3) {
                            roomAmount[SHAPE_ROOM3][i]--;
                            roomAmount[SHAPE_ROOM4][i]++;
                            placed = 1;
                        }
                        if (placed) {
                            GXY(x2, y2) = GXY(x2, y2) + 1;
                            GXY(x, y) = 1;
                            roomAmount[SHAPE_ROOM1][i]++;
                            need--;
                        }
                    }
                }
                if (need == 0) break;
            }
            if (need == 0) break;
        }
    }

    /* ---- force more ROOM4 and ROOM2C (5395-5583) ---- */
    for (int i = 0; i < 3; i++) {
        int yMin = (i == 2) ? 2 : (i == 0 ? transition0 : transition1);
        int yMax = (i == 0) ? GRID - 2 : ((i == 1 ? transition0
                                                  : transition1) - 2);
        if (roomAmount[SHAPE_ROOM4][i] < 1) {
            int t = 0;
            for (y = yMin; y <= yMax && !t; y++) {
                for (x = 1; x <= GRID - 2 && !t; x++) {
                    if (GXY(x, y) != 3) continue;
                    if (!(G((x + 1) + y * GRID)
                          || G((x + 1) + (y + 1) * GRID)
                          || G((x + 1) + (y - 1) * GRID)
                          || G((x + 2) + y * GRID) || x == GRID - 2)) {
                        GXY(x + 1, y) = 1;
                        t = 1;
                    } else if (!(G((x - 1) + y * GRID)
                                 || G((x - 1) + (y + 1) * GRID)
                                 || G((x - 1) + (y - 1) * GRID)
                                 || G((x - 2) + y * GRID) || x == 1)) {
                        GXY(x - 1, y) = 1;
                        t = 1;
                    } else if (!(G(x + (y + 1) * GRID)
                                 || G((x + 1) + (y + 1) * GRID)
                                 || G((x - 1) + (y + 1) * GRID)
                                 || G(x + (y + 2) * GRID)
                                 || (i == 0 && y == yMax))) {
                        GXY(x, y + 1) = 1;
                        t = 1;
                    } else if (!(G(x + (y - 1) * GRID)
                                 || G((x + 1) + (y - 1) * GRID)
                                 || G((x - 1) + (y - 1) * GRID)
                                 || G(x + (y - 2) * GRID)
                                 || (i < 2 && y == yMin))) {
                        GXY(x, y - 1) = 1;
                        t = 1;
                    }
                    if (t) {
                        GXY(x, y) = 4;
                        roomAmount[SHAPE_ROOM4][i]++;
                        roomAmount[SHAPE_ROOM3][i]--;
                        roomAmount[SHAPE_ROOM1][i]++;
                    }
                }
            }
        }
        if (roomAmount[SHAPE_ROOM2C][i] < 2) {
            int t = 0;
            for (y = yMax; y >= yMin && !t; y--) {
                for (x = 1; x <= GRID - 2 && !t; x++) {
                    if (GXY(x, y) != 1) continue;
                    if (G((x - 1) + y * GRID) > 0) {
                        if ((G((x + 1) + (y - 1) * GRID)
                             + G((x + 1) + (y + 1) * GRID)
                             + G((x + 2) + y * GRID)) == 0
                            && x < GRID - 2) {
                            if ((G((x + 1) + (y - 2) * GRID)
                                 + G((x + 2) + (y - 1) * GRID)) == 0
                                && (y > yMin || i == 2)) {
                                GXY(x, y) = 2;
                                GXY(x + 1, y) = 2;
                                GXY(x + 1, y - 1) = 1;
                                t = 1;
                            } else if ((G((x + 1) + (y + 2) * GRID)
                                        + G((x + 2) + (y + 1) * GRID)) == 0
                                       && (y < yMax || i > 0)) {
                                GXY(x, y) = 2;
                                GXY(x + 1, y) = 2;
                                GXY(x + 1, y + 1) = 1;
                                t = 1;
                            }
                        }
                    } else if (G((x + 1) + y * GRID) > 0) {
                        if ((G((x - 1) + (y - 1) * GRID)
                             + G((x - 1) + (y + 1) * GRID)
                             + G((x - 2) + y * GRID)) == 0 && x > 1) {
                            if ((G((x - 1) + (y - 2) * GRID)
                                 + G((x - 2) + (y - 1) * GRID)) == 0
                                && (y > yMin || i == 2)) {
                                GXY(x, y) = 2;
                                GXY(x - 1, y) = 2;
                                GXY(x - 1, y - 1) = 1;
                                t = 1;
                            } else if ((G((x - 1) + (y + 2) * GRID)
                                        + G((x - 2) + (y + 1) * GRID)) == 0
                                       && (y < yMax || i > 0)) {
                                GXY(x, y) = 2;
                                GXY(x - 1, y) = 2;
                                GXY(x - 1, y + 1) = 1;
                                t = 1;
                            }
                        }
                    } else if (G(x + (y - 1) * GRID) > 0) {
                        if ((G((x - 1) + (y + 1) * GRID)
                             + G((x + 1) + (y + 1) * GRID)
                             + G(x + (y + 2) * GRID)) == 0
                            && (y < yMax || i > 0)) {
                            if ((G((x - 2) + (y + 1) * GRID)
                                 + G((x - 1) + (y + 2) * GRID)) == 0
                                && x > 1) {
                                GXY(x, y) = 2;
                                GXY(x, y + 1) = 2;
                                GXY(x - 1, y + 1) = 1;
                                t = 1;
                            } else if ((G((x + 2) + (y + 1) * GRID)
                                        + G((x + 1) + (y + 2) * GRID)) == 0
                                       && x < GRID - 2) {
                                GXY(x, y) = 2;
                                GXY(x, y + 1) = 2;
                                GXY(x + 1, y + 1) = 1;
                                t = 1;
                            }
                        }
                    } else if (G(x + (y + 1) * GRID) > 0) {
                        if ((G((x - 1) + (y - 1) * GRID)
                             + G((x + 1) + (y - 1) * GRID)
                             + G(x + (y - 2) * GRID)) == 0
                            && (y > yMin || i == 2)) {
                            if ((G((x - 2) + (y - 1) * GRID)
                                 + G((x - 1) + (y - 2) * GRID)) == 0
                                && x > 1) {
                                GXY(x, y) = 2;
                                GXY(x, y - 1) = 2;
                                GXY(x - 1, y - 1) = 1;
                                t = 1;
                            } else if ((G((x + 2) + (y - 1) * GRID)
                                        + G((x + 1) + (y - 2) * GRID)) == 0
                                       && x < GRID - 2) {
                                GXY(x, y) = 2;
                                GXY(x, y - 1) = 2;
                                GXY(x + 1, y - 1) = 1;
                                t = 1;
                            }
                        }
                    }
                    if (t) {
                        roomAmount[SHAPE_ROOM2C][i]++;
                        roomAmount[SHAPE_ROOM2][i]++;
                    }
                }
            }
        }
    }

    /* ---- MaxRooms + forced-room queues (5585-5618) ---- */
    int maxRooms = 0;
    for (int sshape = 0; sshape < SHAPE_COUNT; sshape++) {
        int t = roomAmount[sshape][0] + roomAmount[sshape][1]
              + roomAmount[sshape][2];
        if (t > maxRooms) maxRooms = t;
    }
    int mapRoom[SHAPE_COUNT][MAX_FORCED];
    for (int sh = 0; sh < SHAPE_COUNT; sh++) {
        for (int p = 0; p < MAX_FORCED; p++) mapRoom[sh][p] = -1;
    }
    for (unsigned f = 0; f < sizeof(FORCED_ROOMS) / sizeof(FORCED_ROOMS[0]);
         f++) {
        const ForcedRoom *fr = &FORCED_ROOMS[f];
        float weight = fr->weight;
        if (!strcmp(fr->name, "room3_storage")) {
            weight = bbRnd(0.2f, 0.6f);      /* the one live SetRoom draw */
        }
        setRoom(mapRoom, roomAmount, fr->zone, fr->shape,
                findByName(templates, fr->name), weight);
    }
    int gateAPlaced = 0, gateBPlaced = 0;
    {
        int gA = findByName(templates, "gate_a_entrance");
        int gB = findByName(templates, "gate_b_entrance");
        for (int sh = 0; sh < SHAPE_COUNT; sh++) {
            for (int p = 0; p < MAX_FORCED; p++) {
                if (mapRoom[sh][p] == gA) gateAPlaced = 1;
                if (mapRoom[sh][p] == gB) gateBPlaced = 1;
            }
        }
    }
    int roomID[SHAPE_COUNT];
    memset(roomID, 0, sizeof(roomID));

    int chkLczHcz = findByName(templates, "room2_checkpoint_lcz_hcz");
    int chkHczEz = findByName(templates, "room2_checkpoint_hcz_ez");

    /* ---- room creation loop (5620-5737): y desc, x asc; the zone
     * uses the loop's own formula (which disagrees with GetZone on the
     * boundary row - reproduced as-is). ---- */
    for (y = GRID - 1; y >= 1; y--) {
        int zone;
        if (y < GRID / 3 + 1) zone = 3;
        else if ((double)y < GRID * (2.0 / 3.0)) zone = 2;
        else zone = 1;
        for (x = 1; x <= GRID - 2; x++) {
            if (GXY(x, y) == 0) continue;
            int tpl = -1, angle = 0;
            const char *nm;
            if (GXY(x, y) == 255) {
                tpl = (y > GRID / 2) ? chkLczHcz : chkHczEz;
                if (tpl < 0) return 0;
                nm = templates->items[tpl].name;
                int extraDoor = GXY(x, y - 1) == 0;
                fillRoomDraws(nm, out->forestGrid, 0, extraDoor);
                if (!addRoom(out, tpl, x, y, 0)) { mapFree(out); return 0; }
                continue;
            }
            int conn = GXY(x, y);
            int shape;
            switch (conn) {
                case 1:
                    shape = SHAPE_ROOM1;
                    if (G(x + (y + 1) * GRID)) angle = 2;
                    else if (G((x - 1) + y * GRID)) angle = 3;
                    else if (G((x + 1) + y * GRID)) angle = 1;
                    else angle = 0;
                    break;
                case 2:
                    if (G((x - 1) + y * GRID) > 0
                        && G((x + 1) + y * GRID) > 0) {
                        shape = SHAPE_ROOM2;
                        angle = (bbRand(1, 2) == 1) ? 1 : 3;
                    } else if (G(x + (y - 1) * GRID) > 0
                               && G(x + (y + 1) * GRID) > 0) {
                        shape = SHAPE_ROOM2;
                        angle = (bbRand(1, 2) == 1) ? 2 : 0;
                    } else {
                        shape = SHAPE_ROOM2C;
                        if (G((x - 1) + y * GRID) > 0
                            && G(x + (y + 1) * GRID) > 0) angle = 2;
                        else if (G((x + 1) + y * GRID) > 0
                                 && G(x + (y + 1) * GRID) > 0) angle = 1;
                        else if (G((x - 1) + y * GRID) > 0
                                 && G(x + (y - 1) * GRID) > 0) angle = 3;
                        else angle = 0;
                    }
                    break;
                case 3:
                    shape = SHAPE_ROOM3;
                    if (!G(x + (y - 1) * GRID)) angle = 2;
                    else if (!G((x - 1) + y * GRID)) angle = 1;
                    else if (!G((x + 1) + y * GRID)) angle = 3;
                    else angle = 0;
                    break;
                default:
                    shape = SHAPE_ROOM4;
                    angle = bbRand(1, 4);
                    break;
            }
            if (roomID[shape] < maxRooms && roomID[shape] < MAX_FORCED
                && mapRoom[shape][roomID[shape]] >= 0) {
                tpl = mapRoom[shape][roomID[shape]];
            }
            if (tpl < 0) tpl = createRoomPick(templates, zone, shape);
            roomID[shape]++;
            if (tpl < 0) { mapFree(out); return 0; }
            nm = templates->items[tpl].name;
            int gateFlag = !strcmp(nm, "gate_a") ? gateAPlaced
                         : !strcmp(nm, "gate_b") ? gateBPlaced : 0;
            fillRoomDraws(nm, out->forestGrid, gateFlag, 0);
            if (!addRoom(out, tpl, x, y, angle % 4)) {
                mapFree(out);
                return 0;
            }
        }
    }

    /* ---- off-grid rooms (5740-5758): their FillRoom draws land in
     * the same stream before the door pass. The port appends its own
     * versions of these rooms elsewhere; only the draws matter. ---- */
    fillRoomDraws("gate_b", out->forestGrid, gateBPlaced, 0);
    fillRoomDraws("gate_a", out->forestGrid, gateAPlaced, 0);
    fillRoomDraws("dimension_106", out->forestGrid, 0, 0);
    if (introEnabled) fillRoomDraws("cont1_173_intro", out->forestGrid, 0, 0);
    fillRoomDraws("dimension_1499", out->forestGrid, 0, 0);

    /* ---- grid doors (5836-5915): tiles flattened to 0/1 first, then
     * y desc / x desc; +X and +Z doors per shape/angle with the
     * Rand(-3,1) open roll and CreateDoor's internal Rand(10). ---- */
    for (y = 0; y <= GRID; y++) {
        for (x = 0; x <= GRID; x++) {
            int ix = x + y * GRID;
            G(ix) = G(ix) != 0 ? 1 : 0;
        }
    }
    for (y = GRID; y >= 0; y--) {
        int zone;
        if (y < transition1 - 1) zone = 3;
        else if (y < transition0 - 1) zone = 2;
        else zone = 1;
        int heavy = ((zone - 1) % 2) != 0 ? 1 : 0;
        for (x = GRID; x >= 0; x--) {
            int ix = x + y * GRID;
            if (G(ix) == 0) continue;
            /* Find the room at this cell (grid rooms only). */
            const RoomPlacement *rp = NULL;
            for (uint32_t r = 0; r < out->roomCount; r++) {
                if (out->rooms[r].gridX == x && out->rooms[r].gridY == y) {
                    rp = &out->rooms[r];
                    break;
                }
            }
            if (!rp) continue;
            int shape = templates->items[rp->templateIndex].shape;
            int ang = ((rp->angle * 90) % 360 + 360) % 360;
            int spawn;
            switch (shape) {
                case SHAPE_ROOM1:  spawn = ang == 90; break;
                case SHAPE_ROOM2:  spawn = ang == 90 || ang == 270; break;
                case SHAPE_ROOM2C: spawn = ang == 0 || ang == 90; break;
                case SHAPE_ROOM3:  spawn = ang == 0 || ang == 180
                                        || ang == 90; break;
                default:           spawn = 1; break;
            }
            if (spawn && x + 1 < GRID + 1
                && G((x + 1) + y * GRID) != 0) {
                int open = bbRand(-3, 1);
                if (open < 0) open = 0;
                drawDOOR();
                GridDoorGen *grown = (GridDoorGen *)realloc(
                    out->gridDoors,
                    (out->gridDoorCount + 1) * sizeof(GridDoorGen));
                if (grown) {
                    out->gridDoors = grown;
                    GridDoorGen *d = &out->gridDoors[out->gridDoorCount++];
                    d->x = (float)x * 2048.0f + 1024.0f;
                    d->z = (float)y * 2048.0f;
                    d->angle = 90;
                    d->heavy = heavy;
                    d->open = open;
                }
            }
            switch (shape) {
                case SHAPE_ROOM1:  spawn = ang == 180; break;
                case SHAPE_ROOM2:  spawn = ang == 0 || ang == 180; break;
                case SHAPE_ROOM2C: spawn = ang == 180 || ang == 90; break;
                case SHAPE_ROOM3:  spawn = ang == 180 || ang == 90
                                        || ang == 270; break;
                default:           spawn = 1; break;
            }
            if (spawn && y + 1 < GRID + 1
                && G(x + (y + 1) * GRID) != 0) {
                int open = bbRand(-3, 1);
                if (open < 0) open = 0;
                drawDOOR();
                GridDoorGen *grown = (GridDoorGen *)realloc(
                    out->gridDoors,
                    (out->gridDoorCount + 1) * sizeof(GridDoorGen));
                if (grown) {
                    out->gridDoors = grown;
                    GridDoorGen *d = &out->gridDoors[out->gridDoorCount++];
                    d->x = (float)x * 2048.0f;
                    d->z = (float)y * 2048.0f + 1024.0f;
                    d->angle = 0;
                    d->heavy = heavy;
                    d->open = open;
                }
            }
        }
    }
#undef G
#undef GXY
#undef OCC1
    return out->roomCount > 0;
}

void mapFree(GeneratedMap *map) {
    free(map->rooms);
    free(map->gridDoors);
    memset(map, 0, sizeof(*map));
}
