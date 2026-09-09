// Rollback session: input/state rings, prediction, rewind and resim.
// Tier 1: no raylib, no float, no libm, no sockets. Calls sim_step, nothing else.
#ifndef OB_ROLLBACK_H
#define OB_ROLLBACK_H

#include <stdint.h>
#include "sim.h"
#include "proto.h"

// Slots are tagged with the frame they hold rather than tracked with a
// occupancy bitset. The ring wraps every RB_N frames, and a bitset would happily
// report a stale frame as present the moment a slot was reused - a bug that
// would surface as a rare, unreproducible desync.
typedef struct RbSession {
  GameState live;                  // state BEFORE `frame` is stepped
  GameState hist[RB_N];
  uint32_t  hist_frame[RB_N];
  uint32_t  hash[RB_N];

  uint8_t   in[2][RB_N];           // real inputs, as received
  uint32_t  in_frame[2][RB_N];
  uint8_t   used[2][RB_N];         // what was actually fed to sim_step
  uint32_t  used_frame[RB_N];

  uint32_t  frame;                 // next frame to simulate
  uint32_t  confirmed;             // every frame BELOW this has real inputs from both
  uint32_t  remote_frame;          // newest frame the peer reports reaching
  uint32_t  dirty;                 // lowest frame whose remote input changed; UINT32_MAX = clean
  uint8_t   last_remote_in;        // prediction source: repeat the peer's last known input
  uint8_t   seat;
  uint8_t   peer_flags;
  uint8_t   local_hidden;          // set by the caller; travels in the packet flags

  uint8_t   desync;                // latched; v1 policy is stop loudly
  uint32_t  desync_frame, desync_mine, desync_theirs;

  // The peer's most recent checksum, held until OUR copy of that frame is
  // settled. Comparing on arrival gives false positives: at that moment the
  // frame may still be predicted here, or a correction for it may be sitting in
  // `dirty` unapplied, and either makes a legitimately different hash.
  uint32_t  peer_ck_frame, peer_ck_hash;
  uint8_t   peer_ck_valid;

  int       stall_run;             // consecutive stalls, so a dead peer cannot deadlock us
  uint32_t  seed;

  // diagnostics, not part of the simulation
  uint32_t  rollbacks, resim_frames, stalls, packets_in, packets_out;
} RbSession;

void    rb_start(RbSession *s, int seat, uint32_t seed);

int     rb_has(const RbSession *s, int seat, uint32_t f);
uint8_t rb_input(const RbSession *s, int seat, uint32_t f);   // real, or predicted
void    rb_store(RbSession *s, int seat, uint32_t f, uint8_t mask);

// True when this peer must NOT advance: it has predicted as far as it is allowed
// to, or has run too far ahead of the other side's clock.
int     rb_should_stall(RbSession *s);

// Apply any pending rewind WITHOUT advancing. rb_tick calls this first; a
// caller that has stopped ticking (match over, or paused) needs it too, or a
// correction that arrives after the last tick is never applied.
void    rb_catchup(RbSession *s);

// One 60 Hz tick: stores local input, rewinds and resimulates if a received
// input contradicted a prediction, then advances exactly one frame.
void    rb_tick(RbSession *s, uint8_t local_in);

int     rb_build_packet(RbSession *s, uint8_t *buf, int cap);
void    rb_on_packet(RbSession *s, const uint8_t *buf, int len);

#endif // OB_ROLLBACK_H
