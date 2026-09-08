// Pitch, goals, players and ball, drawn from a GameState with raylib
// primitives. v1 uses no image assets at all; sprites replace these shapes
// later without the sim ever knowing.
#include <math.h>
#include "raylib.h"
#include "render.h"
#include "config.h"
#include "touch.h"

#define SKY        (Color){  38,  42,  62, 255 }
#define GRASS      (Color){  46, 122,  62, 255 }
#define GRASS_DARK (Color){  40, 108,  55, 255 }
#define LINE       (Color){ 230, 235, 240, 190 }
#define NETCOL     (Color){ 220, 226, 235, 110 }
#define POSTCOL    (Color){ 244, 246, 250, 255 }
#define P0COL      (Color){ 224,  82,  74, 255 }
#define P1COL      (Color){  74, 132, 224, 255 }
#define BALLCOL    (Color){ 248, 248, 248, 255 }

static const float GY = FX2F(GROUND_Y);
static const float FW = FX2F(FIELD_W);
static const float GD = FX2F(GOAL_DEPTH);
static const float CBY = FX2F(GROUND_Y - GOAL_H);   // crossbar y

// Everything is drawn into this at exactly VIRT_W x VIRT_H, then blitted to
// the window. Windows display scaling (and the web canvas) make the real
// framebuffer an arbitrary size; the pitch must not care.
static RenderTexture2D g_target;
static float  g_scale = 1.0f;   // virtual -> screen
static Vector2 g_offset;        // letterbox origin, screen px

void render_init(void) {
  g_target = LoadRenderTexture(VIRT_W, VIRT_H);
  SetTextureFilter(g_target.texture, TEXTURE_FILTER_BILINEAR);
}

void render_shutdown(void) { UnloadRenderTexture(g_target); }

static void update_letterbox(void) {
  // Logical size. With the process marked DPI-aware (see dpi_win32.c) this is
  // also the framebuffer size, so there is no second coordinate space to get
  // wrong - which is exactly the bug this arrangement removes.
  float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
  float sx = sw / (float)VIRT_W, sy = sh / (float)VIRT_H;
  g_scale = (sx < sy) ? sx : sy;
  g_offset = (Vector2){ (sw - VIRT_W * g_scale) * 0.5f, (sh - VIRT_H * g_scale) * 0.5f };
}

Vector2 render_to_virtual(Vector2 p) {
  return (Vector2){ (p.x - g_offset.x) / g_scale, (p.y - g_offset.y) / g_scale };
}

void render_export_shot(const char *path) {
  Image img = LoadImageFromTexture(g_target.texture);
  ImageFlipVertical(&img);          // GL origin is bottom-left
  ExportImage(img, path);
  UnloadImage(img);
}

// One goal. side = -1 for the left goal, +1 for the right.
static void draw_goal(int side) {
  float x0 = (side < 0) ? 0.0f : FW - GD;
  float pr = FX2F(POST_R);

  // net hatching, drawn first so the frame sits on top of it
  for (float x = x0 + 8.0f; x < x0 + GD; x += 14.0f)
    DrawLineEx((Vector2){ x, CBY }, (Vector2){ x, GY }, 1.0f, NETCOL);
  for (float y = CBY + 10.0f; y < GY; y += 14.0f)
    DrawLineEx((Vector2){ x0, y }, (Vector2){ x0 + GD, y }, 1.0f, NETCOL);

  // crossbar, and the post tip the ball actually collides with (a circle, so
  // the sim reuses its circle-circle path)
  float bar_x = (side < 0) ? 0.0f : FW - GD;
  DrawRectangleRec((Rectangle){ bar_x, CBY - pr, GD, pr * 2.0f }, POSTCOL);
  float tip_x = (side < 0) ? GD : FW - GD;
  DrawCircleV((Vector2){ tip_x, CBY }, pr, POSTCOL);
}

static void draw_player(const Player *p, Color c, int seat) {
  float hx = FX2F(p->x), hy = FX2F(p->y), hr = FX2F(HEAD_R);

  DrawCircleV((Vector2){ hx, hy }, hr, c);
  DrawCircleLinesV((Vector2){ hx, hy }, hr, Fade(BLACK, 0.35f));

  // eye, so facing is readable at a glance
  float dir = p->facing ? 1.0f : -1.0f;
  DrawCircleV((Vector2){ hx + dir * hr * 0.34f, hy - hr * 0.14f }, hr * 0.16f, RAYWHITE);
  DrawCircleV((Vector2){ hx + dir * hr * 0.40f, hy - hr * 0.14f }, hr * 0.07f, BLACK);

  // The leg goes ON TOP of the head. Drawn behind it, a forward swing is
  // swallowed by the head circle exactly when the player needs to see it.
  // Position comes from sim_foot() - the SAME call the hitbox uses - so what
  // you see is precisely what the ball collides with.
  fx fpx, fpy;
  sim_foot(p, &fpx, &fpy);
  Vector2 hip  = { hx, FX2F(sim_hip_y(p)) };
  Vector2 foot = { FX2F(fpx), FX2F(fpy) };
  Color   dark = { (unsigned char)(c.r * 52 / 100), (unsigned char)(c.g * 52 / 100),
                   (unsigned char)(c.b * 52 / 100), 255 };
  DrawLineEx(hip, foot, FX2F(FOOT_R) * 1.4f, dark);
  DrawCircleV(foot, FX2F(FOOT_R), dark);
  (void)seat;
}

static void draw_hud(const GameState *s) {
  int secs = (int)(s->clock / TICK_HZ);
  const char *score = TextFormat("%d  -  %d", s->score[0], s->score[1]);
  const char *clock = TextFormat("%d:%02d", secs / 60, secs % 60);
  DrawText(score, (int)(FW / 2) - MeasureText(score, 44) / 2, 18, 44, RAYWHITE);
  DrawText(clock, (int)(FW / 2) - MeasureText(clock, 24) / 2, 66, 24, Fade(RAYWHITE, 0.75f));
}

static void draw_ball(const GameState *s) {
  Vector2 c = { FX2F(s->ball_x), FX2F(s->ball_y) };
  float   r = FX2F(BALL_R);
  DrawCircleV(c, r, BALLCOL);
  DrawCircleLinesV(c, r, Fade(BLACK, 0.45f));

  // Spin is carried in the sim state (so it can never diverge between peers)
  // purely so the ball reads as rolling rather than sliding.
  float a = FX2F(s->ball_spin) * (float)(PI / 180.0);
  for (int i = 0; i < 2; i++) {
    float t = a + (float)i * (float)(PI / 2.0);
    Vector2 e = { c.x + cosf(t) * r * 0.62f, c.y + sinf(t) * r * 0.62f };
    DrawLineEx(c, e, 3.0f, Fade(BLACK, 0.30f));
  }
}

static void draw_banner(const GameState *s) {
  const char *msg = 0;
  int         sub = 0;

  if (s->phase == PH_KICKOFF) {
    msg = TextFormat("%d", (int)(s->phase_timer / TICK_HZ) + 1);
    sub = 1;
  } else if (s->phase == PH_GOAL) {
    msg = "GOAL";
  } else if (s->phase == PH_OVER) {
    msg = (s->score[0] == s->score[1]) ? "DRAW"
        : (s->score[0] >  s->score[1]) ? "RED WINS" : "BLUE WINS";
  }
  if (!msg) return;

  int fs = 72;
  int tw = MeasureText(msg, fs);
  DrawRectangleRec((Rectangle){ 0, 250, FW, 120 }, Fade(BLACK, 0.35f));
  DrawText(msg, (int)(FW / 2) - tw / 2, 268, fs, RAYWHITE);

  if (sub) {
    const char *hint = "RED: A D move, W jump, S kick     BLUE: arrows, UP jump, DOWN kick";
    int hs = 18, hw = MeasureText(hint, hs);
    DrawText(hint, (int)(FW / 2) - hw / 2, 392, hs, Fade(RAYWHITE, 0.70f));
  }
}

// Draws the pitch into the virtual framebuffer.
static void draw_scene(const GameState *s) {
  ClearBackground(SKY);

  // pitch
  DrawRectangleRec((Rectangle){ 0, GY, FW, FX2F(FIELD_H) - GY }, GRASS);
  DrawRectangleRec((Rectangle){ 0, GY, FW, 6 }, GRASS_DARK);
  DrawLineEx((Vector2){ FW / 2, CBY - 40 }, (Vector2){ FW / 2, GY }, 2.0f, Fade(LINE, 0.5f));

  draw_goal(-1);
  draw_goal(+1);

  draw_player(&s->p[0], P0COL, 0);
  draw_player(&s->p[1], P1COL, 1);

  draw_ball(s);
  draw_hud(s);
  draw_banner(s);
}

// Interpolate a position between ticks. A large jump means a teleport - kickoff
// reset, goal reset - and must SNAP, or the ball smears across the pitch.
static fx lerp_fx(fx a, fx b, float t) {
  if (fx_abs(b - a) > FXI(240)) return b;
  return a + (fx)((float)(b - a) * t);
}

// Ball spin wraps at 360, so interpolate the shortest way round.
static fx lerp_ang(fx a, fx b, float t) {
  fx d = b - a;
  while (d >  FXI(180)) d -= FXI(360);
  while (d < -FXI(180)) d += FXI(360);
  return a + (fx)((float)d * t);
}

void render_frame(const GameState *prev, const GameState *cur, float alpha) {
  if (alpha < 0.0f) alpha = 0.0f;
  if (alpha > 1.0f) alpha = 1.0f;

  // A display-only copy. The sim state itself is never touched here.
  GameState v = *cur;
  for (int i = 0; i < 2; i++) {
    v.p[i].x   = lerp_fx(prev->p[i].x,   cur->p[i].x,   alpha);
    v.p[i].y   = lerp_fx(prev->p[i].y,   cur->p[i].y,   alpha);
    v.p[i].leg = lerp_fx(prev->p[i].leg, cur->p[i].leg, alpha);
  }
  v.ball_x    = lerp_fx (prev->ball_x,    cur->ball_x,    alpha);
  v.ball_y    = lerp_fx (prev->ball_y,    cur->ball_y,    alpha);
  v.ball_spin = lerp_ang(prev->ball_spin, cur->ball_spin, alpha);
  const GameState *s = &v;

  BeginTextureMode(g_target);
    draw_scene(s);
  EndTextureMode();

  update_letterbox();
  BeginDrawing();
    ClearBackground(BLACK);
    // Source height is negative: the render texture is bottom-up in GL.
    DrawTexturePro(g_target.texture,
                   (Rectangle){ 0, 0, (float)VIRT_W, -(float)VIRT_H },
                   (Rectangle){ g_offset.x, g_offset.y,
                                VIRT_W * g_scale, VIRT_H * g_scale },
                   (Vector2){ 0, 0 }, 0.0f, WHITE);
    // Touch controls go here, in SCREEN space and outside the render texture,
    // so they keep a usable physical size instead of shrinking with the pitch.
    touch_draw();
  EndDrawing();
}
