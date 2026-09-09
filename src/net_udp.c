// Native UDP transport. Tier 2: no raylib.
//
// This exists to make rollback DEBUGGABLE. Two local processes over loopback
// give a real socket - genuine reordering, duplication and MTU behaviour - plus
// a debugger, printf, and reproducible latency. Two browser tabs give none of
// that. It also doubles as the LAN transport.
//
// Delay, jitter and loss are injected on the RECEIVE side so each process is
// configured independently and neither needs to know the other's settings:
//   OB_PORT=41001            local bind port
//   OB_PEER=127.0.0.1:41002  peer address
//   OB_LAG_MS=80             one-way delay applied on receive
//   OB_JITTER_MS=25          uniform +/- jitter
//   OB_LOSS=8                percent, integer
//   OB_NETSEED=1234          xorshift seed, so a bad case replays EXACTLY
//
// The RNG is a seeded xorshift, never rand(): rand()'s sequence is not
// guaranteed across libcs, and a network fault you cannot reproduce is a network
// fault you cannot fix.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "proto.h"

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int socklen_t;
  #define OB_CLOSESOCK closesocket
  #define OB_SOCK SOCKET
  #define OB_BAD_SOCK INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <time.h>
  #define OB_CLOSESOCK close
  #define OB_SOCK int
  #define OB_BAD_SOCK (-1)
#endif

#define QMAX 256

typedef struct { uint64_t due_ms; int len; uint8_t buf[PKT_MAX]; } QMsg;

static OB_SOCK  g_sock = OB_BAD_SOCK;
static struct sockaddr_in g_peer;
static int      g_have_peer;
static NetState g_state = NET_IDLE;
static char     g_err[128];

static QMsg     g_q[QMAX];
static int      g_qn;

static int      g_lag, g_jitter, g_loss;
static uint32_t g_rng = 1;

static uint32_t g_my_id, g_peer_id;
static int      g_seat = -1;
static uint64_t g_last_hello;
static uint64_t g_last_reply;

static uint32_t xr(void) {
  uint32_t x = g_rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return (g_rng = x);
}

static uint64_t now_ms(void) {
#if defined(_WIN32)
  return (uint64_t)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static int env_i(const char *name, int dflt) {
  const char *v = getenv(name);
  if (!v || !*v) return dflt;
  return atoi(v);
}

// FNV-1a, so the id is a stable function of the port rather than of clock or
// address ordering. Seats must be decided the same way by both peers.
static uint32_t hash32(const void *p, int n) {
  const uint8_t *b = (const uint8_t *)p;
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
  return h;
}

static void set_nonblocking(OB_SOCK s) {
#if defined(_WIN32)
  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);
#else
  int fl = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

static void fail(const char *msg) {
  snprintf(g_err, sizeof g_err, "%s", msg);
  g_state = NET_FAILED;
}

int net_open(const char *room) {
  g_state = NET_CONNECTING;
  g_err[0] = 0;
  g_qn = 0;
  g_seat = -1;
  g_have_peer = 0;
  g_peer_id = 0;

  g_lag    = env_i("OB_LAG_MS", 0);
  g_jitter = env_i("OB_JITTER_MS", 0);
  g_loss   = env_i("OB_LOSS", 0);
  g_rng    = (uint32_t)env_i("OB_NETSEED", 1);
  if (!g_rng) g_rng = 1;

#if defined(_WIN32)
  WSADATA wsa;
  static int wsa_up = 0;
  if (!wsa_up) {
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fail("WSAStartup failed"); return 0; }
    wsa_up = 1;
  }
#endif

  int port = env_i("OB_PORT", 0);
  if (port == 0 && room && *room) {
    // No explicit port: derive a stable pair from the room code so two local
    // instances with the same code find each other with no configuration.
    port = 41000 + (int)(hash32(room, (int)strlen(room)) % 500u) * 2;
  }
  if (port == 0) port = 41001;

  g_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (g_sock == OB_BAD_SOCK) { fail("socket() failed"); return 0; }

  struct sockaddr_in me;
  memset(&me, 0, sizeof me);
  me.sin_family = AF_INET;
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  me.sin_port = htons((unsigned short)port);
  if (bind(g_sock, (struct sockaddr *)&me, sizeof me) != 0) {
    OB_CLOSESOCK(g_sock); g_sock = OB_BAD_SOCK;
    snprintf(g_err, sizeof g_err, "bind to port %d failed (already in use?)", port);
    g_state = NET_FAILED;
    return 0;
  }
  set_nonblocking(g_sock);

  // Identity is a function of the bound port, so both sides compute the same
  // ordering and the seat assignment cannot disagree.
  unsigned short pp = (unsigned short)port;
  g_my_id = hash32(&pp, (int)sizeof pp);

  const char *peer = getenv("OB_PEER");
  memset(&g_peer, 0, sizeof g_peer);
  g_peer.sin_family = AF_INET;
  if (peer && *peer) {
    char host[64] = {0};
    int pport = 0;
    const char *colon = strrchr(peer, ':');
    if (!colon) { fail("OB_PEER must be host:port"); return 0; }
    int hl = (int)(colon - peer);
    if (hl <= 0 || hl >= (int)sizeof host) { fail("OB_PEER host too long"); return 0; }
    memcpy(host, peer, (size_t)hl);
    pport = atoi(colon + 1);
    if (pport <= 0) { fail("OB_PEER port invalid"); return 0; }
    g_peer.sin_port = htons((unsigned short)pport);
    if (inet_pton(AF_INET, host, &g_peer.sin_addr) != 1) {
      fail("OB_PEER host is not a dotted IPv4 address"); return 0;
    }
    g_have_peer = 1;
  } else {
    // Same-machine default: the other half of the derived port pair.
    g_peer.sin_port = htons((unsigned short)(port ^ 1));
    inet_pton(AF_INET, "127.0.0.1", &g_peer.sin_addr);
    g_have_peer = 1;
  }

  g_last_hello = 0;
  return 1;
}

static void send_raw(const void *buf, int len) {
  if (g_sock == OB_BAD_SOCK || !g_have_peer) return;
  sendto(g_sock, (const char *)buf, len, 0,
         (struct sockaddr *)&g_peer, (socklen_t)sizeof g_peer);
}

// Queue an arriving packet for later delivery, applying this side's configured
// delay, jitter and loss.
static void enqueue(const uint8_t *buf, int len) {
  if (len <= 0 || len > PKT_MAX) return;
  if (g_loss > 0 && (int)(xr() % 100u) < g_loss) return;
  if (g_qn >= QMAX) return;
  int d = g_lag;
  if (g_jitter > 0) d += (int)(xr() % (uint32_t)(2 * g_jitter + 1)) - g_jitter;
  if (d < 0) d = 0;
  QMsg *m = &g_q[g_qn++];
  m->due_ms = now_ms() + (uint64_t)d;
  m->len = len;
  memcpy(m->buf, buf, (size_t)len);
}

void net_pump(void) {
  if (g_sock == OB_BAD_SOCK) return;

  uint8_t buf[PKT_MAX];
  for (;;) {
    struct sockaddr_in from;
    socklen_t fl = (socklen_t)sizeof from;
    int n = (int)recvfrom(g_sock, (char *)buf, (int)sizeof buf, 0,
                          (struct sockaddr *)&from, &fl);
    if (n <= 0) break;

    uint8_t t = proto_type(buf, n);
    if (t == PKT_HELLO) {
      uint32_t id = 0;
      if (proto_decode_hello(buf, n, &id)) {
        // ALWAYS answer a HELLO, whatever our own state. Replying only while
        // still CONNECTING means the first peer to finish stops announcing, and
        // the other one - whose HELLOs we already consumed - waits forever. A
        // simultaneous start hides this completely, which is why it survived the
        // headless test and only showed up with two hand-launched windows.
        uint64_t now = now_ms();
        if (now - g_last_reply > 40) {
          g_last_reply = now;
          uint8_t h[PKT_HELLO_SIZE];
          int hn = proto_encode_hello(h, (int)sizeof h, g_my_id);
          sendto(g_sock, (const char *)h, hn, 0,
                 (struct sockaddr *)&from, (socklen_t)sizeof from);
        }
        // Learn the peer's address from whoever actually answered, so a wrong
        // OB_PEER port still converges once they reach us.
        g_peer = from;
        g_have_peer = 1;
        g_peer_id = id;
        if (g_seat < 0 && id != g_my_id) {
          // Lower identifier takes seat 0. Both sides run the same comparison,
          // so there is no host/join role and two hosts are impossible.
          g_seat = (g_my_id < id) ? 0 : 1;
          g_state = NET_OPEN;
        }
      }
      continue;
    }
    if (t == PKT_BYE) { g_state = NET_FAILED; snprintf(g_err, sizeof g_err, "peer left"); continue; }
    if (t == PKT_INPUT) enqueue(buf, n);
  }

  // Keep announcing until the seat is settled. Cheap, and it means neither side
  // has to start first.
  if (g_state == NET_CONNECTING) {
    uint64_t t = now_ms();
    if (t - g_last_hello > 100) {
      g_last_hello = t;
      uint8_t h[PKT_HELLO_SIZE];
      int n = proto_encode_hello(h, (int)sizeof h, g_my_id);
      send_raw(h, n);
    }
  }
}

int net_poll(uint8_t *buf, int cap) {
  uint64_t t = now_ms();
  for (int i = 0; i < g_qn; i++) {
    if (g_q[i].due_ms <= t) {
      int len = g_q[i].len;
      if (len > cap) { g_q[i] = g_q[--g_qn]; return -1; }
      memcpy(buf, g_q[i].buf, (size_t)len);
      g_q[i] = g_q[--g_qn];      // order is not preserved, and need not be:
      return len;                // inputs are absolute-frame-indexed
    }
  }
  return -1;
}

void net_send(const void *buf, int len) { send_raw(buf, len); }

NetState net_state(void) { return g_state; }
int      net_seat(void)  { return g_seat; }
const char *net_last_error(void) { return g_err; }

void net_close(void) {
  if (g_sock != OB_BAD_SOCK) {
    uint8_t b = PKT_BYE;
    send_raw(&b, 1);
    OB_CLOSESOCK(g_sock);
    g_sock = OB_BAD_SOCK;
  }
  g_state = NET_IDLE;
  g_qn = 0;
  g_seat = -1;
}

int net_url_room(char *out, int cap) {
  // No URL to read from natively; the room comes from OB_ROOM.
  if (out && cap > 0) out[0] = 0;
  return 0;
}

int net_local_hidden(void) {
  // A native window is never throttled the way a background browser tab is.
  return 0;
}
