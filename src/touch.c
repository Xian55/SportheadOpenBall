// On-screen touch controls, laid out and hit-tested in VIRTUAL pitch space so
// they stay put no matter how the canvas is scaled or letterboxed.
#include <stdlib.h>
#include "raylib.h"
#include "touch.h"
#include "render.h"
#include "config.h"

typedef struct TouchBtn {
  float       x, y, r;
  uint8_t     bit;
  const char *label;
} TouchBtn;

// Movement on the left thumb, actions on the right. They sit on the grass strip
// so they never cover the ball or the goals.
static const TouchBtn BTNS[] = {
  {  118.0f, 632.0f, 56.0f, IN_LEFT,  "<"  },
  {  258.0f, 632.0f, 56.0f, IN_RIGHT, ">"  },
  { 1022.0f, 632.0f, 56.0f, IN_KICK,  "KICK" },
  { 1162.0f, 632.0f, 56.0f, IN_JUMP,  "JUMP" },
};
#define NBTN ((int)(sizeof BTNS / sizeof BTNS[0]))

static uint8_t g_mask;
static int     g_active;   // latched: once this device has produced a touch

void touch_init(void) {
  g_mask = 0;
  // Lets the overlay be exercised on a desktop box without a touchscreen.
  g_active = (getenv("OB_TOUCH") != NULL);
}

void touch_update(void) {
  int n = GetTouchPointCount();
  if (n > 0) g_active = 1;      // latch: never flicker the UI back off
  if (!g_active) { g_mask = 0; return; }

  uint8_t m = 0;
  for (int i = 0; i < n; i++) {
    // Touch points arrive in real window pixels; the buttons live in virtual
    // pitch space, so map through the renderer's letterbox transform.
    Vector2 v = render_to_virtual(GetTouchPosition(i));
    for (int b = 0; b < NBTN; b++) {
      float dx = v.x - BTNS[b].x, dy = v.y - BTNS[b].y;
      if (dx * dx + dy * dy <= BTNS[b].r * BTNS[b].r) m |= BTNS[b].bit;
    }
  }

  // A mouse press counts too, but only once touch has been seen - otherwise the
  // overlay would hijack ordinary desktop clicks.
  if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
    Vector2 v = render_to_virtual(GetMousePosition());
    for (int b = 0; b < NBTN; b++) {
      float dx = v.x - BTNS[b].x, dy = v.y - BTNS[b].y;
      if (dx * dx + dy * dy <= BTNS[b].r * BTNS[b].r) m |= BTNS[b].bit;
    }
  }
  g_mask = m;
}

uint8_t touch_mask(void) { return g_mask; }
int     touch_active(void) { return g_active; }

void touch_draw(void) {
  if (!g_active) return;
  for (int b = 0; b < NBTN; b++) {
    int held = (g_mask & BTNS[b].bit) != 0;
    Color fill = held ? Fade(RAYWHITE, 0.34f) : Fade(RAYWHITE, 0.13f);
    DrawCircleV((Vector2){ BTNS[b].x, BTNS[b].y }, BTNS[b].r, fill);
    DrawCircleLinesV((Vector2){ BTNS[b].x, BTNS[b].y }, BTNS[b].r,
                     Fade(RAYWHITE, 0.42f));
    int fs = (BTNS[b].label[1] == '\0') ? 40 : 20;
    int tw = MeasureText(BTNS[b].label, fs);
    DrawText(BTNS[b].label, (int)BTNS[b].x - tw / 2, (int)BTNS[b].y - fs / 2,
             fs, Fade(RAYWHITE, 0.85f));
  }
}
