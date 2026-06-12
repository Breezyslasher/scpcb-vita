#ifndef VITA_INPUT_H
#define VITA_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Game action set, mirroring the bindable actions of the original game
 * (Source Code/KeyBinds_Core.bb) plus the hardcoded mouse actions.
 */
typedef enum {
    ACTION_INTERACT = 0,   /* left mouse  */
    ACTION_USE_ITEM,       /* right mouse */
    ACTION_BLINK,
    ACTION_SPRINT,
    ACTION_CROUCH,
    ACTION_LEAN_LEFT,
    ACTION_LEAN_RIGHT,
    ACTION_INVENTORY,
    ACTION_SAVE,
    ACTION_CONSOLE,
    ACTION_MENU,           /* Esc / pause */
    ACTION_COUNT
} GameAction;

typedef struct {
    float x;
    float y;
} StickState;

typedef struct {
    bool  active;
    float x; /* 0..960  */
    float y; /* 0..544  */
} TouchState;

/* Call once at startup. Enables analog sampling and front/back touch. */
void inputInit(void);

/* Call once per frame before querying any state. */
void inputUpdate(void);

/* Action is currently held down. */
bool inputDown(GameAction action);

/* Action was pressed this frame (edge-triggered). */
bool inputHit(GameAction action);

/* Left stick (movement) and right stick (camera), deadzone applied,
 * components in -1..1. */
StickState inputMove(void);
StickState inputLook(void);

/* Front touch panel, mapped to screen coordinates (for menus/inventory). */
TouchState inputTouch(void);

/* Human-readable Vita button name bound to an action, for UI/help screens. */
const char *inputBindingName(GameAction action);

/* Human-readable action name. */
const char *inputActionName(GameAction action);

#endif
