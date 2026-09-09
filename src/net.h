// Unreliable datagram channel to exactly one peer. Tier 2: no raylib.
//
// Two implementations satisfy this, selected in CMake: net_udp.c natively and
// net_web.c on wasm (M4). Keeping the interface this small is what lets the
// rollback layer above it be transport-agnostic.
//
// Opening is ALWAYS asynchronous. A blocking connect would freeze the frame
// loop, and on the web there is no way to block at all without ASYNCIFY, which
// this project deliberately does not use.
#ifndef OB_NET_H
#define OB_NET_H

#include <stdint.h>

typedef enum {
  NET_IDLE = 0,
  NET_CONNECTING,
  NET_OPEN,
  NET_FAILED
} NetState;

// room is advisory: the UDP transport uses OB_PEER/OB_PORT when set and derives
// a port pair from the room code otherwise. Never blocks; poll net_state().
int      net_open(const char *room);

void     net_pump(void);                    // service once per frame
int      net_poll(uint8_t *buf, int cap);   // -1 = nothing pending, else length
void     net_send(const void *buf, int len);
NetState net_state(void);
void     net_close(void);

// -1 until the handshake settles, then 0 or 1. Seats are decided by comparing
// peer identifiers, so there is no host/join role and two hosts are impossible.
int         net_seat(void);
const char *net_last_error(void);

// Room code carried in the page URL (?room=CODE), for invite links. Returns 1
// and fills `out` when one is present. Only the web transport can have one; the
// others report none, so main.c needs no platform test.
int         net_url_room(char *out, int cap);

// Is this client backgrounded? A browser throttles a hidden tab's animation
// frames to about 1 Hz, which starves the peer. Reported to the other side so it
// can say "opponent tabbed out" instead of appearing to freeze for no reason.
int         net_local_hidden(void);

#endif // OB_NET_H
