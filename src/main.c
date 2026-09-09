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
#include "net.h"
#include "rollback.h"
#include "proto.h"

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

// --- networked play ---------------------------------------------------------
// OB_PEER or OB_ROOM switches from local hotseat to rollback over UDP. Both
// peers derive the sim seed from the room string, so it never crosses the wire.
static int        g_net_mode;      // 0 = hotseat, 1 = networked
static int        g_rb_started;
// OB_SIMSHOT=N: print the checksum of the state after N simulated frames, then
// exit 0. Render-frame counts drift between peers whenever one stalls, so
// comparing two live processes needs a SIM-frame reference point, and the frame
// must be CONFIRMED - a still-predicted frame is legitimately different.
static uint32_t   g_simshot;
static RbSession  g_rb;
static char       g_room[32] = "local";
static double     g_wait_t0;       // when we started waiting for an opponent

static void net_frame(void) {
  net_pump();

  uint8_t buf[PKT_MAX];
  int n;
  while ((n = net_poll(buf, (int)sizeof buf)) > 0) {
    if (g_rb_started) rb_on_packet(&g_rb, buf, n);
  }

  if (!g_rb_started) {
    if (net_state() == NET_OPEN && net_seat() >= 0) {
      rb_start(&g_rb, net_seat(), checksum_bytes(g_room, (uint32_t)strlen(g_room)));
      g_rb_started = 1;
      render_set_local_seat(net_seat());
      g_acc = 0.0;
      render_set_status(NULL);
      printf("net: connected as seat %d, room \"%s\"\n", net_seat(), g_room);
      fflush(stdout);
    } else if (net_state() == NET_FAILED) {
      render_set_status(net_last_error());
    } else {
      // ANIMATED, and with the elapsed time. Signalling legitimately takes a
      // while - Trystero dials several public Nostr relays and some are always
      // down, so the console fills with failed WebSocket connections that are
      // expected and survivable - but a caption frozen on "connecting to
      // peer..." for twenty seconds is indistinguishable from a hung game. A
      // moving cue says the client is alive; the seconds say how long it has
      // been trying, which is what tells you when to give up and re-share.
      static const char *dots[4] = { "", ".", "..", "..." };
      if (g_wait_t0 == 0.0) g_wait_t0 = GetTime();
      int  n    = (int)(GetTime() * 2.0) & 3;         // 2 Hz
      int  secs = (int)(GetTime() - g_wait_t0);
      char msg[96];
      snprintf(msg, sizeof msg, "waiting for opponent%s  (%ds)", dots[n], secs);
      render_set_status(msg);
    }
    // Nothing to simulate yet. Render the kickoff state so the window is not
    // blank, but the hotseat control hints below it are misleading here.
    render_frame(&g_state, &g_state, 0.0f);
    return;
  }

  g_rb.local_hidden = (uint8_t)(net_local_hidden() ? 1 : 0);

  // If either side is backgrounded, PAUSE rather than crawl. A hidden tab gets
  // animation frames at about 1 Hz, and because both peers must supply inputs
  // for the sim to advance, the visible side is dragged down with it - roughly
  // a hundredfold slowdown that looks like a broken game rather than a
  // throttled tab. Stopping is safe: the simulation only ever advances when
  // both inputs exist, so neither side can drift.
  int paused = g_rb.local_hidden || (g_rb.peer_flags & PKT_FLAG_HIDDEN);
  if (paused) {
    g_acc = 0.0;
    render_set_status(g_rb.local_hidden
      ? "paused - this tab is in the background"
      : "paused - opponent tabbed out");
    // Keep talking: the packet carries our hidden flag and the ack, so the far
    // side learns why it is paused and both resume together.
    int pn = rb_build_packet(&g_rb, buf, (int)sizeof buf);
    if (pn > 0) net_send(buf, pn);
    render_frame(&g_prev, &g_rb.live, 0.0f);
    return;
  }

  g_acc += (double)GetFrameTime();
  if (g_acc > 0.25) g_acc = 0.25;

  int steps = 0;
  while (g_acc >= TICK_DT && steps < MAX_CATCHUP) {
    // Stalling is not an error: it is how a peer that has predicted as far as it
    // may waits for the other side to catch up.
    if (rb_should_stall(&g_rb)) break;
    g_acc -= TICK_DT;
    uint8_t in = (uint8_t)(input_consume(0) | g_force_in[0]);
    g_prev = g_rb.live;
    rb_tick(&g_rb, in);
    steps++;
  }
  if (steps == MAX_CATCHUP) g_acc = 0.0;

  // Send EVERY frame, not only when a tick ran. Sending from inside the tick
  // loop meant a stalled peer went silent, which starved the other peer's
  // `confirmed` and made it stall too - the two then deadlocked, creeping
  // forward only via the anti-deadlock escape hatch. Eighteen simulated frames
  // in ten seconds.
  //
  // A stalled peer has no NEW input to offer, but the packet still carries the
  // last INPUT_HIST frames and the ack, which is exactly what the other side
  // needs to advance and stop stalling.
  {
    int pn = rb_build_packet(&g_rb, buf, (int)sizeof buf);
    if (pn > 0) net_send(buf, pn);
  }

  if (g_rb.desync) {
    static char msg[96];
    snprintf(msg, sizeof msg, "DESYNC at frame %u  %08x vs %08x",
             g_rb.desync_frame, g_rb.desync_mine, g_rb.desync_theirs);
    render_set_status(msg);
  } else if (!getenv("OB_NONETSTAT")) {
    static char msg[96];
    // Render rate belongs here: a browser throttles a BACKGROUND tab's
    // requestAnimationFrame to about 1 Hz, which starves the sim and looks
    // exactly like a netcode fault. Without this number on screen the two are
    // indistinguishable.
    double el = GetTime() - g_t0;
    snprintf(msg, sizeof msg, "seat %d  f%u  %.0ffps  ahead %d  rb %u  resim %u  stalls %u",
             g_rb.seat, g_rb.frame, el > 0.5 ? (double)g_frames / el : 0.0,
             (int)(g_rb.frame - g_rb.confirmed),
             g_rb.rollbacks, g_rb.resim_frames, g_rb.stalls);
    render_set_status(msg);
  }

  render_frame(&g_prev, &g_rb.live, (float)(g_acc / TICK_DT));

  if (g_simshot && g_rb.confirmed > g_simshot &&
      g_rb.hist_frame[g_simshot & RB_MASK] == g_simshot) {
    printf("SIMSHOT seat=%d frame=%u checksum=%08x rollbacks=%u resim=%u stalls=%u",
           g_rb.seat, g_simshot, g_rb.hash[g_simshot & RB_MASK],
           g_rb.rollbacks, g_rb.resim_frames, g_rb.stalls);
    putchar(10);
    fflush(stdout);
    net_close();
    render_shutdown();
    CloseWindow();
    exit(0);
  }
}

#ifdef __EMSCRIPTEN__
// raylib's web backend resizes the canvas behind GLFW's back, so every mouse
// coordinate is scaled by the InitWindow-time ratio until we push the real CSS
// size back through SetWindowSize. Without this, menu hit-testing and touch
// button hit-testing are wrong the moment anyone goes fullscreen.
static void sync_canvas_size(void) {
  double cw = 0, ch = 0;
  if (emscripten_get_element_css_size("#canvas", &cw, &ch) != EMSCRIPTEN_RESULT_SUCCESS) return;

  // Size the DRAWING BUFFER in device pixels, not CSS pixels. The CSS size keeps
  // the canvas filling the viewport; the buffer is what we actually rasterise
  // into. Sizing it in CSS pixels on a phone with devicePixelRatio 3 meant
  // rendering at a third of the screen's real resolution and letting the browser
  // upscale - which is exactly what "high resolution and yet blurry" looks like.
  //
  // Capped at 2x: beyond that the fill-rate cost on a phone outweighs any
  // visible sharpening.
  double dpr = emscripten_get_device_pixel_ratio();
  if (dpr < 1.0) dpr = 1.0;
  if (dpr > 2.0) dpr = 2.0;

  int w = (int)(cw * dpr + 0.5), h = (int)(ch * dpr + 0.5);
  if (w > 0 && h > 0 && (w != GetScreenWidth() || h != GetScreenHeight())) SetWindowSize(w, h);
}
#endif

static void frame(void) {
#ifdef __EMSCRIPTEN__
  sync_canvas_size();
#endif

  touch_update();
  input_poll();

  if (g_net_mode) { net_frame(); g_frames++; goto shot_check; }

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

  if (g_simshot && g_state.frame >= g_simshot) {
    printf("SIMSHOT seat=- frame=%u checksum=%08x", g_state.frame, checksum_state(&g_state));
    putchar(10);
    fflush(stdout);
    render_shutdown();
    CloseWindow();
    exit(0);
  }

  g_frames++;
shot_check:
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
    // CAUTION reading these: TakeScreenshot reads render*GetWindowScaleDPI().
    // Without HIGHDPI that correctly captures the real framebuffer, but WITH it
    // render already IS the framebuffer, so the capture over-reads by the DPI
    // factor and the extra margin is garbage. Judge the window by eye, not by
    // this file.
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
  const char *ss = getenv("OB_SIMSHOT");
  if (ss) g_simshot = (uint32_t)strtoul(ss, NULL, 0);
  const char *seed = getenv("OB_SEED");
  if (seed) g_seed = (uint32_t)strtoul(seed, NULL, 0);

  SetTraceLogLevel(LOG_WARNING);
  // FLAG_WINDOW_HIGHDPI is REQUIRED on a scaled display, from raylib 5.5's
  // source: without it InitPlatform never calls glfwGetFramebufferSize and just
  // assumes the framebuffer equals the requested size. At 125% scaling Windows
  // hands back a larger buffer anyway, so raylib sets a 1280x720 viewport inside
  // a 1600x900 one and the scene renders into a corner with black bars.
  //
  // With the flag it queries the real framebuffer, sets render to match, and
  // installs a screenScale matrix - so drawing still uses LOGICAL coordinates
  // (GetScreenWidth/Height) and raylib scales them onto the full buffer. That is
  // why the letterbox maths in render.c uses the logical size.
  SetConfigFlags(FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
  InitWindow(1280, 720, "SportheadOpenBall " OB_VERSION);
  SetTargetFPS(60);

  g_t0 = GetTime();
  // OB_WINSIZE=WxH resizes the window at startup, so phone-shaped layouts can
  // be verified on a desktop without a device.
  const char *ws = getenv("OB_WINSIZE");
  if (ws) { int w=0,h=0; if (sscanf(ws, "%dx%d", &w, &h) == 2 && w > 160 && h > 120) SetWindowSize(w, h); }

  // Networked when a peer or a room is named; local hotseat otherwise.
  const char *room = getenv("OB_ROOM");
  if (room && *room) {
    size_t i = 0;
    while (room[i] && i < sizeof g_room - 1) { g_room[i] = room[i]; i++; }
    g_room[i] = 0;
  }
  g_net_mode = (getenv("OB_PEER") != NULL) || (room && *room);

  // An invite link (?room=CODE) is enough on its own to start a networked match,
  // so a shared URL needs no further UI to act on.
  if (!g_net_mode) {
    char url_room[32];
    if (net_url_room(url_room, (int)sizeof url_room) && url_room[0]) {
      size_t i = 0;
      while (url_room[i] && i < sizeof g_room - 1) { g_room[i] = url_room[i]; i++; }
      g_room[i] = 0;
      g_net_mode = 1;
    }
  }

  render_init(g_shot_frame >= 0);
  touch_init();
  sim_init(&g_state, g_seed);
  g_prev = g_state;

  if (g_net_mode) {
    if (!net_open(g_room)) { printf("net: %s", net_last_error()); putchar(10); fflush(stdout); }
  }

#ifdef __EMSCRIPTEN__
  // fps 0 = requestAnimationFrame. Never pass a fixed fps here: that path uses
  // setTimeout and drifts.
  emscripten_set_main_loop(frame, 0, 1);
#else
  while (!WindowShouldClose()) frame();
  if (g_net_mode) net_close();
  render_shutdown();
  CloseWindow();
#endif
  return 0;
}
