// THE netcode gate: rollback must be INDISTINGUISHABLE from lockstep.
//
// Runs a fixed input script three ways - once lockstep with no network at all,
// and once through two rollback sessions talking over a lossy, jittery, delayed
// link - and requires the final GameState to be byte-identical. If prediction,
// rewind or resimulation is wrong anywhere, the states diverge and this fails.
//
// Everything runs on a VIRTUAL clock. No sleeps, no wall-clock, no threads: the
// whole lag x loss x seed matrix finishes in well under a second, and any
// failure reproduces exactly from its (lag, jitter, loss, seed) tuple. A test
// that needed real time here would be slow AND flaky, and a flaky determinism
// gate is worse than none - it trains you to re-run instead of investigate.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sim.h"
#include "checksum.h"
#include "rollback.h"

#define FRAMES 1800          // 30 s at 60 Hz
#define SIM_SEED 0x5eed1234u

// Deterministic script. Never rand(): its sequence is not guaranteed across
// libcs, which would make this test platform-dependent - exactly what it exists
// to rule out.
static uint8_t script(uint32_t f, int seat) {
  // The first INPUT_DELAY frames are defined as zero on both sides (see
  // rb_start), so the reference must agree or the two can never match.
  if (f < (uint32_t)INPUT_DELAY) return 0;
  uint32_t h = f * 2654435761u + (uint32_t)seat * 40503u;
  h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
  uint8_t m = 0;
  if ((h & 7) < 3)          m |= IN_LEFT;
  else if ((h & 7) < 6)     m |= IN_RIGHT;
  if (((h >> 3) & 15) == 0) m |= IN_JUMP;
  if (((h >> 7) & 7)  == 0) m |= IN_KICK;
  return m;
}

// --- a deliberately nasty link ----------------------------------------------
#define LINKQ 1024
typedef struct { uint32_t due; int len; uint8_t buf[PKT_MAX]; } Msg;

typedef struct {
  Msg      q[LINKQ];
  int      n;
  int      lag, jitter, loss;   // frames, frames, percent
  uint32_t rng;
} Link;

static uint32_t xr(Link *l) {          // xorshift32: seeded, so failures replay
  uint32_t x = l->rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return (l->rng = x);
}

static void link_init(Link *l, int lag, int jitter, int loss, uint32_t seed) {
  memset(l, 0, sizeof *l);
  l->lag = lag; l->jitter = jitter; l->loss = loss;
  l->rng = seed ? seed : 1u;
}

static void link_send(Link *l, uint32_t t, const uint8_t *buf, int len) {
  if (len <= 0 || len > PKT_MAX) return;
  if (l->loss > 0 && (int)(xr(l) % 100u) < l->loss) return;      // dropped
  if (l->n >= LINKQ) return;                                     // queue full
  int d = l->lag;
  if (l->jitter > 0) d += (int)(xr(l) % (uint32_t)(2 * l->jitter + 1)) - l->jitter;
  if (d < 0) d = 0;
  Msg *m = &l->q[l->n++];
  m->due = t + (uint32_t)d;
  m->len = len;
  memcpy(m->buf, buf, (size_t)len);
}

// Delivers everything due, in queue order. Reordering is therefore possible and
// intended: absolute frame indexing means out-of-order arrival needs no special
// handling, and this is what proves it.
static int link_recv(Link *l, uint32_t t, uint8_t *buf, int *len) {
  for (int i = 0; i < l->n; i++) {
    if (l->q[i].due <= t) {
      *len = l->q[i].len;
      memcpy(buf, l->q[i].buf, (size_t)l->q[i].len);
      l->q[i] = l->q[--l->n];
      return 1;
    }
  }
  return 0;
}

static void peer_tick(RbSession *s, Link *inbox, Link *outbox, uint32_t t) {
  uint8_t buf[PKT_MAX]; int len;
  while (link_recv(inbox, t, buf, &len)) rb_on_packet(s, buf, len);

  if (s->frame >= FRAMES) return;
  if (rb_should_stall(s)) return;

  // The input sampled NOW applies INPUT_DELAY frames from now, so the reference
  // must see script(f) at frame f - hence the offset here.
  rb_tick(s, script(s->frame + (uint32_t)INPUT_DELAY, s->seat));

  int n = rb_build_packet(s, buf, sizeof buf);
  if (n > 0) link_send(outbox, t, buf, n);
}

static int run_case(int lag, int jitter, int loss, uint32_t seed, int verbose) {
  // 1. the reference: no network, no prediction, no rollback.
  GameState ref;
  sim_init(&ref, SIM_SEED);
  for (uint32_t f = 0; f < FRAMES; f++) {
    const uint8_t in[2] = { script(f, 0), script(f, 1) };
    sim_step(&ref, in);
  }

  // 2. the same match over a hostile link.
  RbSession a, b;
  rb_start(&a, 0, SIM_SEED);
  rb_start(&b, 1, SIM_SEED);

  Link a2b, b2a;
  link_init(&a2b, lag, jitter, loss, seed);
  link_init(&b2a, lag, jitter, loss, seed ^ 0x5bf03635u);

  uint32_t t = 0;
  const uint32_t limit = FRAMES * 8u;         // generous; livelock guard below
  while ((a.frame < FRAMES || b.frame < FRAMES) && t < limit) {
    peer_tick(&a, &b2a, &a2b, t);
    peer_tick(&b, &a2b, &b2a, t);
    t++;
  }
  if (t >= limit) {
    printf("FAIL lag=%d jit=%d loss=%d seed=%u: livelock, a=%u b=%u\n",
           lag, jitter, loss, seed, a.frame, b.frame);
    return 1;
  }

  // 3. drain the link and let any late correction land. Without this a packet
  //    still in flight at the final tick would never be applied, and the last
  //    few frames could legitimately differ.
  for (int guard = 0; guard < 4096; guard++) {
    uint8_t buf[PKT_MAX]; int len; int moved = 0;
    while (link_recv(&b2a, t, buf, &len)) { rb_on_packet(&a, buf, len); moved = 1; }
    while (link_recv(&a2b, t, buf, &len)) { rb_on_packet(&b, buf, len); moved = 1; }
    rb_catchup(&a);
    rb_catchup(&b);
    if (!moved && a2b.n == 0 && b2a.n == 0) break;
    t++;
  }

  int bad = 0;
  if (memcmp(&a.live, &ref, sizeof ref) != 0) {
    printf("FAIL lag=%d jit=%d loss=%d seed=%u: peer A != lockstep (%08x vs %08x)\n",
           lag, jitter, loss, seed, checksum_state(&a.live), checksum_state(&ref));
    bad = 1;
  }
  if (memcmp(&b.live, &ref, sizeof ref) != 0) {
    printf("FAIL lag=%d jit=%d loss=%d seed=%u: peer B != lockstep (%08x vs %08x)\n",
           lag, jitter, loss, seed, checksum_state(&b.live), checksum_state(&ref));
    bad = 1;
  }
  if (a.desync || b.desync) {
    printf("FAIL lag=%d jit=%d loss=%d seed=%u: desync flag raised (a=%u b=%u frame=%u)\n",
           lag, jitter, loss, seed, a.desync, b.desync, a.desync_frame);
    bad = 1;
  }
  if (verbose && !bad) {
    printf("  lag=%3d jit=%2d loss=%2d%%  rollbacks a=%-5u b=%-5u  resim a=%-6u  stalls a=%-4u  pkts %u/%u\n",
           lag, jitter, loss, a.rollbacks, b.rollbacks, a.resim_frames,
           a.stalls, a.packets_in, a.packets_out);
  }
  return bad;
}

int main(void) {
  int lags[]   = { 0, 1, 3, 6, 9 };        // frames one-way: 0..150 ms
  int losses[] = { 0, 5, 10, 20 };
  int fails = 0, cases = 0;

  printf("rollback_test: %d frames per case, virtual clock\n", FRAMES);

  // A readable sample first, then the full matrix.
  for (unsigned i = 0; i < sizeof lags / sizeof lags[0]; i++)
    fails += run_case(lags[i], lags[i] / 3, 5, 7919u, 1), cases++;

  for (unsigned i = 0; i < sizeof lags / sizeof lags[0]; i++)
    for (unsigned j = 0; j < sizeof losses / sizeof losses[0]; j++)
      for (uint32_t s = 1; s <= 4; s++) {
        fails += run_case(lags[i], lags[i] / 3, losses[j], s * 7919u, 0);
        cases++;
      }

  if (fails) { printf("rollback_test: %d/%d FAILED\n", fails, cases); return 1; }
  printf("rollback_test OK - %d cases, rollback is byte-identical to lockstep\n", cases);
  return 0;
}
