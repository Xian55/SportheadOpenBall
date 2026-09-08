// On-screen touch controls. Tier 3.
//
// These feed the SAME 1-byte mask as the keyboard, so the simulation and the
// netcode never learn touch exists - that is the payoff of making the input a
// bitmask rather than a struct of sources.
#ifndef OB_TOUCH_H
#define OB_TOUCH_H

#include <stdint.h>

void    touch_init(void);
void    touch_update(void);   // once per rendered frame, before input_poll()
uint8_t touch_mask(void);     // bits currently held on the overlay
int     touch_active(void);   // has this device ever produced a touch?
void    touch_draw(void);     // called from the renderer, in virtual space

#endif // OB_TOUCH_H
