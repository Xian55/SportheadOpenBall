#!/usr/bin/env bash
# Tier-1 purity gate.
#
# The simulation must stay integer-only: a stray float, double or `long` is a
# silent desync between the MinGW native build and the wasm build, and those are
# expensive to chase. `long` is banned because it is 32-bit on MinGW and wasm32
# but 64-bit on linux gcc.
#
# This is the cheap prevention layer. The real detection is the M2 golden replay
# diffed between native and wasm.
#
# Comments are stripped with `gcc -fpreprocessed`, which removes them WITHOUT
# expanding macros - expanding would turn every FXF() into a double literal and
# defeat the whole check.
set -uo pipefail
cd "$(dirname "$0")/.."

TIER1="src/fixed.h src/config.h src/sim.h src/sim.c src/checksum.h src/checksum.c
       src/proto.h src/proto.c src/rollback.h src/rollback.c"

bad=0
for f in $TIER1; do
  [ -f "$f" ] || continue
  hits=$(gcc -fpreprocessed -dD -E "$f" 2>/dev/null \
         | grep -nE '\b(float|double)\b|<math\.h>|\blong\b' \
         | grep -vE '#define (FXF|FX)\(' || true)
  if [ -n "$hits" ]; then
    echo "tier-1 purity violation in $f:"
    echo "$hits" | sed 's/^/    /'
    bad=1
  fi
done

if [ "$bad" -ne 0 ]; then
  echo
  echo "Tier 1 must not use float, double, long or math.h. See README 'Layout'."
  exit 1
fi
echo "tier-1 purity gate: OK"
