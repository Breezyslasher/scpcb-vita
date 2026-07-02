#include "input.h"

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <math.h>
#include <string.h>

#define STICK_DEADZONE 0.15f

/*
 * Default controller layout:
 *
 *   Left stick      move
 *   Right stick     camera
 *   Cross           interact (left mouse)
 *   Square          use equipped item (right mouse)
 *   Circle          crouch
 *   Triangle        inventory
 *   L trigger       sprint
 *   R trigger       blink
 *   D-pad left      lean left
 *   D-pad right     lean right
 *   D-pad up        quick save
 *   Select          console
 *   Start           pause menu
 *   Front touch     menu / inventory cursor
 */
static const uint32_t actionButtons[ACTION_COUNT] = {
    [ACTION_INTERACT]   = SCE_CTRL_CROSS,
    [ACTION_USE_ITEM]   = SCE_CTRL_SQUARE,
    [ACTION_BLINK]      = SCE_CTRL_RTRIGGER,
    [ACTION_SPRINT]     = SCE_CTRL_LTRIGGER,
    [ACTION_CROUCH]     = SCE_CTRL_CIRCLE,
    [ACTION_LEAN_LEFT]  = SCE_CTRL_LEFT,
    [ACTION_LEAN_RIGHT] = SCE_CTRL_RIGHT,
    [ACTION_INVENTORY]  = SCE_CTRL_TRIANGLE,
    [ACTION_SAVE]       = SCE_CTRL_UP,
    [ACTION_CONSOLE]    = SCE_CTRL_SELECT,
    [ACTION_MENU]       = SCE_CTRL_START,
};

static const char *bindingNames[ACTION_COUNT] = {
    [ACTION_INTERACT]   = "Cross",
    [ACTION_USE_ITEM]   = "Square",
    [ACTION_BLINK]      = "R",
    [ACTION_SPRINT]     = "L",
    [ACTION_CROUCH]     = "Circle",
    [ACTION_LEAN_LEFT]  = "D-Pad Left",
    [ACTION_LEAN_RIGHT] = "D-Pad Right",
    [ACTION_INVENTORY]  = "Triangle",
    [ACTION_SAVE]       = "D-Pad Up",
    [ACTION_CONSOLE]    = "Select",
    [ACTION_MENU]       = "Start",
};

static const char *actionNames[ACTION_COUNT] = {
    [ACTION_INTERACT]   = "Interact",
    [ACTION_USE_ITEM]   = "Use Item",
    [ACTION_BLINK]      = "Blink",
    [ACTION_SPRINT]     = "Sprint",
    [ACTION_CROUCH]     = "Crouch",
    [ACTION_LEAN_LEFT]  = "Lean Left",
    [ACTION_LEAN_RIGHT] = "Lean Right",
    [ACTION_INVENTORY]  = "Inventory",
    [ACTION_SAVE]       = "Save",
    [ACTION_CONSOLE]    = "Console",
    [ACTION_MENU]       = "Menu",
};

static SceCtrlData ctrl;
static SceCtrlData ctrlPrev;
static SceTouchData touch;
static SceTouchData touchPrev;

void inputInit(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    memset(&ctrl, 0, sizeof(ctrl));
    memset(&ctrlPrev, 0, sizeof(ctrlPrev));
    memset(&touch, 0, sizeof(touch));
}

void inputUpdate(void) {
    ctrlPrev = ctrl;
    touchPrev = touch;
    sceCtrlPeekBufferPositive(0, &ctrl, 1);
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
}

bool inputTouchTap(float *x, float *y) {
    if (touch.reportNum > 0 && touchPrev.reportNum == 0) {
        /* Touch panel reports 1920x1088; screen is 960x544. */
        *x = touch.report[0].x / 2.0f;
        *y = touch.report[0].y / 2.0f;
        return true;
    }
    return false;
}

bool inputDown(GameAction action) {
    return (ctrl.buttons & actionButtons[action]) != 0;
}

bool inputHit(GameAction action) {
    return (ctrl.buttons & actionButtons[action]) != 0
        && (ctrlPrev.buttons & actionButtons[action]) == 0;
}

bool inputDpadDownHit(void) {
    /* D-pad down has no game action (crouch is Circle); menus use it
     * raw. Edge-triggered. */
    return (ctrl.buttons & SCE_CTRL_DOWN) != 0
        && (ctrlPrev.buttons & SCE_CTRL_DOWN) == 0;
}

static StickState readStick(uint8_t rawX, uint8_t rawY) {
    StickState s;
    s.x = (rawX - 128) / 128.0f;
    s.y = (rawY - 128) / 128.0f;
    float mag = sqrtf(s.x * s.x + s.y * s.y);
    if (mag < STICK_DEADZONE) {
        s.x = 0.0f;
        s.y = 0.0f;
    } else {
        /* Rescale so output ramps smoothly from 0 at the deadzone edge. */
        float scale = (mag - STICK_DEADZONE) / (1.0f - STICK_DEADZONE) / mag;
        s.x *= scale;
        s.y *= scale;
        if (s.x > 1.0f) s.x = 1.0f;
        if (s.x < -1.0f) s.x = -1.0f;
        if (s.y > 1.0f) s.y = 1.0f;
        if (s.y < -1.0f) s.y = -1.0f;
    }
    return s;
}

StickState inputMove(void) {
    return readStick(ctrl.lx, ctrl.ly);
}

StickState inputLook(void) {
    return readStick(ctrl.rx, ctrl.ry);
}

TouchState inputTouch(void) {
    TouchState t;
    t.active = touch.reportNum > 0;
    if (t.active) {
        /* Touch panel reports 1920x1088; screen is 960x544. */
        t.x = touch.report[0].x / 2.0f;
        t.y = touch.report[0].y / 2.0f;
    } else {
        t.x = 0.0f;
        t.y = 0.0f;
    }
    return t;
}

const char *inputBindingName(GameAction action) {
    return bindingNames[action];
}

const char *inputActionName(GameAction action) {
    return actionNames[action];
}
