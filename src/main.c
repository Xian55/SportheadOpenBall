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

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

#ifndef OB_VERSION
#define OB_VERSION "dev"
#endif

static GameState g_state;
static int       g_shot_frame = -1;   // OB_SHOT=N: screenshot at frame N, then exit 0
static int       g_frames = 0;

#ifdef __EMSCRIPTEN__
// raylib's web backend resizes the canvas behind GLFW's back, so every mouse
// coordinate is scaled by the InitWindow-time ratio until we push the real CSS
// size back through SetWindowSize. Without this, menu hit-testing is wrong the
// moment anyone goes fullscreen.
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

  // M1 replaces this with the real input -> rollback -> sim pipeline.
  const uint8_t in[2] = { 0, 0 };
  sim_step(&g_state, in);

  render_frame(&g_state);

  g_frames++;
  if (g_shot_frame >= 0 && g_frames >= g_shot_frame) {
    // Exports the 1280x720 VIRTUAL framebuffer, not the window, so the output
    // is identical regardless of Windows display scaling or canvas size.
    // Lands in the LAUNCH cwd: raylib caches the working directory at InitWindow.
    render_export_shot("openball_shot.png");
    printf("OB_SHOT frame=%d checksum=%08x\n", g_frames, checksum_state(&g_state));
    fflush(stdout);
    render_shutdown();
    CloseWindow();
    exit(0);
  }
}

int main(void) {
  const char *shot = getenv("OB_SHOT");
  if (shot) g_shot_frame = atoi(shot);

  SetTraceLogLevel(LOG_WARNING);
  SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
  InitWindow(1280, 720, "SportheadOpenBall " OB_VERSION);
  SetTargetFPS(60);

  render_init();
  sim_init(&g_state, 0x5eed1234u);

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
