// Q16.16 fixed point - the simulation's only numeric type.
// Every operation here is integer, which is what makes the sim bit-identical
// between the MinGW native build and the wasm build. Tier-1 sources may not
// use float, double, math.h or `long` (long is 32-bit on MinGW/wasm32 but
// 64-bit on linux gcc). tests/lint_sim.cmd and -Werror=double-promotion
// enforce this; the sim tests also link without -lm so a stray libm call is a
// link error rather than a silent desync.
#ifndef OB_FIXED_H
#define OB_FIXED_H

#include <stdint.h>

typedef int32_t fx;   // Q16.16: range +-32767.99998, resolution 1/65536
typedef int64_t fx2;  // Q32.32: squared / intermediate values. NEVER narrow to fx.

#define FX_SHIFT 16
#define FX_ONE   ((fx)65536)
#define FX_HALF  ((fx)32768)
#define FX_ZERO  ((fx)0)

// Integer -> fx. |n| must stay under 32767 or this overflows silently.
#define FXI(n)  ((fx)((int32_t)(n) * FX_ONE))

// Compile-time decimal -> fx, round-to-nearest. ONLY legal in constant
// expressions (config.h): the compiler folds it, so no double ever reaches the
// runtime sim. const_test pins the folded values against a golden so a
// toolchain change cannot silently move a constant.
#define FXF(d)  ((fx)((d) * 65536.0 + ((d) < 0 ? -0.5 : 0.5)))

// Arithmetic shift right = floor for negatives, which is what we rely on.
#define FX_FLOOR(v) ((int32_t)((v) >> FX_SHIFT))

// C11 leaves >> on negative signed ints implementation-defined. GCC and LLVM
// both define it as arithmetic; assert it rather than assume it.
_Static_assert((-3 >> 1) == -2, "arithmetic right shift required");

// Rounding is asymmetric ON PURPOSE and must never be mixed: >> floors toward
// -inf, / truncates toward 0. Both are identical on MinGW and wasm, which is
// all determinism needs. Do not "fix" fx_mul to round-to-nearest without
// re-blessing every golden.
static inline fx fx_mul(fx a, fx b) { return (fx)(((int64_t)a * (int64_t)b) >> FX_SHIFT); }
static inline fx fx_div(fx a, fx b) { return (fx)((((int64_t)a) << FX_SHIFT) / (int64_t)b); }
static inline fx fx_abs(fx a)       { return a < 0 ? -a : a; }
static inline fx fx_min(fx a, fx b) { return a < b ? a : b; }
static inline fx fx_max(fx a, fx b) { return a > b ? a : b; }
static inline fx fx_clamp(fx v, fx lo, fx hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Squared distance of two fx deltas -> Q32.32 in an int64. NEVER store this in
// an fx: at 1280 px field width dx is ~8.4e7, so dx*dx is ~7e15, which is
// 400000x past INT32_MAX. Worst-case fx_d2 is ~1.4e16 against an INT64_MAX of
// 9.2e18 - about 650x headroom. Compare fx2 against fx2; do not take roots to
// decide whether something overlaps.
static inline fx2 fx_d2(fx dx, fx dy) { return (fx2)dx * dx + (fx2)dy * dy; }

// Digit-by-digit integer square root. Exact, branch-deterministic, no libm.
static inline uint64_t ob_isqrt64(uint64_t v) {
  uint64_t rem = 0, root = 0;
  for (int i = 0; i < 32; i++) {
    root <<= 1;
    rem = (rem << 2) | (v >> 62);
    v <<= 2;
    if (root < rem) { rem -= root | 1; root += 2; }
  }
  return root >> 1;
}

// The key identity: sqrt of a Q32.32 value IS Q16.16. No shifting needed.
static inline fx fx_len(fx dx, fx dy) { return (fx)ob_isqrt64((uint64_t)fx_d2(dx, dy)); }

// sqrt of an fx (rare): sqrt(a/2^16)*2^16 == isqrt(a << 16). Caller guarantees a >= 0.
static inline fx fx_sqrt(fx a) {
  if (a <= 0) return 0;
  return (fx)ob_isqrt64((uint64_t)(uint32_t)a << FX_SHIFT);
}

#endif // OB_FIXED_H
