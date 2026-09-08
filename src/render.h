// Draws a GameState with raylib primitives. Tier 3: raylib and float are fine
// here. render_* must NEVER write to a GameState.
#ifndef OB_RENDER_H
#define OB_RENDER_H
#include "sim.h"

// fx -> float, for drawing only. Never call this in tier 1.
#define FX2F(v) ((float)(v) * (1.0f / 65536.0f))

// The pitch is a FIXED 1280x720 virtual space. Everything draws into a render
// texture at that size, which is then blitted letterboxed to whatever the
// window/canvas actually is. This keeps the game resolution-independent, keeps
// OB_SHOT output byte-comparable across machines regardless of Windows display
// scaling, and gives us one place to map cursor -> pitch coordinates.
#define VIRT_W 1280
#define VIRT_H 720

void render_init(void);
void render_shutdown(void);
void render_frame(const GameState *s);          // draws AND presents the frame

// Exports the 1280x720 virtual framebuffer, not the window. Used by OB_SHOT.
void render_export_shot(const char *path);

// Window/canvas pixel -> virtual pitch coordinate, accounting for letterboxing.
Vector2 render_to_virtual(Vector2 screen_px);
#endif // OB_RENDER_H
