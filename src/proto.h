// Wire packets. Tier 1: pure byte functions, no sockets, no raylib, no float.
//
// Explicit little-endian on both encode and decode, never a struct memcpy: the
// two peers may be a MinGW native build and a wasm one, and relying on layout
// or host byte order would be exactly the kind of assumption this project keeps
// proving wrong.
#ifndef OB_PROTO_H
#define OB_PROTO_H

#include <stdint.h>
#include "config.h"

// High nibble is magic+version so a stray datagram, or a peer on a different
// PROTO_VER, is rejected rather than half-parsed into a desync.
#define PKT_MAGIC   0xB0
#define PKT_INPUT   (PKT_MAGIC | 0x01)
#define PKT_HELLO   (PKT_MAGIC | 0x02)
#define PKT_BYE     (PKT_MAGIC | 0x03)

#define PKT_INPUT_SIZE  24
#define PKT_HELLO_SIZE  8
#define PKT_MAX         64

typedef struct PktInput {
  uint32_t frame;                 // frame of hist[INPUT_HIST-1], the NEWEST input
  uint8_t  hist[INPUT_HIST];      // oldest first: frame-(N-1) .. frame
  uint32_t ack;                   // sender's highest contiguous confirmed frame
  uint32_t ck_hash;               // state checksum at ck_frame
  uint16_t ck_delta;              // frame - ck_frame (0 = no checksum enclosed)
  uint8_t  flags;                 // bit0 hidden/backgrounded, bit1 desync detected
} PktInput;

#define PKT_FLAG_HIDDEN  0x01
#define PKT_FLAG_DESYNC  0x02

// Encode returns bytes written; decode returns 1 on success, 0 on a malformed
// or foreign packet. Decode NEVER trusts the buffer: every field is bounds
// checked and inputs are masked to IN_MASK so a peer cannot inject unknown bits.
int proto_encode_input(uint8_t *buf, int cap, const PktInput *p);
int proto_decode_input(const uint8_t *buf, int len, PktInput *out);

int proto_encode_hello(uint8_t *buf, int cap, uint32_t id_hash);
int proto_decode_hello(const uint8_t *buf, int len, uint32_t *id_hash);

// Packet type, or 0 if the buffer is not one of ours.
uint8_t proto_type(const uint8_t *buf, int len);

#endif // OB_PROTO_H
