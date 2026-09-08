// GameState and the one function that may mutate it. Tier 1: no raylib, no
// float, no libm. This struct is memcpy'd into the rollback ring every frame
// and hashed byte-for-byte, so its layout is a hard contract.
#ifndef OB_SIM_H
#define OB_SIM_H

#include <stdint.h>
#include "config.h"
#include "fixed.h"

typedef struct Player {
  fx      x, y;        //  0..7   head centre
  fx      vx, vy;      //  8..15
  uint8_t on_ground;   // 16
  uint8_t kick_timer;  // 17      frames the foot hitbox stays live
  uint8_t kick_cd;     // 18
  uint8_t facing;      // 19      0 = left, 1 = right
} Player;               // 20 bytes, no padding

typedef struct GameState {
  uint32_t frame;        //  0   frames since kickoff
  uint32_t rng;          //  4   xorshift32; reserved in v1, keeps the hash layout stable
  Player   p[2];         //  8..47
  fx       ball_x, ball_y;    // 48..55
  fx       ball_vx, ball_vy;  // 56..63
  fx       ball_spin;    // 64  visual only, but IN the state so it can never diverge
  uint32_t clock;        // 68  frames remaining in the match
  uint8_t  score[2];     // 72..73
  uint8_t  phase;        // 74  PH_KICKOFF / PH_PLAY / PH_GOAL / PH_OVER
  uint8_t  phase_timer;  // 75
  uint8_t  last_scorer;  // 76
  uint8_t  _pad[3];      // 77..79  EXPLICIT: the checksum reads these bytes, so
                         //         they must be zeroed, never left indeterminate
} GameState;

// A layout drift between x86-64 and wasm32 must be a build error, not a
// runtime mystery. This assert compiles on both targets.
_Static_assert(sizeof(GameState) == 80, "GameState must be 80 bytes on every target");
_Static_assert(sizeof(Player) == 20, "Player must be 20 bytes on every target");

// seed comes from hash(room code) - computed identically by both peers and
// never transmitted.
void sim_init(GameState *s, uint32_t seed);

// Exactly one 1/60 s tick. in[seat] is a bitmask of IN_*.
void sim_step(GameState *s, const uint8_t in[2]);

// Resting head-centre y for a player standing on the ground.
#define PLAYER_REST_Y (GROUND_Y - BODY_OFF_Y - BODY_R)

#endif // OB_SIM_H
