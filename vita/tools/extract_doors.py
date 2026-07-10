#!/usr/bin/env python3
"""Generate vita/src/game/room_doors.h from the Blitz sources.

Extracts room-internal doors from FillRoom's CreateDoor(...) calls in
Rooms_Core.bb (containment chamber doors, elevator covers, locked
service doors), attributed to the enclosing Case r_<room> label.
These are what the grid-door pass (between rooms) cannot produce; a
room like cont2_427_714_860_1025 creates eight of them.

Coordinates using runtime expressions (entity positions, Rand, loops)
are skipped; this captures the fixed placements. A `d\\Locked = 1`
within the lines following an assigned CreateDoor marks the door
locked.

Run from the repo root:  python3 vita/tools/extract_doors.py
"""

import re

ROOMS = "Source Code/Rooms_Core.bb"
OUT = "vita/src/game/room_doors.h"

DOOR_TYPES = {
    "DEFAULT_DOOR": 0, "ELEVATOR_DOOR": 1, "HEAVY_DOOR": 2, "BIG_DOOR": 3,
    "OFFICE_DOOR": 4, "WOODEN_DOOR": 5, "ONE_SIDED_DOOR": 6,
    "SCP_914_DOOR": 7,
}
# Extra types derived from FillRoom follow-up lines (see below):
# 8 = default door with OBJ2 freed (single leaf), 9 = cell door
# (single leaf retextured Door02.jpg), 10 = corrosive default,
# 11 = corrosive one-sided (Door01_Corrosive.png on panel+frame).
# Items_Core.bb: KEY_CARD_n constants; negative KEY_* are special
# scanners the port cannot open, mapped to locked.
KEYCARDS = {
    "KEY_MISC": 0, "KEY_CARD_0": 0, "KEY_CARD_1": 1, "KEY_CARD_2": 2,
    "KEY_CARD_3": 3, "KEY_CARD_4": 4, "KEY_CARD_5": 5, "KEY_CARD_6": 6,
    "KEY_CARD_OMNI": 6, "KEY_005": 0,
}
LOCKED_KEYS = {"KEY_HAND_WHITE", "KEY_HAND_BLACK", "KEY_HAND_YELLOW",
               "KEY_860", "KEY_KEY"}

case_re = re.compile(r'^\s*Case\s+(r_[\w, \t]+)')
door_re = re.compile(r'CreateDoor\(\s*r\s*,([^)]*)\)')
num_expr = re.compile(r'^[0-9+\-*/. ()\t]+$')
tex_re = re.compile(
    r'Tex\s*=\s*LoadTexture_Strict\("GFX\\Map\\Textures\\([^"]+)"\)')


def eval_coord(expr):
    e = expr.strip()
    e = e.replace("r\\x", "0").replace("r\\y", "0").replace("r\\z", "0")
    e = e.replace("RoomScale", "1")
    if not e or not num_expr.match(e):
        return None
    try:
        return float(eval(e))
    except Exception:
        return None


def split_args(argstr):
    # No nested parens in the calls we accept (door_re stops at ')').
    return [a.strip() for a in argstr.split(",")]


doors = []
rooms = []
# Stack of If-block filters: shared Case blocks (Case r_a, r_b) guard
# variant-specific doors with `If r\RoomTemplate\RoomID = r_a`; track
# nesting so those doors only attribute to the named variant.
if_stack = []
roomid_re = re.compile(r'RoomTemplate\\RoomID\s*=\s*(r_\w+)')
if_re = re.compile(r'^\s*If\b', re.IGNORECASE)
endif_re = re.compile(r'^\s*EndIf\b', re.IGNORECASE)
else_re = re.compile(r'^\s*Else(If)?\b', re.IGNORECASE)


def single_line_if(text):
    m = re.search(r'\bThen\b(.*)$', text, re.IGNORECASE)
    return m is not None and m.group(1).split(";")[0].strip() != ""


lines = open(ROOMS, encoding="latin-1").readlines()
cur_tex = None
for li, line in enumerate(lines):
    cm = case_re.match(line)
    if cm:
        rooms = [r.strip()[2:] for r in cm.group(1).split(",")
                 if r.strip().startswith("r_")]
        if_stack = []
        cur_tex = None
        continue
    tm = tex_re.search(line)
    if tm:
        cur_tex = tm.group(1)
    if if_re.match(line) and not single_line_if(line):
        named = [m[2:] for m in roomid_re.findall(line)]
        if_stack.append(named or None)
    elif else_re.match(line) and if_stack:
        named = if_stack[-1]
        if named:
            # Else of a RoomID check: the other rooms of this Case.
            if_stack[-1] = [r for r in rooms if r not in named] or None
    elif endif_re.match(line) and if_stack:
        if_stack.pop()

    dm = door_re.search(line)
    if not dm or not rooms:
        continue
    active = rooms
    for f in if_stack:
        if f is not None:
            active = [r for r in active if r in f]
    if not active:
        continue
    args = split_args(dm.group(1))
    if len(args) < 4:
        continue
    x = eval_coord(args[0])
    y = eval_coord(args[1])
    z = eval_coord(args[2])
    ang = eval_coord(args[3])
    if x is None or y is None or z is None or ang is None:
        continue
    is_open = len(args) > 4 and args[4].strip() == "True"
    dtype = DOOR_TYPES.get(args[5].strip(), 0) if len(args) > 5 else 0
    locked = 0
    keycard = 0
    if len(args) > 6:
        k = args[6].strip()
        if k in LOCKED_KEYS:
            locked = 1
        else:
            keycard = KEYCARDS.get(k, 0)
    # Keypad-code doors (8th arg): keep which code they use so the
    # keypad UI can check it (CODE_LOCKED never opens).
    CODES = {"CODE_DR_HARP": 1, "CODE_DR_L": 2, "CODE_CONT1_035": 3,
             "CODE_DR_MAYNARD": 4, "CODE_O5_COUNCIL": 5,
             "CODE_MAINTENANCE_TUNNELS": 6, "CODE_DR_GEARS": 7}
    code_id = 0
    if len(args) > 7 and args[7].strip() not in ("0", ""):
        c = args[7].strip()
        if c == "CODE_LOCKED":
            locked = 1
        else:
            code_id = CODES.get(c, 0)
            if code_id == 0:
                locked = 1
    # Follow-up lines on the assigned door: `d\Locked = 1` locks it,
    # FreeEntity(d\Buttons[..]) removes its buttons, FreeEntity(d\OBJ2)
    # leaves a single leaf, EntityTexture(d\OBJ, ...) retextures it
    # (the intro cell door's Door02.jpg, the corrosive 049/Dr. L
    # doors). A texture may be loaded just before the CreateDoor.
    nobuttons = 0
    single = 0
    tex = cur_tex
    door_tex = None
    if "=" in line.split("CreateDoor")[0]:
        for follow in lines[li + 1:li + 11]:
            if case_re.match(follow) or "CreateDoor" in follow:
                break
            if re.search(r'\\Locked\s*=\s*1', follow):
                locked = 1
            if re.search(r'FreeEntity\(d\\Buttons', follow):
                nobuttons = 1
            if re.search(r'FreeEntity\(d\\OBJ2\)', follow):
                single = 1
            tm = tex_re.search(follow)
            if tm:
                tex = tm.group(1)
            if re.search(r'EntityTexture\(d\\OBJ\b', follow):
                door_tex = tex
    if door_tex and door_tex.lower() == "door02.jpg" and dtype == 0:
        dtype = 9
    elif door_tex and door_tex.lower() == "door01_corrosive.png":
        dtype = 11 if dtype == 6 else 10
    elif dtype == 0 and single:
        dtype = 8
    for room in active:
        doors.append((room, x, y, z, ang % 360.0, is_open, dtype,
                      keycard, locked, nobuttons, code_id))


def cf(v):
    s = "%.9g" % v
    if "." not in s and "e" not in s and "E" not in s:
        s += ".0"
    return s + "f"


with open(OUT, "w") as f:
    f.write("/* Generated by vita/tools/extract_doors.py - do not edit.\n"
            " * Room-internal doors from FillRoom's literal-coordinate\n"
            " * CreateDoor calls in Rooms_Core.bb (room-local raw units;\n"
            " * type: 0 default 1 elevator 2 heavy 3 big 4 office 5 wooden\n"
            " * 6 one-sided 7 SCP-914 8 single-leaf default 9 cell door\n"
            " * (Door02.jpg) 10 corrosive default 11 corrosive\n"
            " * one-sided). */\n"
            "#ifndef VITA_GAME_ROOM_DOORS_H\n"
            "#define VITA_GAME_ROOM_DOORS_H\n\n"
            "typedef struct {\n"
            "    const char *room;\n"
            "    float x, y, z;    /* room-local raw units */\n"
            "    float angleDeg;   /* Blitz yaw within the room */\n"
            "    unsigned char open, type, keycard, locked, nobuttons;\n"
            "    unsigned char codeId; /* 1 harp 2 drL 3 035 4 maynard\n"
            "                             5 o5 6 maintenance 7 gears */\n"
            "} RoomDoorDef;\n\n"
            "static const RoomDoorDef ROOM_DOORS[] = {\n")
    for (room, x, y, z, ang, is_open, dtype, keycard, locked, nob,
         cid) in doors:
        f.write('    { "%s", %s, %s, %s, %s, %d, %d, %d, %d, %d, %d },\n'
                % (room, cf(x), cf(y), cf(z), cf(ang), 1 if is_open else 0,
                   dtype, keycard, locked, nob, cid))
    f.write("};\n\n#endif\n")

print("room doors:", len(doors))
