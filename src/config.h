// Every tunable, as a Q16.16 constant. No code lives here.
// Sim space: 1 unit = 1 render pixel on a 1280x720 pitch. Physics is PER FRAME
// at 60 Hz, not per second - there is no dt in the sim at all, which removes a
// whole class of determinism bug. const_test prints this table as raw integers
// and diffs it against tests/const_golden.txt, so an accidental retune or a
// change in how FXF folds is a CI failure.
#ifndef OB_CONFIG_H
#define OB_CONFIG_H

#include "fixed.h"

// --- protocol / netcode contract ------------------------------------------
// Bump PROTO_VER whenever anything below the line changes meaning: peers on
// different versions must refuse each other rather than desync.
#define PROTO_VER      1
#define TICK_HZ        60
#define INPUT_DELAY    2     // frames of local delay. 33 ms, barely felt, and it
                             // roughly halves visible mispredictions - which
                             // matter more here than in a fighter, because a
                             // mispredicted kick teleports the ball.
#define MAX_ROLLBACK   12    // 200 ms of prediction
#define INPUT_HIST     8     // frames of redundant input carried by every packet
#define RB_N           128   // history ring, power of two (~2.1 s)
#define RB_MASK        (RB_N - 1)
#define FRAME_ADV_LIMIT 4    // soft stall threshold vs the peer's clock

// --- pitch geometry --------------------------------------------------------
#define FIELD_W        FXI(1280)
#define FIELD_H        FXI(720)
#define GROUND_Y       FXI(620)     // y grows downward; this is the grass line
#define CEIL_Y         FXI(0)
#define GOAL_DEPTH     FXI(96)      // how far the net box cuts into the wall
// Goal mouth: two head-diameters tall plus a little headroom. Derived from
// HEAD_R rather than hardcoded so retuning the head keeps the goal in scale.
#define GOAL_H         (HEAD_R * 4 + FXI(16))   // = 2 * (2*HEAD_R) + 16
#define POST_R         FXI(7)       // crossbar tip modelled as a CIRCLE so it
                                    // reuses the circle-circle path exactly

// --- players ---------------------------------------------------------------
// A player is a BIG HEAD and a leg. There is no torso: the head is the body,
// and it is what heads the ball.
// LEG_LEN must exceed HEAD_R, or the foot at full forward swing ends up INSIDE
// the head circle and the leg is invisible exactly when it matters. That, plus
// "the foot rests on the turf", fixes the head height:
//   PLAYER_REST_Y = GROUND_Y - FOOT_R - LEG_LEN - LEG_PIVOT_Y
// The hip sits LOW, near the bottom of the head, where the head circle is
// narrow - so a forward swing sweeps clear of it instead of vanishing behind
// the widest part. The renderer also draws the leg IN FRONT of the head.
#define HEAD_R         FXI(42)
#define LEG_PIVOT_Y    FXI(28)      // hip, low on the head
#define LEG_LEN        FXI(40)      // hip -> foot centre
#define FOOT_R         FXI(10)
#define P_MOVE         FXF(5.0)     // px/frame. Arcade feel: velocity is SET, not accelerated
#define P_JUMP         FXF(-11.5)
#define P_MAX_VY       FXF(22.0)
#define SPAWN_X_P0     FXI(320)
#define SPAWN_X_P1     FXI(960)

// --- the leg ---------------------------------------------------------------
// Angle is in DEGREES (Q16.16), measured from straight down and signed toward
// the direction the player faces: 0 = hanging at rest, +90 = stuck straight out
// in front, negative = wound back behind.
#define LEG_REST       FXI(0)
#define LEG_MAX_FWD    FXI(90)
#define LEG_MAX_BACK   FXI(75)
#define LEG_SWING_RATE FXF(9.0)     // deg/frame while the button is held
#define LEG_RETURN_RATE FXF(6.0)    // deg/frame drifting back to rest
#define LEG_SPRING     FXF(0.55)    // windup angle -> release angular velocity
// Cap on the snap-back speed. The foot only moves BETWEEN frames while the ball
// is tested within them, so an uncapped spring (49.5 deg/frame at a full cock =
// 34 px of arc) can sweep straight past the ball and register no contact. At 28
// deg/frame the foot travels ~19.5 px per frame against a 26 px combined
// foot+ball radius, which cannot tunnel.
#define LEG_MAX_VEL    FXF(28.0)
#define LEG_VEL_REF    LEG_MAX_VEL  // a full-speed snap is a full-power kick

// --- ball ------------------------------------------------------------------
#define BALL_R         FXI(16)
#define BALL_SUBSTEPS  2            // anti-tunnelling. Part of the protocol contract.
#define BALL_MAX_SPD   FXF(24.0)    // < BALL_R * BALL_SUBSTEPS, so it cannot pass a post
#define E_GROUND       FXF(0.72)
#define E_WALL         FXF(0.80)
#define E_HEAD         FXF(0.85)
#define E_POST         FXF(0.65)
#define BALL_DRAG_X    FXF(0.995)
#define BALL_FRIC_G    FXF(0.980)
#define KICK_IMPULSE   FXF(15.0)    // at LEG_VEL_REF; scaled by actual swing speed
// A kick fires along a blend of the contact normal and the direction the foot
// is actually SWINGING. The tangent is what lets you scoop: with the leg low
// the foot travels forward-and-up, so getting under the ball lifts it, while a
// pure-normal impulse would only ever drive it flat.
#define KICK_W_NORMAL  FXF(0.45)
#define KICK_W_TANGENT FXF(0.55)

// --- shared ----------------------------------------------------------------
#define GRAVITY        FXF(0.55)    // px/frame^2
#define KICKOFF_FREEZE 60
#define CELEBRATE_FRAMES (TICK_HZ * 2)
#define MATCH_SECONDS  90
#define MATCH_FRAMES   (MATCH_SECONDS * TICK_HZ)

// --- input bits (one byte per player per frame, straight onto the wire) -----
#define IN_LEFT        0x01
#define IN_RIGHT       0x02
#define IN_JUMP        0x04
#define IN_KICK        0x08
#define IN_DOWN        0x10   // reserved, unused in v1
#define IN_MASK        0x1F   // decode ANDs with this: a peer cannot inject unknown bits

// --- match phases (uint8_t in GameState; NOT an enum - enum size is
//     implementation-defined and this struct is hashed byte-for-byte) --------
#define PH_KICKOFF     0
#define PH_PLAY        1
#define PH_GOAL        2
#define PH_OVER        3

#endif // OB_CONFIG_H
