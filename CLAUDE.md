# SportheadOpenBall — C11 + raylib 5.5, deterministic rollback netcode, zero backend

An original reimplementation of the classic "sports heads football" flash game. One
codebase ships a native Windows client and a WASM build hosted on GitHub Pages, with
peer-to-peer 2-player multiplayer and **no server of any kind**.

Live: https://xian55.github.io/SportheadOpenBall/

## Working agreement

- The sim is the contract. Anything that changes `sim_step` behaviour re-blesses every
  golden fixture, and that must be called out in the commit message. Never re-bless a
  red determinism gate to make CI green without understanding what moved.
- Tier discipline (below) is not a style preference; it is what makes the netcode work.
- `tools/lint_sim.sh` must stay green.

## Quick start

```
build.cmd            native  -> ..\SportheadOpenBall_build\native\openball.exe
build.cmd test       native build + headless test battery
build.cmd web        wasm    -> ..\SportheadOpenBall_build\web\index.{html,js,wasm}
serve.cmd            http://localhost:8080/
```

emsdk is NOT installed system-wide. An existing checkout lives at `F:\Programming\emsdk`;
`build.cmd web` finds it via `EMSDK`, a `..\emsdk` sibling, or that path. Do not bootstrap
a second multi-GB copy.

Build artifacts live **out of tree** in a sibling `..\SportheadOpenBall_build\`.

## Architecture: three tiers, hard include rules

| Tier | Files | May include |
|---|---|---|
| 1 sim | `fixed.h` `trig.h` `config.h` `sim.c` `checksum.c` `proto.c` `rollback.c` | **no raylib, no float/double, no math.h, no `long`** |
| 2 transport | `net.h` `net_udp.c` `net_web.c` | no raylib |
| 3 presentation | `main.c` `render.c` `input.c` `touch.c` `ui.c` | raylib + float fine |

Tier 1 is a CMake OBJECT library (`ob_sim`) built with `-fwrapv
-Werror=double-promotion -Werror=float-conversion -Werror=float-equal`. Tests link it
**without `-lm`**, so a stray `sqrtf` is a link error rather than a silent desync.
`long` is banned because it is 32-bit on MinGW/wasm32 but 64-bit on linux gcc.

Enforcement is three-layered: compiler flags, the no-libm link, and `tools/lint_sim.sh`
(which strips comments via `gcc -fpreprocessed` — it must NOT expand macros, or every
`FXF()` becomes a double literal and the check is defeated).

## Netcode design (decided, do not relitigate)

- **Deterministic rollback**, Q16.16 fixed point, no floats in the sim. 1-byte input
  bitmask per frame.
- **`GameState` is 96 bytes** (80 before the leg gained angle + angular velocity),
  POD, explicit `_pad`, `_Static_assert` on `sizeof`.
  It is memcpy'd into the ring every frame and hashed byte-for-byte, so indeterminate
  padding would desync two compilers. `sim_init` memsets before filling.
- **Snapshot every frame unconditionally.** GGPO's save/load-callback machinery exists
  for megabyte states; ours is 80 bytes. A 128-frame ring is ~11 KB, all static.
- **Only rewind when a received input differs from what was already fed to `sim_step`**
  (keep a `used[]` array). Removes ~80% of rollbacks.
- **`INPUT_DELAY = 2` frames.** Matters more here than in a fighting game: a mispredicted
  kick teleports the ball.
- **Every packet carries the last 8 frames of input.** The DataChannel is
  `{ordered:false, maxRetransmits:0}`, so drops are expected; redundancy means a single
  loss costs nothing and needs no retransmit logic. Inputs are absolute-frame-indexed, so
  reorder and duplicate delivery need no special handling.
- **Seats are assigned by comparing `selfId` to `peerId` lexicographically.** No host/join
  roles — one code box, one Play button, and two hosts are impossible.
- Desync policy v1: **hard stop with a loud banner** naming the frame and both hashes.
  Silently continuing means two people playing different games and destroys debuggability.

## Signalling

Trystero over public Nostr relays, for signalling only — game data goes direct over the
DataChannel. The bundle is **vendored and committed** (`web/trystero-nostr.min.js`), not
pulled from a CDN: the site must survive an upstream change, a CDN block, and years of no
commits. Pin the version + SHA in `web/TRYSTERO_VERSION.txt`.

The dynamic `import()` must resolve against `document.baseURI`, **not** an absolute `/`
path — this is a GitHub *project* page served under `/SportheadOpenBall/`.

No TURN is possible without a backend, so peers behind symmetric/carrier-grade NAT will
fail to connect. Make that failure legible (distinguish "no peer found" from "peer found,
connection failed") and never show an infinite spinner.

## Physics constants are MEASURED, not guessed

Gravity, restitution, drag and move speed were measured from a screen capture of
the original with ffmpeg + numpy: track the ball by motion and brightness, fit
parabolas to its free-flight arcs. 48 clean arcs gave gravity 0.1312 px per
video-frame^2 (quartiles 0.1235-0.1377). Anything derived that way is marked
**MEASURED** in `config.h` with its sample count. Do not "tidy" those values.

**The original renders at 30 Hz, not 60** - 29 of 59 consecutive video frames are
byte-identical. Our sim ticks at 60, so anything taken from it in per-tick units
must be converted: velocities halve, accelerations quarter. Their playfield is
1452 px against our 1280, so the space scale is 0.882.

Still NOT measured, and flagged as provisional in `config.h`: **jump height**
(three tracking attempts were defeated by the animated crowd and the
goal-celebration overlay) and **kick impulse**. Do not present either as derived.

## Netcode gotchas (M3/M4), all found by running it rather than by tests

- **Send a packet EVERY frame, never only when a tick runs.** Building the packet
  inside the tick loop meant a stalled peer went silent, which starved the other
  peer's `confirmed` and made it stall too. The two deadlocked and crept forward
  only via the anti-deadlock escape hatch: eighteen simulated frames in ten
  seconds. A stalled peer has no new input to offer, but the packet still carries
  the input history and the ack the other side needs. Fixing it took UDP stalls at
  60 ms from 250 to 0.
- **The anti-deadlock escape hatch must be BOUNDED.** It exists so a dead peer
  cannot freeze us forever, but unbounded it drove `frame - confirmed` to 72 and
  climbing against a throttled peer. Past `RB_N` there is no snapshot left to
  rewind to, so "peer is slow" would have become a guaranteed desync. Hard-stall
  at `RB_N/2` whatever else is true.
- **Verify a peer's checksum only once OUR copy of that frame is settled** - below
  `confirmed`, with no rewind pending. Comparing on arrival gives constant false
  desyncs, because the frame may still be predicted locally or have an unapplied
  correction queued. That bug failed 60 of 85 rollback_test cases, and every
  failure said "desync flag raised" while none said "peer != lockstep" - the
  states were always right, the detector was wrong.
- **Two browser tabs in one window cannot test this.** Only one tab is visible, so
  the other is throttled to ~1 Hz, and since the sim advances only when both peers
  supply inputs the visible side is dragged down with it - about a hundredfold
  slowdown. Use two separate WINDOWS side by side: browsers throttle on tab
  visibility, not window focus. The game now pauses explicitly and names which
  side is backgrounded rather than crawling.
- **Trystero 0.25 API, checked against the shipped README rather than memory:**
  `onPeerJoin`/`onPeerLeave` are assignable PROPERTIES, not methods (calling them
  throws "is not a function"); redundancy lives under `relayConfig`; and
  `joinRoom`'s third `callbacks` argument carries `onJoinError`, which separates
  "no peer found" from "peers exchanged SDP but WebRTC could not connect" - the
  strict-NAT case worth naming precisely.
- **Trystero actions are reliable and ordered**, which is the opposite of what
  rollback wants. It does SIGNALLING ONLY; game data rides our own negotiated
  channel on the `RTCPeerConnection` from `getPeers()`, unordered with zero
  retransmits. Losing a packet is free (every packet carries `INPUT_HIST` frames);
  waiting for a retransmit stalls inputs already superseded.
- **Connection is not instant.** Relay discovery plus the WebRTC handshake takes
  10-20 s. Always show progress; a silent wait is indistinguishable from a hang.

## Gotchas learned the hard way

- **Windows display scaling breaks raylib's framebuffer assumptions.** At 125% a 1280x720
  window got a 1600x900 framebuffer; GL's bottom-left origin then put the scene in the
  corner. `FLAG_WINDOW_HIGHDPI` is what fixes it: raylib then queries the real
  framebuffer and installs a screenScale matrix, so drawing uses LOGICAL coordinates
  (`GetScreenWidth/Height`) and raylib stretches them across the full buffer. The
  letterbox math is therefore in logical space.
- **Do NOT render the pitch through a fixed 1280x720 `RenderTexture`.** It used to, and
  it cost real sharpness: the blit resamples every pixel a second time, and wherever it
  DOWNSCALES (a landscape phone sits near 0.54) a 1 px net strand lands on half a pixel
  and vanishes — losing only *some* strands, which reads as a rendering fault rather than
  a thin line. Normal play now draws straight to the backbuffer through a `Camera2D`
  carrying the same letterbox transform, at the display's own resolution. The render
  texture survives for `OB_SHOT` alone, where a byte-comparable 1280x720 export is the
  point; that path forces the scale factors to 1 so the export is unaffected by the
  window.
- **Size strokes in PHYSICAL pixels, not virtual units.** `stroke(n)` in `render.c`
  returns the virtual width that covers `n` whole framebuffer pixels, and never returns
  less than `n`. Virtual-unit hairlines are the first thing a scaled-down pitch loses.
- **NEVER set `FLAG_WINDOW_RESIZABLE` on web.** It is the only thing gating raylib's
  `EmscriptenResizeCallback`, which on every window resize event does:

  ```c
  int width = EM_ASM_INT( return window.innerWidth; );   // CSS pixels
  emscripten_set_canvas_element_size("#canvas", width, height);
  SetupViewport(width, height);
  ```

  That hardcodes CSS pixels into both the canvas backing store and the GL viewport, so it
  fights any code that sizes the canvas itself. With `sync_canvas_size` writing DEVICE
  pixels, whichever writer ran last won each half and the framebuffer ended up
  `dpr x viewport`: everything drew at half scale anchored bottom-left (GL's origin) and
  `SetWindowSize` fired every frame instead of settling. On a phone the URL bar hiding
  and showing fires resize events constantly; a static desktop window fires none, which
  is exactly why desktop could not reproduce it for two rounds of guessing.
  **To reproduce on desktop: `window.dispatchEvent(new Event('resize'))`.** With the flag
  set, `buf` and `css` visibly oscillate and their ratio lands on `devicePixelRatio`.
- **`FLAG_WINDOW_HIGHDPI` is a no-op on web** — `GetWindowScaleDPI()` is a stub returning
  `1,1` and the HIGHDPI branch of `WindowSizeCallback` divides by it — except that the
  stub logs, which is the `GetWindowScaleDPI() not implemented on target platform` spam.
  It is REQUIRED natively (above) and pointless on web, so it is set per-platform.
- **The web canvas buffer is sized in DEVICE pixels** by `sync_canvas_size`: CSS size x
  `emscripten_get_device_pixel_ratio()`, capped at 2x for fill rate. In CSS pixels a phone
  at dPR 3 renders at a third of its real resolution and the browser upscales — "high
  resolution and yet blurry". This is only safe because the flag above makes it the single
  writer. It also tolerates a 1 px difference before calling `SetWindowSize`, because
  `emscripten_get_element_css_size` reports a FRACTIONAL size and the values downstream
  get floored — an exact comparison can miss by one forever and resize every frame.
  Input needs no matching fix: raylib's `EmscriptenTouchCallback` already scales touches
  by `GetScreenWidth()/cssWidth`, and emscripten's GLFW `calculateMouseCoords` by
  `GLFW.active.width/rect.width`, so both follow the buffer automatically.
  `?diag` prints `dpr / css / buf / screen / render` in screen space, outside the pitch
  transform so its own position is trustworthy when the viewport is wrong; `buf`,
  `screen` and `render` agreeing is the healthy state. `?nohidpi` falls back to CSS-pixel
  sizing.
- **raylib's default font is a 10px bitmap.** Scaled to 44px its zero glyph legitimately
  looks like a rectangle with a slot. That is not tofu and not a bug — do not go hunting
  for a font problem. Verified by dumping the string bytes: `30 20 20 2d 20 20 30`.
- **Do not enable emscripten memory growth.** Fixed `-sINITIAL_MEMORY=33554432` instead.
  Growth detaches and replaces the `ArrayBuffer`, which invalidates any cached `HEAPU8`
  and makes `TextDecoder.decode` / `texImage2D` reject views into it. If it is ever truly
  needed, it MUST be paired with `-sGROWABLE_ARRAYBUFFERS=0`.
- In JS glue, read `HEAPU8` fresh on every call and **copy** (`.slice()`) before handing a
  buffer to a Web API that may retain it — `RTCDataChannel.send` does.
- **Do not enable `--closure 1`.** It mangles js-library names for ~15 KB.
- `.cmd` files must be CRLF (cmd.exe misparses LF-only batch files); `.sh` files must be
  LF (`bad interpreter` with a trailing CR on Linux). Both are pinned in `.gitattributes`.
- GitHub blocks pushes that expose a private email. Commits here use
  `367101+Xian55@users.noreply.github.com`, set in repo-local git config.
- **Render MUST interpolate between sim ticks.** The sim is a fixed 60 Hz while the
  display runs at its own rate and `GetFrameTime()` drifts either side of 1/60, so
  the accumulator lands 0 steps on one frame and 2 on the next. Without
  interpolation everything freezes then jumps double, which reads as tearing.
  `render_frame()` takes prev + cur + alpha; large jumps SNAP or a kickoff reset
  smears the ball across the pitch.
- **raylib's `GetFPS()` is unreliable read once before exit** - it reported 1800 on a
  60 Hz display and sent me hunting a vsync fault that did not exist. Measure
  sustained wall-clock rate instead.
- **Input FEEL settings are local; they never go in `config.h`.** Constants there
  are protocol - hashed into the const golden, and both peers must agree on them.
  Stick deadzone and the like shape which input BITS a device produces, never how
  the sim reads them, so they live in `touch.c` and two players can run different
  settings and stay in sync. Putting one in `config.h` would quietly turn a
  comfort setting into a desync source.
- **Directional controls go on the axis they control.** LEFT and RIGHT are a
  horizontal pair and are never stacked, even when stacking would fit a layout
  neatly. Stacking them made "up" mean "left", which forces a mental translation
  on every input. KICK and JUMP carry no direction, so they are free to be placed
  wherever there is room - that asymmetry is what the layout exploits. Movement
  is now a floating stick rather than discrete arrows, which sidesteps the axis
  problem entirely by BEING the axis, and folds jump into pushing up.
- **Mobile browsers synthesise MOUSE events from touches.** Any mouse handling
  must be gated behind `GetTouchPointCount() == 0`, or the touch and mouse paths
  fight each other every frame. Here it made the floating stick's base chase the
  thumb (touch engaged it, the emulated mouse re-claimed and re-based it, the id
  stopped matching, it re-acquired at the new position) and killed movement the
  instant KICK was pressed, because the emulated mouse landed in the kick circle.
  raylib's touch IDs are fine - it copies the browser's identifiers, which are
  stable - so the IDs were never the problem.
- **Touch controls are sized in SCREEN space, never virtual pitch space.** In virtual
  space they shrink with the letterbox: a landscape phone scales the pitch by ~0.54,
  turning a 56 px button into ~30 physical px against a ~48 px minimum target. They
  are laid out and drawn after the blit, at 13% of screen height.
- **Never size a web canvas with `100vh`.** On mobile it measures the viewport as if
  the URL bar were hidden, so the bottom of the game is cut off below the screen
  edge. Use `100dvh` with `vh` as fallback. Also needed: `touch-action: none`
  (a drag otherwise scrolls the page), `overscroll-behavior: none` (pull-to-refresh)
  and `user-scalable=no` (pinch-zoom fights the controls).
- GitHub Pages sends no COOP/COEP headers, so no `SharedArrayBuffer` and no pthreads.
  Never add `-pthread`. The build must stay single-threaded (it costs nothing — the sim
  is integer math and a 12-frame resim is microseconds).

## Rejected alternatives (with the reason, so they stay rejected)

- **Room UI in an HTML overlay** — rejected in favour of raylib. An `<input>` over the
  canvas fights for focus inside a Pages/itch iframe, drifts out of alignment when the
  canvas is CSS-scaled, and would need a second implementation for native LAN play. Only
  the `?room=` prefill comes from JS, pulled by C at startup.
- **WAJIC / a slimmer wasm toolchain** — rejected. raylib's `rcore_web.c` makes 36
  distinct `emscripten_*` calls, has 83 GLFW call sites depending on `-sUSE_GLFW=3`
  emulation, and `raudio.c` uses its WebAudio glue. Switching means writing a new raylib
  platform backend and maintaining a fork, to save at most the ~24 KB gzipped that the JS
  glue costs. Total payload is already 79 KB gzipped.
- **WebP textures** — rejected. raylib decodes via stb_image, which has no WebP decoder;
  adding one means vendoring libwebp (~200-300 KB of wasm) to save very little on
  palette-limited pixel art, where PNG is near-optimal. raylib supports **QOI** natively
  if the atlas ever gets big.
- **A float prototype before the fixed-point sim** — rejected. The sim is ~250 lines;
  porting float to fixed is not mechanical (every constant, comparison and collision
  response is re-derived) and it throws away every golden fixture and tuning value.

## Assets (M5 — v1 is primitives only)

Aseprite, driven headlessly via the **`ASEPRITE`** env var (already set User-scope on this
machine), mirroring the pipeline in `F:\Programming\Adventure`:

```
"$ASEPRITE" -b art/head_red.aseprite --sheet assets/head_red.png \
  --data assets/head_red.json --sheet-type packed --list-tags
```

Divergence from that project, which gitignores `assets/`: here the exported PNGs **must be
committed**, because GitHub Actions builds the wasm and bakes assets in at link time and
CI runners have no Aseprite. Commit both the `.aseprite` sources and the exports.

## Debug hooks

`OB_SHOT=N` — export the 1280x720 virtual framebuffer at frame N, print its checksum,
exit 0. Lands in the launch cwd (raylib caches it at `InitWindow`).

Landing with the netcode: `OB_ROOM`, `OB_NETSTAT`, `OB_LAG_MS`, `OB_JITTER_MS`, `OB_LOSS`,
`OB_NETSEED` (seeded xorshift, never `rand()`, so a bad case replays exactly), `OB_PORT`,
`OB_PEER`.

Web-only, read from the query string because the interesting failures only happen on a
real device: `?diag` draws `dpr / css / buf / screen / render` in screen space, and
`?nohidpi` falls back to CSS-pixel canvas sizing. They combine: `?diag&nohidpi`.

## Milestones

- **M0 done** — skeleton, both platforms building, Pages deploying, virtual-res renderer.
- **M1 done** — fixed-point sim, head-and-leg players with a pendulum leg, ball
  physics, goals, clock and phases, keyboard + touch input, render interpolation,
  physics measured from the original. Open: touch buttons overlay the goalmouths in
  landscape, and the Windows DPI viewport mismatch (fails safely - whole field
  visible, black bars).
- **M2 done** — determinism harness. `fixed_test`, `const_test`, and a golden replay
  diffed native-vs-wasm-under-node. That diff is the real gate; everything else is
  prevention.
- **M3 done** — rollback netcode. `rollback_test` proves it byte-identical to lockstep
  across 85 lag x loss x seed cases on a virtual clock; `net_udp.c` gives two native
  processes a real socket with injected lag/jitter/loss.
- **M4 done** — WebRTC transport over vendored Trystero. Confirmed by a real match
  between two devices across the internet, with no backend.
- **M5** — room UI (so nobody has to hand-edit a URL), sprites via the Aseprite
  pipeline, sound, match polish.

## On the original game

Mechanics are not copyrightable; sprites, audio and branding are. Reimplement the
mechanics with original art. Never commit the original's assets. Keep any local
reference material in `reference/` (gitignored).
