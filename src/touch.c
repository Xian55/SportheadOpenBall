// On-screen touch controls: a floating movement stick plus one kick button.
//
// These feed the SAME 1-byte mask as the keyboard, so the simulation and the
// netcode never learn touch exists - that is the payoff of making the input a
// bitmask rather than a struct of sources.
//
// Laid out and hit-tested in SCREEN space, deliberately NOT in the 1280x720
// virtual pitch space everything else uses. Virtual-space controls shrink with
// the letterbox scale: on a landscape phone the pitch scales by about 0.54, so
// a 56 px control lands at ~30 physical px against a ~48 px minimum touch
// target. Sizing from the real screen keeps them usable on any display, and it
// is why touch_draw() runs after the blit rather than inside the render texture.
//
// Why a stick rather than discrete arrows: a directional control has to sit on
// the axis it controls, and separate LEFT/RIGHT buttons kept fighting that -
// stacking them to fit a layout made "up" mean "left". A stick sidesteps the
// problem by BEING the axis, and pushing up for jump is the natural reading,
// which removes a third target from the screen entirely.
//
// The stick FLOATS: it centres wherever the thumb first lands in the left half,
// rather than sitting at a fixed spot the player must find without looking.
#include <math.h>
#include <stdlib.h>
#include "raylib.h"
#include "touch.h"
#include "config.h"
#include "render.h"

#define STICK_MOUSE (-2)        // pseudo touch id for a desktop mouse drag

// --- tunables ---------------------------------------------------------------
// Deliberately NOT in config.h. Those constants are the protocol: they are
// hashed into the const golden and both peers must agree on them. Stick feel is
// LOCAL, like mouse sensitivity - it shapes which input bits this device
// produces, never how the simulation interprets them, so two players can run
// different settings and still stay in sync.
//
// Overridable at runtime for tuning by feel without a rebuild:
//   OB_DEADZONE=0.25   fraction of travel ignored around centre
//   OB_JUMPZONE=1.5    up-threshold as a multiple of the deadzone
//   OB_STICKR=1.6      travel radius as a multiple of the base button size
static float g_deadzone  = 0.30f;   // too big feels unresponsive, too small drifts
static float g_jumpzone  = 1.25f;   // > 1 so running does not accidentally jump
static float g_stick_mul = 1.35f;

static float env_f(const char *name, float dflt, float lo, float hi) {
  const char *v = getenv(name);
  if (!v || !*v) return dflt;
  float f = (float)atof(v);
  if (f < lo || f > hi) return dflt;
  return f;
}

static uint8_t g_mask;
static int     g_active;        // latched: once this device has produced a touch

static int   g_stick_id = -1;   // touch id driving the stick, -1 = idle
static float g_bx, g_by;        // stick base (centre), screen px
static float g_kx, g_ky;        // stick knob
static float g_stick_r;         // travel radius
static float g_home_x, g_home_y;

static float g_kick_x, g_kick_y, g_kick_r;

void touch_init(void) {
  g_mask = 0;
  g_stick_id = -1;
  g_deadzone  = env_f("OB_DEADZONE", 0.30f, 0.02f, 0.80f);
  g_jumpzone  = env_f("OB_JUMPZONE", 1.25f, 0.50f, 4.00f);
  g_stick_mul = env_f("OB_STICKR",   1.35f, 0.60f, 3.00f);
  // Lets the overlay be exercised on a desktop box without a touchscreen.
  g_active = (getenv("OB_TOUCH") != NULL);
}

// Rebuilt every frame: the window (or phone orientation) can change at any time.
static void layout(void) {
  float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
  float r = sh * 0.13f;
  if (r < 46.0f) r = 46.0f;
  if (r > 92.0f) r = 92.0f;

  g_stick_r = r * g_stick_mul;
  g_kick_r  = r;

  float m = r * 0.55f;
  g_home_x = m + g_stick_r;
  g_home_y = sh - m - g_stick_r;

  // KICK carries no direction, so it is free to go wherever there is room. Where
  // the letterbox leaves a wide enough bar down the right side, park it there
  // and it covers no pitch at all.
  Rectangle pr = render_pitch_rect();
  if (pr.x >= g_kick_r * 2.1f) {
    g_kick_x = sw - pr.x * 0.5f;
    g_kick_y = sh * 0.5f;
  } else {
    g_kick_x = sw - m - g_kick_r;
    g_kick_y = sh - m - g_kick_r;
  }

  if (g_stick_id == -1) { g_bx = g_home_x; g_by = g_home_y; g_kx = g_bx; g_ky = g_by; }
}

static int in_kick(float x, float y) {
  float dx = x - g_kick_x, dy = y - g_kick_y;
  float rr = g_kick_r * 1.15f;      // generous: fingers are imprecise
  return dx * dx + dy * dy <= rr * rr;
}

// Deflection -> input bits. The deadzone stops a resting thumb from drifting the
// player, and the up-threshold is larger than the sideways one so that running
// does not accidentally jump.
static uint8_t stick_bits(void) {
  float dx = g_kx - g_bx, dy = g_ky - g_by;
  float dead = g_stick_r * g_deadzone;
  uint8_t m = 0;
  if (dx < -dead) m |= IN_LEFT;
  if (dx >  dead) m |= IN_RIGHT;
  if (dy < -dead * g_jumpzone) m |= IN_JUMP;
  return m;
}

static void clamp_knob(void) {
  float dx = g_kx - g_bx, dy = g_ky - g_by;
  float d = sqrtf(dx * dx + dy * dy);
  if (d > g_stick_r && d > 0.0f) {
    g_kx = g_bx + dx / d * g_stick_r;
    g_ky = g_by + dy / d * g_stick_r;
  }
}

void touch_update(void) {
  layout();
  int n = GetTouchPointCount();
  if (n > 0) g_active = 1;          // latch: never flicker the UI back off
  if (!g_active) { g_mask = 0; return; }

  float sw = (float)GetScreenWidth();
  uint8_t m = 0;
  int stick_seen = 0;

  for (int i = 0; i < n; i++) {
    Vector2 p  = GetTouchPosition(i);
    int     id = GetTouchPointId(i);

    if (id == g_stick_id) {         // the finger already driving the stick
      g_kx = p.x; g_ky = p.y;
      stick_seen = 1;
      continue;
    }
    if (in_kick(p.x, p.y)) { m |= IN_KICK; continue; }
    // == -1, not < 0: STICK_MOUSE is negative too, and treating it as "free"
    // let a stale mouse claim re-acquire the stick every frame.
    if (g_stick_id == -1 && p.x < sw * 0.5f) {
      g_stick_id = id;              // float the stick to where the thumb landed
      g_bx = p.x; g_by = p.y;
      g_kx = p.x; g_ky = p.y;
      stick_seen = 1;
    }
  }
  if (g_stick_id >= 0 && !stick_seen) g_stick_id = -1;   // lifted: recentre

  // Mouse is for DESKTOP testing only, and is read solely when no finger is
  // down. Mobile browsers synthesise mouse events from touches, so without this
  // guard the two paths fight every frame: the touch loop would engage the stick
  // with a real id, the emulated mouse would immediately re-claim it as
  // STICK_MOUSE and re-base to the finger, and the next frame the id no longer
  // matched so the stick re-acquired at the thumb's new position - the base
  // chasing the finger instead of staying put. The same collision killed
  // movement the moment KICK was pressed, because the emulated mouse landed in
  // the kick circle and stopped feeding the stick.
  if (n == 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
    Vector2 p = GetMousePosition();
    if (in_kick(p.x, p.y)) {
      m |= IN_KICK;
    } else if (p.x < sw * 0.5f) {
      if (g_stick_id != STICK_MOUSE) { g_stick_id = STICK_MOUSE; g_bx = p.x; g_by = p.y; }
      g_kx = p.x; g_ky = p.y;
    }
  } else if (g_stick_id == STICK_MOUSE) {
    g_stick_id = -1;   // mouse released, or a finger took over
  }

  if (g_stick_id != -1) { clamp_knob(); m |= stick_bits(); }
  g_mask = m;
}

uint8_t touch_mask(void)   { return g_mask; }
int     touch_active(void) { return g_active; }

void touch_draw(void) {
  if (!g_active) return;
  layout();

  int   engaged = (g_stick_id != -1);
  float br = g_stick_r;

  DrawCircleV((Vector2){ g_bx, g_by }, br, Fade(RAYWHITE, engaged ? 0.13f : 0.07f));
  DrawCircleLinesV((Vector2){ g_bx, g_by }, br, Fade(RAYWHITE, engaged ? 0.34f : 0.20f));

  // Faint axis ticks: they show at a glance that this thing is left/right plus
  // up-to-jump, without needing a legend.
  Color tick = Fade(RAYWHITE, 0.22f);
  DrawLineEx((Vector2){ g_bx - br * 0.78f, g_by }, (Vector2){ g_bx - br * 0.46f, g_by }, 3.0f, tick);
  DrawLineEx((Vector2){ g_bx + br * 0.46f, g_by }, (Vector2){ g_bx + br * 0.78f, g_by }, 3.0f, tick);
  DrawLineEx((Vector2){ g_bx, g_by - br * 0.78f }, (Vector2){ g_bx, g_by - br * 0.46f }, 3.0f, tick);

  DrawCircleV((Vector2){ g_kx, g_ky }, br * 0.46f, Fade(RAYWHITE, engaged ? 0.42f : 0.18f));

  int held = (g_mask & IN_KICK) != 0;
  DrawCircleV((Vector2){ g_kick_x, g_kick_y }, g_kick_r, Fade(RAYWHITE, held ? 0.30f : 0.08f));
  DrawCircleLinesV((Vector2){ g_kick_x, g_kick_y }, g_kick_r, Fade(RAYWHITE, 0.30f));
  int fs = (int)(g_kick_r * 0.34f);
  int tw = MeasureText("KICK", fs);
  DrawText("KICK", (int)g_kick_x - tw / 2, (int)g_kick_y - fs / 2, fs, Fade(RAYWHITE, 0.62f));
}
