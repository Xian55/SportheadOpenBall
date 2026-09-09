cmake_minimum_required(VERSION 3.24)
project(openball C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(OB_DEV_TOOLS "Build headless tests + replay tools" ON)
# OFF for now: the whole development loop is two native console windows
# shouting rollback stats at each other. Flip it on for release builds.
option(OB_WIN_GUI   "Windows: GUI subsystem (hides the console)" OFF)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
# Once raylib is cloned, don't hit git again on reconfigure.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
FetchContent_Declare(raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(raylib)

set(OB_VERSION "dev" CACHE STRING "version string shown in the window title")

# ---------------------------------------------------------------------------
# Tier 1: the deterministic core. Shared VERBATIM by the game and every test,
# as an OBJECT library, so a test can never pass against differently-compiled
# sim code.
#
#   -fwrapv                  signed overflow must wrap, not be UB the compiler exploits
#   -Werror=double-promotion these three are the float tripwire: the sim is
#   -Werror=float-conversion integer-only, and a stray float is a silent desync
#   -Werror=float-equal      between the MinGW and wasm builds
# ---------------------------------------------------------------------------
add_library(ob_sim OBJECT
  src/sim.c
  src/checksum.c
  src/proto.c
  src/rollback.c)
target_include_directories(ob_sim PUBLIC src)
target_compile_options(ob_sim PRIVATE
  -fwrapv -Wall -Wextra
  -Werror=double-promotion -Werror=float-conversion -Werror=float-equal)

# ---------------------------------------------------------------------------
# The game
# ---------------------------------------------------------------------------
add_executable(openball
  src/main.c
  src/render.c
  src/input.c
  src/touch.c
  $<TARGET_OBJECTS:ob_sim>)
target_include_directories(openball PRIVATE src)
target_link_libraries(openball raylib)
target_compile_definitions(openball PRIVATE OB_VERSION="${OB_VERSION}")

# Transport is selected here and nowhere else.
if (EMSCRIPTEN)
  # src/net_web.c lands in M4.
else()
  target_sources(openball PRIVATE src/net_udp.c)
  if (WIN32)
    target_link_libraries(openball ws2_32)
  endif()
endif()

if (EMSCRIPTEN)
  # OUTPUT_NAME index so GitHub Pages serves the game at the site root with no
  # rename step and no redirect.
  set_target_properties(openball PROPERTIES SUFFIX ".html" OUTPUT_NAME "index")
  target_link_options(openball PRIVATE
    -sUSE_GLFW=3 -sSTACK_SIZE=1048576
    # Fixed 32 MB heap, NO memory growth. The sim is ~12 KB of rollback state
    # and v1 ships no image assets, so growth buys nothing - and skipping it
    # sidesteps the entire detached-ArrayBuffer trap class (TextDecoder and
    # texImage2D both reject views into resizable buffers, and a cached HEAPU8
    # goes stale on grow).
    -sINITIAL_MEMORY=33554432
    -sFILESYSTEM=0
    # Drop the node/worker/shell support code from the JS glue - this only ever
    # runs in a browser tab. emmalloc is a smaller allocator than dlmalloc and
    # is plenty for a game that allocates almost nothing after startup.
    -sENVIRONMENT=web
    -sMALLOC=emmalloc
    --shell-file ${CMAKE_SOURCE_DIR}/web/shell.html)
  # The shell is baked in at LINK time, so edits must force a relink.
  set_property(TARGET openball APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_SOURCE_DIR}/web/shell.html)
elseif (WIN32)
  if (OB_WIN_GUI)
    target_link_options(openball PRIVATE -mwindows)
    target_compile_definitions(openball PRIVATE OB_GUI_SUBSYSTEM)
  endif()
endif()

# ---------------------------------------------------------------------------
# Headless tests: raylib-free, and deliberately NOT linked against m. A stray
# sqrtf/fabs in tier 1 becomes a link error instead of a silent desync.
# ---------------------------------------------------------------------------
set(OB_TESTS fixed_test const_test replay_test proto_test rollback_test)

if (NOT EMSCRIPTEN AND OB_DEV_TOOLS)
  foreach(t ${OB_TESTS})
    if (EXISTS ${CMAKE_SOURCE_DIR}/tests/${t}.c)
      add_executable(${t} tests/${t}.c $<TARGET_OBJECTS:ob_sim>)
      target_include_directories(${t} PRIVATE src tests)
    endif()
  endforeach()
endif()

# The determinism gate proper: the SAME replay built to wasm and run under node.
# Diffing its output against the native run is the only thing that actually
# proves both toolchains compute bit-identical results - every other guard in
# this file is prevention, this is detection.
if (EMSCRIPTEN AND OB_DEV_TOOLS)
  add_executable(replay_test tests/replay_test.c $<TARGET_OBJECTS:ob_sim>)
  target_include_directories(replay_test PRIVATE src tests)
  set_target_properties(replay_test PROPERTIES SUFFIX ".js")
  # Plain node program: no canvas, no GL, no shell file.
  target_link_options(replay_test PRIVATE -sENVIRONMENT=node -sNODERAWFS=0)
endif()
