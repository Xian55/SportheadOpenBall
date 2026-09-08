// Unit tests for the Q16.16 primitives.
//
// Deliberately uses NO floating point and NO libm, even though a test would be
// allowed to: every property below is checked against integer reference
// arithmetic instead. That keeps this binary linkable without -lm, which is
// what makes a stray libm call in tier 1 a link error rather than a silent
// desync between the native and wasm builds.
#include <stdio.h>
#include <stdint.h>
#include "fixed.h"
#include "trig.h"

static int fails = 0;

static void check(int cond, const char *what) {
  if (!cond) { printf("FAIL: %s\n", what); fails++; }
}

static void check_eq(int64_t got, int64_t want, const char *what) {
  if (got != want) {
    printf("FAIL: %s (got %lld want %lld)\n", what, (long long)got, (long long)want);
    fails++;
  }
}

// --- the assumptions the sim is built on -----------------------------------
static void test_shift_semantics(void) {
  // FX_FLOOR relies on >> being ARITHMETIC on negatives. C11 leaves this
  // implementation-defined; GCC and LLVM both define it. Assert, do not assume.
  check_eq(-3 >> 1, -2, "arithmetic shift right on negative");
  check_eq(FX_FLOOR(FXI(-3)), -3, "FX_FLOOR of exact negative");
  check_eq(FX_FLOOR(-FX_ONE / 2), -1, "FX_FLOOR rounds toward -inf, not zero");
  check_eq(FX_FLOOR(FX_ONE / 2), 0, "FX_FLOOR of +0.5");

  // Integer division truncates toward ZERO - the opposite of the shift. The sim
  // mixes both deliberately; this pins the difference so it cannot drift.
  check_eq((int32_t)(-3 / 2), -1, "integer division truncates toward zero");

  check_eq(sizeof(fx), 4, "fx is 32-bit");
  check_eq(sizeof(fx2), 8, "fx2 is 64-bit");
}

// --- multiply / divide ------------------------------------------------------
static void test_mul_div(void) {
  check_eq(fx_mul(FXI(3), FXI(4)), FXI(12), "3*4");
  check_eq(fx_mul(FXI(-3), FXI(4)), FXI(-12), "-3*4");
  check_eq(fx_mul(FX_ONE, FX_ONE), FX_ONE, "1*1");
  check_eq(fx_mul(FX_HALF, FX_HALF), FX_ONE / 4, "0.5*0.5");
  check_eq(fx_mul(0, FXI(1000)), 0, "0*n");

  check_eq(fx_div(FXI(12), FXI(4)), FXI(3), "12/4");
  check_eq(fx_div(FXI(-12), FXI(4)), FXI(-3), "-12/4");
  check_eq(fx_div(FX_ONE, FXI(2)), FX_HALF, "1/2");

  // Exhaustive-ish cross-check against int64 reference arithmetic. fx_mul must
  // cast BEFORE multiplying; this catches a regression to (int32*int32)>>16.
  for (int32_t a = -40000; a <= 40000; a += 977) {
    for (int32_t b = -40000; b <= 40000; b += 1013) {
      fx  got  = fx_mul(a, b);
      fx  want = (fx)(((int64_t)a * (int64_t)b) >> FX_SHIFT);
      if (got != want) { check_eq(got, want, "fx_mul vs int64 reference"); return; }
      if (b != 0) {
        fx gd = fx_div(a, b);
        fx wd = (fx)((((int64_t)a) << FX_SHIFT) / (int64_t)b);
        if (gd != wd) { check_eq(gd, wd, "fx_div vs int64 reference"); return; }
      }
    }
  }
}

// --- isqrt / length ---------------------------------------------------------
static void test_sqrt(void) {
  // The DEFINING property of floor(sqrt(v)): r*r <= v < (r+1)*(r+1). Checking
  // that needs no reference sqrt at all, so no libm.
  uint64_t probes[] = { 0, 1, 2, 3, 4, 8, 15, 16, 17, 99, 100, 101,
                        65535, 65536, 65537, 1000000, 4294967295u,
                        1ull<<40, (1ull<<62) - 1 };
  for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; i++) {
    uint64_t v = probes[i], r = ob_isqrt64(v);
    check(r * r <= v, "isqrt64: r*r <= v");
    check((r + 1) > r && (r + 1) * (r + 1) > v, "isqrt64: (r+1)^2 > v");
  }
  for (uint64_t n = 0; n < 3000; n++) {
    check_eq((int64_t)ob_isqrt64(n * n), (int64_t)n, "isqrt64 of a perfect square");
    if (n) check_eq((int64_t)ob_isqrt64(n * n - 1), (int64_t)n - 1, "isqrt64 just below a square");
  }

  check_eq(fx_sqrt(FXI(4)), FXI(2), "sqrt(4)=2");
  check_eq(fx_sqrt(FXI(9)), FXI(3), "sqrt(9)=3");
  check_eq(fx_sqrt(0), 0, "sqrt(0)=0");
  check_eq(fx_sqrt(-FXI(5)), 0, "sqrt of negative clamps to 0");

  // 3-4-5, the one exact triangle - proves fx_len's Q32.32 -> Q16.16 identity.
  check_eq(fx_len(FXI(3), FXI(4)), FXI(5), "fx_len(3,4)=5");
  check_eq(fx_len(FXI(-3), FXI(-4)), FXI(5), "fx_len is sign-independent");
  check_eq(fx_len(0, 0), 0, "fx_len(0,0)=0");
  check_eq(fx_len(FXI(1280), 0), FXI(1280), "fx_len along an axis at pitch width");
}

// --- overflow headroom ------------------------------------------------------
static void test_overflow(void) {
  // The whole reason squared quantities live in fx2: at pitch width a squared
  // delta is ~7e15, which is 400000x past INT32_MAX.
  fx  big = FXI(1280);
  fx2 d2  = fx_d2(big, big);
  check(d2 > 0, "fx_d2 at pitch width does not overflow or go negative");
  check(d2 > (fx2)INT32_MAX, "fx_d2 genuinely exceeds 32 bits (so fx2 is required)");
  check(d2 < (fx2)1 << 62, "fx_d2 stays inside int64 with headroom");
  check_eq(fx_len(big, big), (fx)ob_isqrt64((uint64_t)d2), "fx_len == isqrt of fx_d2");

  check_eq(fx_clamp(FXI(5), FXI(0), FXI(3)), FXI(3), "clamp high");
  check_eq(fx_clamp(FXI(-5), FXI(0), FXI(3)), FXI(0), "clamp low");
  check_eq(fx_abs(FXI(-7)), FXI(7), "abs");
}

// --- trig table -------------------------------------------------------------
static void test_trig(void) {
  check_eq(fx_sin_deg(0), 0, "sin(0)=0");
  check_eq(fx_sin_deg(FXI(90)), FX_ONE, "sin(90)=1");
  check_eq(fx_sin_deg(FXI(180)), 0, "sin(180)=0");
  check_eq(fx_sin_deg(FXI(270)), -FX_ONE, "sin(270)=-1");
  check_eq(fx_cos_deg(0), FX_ONE, "cos(0)=1");
  check_eq(fx_cos_deg(FXI(90)), 0, "cos(90)=0");

  // Index must wrap cleanly in both directions - the leg angle goes negative.
  check_eq(fx_sin_deg(FXI(360)), fx_sin_deg(0), "sin wraps at 360");
  check_eq(fx_sin_deg(FXI(-90)), -FX_ONE, "sin(-90)=-1");
  check_eq(fx_sin_deg(FXI(-360)), fx_sin_deg(0), "sin wraps negatively");

  // sin^2 + cos^2 == 1, within the table's 1-degree quantisation.
  for (int32_t d = -720; d <= 720; d += 7) {
    fx s = fx_sin_deg(FXI(d)), c = fx_cos_deg(FXI(d));
    fx2 sum = (fx2)s * s + (fx2)c * c;
    fx2 one = (fx2)FX_ONE * FX_ONE;
    fx2 err = sum > one ? sum - one : one - sum;
    check(err < one / 1000, "sin^2+cos^2 == 1");
  }
}

int main(void) {
  test_shift_semantics();
  test_mul_div();
  test_sqrt();
  test_overflow();
  test_trig();
  if (fails) { printf("fixed_test: %d FAILURES\n", fails); return 1; }
  puts("fixed_test OK");
  return 0;
}
