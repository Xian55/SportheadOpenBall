// Prints every tuning constant as a raw integer, for diffing against
// tests/const_golden.txt.
//
// Two things this catches that nothing else does. First, an accidental retune:
// a constant nudged while chasing something else shows up as a one-line diff
// rather than as a mysterious golden-replay failure. Second, FXF() folding
// drift: FXF multiplies by 65536.0 at COMPILE time, so a toolchain that folds
// that differently would silently shift the whole simulation. Pinning the folded
// integers turns "did the compiler agree?" into a CI gate.
//
// No floating point here: the point is to print exactly the integers the sim
// will use.
#include <stdio.h>
#include "config.h"
#include "sim.h"

#define P(name) printf("%-18s %d\n", #name, (int)(name))

int main(void) {
  puts("# raw Q16.16 integers. Regenerate with: const_test > tests/const_golden.txt");
  puts("# and say so explicitly in the commit message.");

  puts("[protocol]");
  P(PROTO_VER); P(TICK_HZ); P(INPUT_DELAY); P(MAX_ROLLBACK);
  P(INPUT_HIST); P(RB_N); P(FRAME_ADV_LIMIT);

  puts("[pitch]");
  P(FIELD_W); P(FIELD_H); P(GROUND_Y); P(CEIL_Y);
  P(GOAL_DEPTH); P(GOAL_H); P(POST_R);

  puts("[player]");
  P(HEAD_R); P(LEG_PIVOT_Y); P(LEG_LEN); P(FOOT_R);
  P(P_MOVE); P(P_JUMP); P(P_MAX_VY);
  P(SPAWN_X_P0); P(SPAWN_X_P1); P(PLAYER_REST_Y);

  puts("[leg]");
  P(LEG_REST); P(LEG_MAX_FWD); P(LEG_MAX_BACK);
  P(LEG_GRAVITY); P(LEG_DAMP); P(LEG_KICK_VEL); P(LEG_HOLD_TORQUE);
  P(LEG_SETTLE_A); P(LEG_SETTLE_V); P(LEG_MAX_VEL); P(LEG_VEL_REF);

  puts("[ball]");
  P(BALL_R); P(BALL_SUBSTEPS); P(BALL_MAX_SPD);
  P(E_GROUND); P(E_WALL); P(E_HEAD); P(E_POST);
  P(BALL_DRAG_X); P(BALL_FRIC_G);
  P(KICK_IMPULSE); P(KICK_W_NORMAL); P(KICK_W_TANGENT);

  puts("[shared]");
  P(GRAVITY); P(KICKOFF_FREEZE); P(CELEBRATE_FRAMES); P(MATCH_FRAMES);

  puts("[layout]");
  printf("%-18s %d\n", "sizeof_Player",    (int)sizeof(Player));
  printf("%-18s %d\n", "sizeof_GameState", (int)sizeof(GameState));
  return 0;
}
