// WebRTC transport: thin C glue over src/net_web.js. Tier 2: no raylib.
//
// Everything interesting lives in the js-library shim; this file only satisfies
// net.h so that main.c and the rollback layer above it are identical on both
// platforms. Swapping net_null.c for this file is the whole of M4 from their
// point of view - which is the payoff for keeping the interface to six calls.
#include <string.h>
#include "net.h"

extern void ob_net_open(const char *room);
extern void ob_net_send(const void *p, int len);
extern int  ob_net_poll(void *p, int cap);
extern int  ob_net_state(void);
extern int  ob_net_seat(void);
extern void ob_net_error(char *p, int cap);
extern void ob_net_close(void);
extern void ob_net_url_room(char *p, int cap);
extern int  ob_net_hidden(void);

static char g_err[128];

int net_open(const char *room) {
  ob_net_open(room ? room : "");
  return 1;                      // async: the caller polls net_state()
}

void net_pump(void) {
  // Nothing to do: the browser event loop delivers into the JS-side queue.
}

int net_poll(uint8_t *buf, int cap) { return ob_net_poll(buf, cap); }
void net_send(const void *buf, int len) { ob_net_send(buf, len); }
NetState net_state(void) { return (NetState)ob_net_state(); }
int net_seat(void) { return ob_net_seat(); }
void net_close(void) { ob_net_close(); }

const char *net_last_error(void) {
  ob_net_error(g_err, (int)sizeof g_err);
  if (!g_err[0]) {
    // Never leave the player looking at a blank status while nothing happens.
    return "connecting through public relays...";
  }
  return g_err;
}

int net_local_hidden(void) { return ob_net_hidden(); }

int net_url_room(char *out, int cap) {
  if (!out || cap <= 0) return 0;
  out[0] = 0;
  ob_net_url_room(out, cap);
  return out[0] ? 1 : 0;
}
