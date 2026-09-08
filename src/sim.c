// The deterministic match simulation. Tier 1: no raylib, no float, no libm.
//
// sim_step's ORDER OF OPERATIONS is part of the protocol contract. Reordering
// it changes results and re-blesses every golden fixture:
//   1. phase timers (kickoff / goal freeze countdown)
//   2. player 0 then player 1: apply input -> vx, jump, leg swing
//   3. players integrate (gravity, position), collide vs ground/walls, then each other
//   4. ball x BALL_SUBSTEPS:
//        integrate -> crossbars -> p0 head/foot -> p1 head/foot
//        -> ground/wall/ceiling LAST (hard constraint)
//   5. goal test (ball centre inside either goal mouth)
//   6. clock tick, phase transitions
//   7. frame++
//
// Physics is PER FRAME, not per second: there is no dt anywhere in this file.
// A frame is always 1/60 s by definition, which removes an entire class of
// determinism bug.
#include <string.h>
#include "sim.h"
#include "trig.h"

// Crossbar height, derived once. y grows downward, so this is ABOVE the ground.
#define CROSSBAR_Y   (GROUND_Y - GOAL_H)
#define RIGHT_GOAL_X (FIELD_W - GOAL_DEPTH)

// ---------------------------------------------------------------------------
// Collision primitives.
//
// Every overlap TEST compares squared quantities in fx2 and takes no root; only
// the handful of collisions that actually fire pay for an isqrt. Squared values
// are Q32.32 and must never be narrowed to an fx - see fixed.h.
// ---------------------------------------------------------------------------

// Circle a vs circle b. On overlap fills the unit normal (pointing from b to a)
// and the penetration depth.
static int hit_circle(fx ax, fx ay, fx ar, fx bx, fx by, fx br,
                      fx *nx, fx *ny, fx *pen) {
  fx  dx = ax - bx, dy = ay - by;
  fx2 d2 = fx_d2(dx, dy);
  fx  rs = ar + br;
  fx2 r2 = (fx2)rs * (fx2)rs;          // Q32.32, directly comparable with d2
  if (d2 >= r2) return 0;

  fx d = fx_len(dx, dy);
  if (d <= 0) {                        // exactly concentric: push straight up
    *nx = 0; *ny = -FX_ONE; *pen = rs;
    return 1;
  }
  *nx  = fx_div(dx, d);
  *ny  = fx_div(dy, d);
  *pen = rs - d;
  return 1;
}

// Circle vs axis-aligned box, via the closest point on the box.
static int hit_aabb(fx cx, fx cy, fx r, fx x0, fx y0, fx x1, fx y1,
                    fx *nx, fx *ny, fx *pen) {
  fx  qx = fx_clamp(cx, x0, x1), qy = fx_clamp(cy, y0, y1);
  fx  dx = cx - qx, dy = cy - qy;
  fx2 d2 = fx_d2(dx, dy);

  if (d2 == 0) {
    // Centre is inside the box. Eject through the nearest face, which is the
    // only case where the closest-point normal is undefined.
    fx l = cx - x0, rr = x1 - cx, t = cy - y0, b = y1 - cy;
    fx m = fx_min(fx_min(l, rr), fx_min(t, b));
    if      (m == l)  { *nx = -FX_ONE; *ny = 0;       *pen = l  + r; }
    else if (m == rr) { *nx =  FX_ONE; *ny = 0;       *pen = rr + r; }
    else if (m == t)  { *nx = 0;       *ny = -FX_ONE; *pen = t  + r; }
    else              { *nx = 0;       *ny =  FX_ONE; *pen = b  + r; }
    return 1;
  }
  if (d2 >= (fx2)r * (fx2)r) return 0;

  fx d = fx_len(dx, dy);
  *nx  = fx_div(dx, d);
  *ny  = fx_div(dy, d);
  *pen = r - d;
  return 1;
}

// Separate the ball along the normal, then reflect its velocity RELATIVE to the
// thing it hit, so a moving head transfers momentum instead of acting like a
// wall. Only reflects when the two are actually closing.
static void ball_bounce(GameState *s, fx nx, fx ny, fx pen,
                        fx ovx, fx ovy, fx e) {
  s->ball_x += fx_mul(nx, pen);
  s->ball_y += fx_mul(ny, pen);

  fx rvx = s->ball_vx - ovx, rvy = s->ball_vy - ovy;
  fx vn  = fx_mul(rvx, nx) + fx_mul(rvy, ny);
  if (vn < 0) {
    fx j = fx_mul(FX_ONE + e, vn);
    s->ball_vx -= fx_mul(j, nx);
    s->ball_vy -= fx_mul(j, ny);
  }
}

static void clamp_speed(fx *vx, fx *vy, fx maxs) {
  fx2 d2 = fx_d2(*vx, *vy);
  if (d2 <= (fx2)maxs * (fx2)maxs) return;
  fx len = fx_len(*vx, *vy);
  if (len <= 0) return;
  // Unit component first (|result| <= FX_ONE), then scale. Never multiply two
  // large quantities together.
  *vx = fx_mul(fx_div(*vx, len), maxs);
  *vy = fx_mul(fx_div(*vy, len), maxs);
}

// ---------------------------------------------------------------------------
// The leg.
//
// Angle is degrees from straight down, signed toward `facing`: 0 hangs at rest,
// +90 sticks straight out in front, negative is wound back behind. The renderer
// asks sim_foot() for the same position the hitbox uses, so the drawn leg and
// the collision can never disagree.
// ---------------------------------------------------------------------------

fx sim_hip_y(const Player *p) { return p->y + LEG_PIVOT_Y; }

void sim_foot(const Player *p, fx *out_x, fx *out_y) {
  fx s = fx_sin_deg(p->leg), c = fx_cos_deg(p->leg);
  fx reach = fx_mul(LEG_LEN, s);
  *out_x = p->facing ? (p->x + reach) : (p->x - reach);
  *out_y = sim_hip_y(p) + fx_mul(LEG_LEN, c);
}

// Pressing kick FIRES the leg forward and up - that sweep is the strike. It
// holds at full extension while the button is down, then drifts back to rest on
// release.
//
// The direction is not a style choice, it falls out of the geometry: with the
// foot at (dir*L*sin t, L*cos t) and y pointing down, a RISING leg (t
// increasing) moves the foot forward and up, while a descending one moves it
// backward and down. Only the upswing can kick a ball forward; the downswing is
// a stomp. An earlier build had the release snap downward and every kick drove
// the ball into the turf at a third of the intended power.
static void leg_update(Player *p, int kick) {
  int press = (kick && !p->kick_held);

  if (press && p->leg <= LEG_REST) {
    p->leg_vel = LEG_MAX_VEL;          // fire
  }

  if (p->leg_vel > 0) {
    p->leg += p->leg_vel;
    if (p->leg >= LEG_MAX_FWD) { p->leg = LEG_MAX_FWD; p->leg_vel = 0; }
  } else if (!kick && p->leg > LEG_REST) {
    // Released: the leg simply falls back to rest. Harmless - leg_vel stays 0,
    // so a returning leg carries no kick power.
    p->leg -= LEG_RETURN_RATE;
    if (p->leg < LEG_REST) p->leg = LEG_REST;
  }

  p->kick_held = (uint8_t)(kick ? 1 : 0);
}

// How fast the foot is travelling, as Q16.16 in 0..1. Only ever non-zero on the
// upswing, which is the only part of the arc that can strike the ball forward.
static fx leg_power(const Player *p) {
  if (p->leg_vel <= 0) return 0;
  fx q = fx_div(p->leg_vel, LEG_VEL_REF);
  return (q > FX_ONE) ? FX_ONE : q;
}

// ---------------------------------------------------------------------------

static void reset_positions(GameState *s) {
  s->p[0].x = SPAWN_X_P0; s->p[0].y = PLAYER_REST_Y;
  s->p[1].x = SPAWN_X_P1; s->p[1].y = PLAYER_REST_Y;
  for (int i = 0; i < 2; i++) {
    s->p[i].vx = 0; s->p[i].vy = 0;
    s->p[i].leg = LEG_REST;
    s->p[i].leg_vel = 0;
    s->p[i].on_ground = 1;
    s->p[i].kick_held = 0;
  }
  s->p[0].facing = 1;   // face the opponent's goal
  s->p[1].facing = 0;

  s->ball_x  = FIELD_W / 2;
  s->ball_y  = GROUND_Y - FXI(260);
  s->ball_vx = 0;
  s->ball_vy = 0;
}

void sim_init(GameState *s, uint32_t seed) {
  // memset FIRST: _pad and every future field must be zero, because the whole
  // struct is hashed byte-for-byte.
  memset(s, 0, sizeof *s);

  s->rng   = seed ? seed : 1u;
  s->clock = MATCH_FRAMES;
  s->phase = PH_KICKOFF;
  s->phase_timer = KICKOFF_FREEZE;
  s->ball_spin = 0;

  reset_positions(s);
}

// --- players ---------------------------------------------------------------

static void player_input(Player *p, uint8_t in) {
  int left  = (in & IN_LEFT)  != 0;
  int right = (in & IN_RIGHT) != 0;

  // Arcade feel: horizontal velocity is SET, not accelerated. Pressing both
  // directions cancels rather than favouring one, so it cannot be exploited.
  if (left && !right)      { p->vx = -P_MOVE; p->facing = 0; }
  else if (right && !left) { p->vx =  P_MOVE; p->facing = 1; }
  else                       p->vx = 0;

  // Jump has no rising-edge requirement: holding it to bunny-hop is the arcade
  // behaviour players expect here.
  if ((in & IN_JUMP) && p->on_ground) {
    p->vy = P_JUMP;
    p->on_ground = 0;
  }

  leg_update(p, (in & IN_KICK) != 0);
}

static void player_integrate(Player *p) {
  p->vy += GRAVITY;
  if (p->vy >  P_MAX_VY) p->vy =  P_MAX_VY;
  if (p->vy < -P_MAX_VY) p->vy = -P_MAX_VY;

  p->x += p->vx;
  p->y += p->vy;
}

// Players are confined to the pitch EDGES, not shut out of the goal mouths:
// standing on your own goal line to defend is core to this game, so the goal
// box is playable space.
static void player_bounds(Player *p) {
  if (p->y >= PLAYER_REST_Y) { p->y = PLAYER_REST_Y; p->vy = 0; p->on_ground = 1; }
  if (p->y < HEAD_R)         { p->y = HEAD_R;        p->vy = 0; }

  fx lo = HEAD_R, hi = FIELD_W - HEAD_R;
  if (p->x < lo) { p->x = lo; if (p->vx < 0) p->vx = 0; }
  if (p->x > hi) { p->x = hi; if (p->vx > 0) p->vx = 0; }

  // They cannot jump THROUGH a crossbar, though. While the head overlaps a goal
  // box horizontally it is capped just under the bar, which also stops a player
  // from perching on top of their own goal.
  if ((p->x - HEAD_R) < GOAL_DEPTH || (p->x + HEAD_R) > RIGHT_GOAL_X) {
    fx cap = CROSSBAR_Y + POST_R + HEAD_R;
    if (p->y < cap) { p->y = cap; if (p->vy < 0) p->vy = 0; }
  }
}

// Heads may not overlap. Full circle separation (not horizontal-only) so one
// player can still jump over the other.
static void players_separate(GameState *s) {
  fx nx, ny, pen;
  if (!hit_circle(s->p[0].x, s->p[0].y, HEAD_R,
                  s->p[1].x, s->p[1].y, HEAD_R, &nx, &ny, &pen)) return;
  fx hx = fx_mul(nx, pen / 2), hy = fx_mul(ny, pen / 2);
  s->p[0].x += hx; s->p[0].y += hy;
  s->p[1].x -= hx; s->p[1].y -= hy;
}

// --- ball ------------------------------------------------------------------

// The ball against one player: the head, then the foot. A foot that is actually
// swinging KICKS - it drives the ball along the contact normal at a speed set
// by the swing. A foot at rest is just a bumper.
static void ball_vs_player(GameState *s, Player *p) {
  fx nx, ny, pen;

  if (hit_circle(s->ball_x, s->ball_y, BALL_R, p->x, p->y, HEAD_R,
                 &nx, &ny, &pen))
    ball_bounce(s, nx, ny, pen, p->vx, p->vy, E_HEAD);

  fx fpx, fpy;
  sim_foot(p, &fpx, &fpy);
  if (hit_circle(s->ball_x, s->ball_y, BALL_R, fpx, fpy, FOOT_R,
                 &nx, &ny, &pen)) {
    fx power = leg_power(p);
    if (power > 0) {
      // A kick overrides the ball's velocity rather than adding to it, so the
      // shot goes where the player aimed regardless of what the ball was doing.
      fx imp = fx_mul(KICK_IMPULSE, power);

      // Direction of travel of the foot: for a leg at angle t swinging
      // forward, the foot's tangential velocity is (dir*cos t, -sin t). Low
      // leg -> forward; mid swing -> forward and up; fully raised -> straight
      // up. Blending that with the contact normal is what makes a scoop work.
      fx cs  = fx_cos_deg(p->leg), sn = fx_sin_deg(p->leg);
      fx tgx = p->facing ? cs : -cs;
      fx tgy = -sn;

      fx bx = fx_mul(nx, KICK_W_NORMAL) + fx_mul(tgx, KICK_W_TANGENT);
      fx by = fx_mul(ny, KICK_W_NORMAL) + fx_mul(tgy, KICK_W_TANGENT);
      fx bl = fx_len(bx, by);
      if (bl > 0) { bx = fx_div(bx, bl); by = fx_div(by, bl); }
      else        { bx = nx; by = ny; }

      s->ball_x += fx_mul(nx, pen);
      s->ball_y += fx_mul(ny, pen);
      s->ball_vx = fx_mul(bx, imp);
      s->ball_vy = fx_mul(by, imp);
      p->leg_vel = 0;          // one contact per swing
    } else {
      ball_bounce(s, nx, ny, pen, p->vx, p->vy, E_HEAD);
    }
  }
}

static void ball_vs_goal_frame(GameState *s) {
  fx nx, ny, pen;
  // Crossbars are boxes; their inner tips are the interesting part and get
  // caught by the same closest-point test.
  if (hit_aabb(s->ball_x, s->ball_y, BALL_R,
               FX_ZERO, CROSSBAR_Y - POST_R, GOAL_DEPTH, CROSSBAR_Y + POST_R,
               &nx, &ny, &pen))
    ball_bounce(s, nx, ny, pen, 0, 0, E_POST);

  if (hit_aabb(s->ball_x, s->ball_y, BALL_R,
               RIGHT_GOAL_X, CROSSBAR_Y - POST_R, FIELD_W, CROSSBAR_Y + POST_R,
               &nx, &ny, &pen))
    ball_bounce(s, nx, ny, pen, 0, 0, E_POST);
}

static void ball_vs_bounds(GameState *s) {
  if (s->ball_y + BALL_R > GROUND_Y) {
    s->ball_y = GROUND_Y - BALL_R;
    if (s->ball_vy > 0) s->ball_vy = -fx_mul(s->ball_vy, E_GROUND);
    s->ball_vx = fx_mul(s->ball_vx, BALL_FRIC_G);
  }
  if (s->ball_y - BALL_R < CEIL_Y) {
    s->ball_y = CEIL_Y + BALL_R;
    if (s->ball_vy < 0) s->ball_vy = -fx_mul(s->ball_vy, E_WALL);
  }

  // Above the crossbar the pitch is walled at the goal's front face; below it
  // the ball is inside the goal recess, where the wall is the back of the net.
  int below = (s->ball_y > CROSSBAR_Y);
  fx  lo    = below ? FX_ZERO : GOAL_DEPTH;
  fx  hi    = below ? FIELD_W : RIGHT_GOAL_X;

  if (s->ball_x - BALL_R < lo) {
    s->ball_x = lo + BALL_R;
    if (s->ball_vx < 0) s->ball_vx = -fx_mul(s->ball_vx, E_WALL);
  }
  if (s->ball_x + BALL_R > hi) {
    s->ball_x = hi - BALL_R;
    if (s->ball_vx > 0) s->ball_vx = -fx_mul(s->ball_vx, E_WALL);
  }
}

// Which seat scored, or -1. Evaluated on the ball CENTRE: with a substep of at
// most BALL_MAX_SPD/2 against a GOAL_DEPTH-deep recess there is no way to cross
// the whole mouth between tests.
static int goal_scored(const GameState *s) {
  if (s->ball_y <= CROSSBAR_Y || s->ball_y >= GROUND_Y) return -1;
  if (s->ball_x < GOAL_DEPTH)   return 1;   // into the LEFT goal: seat 1 scores
  if (s->ball_x > RIGHT_GOAL_X) return 0;   // into the RIGHT goal: seat 0 scores
  return -1;
}

static void ball_step(GameState *s) {
  s->ball_vy += GRAVITY;
  s->ball_vx  = fx_mul(s->ball_vx, BALL_DRAG_X);
  clamp_speed(&s->ball_vx, &s->ball_vy, BALL_MAX_SPD);

  for (int i = 0; i < BALL_SUBSTEPS; i++) {
    s->ball_x += s->ball_vx / BALL_SUBSTEPS;
    s->ball_y += s->ball_vy / BALL_SUBSTEPS;

    ball_vs_goal_frame(s);
    ball_vs_player(s, &s->p[0]);
    ball_vs_player(s, &s->p[1]);
    // Bounds go LAST: they are a hard constraint, and the player/frame
    // responses above apply positional corrections that can shove the ball
    // through the ground or a wall.
    ball_vs_bounds(s);
  }
  clamp_speed(&s->ball_vx, &s->ball_vy, BALL_MAX_SPD);

  // Visual only, but kept IN the state so it can never diverge between peers.
  // vx is capped at BALL_MAX_SPD, so this loops at most once.
  s->ball_spin += fx_mul(s->ball_vx, FXF(2.0));
  while (s->ball_spin >= FXI(360)) s->ball_spin -= FXI(360);
  while (s->ball_spin <  0)        s->ball_spin += FXI(360);
}

// ---------------------------------------------------------------------------

void sim_step(GameState *s, const uint8_t in[2]) {
  // 1. phase timers
  if (s->phase_timer > 0) s->phase_timer--;

  if (s->phase == PH_KICKOFF && s->phase_timer == 0) s->phase = PH_PLAY;
  if (s->phase == PH_GOAL && s->phase_timer == 0) {
    reset_positions(s);
    s->phase = PH_KICKOFF;
    s->phase_timer = KICKOFF_FREEZE;
  }

  if (s->phase == PH_PLAY) {
    // 2 + 3. players
    for (int i = 0; i < 2; i++) {
      player_input(&s->p[i], (uint8_t)(in[i] & IN_MASK));
      player_integrate(&s->p[i]);
      player_bounds(&s->p[i]);
    }
    players_separate(s);
    player_bounds(&s->p[0]);
    player_bounds(&s->p[1]);

    // 4. ball
    ball_step(s);

    // 5. goal
    int scorer = goal_scored(s);
    if (scorer >= 0) {
      if (s->score[scorer] < 255) s->score[scorer]++;
      s->last_scorer = (uint8_t)scorer;
      s->phase = PH_GOAL;
      s->phase_timer = CELEBRATE_FRAMES;
    }

    // 6. clock
    if (s->clock > 0) {
      s->clock--;
      if (s->clock == 0) { s->phase = PH_OVER; s->phase_timer = 0; }
    }
  }

  // 7.
  s->frame++;
}
