// Entry point, window setup, and the frame callback both platforms share.
// The loop body lives in frame() so emscripten can drive it: on web there is
// no way to block in a while loop without ASYNCIFY, which we deliberately do
// not use.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "raylib.h"
#include "sim.h"
#include "render.h"
#include "checksum.h"
#include "input.h"
#include "touch.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

#ifndef OB_VERSION
#define OB_VERSION "dev"
#endif

#define TICK_DT     (1.0 / (double)TICK_HZ)
#define MAX_CATCHUP 4       // sim ticks per rendered frame, hard cap

static GameState g_state;
static GameState g_prev;    // state one tick back, for render interpolation
static uint32_t  g_seed      = 0x5eed1234u;
static int       g_shot_frame = -1;   // OB_SHOT=N: screenshot at frame N, then exit 0
static int       g_frames     = 0;
static double    g_acc        = 0.0;
static uint8_t   g_force_in[2] = { 0, 0 };  // OB_IN0 / OB_IN1 debug hook
static double    g_t0 = 0.0;

#ifdef __EMSCRIPTEN__
// raylib's web backend resizes the canvas behind GLFW's back, so every mouse
// coordinate is scaled by the InitWindow-time ratio until we push the real CSS
// size back through SetWindowSize. Without this, menu hit-testing and touch
// button hit-testing are wrong the moment anyone goes fullscreen.
static void sync_canvas_size(void) {
  double cw = 0, ch = 0;
  if (emscripten_get_element_css_size("#canvas", &cw, &ch) != EMSCRIPTEN_RESULT_SUCCESS) return;
  int w = (int)cw, h = (int)ch;
  if (w > 0 && h > 0 && (w != GetScreenWidth() || h != GetScreenHeight())) SetWindowSize(w, h);
}
#endif

static void frame(void) {
#ifdef __EMSCRIPTEN__
  sync_canvas_size();
#endif

  touch_update();
  input_poll();

  if (g_state.phase == PH_OVER && IsKeyPressed(KEY_R)) {
    sim_init(&g_state, g_seed);
  g_prev = g_state;
    input_clear();
    g_acc = 0.0;
  }

  // Fixed 60 Hz. Rendering runs at whatever the display does; the sim never
  // sees a variable dt. The accumulator cap matters on web: a backgrounded tab
  // gets rAF at ~1 Hz, and without it the first frame back would try to
  // simulate hundreds of ticks in one go.
  g_acc += (double)GetFrameTime();
  if (g_acc > 0.25) g_acc = 0.25;

  int steps = 0;
  while (g_acc >= TICK_DT && steps < MAX_CATCHUP) {
    g_acc -= TICK_DT;
    const uint8_t in[2] = { (uint8_t)(input_consume(0) | g_force_in[0]),
                            (uint8_t)(input_consume(1) | g_force_in[1]) };
    g_prev = g_state;
    sim_step(&g_state, in);
    steps++;
  }
  // Whatever we could not run this frame is dropped rather than banked, so a
  // hitch cannot snowball into the spiral of death.
  if (steps == MAX_CATCHUP) g_acc = 0.0;

  // Whatever time is left over in the accumulator is how far past the last tick
  // the display is; render between the two states rather than snapping to one.
  render_frame(&g_prev, &g_state, (float)(g_acc / TICK_DT));

  g_frames++;
  if (g_shot_frame >= 0 && g_frames >= g_shot_frame) {
    // Exports the 1280x720 VIRTUAL framebuffer, not the window, so the output
    // is identical regardless of Windows display scaling or canvas size.
    // Lands in the LAUNCH cwd: raylib caches the working directory at InitWindow.
    // NOTE: GetFPS() is unreliable when read once just before exit - it reported
    // 1800 on a 60 Hz display and sent me chasing a vsync bug that did not exist.
    // The sustained figure below is wall-clock and is the one to trust.
    printf("DIAG monitor=%dHz fps=%d screen=%dx%d render=%dx%d frametime=%.4f\n",
           GetMonitorRefreshRate(GetCurrentMonitor()),
           (int)(g_frames / (GetTime() - g_t0)),
           GetScreenWidth(), GetScreenHeight(), GetRenderWidth(), GetRenderHeight(),
           (double)GetFrameTime());
    render_export_shot("openball_shot.png");
    // Also capture the real WINDOW framebuffer: the virtual export cannot show
    // a presentation bug, which is exactly how an oversized blit slipped past.
    if (getenv("OB_SHOT_WIN")) TakeScreenshot("openball_win.png");
    printf("OB_SHOT frame=%d sim_frame=%u checksum=%08x score=%u-%u\n",
           g_frames, g_state.frame, checksum_state(&g_state),
           (unsigned)g_state.score[0], (unsigned)g_state.score[1]);
    fflush(stdout);
    render_shutdown();
    CloseWindow();
    exit(0);
  }
}

int main(void) {
  const char *shot = getenv("OB_SHOT");
  if (shot) g_shot_frame = atoi(shot);
  const char *f0 = getenv("OB_IN0");
  if (f0) g_force_in[0] = (uint8_t)strtoul(f0, NULL, 0);
  const char *f1 = getenv("OB_IN1");
  if (f1) g_force_in[1] = (uint8_t)strtoul(f1, NULL, 0);
  const char *seed = getenv("OB_SEED");
  if (seed) g_seed = (uint32_t)strtoul(seed, NULL, 0);

  SetTraceLogLevel(LOG_WARNING);
  // No FLAG_WINDOW_HIGHDPI: with it, raylib's logical size and framebuffer size
  // diverge on a scaled display and the letterbox maths has to guess which one
  // each API wants. The render texture already gives us a fixed 1280x720 image;
  // letting the OS upscale it is a fair trade for unambiguous coordinates.
  SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
  InitWindow(1280, 720, "SportheadOpenBall " OB_VERSION);
  SetTargetFPS(60);

  g_t0 = GetTime();
  // OB_WINSIZE=WxH resizes the window at startup, so phone-shaped layouts can
  // be verified on a desktop without a device.
  const char *ws = getenv("OB_WINSIZE");
  if (ws) { int w=0,h=0; if (sscanf(ws, "%dx%d", &w, &h) == 2 && w > 160 && h > 120) SetWindowSize(w, h); }

  render_init();
  touch_init();
  sim_init(&g_state, g_seed);
  g_prev = g_state;

#ifdef __EMSCRIPTEN__
  // fps 0 = requestAnimationFrame. Never pass a fixed fps here: that path uses
  // setTimeout and drifts.
  emscripten_set_main_loop(frame, 0, 1);
#else
  while (!WindowShouldClose()) frame();
  render_shutdown();
  CloseWindow();
#endif
  return 0;
}
