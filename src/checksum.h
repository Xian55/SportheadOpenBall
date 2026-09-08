// FNV-1a over a GameState's raw bytes, for desync detection. Tier 1.
#ifndef OB_CHECKSUM_H
#define OB_CHECKSUM_H
#include <stdint.h>
#include "sim.h"
uint32_t checksum_state(const GameState *s);
uint32_t checksum_bytes(const void *p, uint32_t len);
#endif // OB_CHECKSUM_H
