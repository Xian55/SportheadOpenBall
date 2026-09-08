// GameState and the one function that may mutate it. Tier 1: no raylib, no
// float, no libm. This struct is memcpy'd into the rollback ring every frame
// and hashed byte-for-byte, so its layout is a hard contract.
#ifndef OB_SIM_H
#define OB_SIM_H

#include <stdint.h>
#include "config.h"
#include "fixed.h"

// A player is a big head plus a leg. No torso: the head IS the body.
typedef struct Player {
  fx      x, y;        //  0..7   head centre
  fx      vx, vy;      //  8..15
  fx      leg;         // 16..19  leg angle in degrees, signed toward `facing`
  fx      leg_vel;     // 20..23  angular velocity, non-zero during a swing
  uint8_t on_ground;   // 24
  uint8_t kick_held;   // 25      last frame's kick bit, so release is an edge
  uint8_t facing;      // 26      0 = left, 1 = right
  uint8_t _pad;        // 27      EXPLICIT: this struct is hashed byte-for-byte
} Player;              // 28 bytes

typedef struct GameState {
  uint32_t frame;        //  0   frames since kickoff
  uint32_t rng;          //  4   xorshift32; reserved in v1, keeps the hash layout stable
  Player   p[2];         //  8..63
  fx       ball_x, ball_y;    // 64..71
  fx       ball_vx, ball_vy;  // 72..79
  fx       ball_spin;    // 80  visual only, but IN the state so it can never diverge
  uint32_t clock;        // 84  frames remaining in the match
  uint8_t  score[2];     // 88..89
  uint8_t  phase;        // 90  PH_KICKOFF / PH_PLAY / PH_GOAL / PH_OVER
  uint8_t  phase_timer;  // 91
  uint8_t  last_scorer;  // 92
  uint8_t  _pad[3];      // 93..95  EXPLICIT - the checksum reads these bytes, so
                         //         they must be zeroed, never left indeterminate
} GameState;

// A layout drift between x86-64 and wasm32 must be a build error, not a
// runtime mystery. These asserts compile on both targets.
_Static_assert(sizeof(Player) == 28, "Player must be 28 bytes on every target");
_Static_assert(sizeof(GameState) == 96, "GameState must be 96 bytes on every target");

// seed comes from hash(room code) - computed identically by both peers and
// never transmitted.
void sim_init(GameState *s, uint32_t seed);

// Exactly one 1/60 s tick. in[seat] is a bitmask of IN_*.
void sim_step(GameState *s, const uint8_t in[2]);

// Where the foot centre is right now, given the leg angle. Shared with the
// renderer so the drawn leg and the hitbox can never disagree.
void sim_foot(const Player *p, fx *fx_out, fx *fy_out);
fx   sim_hip_y(const Player *p);

// Resting head-centre y: the foot hangs straight down and just touches the turf.
#define PLAYER_REST_Y (GROUND_Y - FOOT_R - LEG_LEN - LEG_PIVOT_Y)

#endif // OB_SIM_H
