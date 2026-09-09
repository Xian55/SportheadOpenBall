// Wire format: golden bytes, round-trip, and a decoder fuzz.
//
// The decoder is the one place a hostile or corrupted peer touches this program
// directly, so it gets fuzzed: a malformed datagram must be REJECTED, never
// half-parsed into state. Golden bytes pin the layout so a field reorder cannot
// slip through and silently break compatibility with an older build.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "proto.h"

static int fails = 0;
static void check(int cond, const char *what) {
  if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

static uint32_t rng = 0x1234567u;
static uint32_t xr(void) {           // seeded, so a fuzz failure replays exactly
  rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
  return rng;
}

static void test_golden(void) {
  PktInput p;
  memset(&p, 0, sizeof p);
  p.frame = 0x01020304u;
  for (int i = 0; i < INPUT_HIST; i++) p.hist[i] = (uint8_t)(i & IN_MASK);
  p.ack = 0x0A0B0C0Du;
  p.ck_hash = 0x11223344u;
  p.ck_delta = 0x0042;      // must be < RB_N, or the decoder zeroes it (below)
  p.flags = PKT_FLAG_HIDDEN;

  uint8_t buf[PKT_MAX];
  int n = proto_encode_input(buf, sizeof buf, &p);
  check(n == PKT_INPUT_SIZE, "input packet is PKT_INPUT_SIZE bytes");

  // Little-endian on the wire, byte for byte, regardless of host order.
  check(buf[0] == PKT_INPUT, "type byte");
  check(buf[1] == 0x04 && buf[2] == 0x03 && buf[3] == 0x02 && buf[4] == 0x01,
        "frame is little-endian");
  check(buf[5 + INPUT_HIST] == 0x0D, "ack is little-endian");
  check(buf[15 + INPUT_HIST] == PKT_FLAG_HIDDEN, "flags byte");

  PktInput q;
  check(proto_decode_input(buf, n, &q) == 1, "decode succeeds");
  check(q.frame == p.frame && q.ack == p.ack && q.ck_hash == p.ck_hash &&
        q.ck_delta == p.ck_delta && q.flags == p.flags, "round-trip fields");
  check(memcmp(q.hist, p.hist, INPUT_HIST) == 0, "round-trip history");

  // A checksum older than the history ring cannot be compared against anything
  // still held, so the decoder reports it as absent rather than as a mismatch.
  // Getting this wrong would manufacture desyncs out of stale packets.
  p.ck_delta = (uint16_t)(RB_N + 5);
  n = proto_encode_input(buf, sizeof buf, &p);
  check(proto_decode_input(buf, n, &q) == 1, "out-of-range ck_delta still decodes");
  check(q.ck_delta == 0, "ck_delta beyond the ring is reported as absent");
}

static void test_masking(void) {
  // A peer must not be able to set reserved bits and drive the sim down a path
  // its own simulation never took.
  PktInput p;
  memset(&p, 0, sizeof p);
  for (int i = 0; i < INPUT_HIST; i++) p.hist[i] = 0xFF;
  uint8_t buf[PKT_MAX];
  int n = proto_encode_input(buf, sizeof buf, &p);
  for (int i = 0; i < INPUT_HIST; i++)
    check(buf[5 + i] == (0xFF & IN_MASK), "encode masks to IN_MASK");

  buf[5] = 0xFF;                       // forge one past the encoder
  PktInput q;
  proto_decode_input(buf, n, &q);
  check(q.hist[0] == (0xFF & IN_MASK), "decode masks to IN_MASK");
}

static void test_rejects(void) {
  uint8_t buf[PKT_MAX];
  PktInput p; memset(&p, 0, sizeof p);
  int n = proto_encode_input(buf, sizeof buf, &p);
  PktInput q;

  check(proto_decode_input(buf, n - 1, &q) == 0, "short packet rejected");
  check(proto_decode_input(NULL, n, &q) == 0, "null buffer rejected");
  uint8_t bad[PKT_MAX];
  memcpy(bad, buf, (size_t)n);
  bad[0] = 0x00;
  check(proto_decode_input(bad, n, &q) == 0, "foreign type byte rejected");
  bad[0] = PKT_MAGIC | 0x0F;
  check(proto_decode_input(bad, n, &q) == 0, "unknown type rejected");
  check(proto_type(buf, n) == PKT_INPUT, "proto_type identifies input");
  check(proto_type(bad, 0) == 0, "proto_type rejects empty");

  uint32_t id = 0;
  int hn = proto_encode_hello(buf, sizeof buf, 0xDEADBEEFu);
  check(hn == PKT_HELLO_SIZE, "hello size");
  check(proto_decode_hello(buf, hn, &id) == 1 && id == 0xDEADBEEFu, "hello round-trip");
  buf[1] = (uint8_t)(PROTO_VER + 1);
  check(proto_decode_hello(buf, hn, &id) == 0,
        "hello from a different PROTO_VER is refused, not half-accepted");
}

static void test_fuzz(void) {
  // Random buffers of random lengths through both decoders. Nothing may write
  // out of bounds, and nothing malformed may be accepted.
  uint8_t buf[PKT_MAX + 8];
  PktInput q;
  uint32_t id;
  int accepted = 0;
  for (int iter = 0; iter < 200000; iter++) {
    int len = (int)(xr() % (uint32_t)(PKT_MAX + 4));
    for (int i = 0; i < len && i < (int)sizeof buf; i++) buf[i] = (uint8_t)xr();
    if ((iter & 3) == 0 && len > 0) buf[0] = PKT_INPUT;   // steer some at the parser
    if ((iter & 3) == 1 && len > 0) buf[0] = PKT_HELLO;

    memset(&q, 0, sizeof q);
    if (proto_decode_input(buf, len, &q)) {
      accepted++;
      check(len >= PKT_INPUT_SIZE, "accepted input packet was long enough");
      for (int i = 0; i < INPUT_HIST; i++)
        check((q.hist[i] & ~IN_MASK) == 0, "fuzz: accepted history is masked");
      check(q.ck_delta < RB_N, "fuzz: accepted ck_delta is in range");
    }
    if (proto_decode_hello(buf, len, &id))
      check(len >= PKT_HELLO_SIZE, "accepted hello was long enough");
  }
  printf("  fuzz: 200000 buffers, %d accepted as well-formed\n", accepted);
}

int main(void) {
  test_golden();
  test_masking();
  test_rejects();
  test_fuzz();
  if (fails) { printf("proto_test: %d FAILURES\n", fails); return 1; }
  puts("proto_test OK");
  return 0;
}
