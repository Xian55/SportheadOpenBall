// Rollback: predict the peer's input, and rewind only when the real input
// contradicts what was already simulated. Tier 1: no raylib, no float, no libm.
//
// The state is 96 bytes, so this snapshots EVERY frame unconditionally. GGPO's
// save/load-callback machinery exists because fighting-game states are megabytes;
// a 128-frame ring here is ~12 KB of static memory, and dropping that complexity
// is what keeps the whole file short enough to reason about.
#include <string.h>
#include "rollback.h"
#include "checksum.h"

#define NO_DIRTY  0xFFFFFFFFu

static uint32_t slot(uint32_t f) { return f & (uint32_t)RB_MASK; }

void rb_start(RbSession *s, int seat, uint32_t seed) {
  memset(s, 0, sizeof *s);
  s->seat  = (uint8_t)(seat & 1);
  s->seed  = seed;
  s->dirty = NO_DIRTY;
  // Tags must not collide with real frame 0, and memset already zeroed them, so
  // mark every slot as holding no frame.
  for (int i = 0; i < RB_N; i++) {
    s->hist_frame[i] = NO_DIRTY;
    s->used_frame[i] = NO_DIRTY;
    s->in_frame[0][i] = NO_DIRTY;
    s->in_frame[1][i] = NO_DIRTY;
  }
  sim_init(&s->live, seed);

  // The first INPUT_DELAY frames have no locally sampled input yet, so their
  // value has to be DEFINED rather than predicted - otherwise each peer would
  // guess for its own seat and the two could diverge before the match starts.
  // Both sides seed the same zeros, deterministically. Those frames sit inside
  // the kickoff freeze anyway, so nothing is lost.
  for (uint32_t f = 0; f < (uint32_t)INPUT_DELAY; f++) {
    rb_store(s, 0, f, 0);
    rb_store(s, 1, f, 0);
  }
}

int rb_has(const RbSession *s, int seat, uint32_t f) {
  return s->in_frame[seat & 1][slot(f)] == f;
}

uint8_t rb_input(const RbSession *s, int seat, uint32_t f) {
  if (rb_has(s, seat, f)) return s->in[seat & 1][slot(f)];
  // Prediction: the peer is most likely still doing whatever it was last doing.
  // In a two-button game that is right the large majority of frames.
  return s->last_remote_in;
}

void rb_store(RbSession *s, int seat, uint32_t f, uint8_t mask) {
  int se = seat & 1;
  s->in[se][slot(f)]       = (uint8_t)(mask & IN_MASK);
  s->in_frame[se][slot(f)] = f;
}

static void rb_advance_confirmed(RbSession *s) {
  while (s->confirmed < s->frame &&
         rb_has(s, 0, s->confirmed) && rb_has(s, 1, s->confirmed)) {
    s->confirmed++;
  }
}

// Snapshot the state and the inputs about to be used, then step once.
static void rb_step(RbSession *s, uint32_t f) {
  uint32_t k = slot(f);
  uint8_t in[2] = { rb_input(s, 0, f), rb_input(s, 1, f) };

  s->hist[k]       = s->live;
  s->hist_frame[k] = f;
  s->hash[k]       = checksum_state(&s->live);
  s->used[0][k]    = in[0];
  s->used[1][k]    = in[1];
  s->used_frame[k] = f;

  sim_step(&s->live, in);
}

int rb_should_stall(RbSession *s) {
  // A peer that has genuinely gone away must not freeze us forever; after a few
  // stalled ticks, run anyway and let prediction carry it.
  if (s->stall_run >= 6) { s->stall_run = 0; return 0; }

  // Hard limit: never predict further ahead than the ring and the design allow.
  if (s->frame > s->confirmed + (uint32_t)MAX_ROLLBACK) {
    s->stall_run++; s->stalls++; return 1;
  }
  // Soft limit: keep the two clocks married, so neither side banks a large
  // advantage that would later have to be given back in one lurch.
  int32_t adv = (int32_t)s->frame - (int32_t)(s->remote_frame + 1u);
  if (adv > FRAME_ADV_LIMIT) { s->stall_run++; s->stalls++; return 1; }

  s->stall_run = 0;
  return 0;
}

// Rewind ONLY if a received input actually contradicted what was simulated.
// Merely receiving an input that was already predicted correctly is not a
// rollback, and skipping those removes the large majority of them.
void rb_catchup(RbSession *s) {
  if (s->dirty == NO_DIRTY || s->dirty >= s->frame) { s->dirty = NO_DIRTY; return; }

  uint32_t start = s->dirty;
  if (s->hist_frame[slot(start)] == start) {
    s->live = s->hist[slot(start)];
    for (uint32_t r = start; r < s->frame; r++) rb_step(s, r);
    s->rollbacks++;
    s->resim_frames += (s->frame - start);
  }
  // If the snapshot has aged out of the ring there is nothing to rewind to.
  // That means the peer fell further behind than RB_N frames, which the stall
  // logic exists to prevent.
  s->dirty = NO_DIRTY;
}

// Compare the peer's checksum only when our own copy of that frame can no
// longer change: it is below `confirmed` (so both sides fed it real inputs) and
// no rewind is pending. A mismatch then can only mean the simulations genuinely
// diverged.
static void rb_verify_checksum(RbSession *s) {
  if (!s->peer_ck_valid || s->desync) return;
  if (s->dirty != NO_DIRTY) return;                  // correction still pending
  uint32_t ck = s->peer_ck_frame;
  if (ck >= s->confirmed) return;                    // not settled here yet
  if (s->hist_frame[slot(ck)] != ck) { s->peer_ck_valid = 0; return; }  // aged out

  uint32_t mine = s->hash[slot(ck)];
  s->peer_ck_valid = 0;
  if (mine != s->peer_ck_hash) {
    // Latch and stop. Continuing means two people playing different games, and
    // it destroys any chance of reproducing the divergence.
    s->desync        = 1;
    s->desync_frame  = ck;
    s->desync_mine   = mine;
    s->desync_theirs = s->peer_ck_hash;
  }
}

void rb_tick(RbSession *s, uint8_t local_in) {
  uint32_t f = s->frame;

  // Local input is scheduled INPUT_DELAY frames out. Those frames are always
  // real for this seat, which is what buys the peer time to deliver theirs.
  rb_store(s, s->seat, f + (uint32_t)INPUT_DELAY, local_in);

  rb_catchup(s);

  rb_step(s, f);
  s->frame = f + 1;
  rb_advance_confirmed(s);
  rb_verify_checksum(s);
}

// A received input for a frame already simulated only forces a rewind when it
// differs from what was fed at the time.
static void rb_merge(RbSession *s, int seat, uint32_t f, uint8_t mask) {
  mask = (uint8_t)(mask & IN_MASK);

  if (f < s->confirmed) return;                 // already settled
  if (f + (uint32_t)RB_N <= s->frame) return;   // older than the ring holds
  if (rb_has(s, seat, f)) return;               // duplicate or reorder: harmless

  rb_store(s, seat, f, mask);
  s->last_remote_in = mask;

  if (f < s->frame && s->used_frame[slot(f)] == f &&
      s->used[seat & 1][slot(f)] != mask) {
    if (s->dirty == NO_DIRTY || f < s->dirty) s->dirty = f;
  }
}

int rb_build_packet(RbSession *s, uint8_t *buf, int cap) {
  PktInput p;
  memset(&p, 0, sizeof p);

  // Newest local input we hold. Every packet repeats the last INPUT_HIST frames,
  // so a dropped datagram costs nothing: the next one refills the gap. That
  // redundancy is why the channel can be unreliable and unordered.
  uint32_t newest = s->frame + (uint32_t)INPUT_DELAY - 1u;
  p.frame = newest;
  for (int i = 0; i < INPUT_HIST; i++) {
    uint32_t f = newest - (uint32_t)(INPUT_HIST - 1 - i);
    p.hist[i] = rb_has(s, s->seat, f) ? s->in[s->seat][slot(f)] : 0;
  }
  p.ack = s->confirmed;

  // Checksum a frame both peers have real inputs for, so a mismatch can only
  // mean the simulations diverged - never that one side merely predicted.
  if (s->confirmed > 0) {
    uint32_t ck = s->confirmed - 1u;
    uint32_t d  = newest - ck;
    if (d > 0 && d < (uint32_t)RB_N && s->hist_frame[slot(ck)] == ck) {
      p.ck_delta = (uint16_t)d;
      p.ck_hash  = s->hash[slot(ck)];
    }
  }
  if (s->desync) p.flags |= PKT_FLAG_DESYNC;

  int n = proto_encode_input(buf, cap, &p);
  if (n > 0) s->packets_out++;
  return n;
}

void rb_on_packet(RbSession *s, const uint8_t *buf, int len) {
  PktInput p;
  if (!proto_decode_input(buf, len, &p)) return;
  s->packets_in++;

  int peer = (s->seat ^ 1);

  for (int i = 0; i < INPUT_HIST; i++) {
    uint32_t f = p.frame - (uint32_t)(INPUT_HIST - 1 - i);
    if (f > p.frame) continue;              // wrapped below zero early in a match
    rb_merge(s, peer, f, p.hist[i]);
  }

  if (p.frame + 1u > s->remote_frame) s->remote_frame = p.frame;
  s->peer_flags = p.flags;

  if (p.ck_delta && !s->desync) {
    // Stash it. Checking here would compare against a frame this peer may still
    // be predicting, or one with an unapplied correction queued in `dirty` -
    // both produce a different hash for entirely legitimate reasons. Verified
    // later, once our own copy of that frame is settled.
    uint32_t ck = p.frame - p.ck_delta;
    if (!s->peer_ck_valid || ck > s->peer_ck_frame) {
      s->peer_ck_frame = ck;
      s->peer_ck_hash  = p.ck_hash;
      s->peer_ck_valid = 1;
    }
  }

  rb_advance_confirmed(s);
}
