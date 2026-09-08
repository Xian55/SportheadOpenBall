// Keyboard and touch -> input bitmask. See input.h for the sampling rule.
#include "raylib.h"
#include "input.h"
#include "touch.h"
#include "config.h"

// Hotseat keymaps. Deliberately symmetric and modifier-free so two people can
// share one keyboard without collisions:
//   seat 0: A / D move, W jump, S kick
//   seat 1: arrows move, UP jump, DOWN kick
static uint8_t g_level[2];    // movement bits: latest sample wins
static uint8_t g_sticky[2];   // action bits: accumulated until consumed

static uint8_t sample_keys(int seat) {
  uint8_t m = 0;
  if (seat == 0) {
    if (IsKeyDown(KEY_A))    m |= IN_LEFT;
    if (IsKeyDown(KEY_D))    m |= IN_RIGHT;
    if (IsKeyDown(KEY_W))    m |= IN_JUMP;
    if (IsKeyDown(KEY_S))    m |= IN_KICK;
  } else {
    if (IsKeyDown(KEY_LEFT))  m |= IN_LEFT;
    if (IsKeyDown(KEY_RIGHT)) m |= IN_RIGHT;
    if (IsKeyDown(KEY_UP))    m |= IN_JUMP;
    if (IsKeyDown(KEY_DOWN))  m |= IN_KICK;
  }
  return m;
}

void input_poll(void) {
  for (int seat = 0; seat < 2; seat++) {
    uint8_t m = sample_keys(seat);

    // Touch drives the LOCAL seat, which is seat 0 while this is hotseat-only.
    // When netplay lands, net_seat() supplies it and nothing else changes -
    // touch is just another source OR'd into the same mask, so the sim and the
    // netcode never learn it exists.
    if (seat == 0) m |= touch_mask();

    g_level[seat]  = (uint8_t)(m & (IN_LEFT | IN_RIGHT));
    g_sticky[seat] = (uint8_t)(g_sticky[seat] | (m & (IN_JUMP | IN_KICK)));
  }
}

uint8_t input_consume(int seat) {
  uint8_t m = (uint8_t)((g_level[seat] | g_sticky[seat]) & IN_MASK);
  g_sticky[seat] = 0;
  return m;
}

void input_clear(void) {
  g_level[0] = g_level[1] = 0;
  g_sticky[0] = g_sticky[1] = 0;
}
