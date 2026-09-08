// THE determinism gate.
//
// Runs a fixed input script through the simulation and prints a checksum stream
// plus a full final dump, for diffing against tests/replay_golden.txt. The same
// binary is built to wasm and run under node in CI: that native-vs-wasm diff is
// the only thing that actually PROVES both builds compute bit-identical
// results, which is the assumption the whole rollback design rests on.
//
// Everything else - the integer-only sim, -fwrapv, the no-libm link, the purity
// gate - is prevention. This is detection.
//
// If this diff goes red, do NOT re-bless the golden to make CI green. Find out
// what moved first, and say so in the commit message.
#include <stdio.h>
#include <stdint.h>
#include "sim.h"
#include "checksum.h"

#define FRAMES 3600          // a full 60 s match at 60 Hz

// Deterministic pseudo-random input. Never rand(): its sequence is not
// guaranteed across libcs, which would make this test platform-dependent -
// exactly the thing it exists to detect.
static uint8_t script(uint32_t f, int seat) {
  uint32_t h = f * 2654435761u + (uint32_t)seat * 40503u;
  h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
  // Bias toward holding a direction for a while, so players actually traverse
  // the pitch and reach the ball rather than jittering on the spot.
  uint8_t m = 0;
  if ((h & 7) < 3)        m |= IN_LEFT;
  else if ((h & 7) < 6)   m |= IN_RIGHT;
  if (((h >> 3) & 15) == 0) m |= IN_JUMP;
  if (((h >> 7) & 7)  == 0) m |= IN_KICK;
  return m;
}

static void dump(const GameState *s) {
  printf("frame        %u\n", s->frame);
  printf("rng          %u\n", s->rng);
  for (int i = 0; i < 2; i++) {
    const Player *p = &s->p[i];
    printf("p%d           x=%d y=%d vx=%d vy=%d leg=%d legv=%d ground=%u kick=%u face=%u\n",
           i, (int)p->x, (int)p->y, (int)p->vx, (int)p->vy,
           (int)p->leg, (int)p->leg_vel,
           (unsigned)p->on_ground, (unsigned)p->kick_held, (unsigned)p->facing);
  }
  printf("ball         x=%d y=%d vx=%d vy=%d spin=%d\n",
         (int)s->ball_x, (int)s->ball_y, (int)s->ball_vx, (int)s->ball_vy,
         (int)s->ball_spin);
  printf("match        clock=%u score=%u-%u phase=%u timer=%u scorer=%u\n",
         s->clock, (unsigned)s->score[0], (unsigned)s->score[1],
         (unsigned)s->phase, (unsigned)s->phase_timer, (unsigned)s->last_scorer);
  printf("checksum     %08x\n", checksum_state(s));
}

int main(void) {
  GameState s;
  sim_init(&s, 0x5eed1234u);

  printf("# replay_test: %d frames, seed 0x5eed1234\n", FRAMES);
  printf("# sizeof(GameState)=%d sizeof(Player)=%d\n",
         (int)sizeof(GameState), (int)sizeof(Player));

  for (uint32_t f = 0; f < FRAMES; f++) {
    const uint8_t in[2] = { script(f, 0), script(f, 1) };
    sim_step(&s, in);
    if ((f % 200) == 199) printf("f%-6u %08x\n", f + 1, checksum_state(&s));
  }

  puts("--- final ---");
  dump(&s);
  return 0;
}
