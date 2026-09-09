// Placeholder transport for builds with no networking yet - currently wasm,
// until net_web.c lands in M4. Tier 2: no raylib.
//
// This exists so main.c can be written ONCE against net.h rather than sprinkled
// with #ifdef __EMSCRIPTEN__. It reports NET_FAILED with an honest message
// instead of pretending to connect, so a player who reaches for multiplayer on
// the web is told plainly rather than left watching a spinner.
//
// Hotseat is entirely unaffected: the game only calls net_open when OB_PEER or
// OB_ROOM asks for a networked match.
#include "net.h"

static NetState g_state = NET_IDLE;

int net_open(const char *room) {
  (void)room;
  g_state = NET_FAILED;
  return 0;
}

void        net_pump(void) { }
int         net_poll(uint8_t *buf, int cap) { (void)buf; (void)cap; return -1; }
void        net_send(const void *buf, int len) { (void)buf; (void)len; }
NetState    net_state(void) { return g_state; }
int         net_seat(void) { return -1; }
void        net_close(void) { g_state = NET_IDLE; }

const char *net_last_error(void) {
  return "online play is not in the web build yet - use two native clients";
}
