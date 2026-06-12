/*
 * SCP - Containment Breach Ultimate Edition Reborn — PS Vita port shell.
 *
 * Milestone 1: boots on the Vita, initializes the renderer, and exposes
 * the full controller layer (buttons, both sticks, front touch) with a
 * live on-screen test so the mapping can be verified on hardware.
 * Game systems are ported on top of this shell — see vita/PORTING.md.
 */

#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#include "input.h"

#define SCREEN_W 960
#define SCREEN_H 544

#define RGBA8(r, g, b, a) ((a) << 24 | (b) << 16 | (g) << 8 | (r))

static const unsigned int COLOR_BG     = RGBA8(10, 10, 12, 255);
static const unsigned int COLOR_TEXT   = RGBA8(200, 200, 200, 255);
static const unsigned int COLOR_TITLE  = RGBA8(255, 255, 255, 255);
static const unsigned int COLOR_ACTIVE = RGBA8(255, 60, 60, 255);
static const unsigned int COLOR_DIM    = RGBA8(90, 90, 95, 255);

static void drawStick(vita2d_pgf *font, const char *label, StickState s,
                      float cx, float cy) {
    const float r = 40.0f;
    vita2d_draw_fill_circle(cx, cy, r, COLOR_DIM);
    vita2d_draw_fill_circle(cx + s.x * r, cy + s.y * r, 8.0f,
                            (s.x != 0.0f || s.y != 0.0f) ? COLOR_ACTIVE
                                                         : COLOR_TEXT);
    vita2d_pgf_draw_textf(font, (int)(cx - r), (int)(cy + r + 22), COLOR_TEXT,
                          1.0f, "%s  %+.2f  %+.2f", label, s.x, s.y);
}

int main(void) {
    vita2d_init();
    vita2d_set_clear_color(COLOR_BG);
    vita2d_pgf *font = vita2d_load_default_pgf();

    inputInit();

    bool running = true;
    int menuHits = 0;

    while (running) {
        inputUpdate();

        /* Press Start three times to exit the shell. */
        if (inputHit(ACTION_MENU)) {
            menuHits++;
            if (menuHits >= 3) {
                running = false;
            }
        }

        vita2d_start_drawing();
        vita2d_clear_screen();

        vita2d_pgf_draw_text(font, 30, 40, COLOR_TITLE, 1.0f,
                             "SCP - Containment Breach Ultimate Edition Reborn");
        vita2d_pgf_draw_text(font, 30, 64, COLOR_TEXT, 1.0f,
                             "PS Vita port shell - controller test "
                             "(press Start 3x to exit)");

        int y = 110;
        for (int i = 0; i < ACTION_COUNT; i++) {
            bool down = inputDown((GameAction)i);
            vita2d_pgf_draw_textf(font, 30, y, down ? COLOR_ACTIVE : COLOR_TEXT,
                                  1.0f, "%-12s %s", inputActionName((GameAction)i),
                                  inputBindingName((GameAction)i));
            y += 26;
        }

        drawStick(font, "Move", inputMove(), 620.0f, 180.0f);
        drawStick(font, "Look", inputLook(), 800.0f, 180.0f);

        TouchState t = inputTouch();
        vita2d_pgf_draw_textf(font, 540, 320, COLOR_TEXT, 1.0f,
                              "Touch: %s", t.active ? "active" : "-");
        if (t.active) {
            vita2d_draw_fill_circle(t.x, t.y, 12.0f, COLOR_ACTIVE);
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    vita2d_free_pgf(font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
