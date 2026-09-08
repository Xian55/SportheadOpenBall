// FNV-1a 32. We are detecting our own bugs, not defending against an attacker,
// so a 32-bit non-cryptographic hash is the right tool.
#include "checksum.h"

uint32_t checksum_bytes(const void *p, uint32_t len) {
  const uint8_t *b = (const uint8_t *)p;
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < len; i++) { h ^= b[i]; h *= 16777619u; }
  return h;
}

// Safe to hash the struct wholesale ONLY because sim_init memsets it and _pad
// is explicit - otherwise indeterminate padding would desync two compilers.
uint32_t checksum_state(const GameState *s) {
  return checksum_bytes(s, (uint32_t)sizeof *s);
}
