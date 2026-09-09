// Wire packet encode/decode. Tier 1: no sockets, no raylib, no float.
//
// Byte-at-a-time little-endian throughout. A struct memcpy would be shorter and
// would work fine right up until a MinGW peer talked to a wasm one, or someone
// added a field and changed the padding.
#include "proto.h"

static void put_u16(uint8_t *b, uint16_t v) {
  b[0] = (uint8_t)(v & 0xFF);
  b[1] = (uint8_t)((v >> 8) & 0xFF);
}
static void put_u32(uint8_t *b, uint32_t v) {
  b[0] = (uint8_t)(v & 0xFF);
  b[1] = (uint8_t)((v >> 8) & 0xFF);
  b[2] = (uint8_t)((v >> 16) & 0xFF);
  b[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint16_t get_u16(const uint8_t *b) {
  return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}
static uint32_t get_u32(const uint8_t *b) {
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

uint8_t proto_type(const uint8_t *buf, int len) {
  if (!buf || len < 1) return 0;
  if ((buf[0] & 0xF0) != PKT_MAGIC) return 0;
  return buf[0];
}

int proto_encode_input(uint8_t *buf, int cap, const PktInput *p) {
  if (cap < PKT_INPUT_SIZE) return 0;
  buf[0] = PKT_INPUT;
  put_u32(buf + 1, p->frame);
  for (int i = 0; i < INPUT_HIST; i++) buf[5 + i] = (uint8_t)(p->hist[i] & IN_MASK);
  put_u32(buf + 5 + INPUT_HIST, p->ack);
  put_u32(buf + 9 + INPUT_HIST, p->ck_hash);
  put_u16(buf + 13 + INPUT_HIST, p->ck_delta);
  buf[15 + INPUT_HIST] = p->flags;
  return PKT_INPUT_SIZE;
}

int proto_decode_input(const uint8_t *buf, int len, PktInput *out) {
  if (!buf || !out) return 0;
  if (len < PKT_INPUT_SIZE) return 0;
  if (buf[0] != PKT_INPUT) return 0;

  out->frame = get_u32(buf + 1);
  // Mask on the way IN. A peer - or a corrupted datagram - must not be able to
  // set reserved bits and drive the sim down a path the sender never took.
  for (int i = 0; i < INPUT_HIST; i++) out->hist[i] = (uint8_t)(buf[5 + i] & IN_MASK);
  out->ack      = get_u32(buf + 5 + INPUT_HIST);
  out->ck_hash  = get_u32(buf + 9 + INPUT_HIST);
  out->ck_delta = get_u16(buf + 13 + INPUT_HIST);
  out->flags    = buf[15 + INPUT_HIST];

  // A checksum older than the history ring cannot be compared against anything
  // we still hold, so treat it as absent rather than as a mismatch.
  if (out->ck_delta >= RB_N) out->ck_delta = 0;
  return 1;
}

int proto_encode_hello(uint8_t *buf, int cap, uint32_t id_hash) {
  if (cap < PKT_HELLO_SIZE) return 0;
  buf[0] = PKT_HELLO;
  buf[1] = (uint8_t)PROTO_VER;
  put_u32(buf + 2, id_hash);
  buf[6] = 0;
  buf[7] = 0;
  return PKT_HELLO_SIZE;
}

int proto_decode_hello(const uint8_t *buf, int len, uint32_t *id_hash) {
  if (!buf || !id_hash) return 0;
  if (len < PKT_HELLO_SIZE) return 0;
  if (buf[0] != PKT_HELLO) return 0;
  // Refuse a peer on a different protocol version outright. Playing on anyway
  // means two people running different rules, which is worse than not playing.
  if (buf[1] != (uint8_t)PROTO_VER) return 0;
  *id_hash = get_u32(buf + 2);
  return 1;
}
