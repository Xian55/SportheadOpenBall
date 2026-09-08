// On-screen touch controls. Tier 3.
//
// These feed the SAME 1-byte mask as the keyboard, so the simulation and the
// netcode never learn touch exists - that is the payoff of making the input a
// bitmask rather than a struct of sources.
//
// Laid out and hit-tested in SCREEN space, deliberately NOT in the 1280x720
// virtual pitch space everything else uses. Virtual-space buttons shrink with
// the letterbox scale: on a landscape phone the pitch scales by about 0.55, so
// a 56 px button lands at ~30 physical px against a ~48 px minimum touch
// target. Sizing from the real screen keeps the controls physically usable on
// any display, and it is why touch_draw() runs after the blit rather than
// inside the render texture.
#include <stdlib.h>
#include "raylib.h"
#include "touch.h"
#include "config.h"
#include "render.h"

#define NBTN 4

typedef struct { float x, y, r; uint8_t bit; const char *label; } TouchBtn;
static TouchBtn g_btn[NBTN];
static uint8_t  g_mask;
static int      g_active;   // latched: once this device has produced a touch

void touch_init(void) {
  g_mask = 0;
  // Lets the overlay be exercised on a desktop box without a touchscreen.
  g_active = (getenv("OB_TOUCH") != NULL);
}

// Rebuilt every frame: the window (or phone orientation) can change at any time.
static void layout(void) {
  float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
  // ~13% of screen height, floored at 46 px so it stays thumb-sized on small
  // screens and capped so it does not swallow a desktop window.
  float r = sh * 0.13f;
  if (r < 46.0f) r = 46.0f;
  if (r > 92.0f) r = 92.0f;
  float m = r * 0.55f;                 // margin from the screen edges

  // If the letterbox leaves a wide enough bar down each side - which a tall
  // phone (20:9, 19.5:9) does - put the controls THERE. They then cover no
  // pitch at all. On 16:9, or on a small viewport where the bar is too narrow,
  // fall back to the bottom corners and accept overlapping the goalmouths;
  // shrinking the pitch to make room costs more than it buys on a small screen.
  Rectangle pr = render_pitch_rect();
  if (pr.x >= r * 2.1f) {
    float lx = pr.x * 0.5f, rx = sw - pr.x * 0.5f;
    float y0 = sh * 0.5f - r * 1.15f, y1 = sh * 0.5f + r * 1.15f;
    g_btn[0] = (TouchBtn){ lx, y0, r, IN_LEFT,  "<"    };
    g_btn[1] = (TouchBtn){ lx, y1, r, IN_RIGHT, ">"    };
    g_btn[2] = (TouchBtn){ rx, y0, r, IN_KICK,  "KICK" };
    g_btn[3] = (TouchBtn){ rx, y1, r, IN_JUMP,  "JUMP" };
    return;
  }

  float y = sh - r - m;
  g_btn[0] = (TouchBtn){ m + r,                 y, r, IN_LEFT,  "<"    };
  g_btn[1] = (TouchBtn){ m + r * 3.3f,          y, r, IN_RIGHT, ">"    };
  g_btn[2] = (TouchBtn){ sw - m - r * 3.3f,     y, r, IN_KICK,  "KICK" };
  g_btn[3] = (TouchBtn){ sw - m - r,            y, r, IN_JUMP,  "JUMP" };
}

static uint8_t hit(float x, float y) {
  uint8_t m = 0;
  for (int b = 0; b < NBTN; b++) {
    float dx = x - g_btn[b].x, dy = y - g_btn[b].y;
    // A slightly generous radius: fingers are imprecise and a missed jump is
    // worse than an occasional double-press.
    float rr = g_btn[b].r * 1.15f;
    if (dx * dx + dy * dy <= rr * rr) m |= g_btn[b].bit;
  }
  return m;
}

void touch_update(void) {
  layout();
  int n = GetTouchPointCount();
  if (n > 0) g_active = 1;      // latch: never flicker the UI back off
  if (!g_active) { g_mask = 0; return; }

  uint8_t m = 0;
  for (int i = 0; i < n; i++) {
    Vector2 p = GetTouchPosition(i);
    m |= hit(p.x, p.y);
  }
  // A mouse press counts too, but only once touch has been seen - otherwise the
  // overlay would hijack ordinary desktop clicks.
  if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
    Vector2 p = GetMousePosition();
    m |= hit(p.x, p.y);
  }
  g_mask = m;
}

uint8_t touch_mask(void)  { return g_mask; }
int     touch_active(void) { return g_active; }

void touch_draw(void) {
  if (!g_active) return;
  layout();
  for (int b = 0; b < NBTN; b++) {
    int   held = (g_mask & g_btn[b].bit) != 0;
    Color fill = held ? Fade(RAYWHITE, 0.30f) : Fade(RAYWHITE, 0.08f);
    Vector2 c = { g_btn[b].x, g_btn[b].y };
    DrawCircleV(c, g_btn[b].r, fill);
    DrawCircleLinesV(c, g_btn[b].r, Fade(RAYWHITE, 0.30f));
    int fs = (int)(g_btn[b].r * (g_btn[b].label[1] == '\0' ? 0.9f : 0.34f));
    int tw = MeasureText(g_btn[b].label, fs);
    DrawText(g_btn[b].label, (int)c.x - tw / 2, (int)c.y - fs / 2, fs,
             Fade(RAYWHITE, 0.62f));
  }
}
