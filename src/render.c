// Pitch, goals, players and ball, drawn from a GameState with raylib
// primitives. v1 uses no image assets at all; sprites replace these shapes
// later without the sim ever knowing.
#include "raylib.h"
#include "render.h"
#include "config.h"

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
  float sw = (float)GetRenderWidth(), sh = (float)GetRenderHeight();
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
  float hx = FX2F(p->x), hy = FX2F(p->y);
  float br = FX2F(BODY_R), hr = FX2F(HEAD_R);
  float by = hy + FX2F(BODY_OFF_Y);

  DrawCircleV((Vector2){ hx, by }, br, c);              // torso
  DrawCircleV((Vector2){ hx, hy }, hr, c);              // head
  DrawCircleLinesV((Vector2){ hx, hy }, hr, Fade(BLACK, 0.35f));

  // eye, so facing is readable at a glance
  float dir = p->facing ? 1.0f : -1.0f;
  DrawCircleV((Vector2){ hx + dir * hr * 0.38f, hy - hr * 0.12f }, hr * 0.13f, RAYWHITE);

  // the kick foot is only drawn while its hitbox is actually live
  if (p->kick_timer > 0) {
    float fx_ = hx + dir * FX2F(FOOT_OFF_X);
    float fy_ = hy + FX2F(FOOT_OFF_Y);
    DrawCircleV((Vector2){ fx_, fy_ }, FX2F(FOOT_R), c);
  }
  (void)seat;
}

static void draw_hud(const GameState *s) {
  int secs = (int)(s->clock / TICK_HZ);
  const char *score = TextFormat("%d  -  %d", s->score[0], s->score[1]);
  const char *clock = TextFormat("%d:%02d", secs / 60, secs % 60);
  DrawText(score, (int)(FW / 2) - MeasureText(score, 44) / 2, 18, 44, RAYWHITE);
  DrawText(clock, (int)(FW / 2) - MeasureText(clock, 24) / 2, 66, 24, Fade(RAYWHITE, 0.75f));
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

  DrawCircleV((Vector2){ FX2F(s->ball_x), FX2F(s->ball_y) }, FX2F(BALL_R), BALLCOL);
  DrawCircleLinesV((Vector2){ FX2F(s->ball_x), FX2F(s->ball_y) }, FX2F(BALL_R), Fade(BLACK, 0.45f));

  draw_hud(s);
}

void render_frame(const GameState *s) {
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
  EndDrawing();
}
