// Draws a GameState with raylib primitives. Tier 3: raylib and float are fine
// here. render_* must NEVER write to a GameState.
#ifndef OB_RENDER_H
#define OB_RENDER_H
#include "sim.h"

// fx -> float, for drawing only. Never call this in tier 1.
#define FX2F(v) ((float)(v) * (1.0f / 65536.0f))

// The pitch is a FIXED 1280x720 virtual space, letterboxed into whatever the
// window/canvas actually is. That keeps the game resolution-independent and
// gives us one place to map cursor -> pitch coordinates.
//
// It is a COORDINATE system, not a buffer: normal play draws straight to the
// backbuffer at the display's own resolution, with a Camera2D applying the
// letterbox transform. Only OB_SHOT goes through a real 1280x720 texture, so
// its export stays byte-comparable across machines.
#define VIRT_W 1280
#define VIRT_H 720

// A short line drawn over the pitch: connection progress, desync, and the like.
// Pass NULL or "" to clear.
void render_set_status(const char *msg);

// Which seat this window drives, or -1 for hotseat. Draws a marker over your own
// player: with two windows open, that is what makes it obvious at a glance
// whether both are showing the SAME game.
void render_set_local_seat(int seat);

// shot_mode != 0 routes drawing through a 1280x720 render texture so OB_SHOT can
// export a byte-comparable image. Normal play draws straight to the backbuffer
// at the display's own resolution instead - going through a fixed-size texture
// resamples the whole frame and eats thin lines on any downscaled display.
void render_init(int shot_mode);
void render_shutdown(void);
// Draws AND presents. `prev` is the state one tick before `cur`, and alpha is
// how far between them the display currently sits. The sim ticks at a fixed
// 60 Hz while the display runs at its own rate, so without this the accumulator
// lands 0 steps on one frame and 2 on the next and everything visibly stutters.
void render_frame(const GameState *prev, const GameState *cur, float alpha);

// Exports the 1280x720 virtual framebuffer, not the window. Used by OB_SHOT.
void render_export_shot(const char *path);

// Where the letterboxed pitch actually sits, in screen coordinates. Touch
// controls use it to tell whether the side bars are wide enough to hold them.
Rectangle render_pitch_rect(void);

// Window/canvas pixel -> virtual pitch coordinate, accounting for letterboxing.
Vector2 render_to_virtual(Vector2 screen_px);
#endif // OB_RENDER_H
