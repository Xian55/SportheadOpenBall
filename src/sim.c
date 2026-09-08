// The deterministic match simulation. Tier 1: no raylib, no float, no libm.
//
// sim_step's ORDER OF OPERATIONS is part of the protocol contract. Reordering
// it changes results and re-blesses every golden fixture:
//   1. phase timers (kickoff / goal freeze countdown)
//   2. player 0 then player 1: apply input -> vx, jump, kick start/tick
//   3. players integrate (gravity, position), collide vs ground/walls, then each other
//   4. ball x BALL_SUBSTEPS:
//        gravity/drag -> integrate -> ground/wall/ceiling -> posts
//        -> p0 head/body/foot -> p1 head/body/foot -> clamp speed
//   5. goal test (ball centre inside either goal rect)
//   6. clock tick, phase transitions
//   7. frame++
#include <string.h>
#include "sim.h"

void sim_init(GameState *s, uint32_t seed) {
  // memset FIRST: _pad and every future field must be zero, because the whole
  // struct is hashed byte-for-byte.
  memset(s, 0, sizeof *s);

  // Designated initialisers only, everywhere. A positional compound literal
  // shifts silently when the struct grows, and in a checksummed state that
  // surfaces as an unexplained desync instead of a compile error.
  s->rng   = seed ? seed : 1u;
  s->clock = MATCH_FRAMES;
  s->phase = PH_KICKOFF;
  s->phase_timer = KICKOFF_FREEZE;

  s->p[0] = (Player){ .x = SPAWN_X_P0, .y = PLAYER_REST_Y, .facing = 1, .on_ground = 1 };
  s->p[1] = (Player){ .x = SPAWN_X_P1, .y = PLAYER_REST_Y, .facing = 0, .on_ground = 1 };

  s->ball_x = FIELD_W / 2;
  s->ball_y = GROUND_Y - FXI(220);
}

void sim_step(GameState *s, const uint8_t in[2]) {
  // M1 implements this. Kept as a no-op that still advances the frame so the
  // rollback ring, checksum and renderer can all be built and exercised first.
  (void)in;
  s->frame++;
}
