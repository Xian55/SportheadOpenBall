# SportheadOpenBall

An original reimplementation of the classic "sports heads football" flash game:
two big-headed players, one ball, two goals, kick/jump/move, a match clock.
C11 + raylib 5.5, running natively on Windows and as WASM on GitHub Pages.

The interesting constraint: **online 2-player multiplayer with no backend.**
GitHub Pages serves static files only, so the game is peer-to-peer over a WebRTC
DataChannel, using public Nostr relays for signalling and nothing else. There is
no server to run, pay for, or keep alive.

Netcode is deterministic rollback (GGPO-style): a Q16.16 fixed-point simulation
with no floating point anywhere, so both peers compute bit-identical results.
Only a 1-byte input bitmask per frame crosses the wire.

## Build

Prerequisites:

```
winget install BrechtSanders.WinLibs.POSIX.UCRT
winget install Kitware.CMake
```

```
build.cmd            native game + tests -> ..\SportheadOpenBall_build\native
build.cmd test       native build, then the headless test battery
build.cmd web        wasm build           -> ..\SportheadOpenBall_build\web
serve.cmd            serve the web build at http://localhost:8080/
```

The web build needs emsdk. Point `EMSDK` at an existing checkout
(`set EMSDK=F:\Programming\emsdk`) or clone one next to this project; `build.cmd
web` finds either. Build artifacts deliberately live **out of tree**, in a
sibling `..\SportheadOpenBall_build\` directory.

## Layout

```
src/     flat, one .c/.h pair per module
tests/   headless test exes + committed golden fixtures
web/     shell.html and the vendored signalling bundle
```

Three tiers with a hard rule about what each may include:

| Tier | Files | May include |
|---|---|---|
| 1 — sim | `fixed.h` `config.h` `sim.c` `checksum.c` `proto.c` `rollback.c` | **no raylib, no float/double, no math.h, no `long`** |
| 2 — transport | `net.h` `net_udp.c` `net_web.c` | no raylib |
| 3 — presentation | `main.c` `render.c` `input.c` `touch.c` `ui.c` | raylib and float are fine |

Tier 1 is compiled as one OBJECT library with `-fwrapv
-Werror=double-promotion -Werror=float-conversion -Werror=float-equal`, and the
tests link it **without `-lm`** — so a stray `sqrtf` in the simulation is a link
error rather than a silent desync between the native and wasm builds.

## Debug hooks

Environment variables, so a headless agent can verify behaviour:

| | |
|---|---|
| `OB_SHOT=N` | export the 1280x720 virtual framebuffer at frame N, print its checksum, exit 0 |

More land with the netcode: `OB_ROOM`, `OB_NETSTAT`, `OB_LAG_MS`, `OB_JITTER_MS`,
`OB_LOSS`, `OB_NETSEED`, `OB_PORT`, `OB_PEER`.

## On the original game

Gameplay mechanics are not copyrightable; sprites, audio and branding are. This
project reimplements the mechanics with original art and ships none of the
original's assets. Do not add them.

If you want to match the original's feel, run it locally under a Flash emulator
and measure gravity, jump apex, restitution and kick impulse, then fit the
constants in `src/config.h`. Keep that material in `reference/`, which is
gitignored.

## Licence

MIT.
