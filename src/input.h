// Turns raylib keyboard (and, via touch.c, on-screen controls) into the 1-byte
// input bitmask the sim consumes. Tier 3: raylib is fine here.
//
// The sim runs at a fixed 60 Hz while rendering runs at the display's rate, so
// sampling is split in two:
//   - MOVEMENT bits are a level: the most recent sample wins.
//   - ACTION bits (jump, kick) are OR-accumulated across every rendered frame
//     since the last tick, then cleared on consume. Without this, a 7 ms tap
//     between two ticks on a 144 Hz monitor is silently dropped.
#ifndef OB_INPUT_H
#define OB_INPUT_H

#include <stdint.h>

void    input_poll(void);            // once per RENDERED frame
uint8_t input_consume(int seat);     // once per SIM tick; clears the action bits
void    input_clear(void);           // drop anything pending (menu transitions)

#endif // OB_INPUT_H
