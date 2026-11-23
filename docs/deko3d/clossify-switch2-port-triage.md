# CL0SSIFY `switch2-port` triage for the Switch 1 port (FIX 155, AI-assisted)

**Question answered:** which changes in the Switch 2 fork are worth carrying into
`deko3d-migration` (Switch 1: deko3d 0.5.0 + libnx, one NRO per game), and which belong to
a future Switch 2 branch.

| | |
|---|---|
| Ours | `deko3d-migration` @ `937f12d6d` |
| Fork | remote `clossify`, branch `switch2-port` @ `e2bc7c9d1` (2026-11-20) |
| Real diff | 114 hunks / ~3500 lines in 7 topic files (the rest of the +16k stat is one generated PNG byte array) |
| Bucket **a** | portable as-is |
| Bucket **b** | portable with adaptation (or only meaningful together with something else) |
| Bucket **c** | not for this branch: Switch 2 only, fixes a failure mode that does not exist on 0.5.0 / with our 60 fps cap, or a regression |

Hunk numbers are those of the per-topic diff index (`/tmp/s2diff/INDEX.txt`, ephemeral).
To re-derive a topic diff: `git diff 937f12d6d clossify/switch2-port -- <paths of the topic>`.

The fork is a different SDK: kaiHEN + hbloader, libounce (libnx fork), drako3d (deko3d API,
Ampere backend), `uam2`, VI2, 100/120 Hz, 4K/1440p. Everything under `__SWITCH2__` /
`NINTENDO_SWITCH2` is bucket c by definition.

## A - renderer (`game/graphics/`), hunks 0-36

| Hunks | What | Bucket | Decision |
|---|---|---|---|
| 0, 22, 23 + queue half of 13 | queue command memory 64 KB -> 1 MB, flush threshold 8 -> 128 KB (`queue_maker_defaults`) | c | **Not ported.** The abort it fixes ("add-mem callback did not add enough command memory") only exists in drako3d. In the 0.5.0 we link, `Queue::addCmdMemory` blocks on the fence ring instead of failing (`third-party/deko3d/source/dk_queue.cpp`). The slice size is `commandMemorySize/16` (`dk_queue.h`), so a bigger ring also changes the fence-flush cadence: an untestable pacing change with no Switch 1 failure behind it. Revisit only with evidence of wrap-around stalls. |
| 1, 2, 3 | image pool 512 MB on S2 + `pool_mb()` | c / a | Not ported (S1 keeps FIX 119's 256 MB; the accessor only feeds a log string, hunk 32) |
| 4 | glow: re-alias the frame depth after a resize | b | **Ported** - in `dkgl::set_frame_size()`, the one place every frame-size change goes through |
| 5, 6, 7, 8 | `drop_dk_image()` on delete / respecify / renderbuffer resize | b | **Ported, gated.** The fork enables it for `__SWITCH2__` only; here it is on only when live resize is on (`gk_dk_resize.txt`). Off = P8, the old behaviour |
| 9, 10 | `gfx.cpp`: drako3d is the only renderer, no GL fallback | c | S2 only |
| 11 | `<algorithm>` | a | Ported (needed by `clamp_to_fb`) |
| 12, 17, 18, 21, 26 | S2 present-flip image (the S2 compositor does not flip for `OriginLowerLeft`) | c | S2 only. Switch 1's path already flips (`nwindowSetTransform(FLIP_V)` in deko3d's swapchain) |
| debug half of 13 | `dk_debug_cb`: a failed call is logged as "FATAL" (= forensic, on the card at once) | a | **Ported** |
| 14, 15, 16 | S2 heap / operation-mode globals, `set_frame_rate_above_60` (VI2), display-cap polling | c | S2 only |
| 19, 20 | `make_fb_layouts()`: layouts shared by `init()` and `resize()` | a | **Ported** |
| 24, clamp part of 25 | `clamp_to_fb()` in `blit_frame_to` / `blit_prev_frame_to` | a | **Ported** (a rect with a negative origin is passed through unchanged, as before) |
| resize part of 25, 27 | `resize()`, `fb_width()`, `fb_height()` | b | **Ported** without the flip image, plus a restore-previous-size fallback the fork does not have |
| rest of 27 | S2 display-cap declarations | c | |
| 28, 29 | `switch_game.h` include; `dk_pass_on` always true on S2 | c | |
| 30, 31 (`effective_target_fps`), 33, 34, 35 | S2 live-follow: TV resolution, 1440p, 100/120 Hz, "display Hz" in the stats line | c | S2 only |
| 33 (idea), 31 (log text), 36 | follow dock/undock live instead of "decided once at launch" | b | **Ported as a Switch 1 variant:** 1280x720 <-> 1920x1080 (the FIX 73c sizes) on an operation-mode change, opt-in |

## B - runtime / kernel / common, hunks 37-71

| Hunks | What | Bucket | Decision |
|---|---|---|---|
| 37, 43, 44, 51-55, 62-67 | multi-game launcher plumbing: `switch_game.{h,cpp}`, `SWITCH_HAS_GAME_DIR`, runtime `SWITCH_LOG_PATH`, `FileUtil`, `kmemcard`, `main.cpp` | b | **Not ported.** Only needed by the launcher; for per-game NROs it is behaviour-equivalent. It also turns `SWITCH_LOG_PATH(x)` into a temporary `std::string` (`(... + "/" x).c_str()`), a dangling pointer if ever bound to a `const char*` |
| 38, 40-42, 45, 46, wrap half of 92 | thread census (`--wrap=threadCreate`, report in `__appExit` / libounce `userAppExit`) | c | **Not ported.** It diagnoses NROs chained in one process (hbloader + launcher). On S1 the NRO runs as its own (title-takeover) process, so exit ends it. The wrap also scans 2 KB of raw stack above the caller's frame: a fault risk that cannot be tested from here |
| 39 | S2 `gk_maxres.txt` cap, 1080p handheld / TV resolution query | c | |
| 47, 60, 61, 72, 73 | `is_docked()`, `pc-switch-docked?`, Jak 1 "60 fps while docked" GOAL rule | c for S1 | **Not ported: moot.** FIX 8a caps S1 at 60 fps (`pc_set_frame_rate`) and the Switch carousel offers 30/60 only, so there is no >60 setting to park |
| 48, 49, 50 | `run_log.h`: `sdmc:/gk_sync_log.txt` makes every line forensic | a | **Ported** (opt-in diagnostic for whole-console power-offs) |
| 56-59, 68, 70 | kaiHEN JIT publish, S2 refresh rate, 120 fps cap | c | S2 only |
| 69, 71 | exit-step logging, `envSetNextLoad` -> launcher, `thpool.shutdown()`; `BS_thread_pool::shutdown()` | c | Not ported (same-process chaining again). The pool shutdown is the one piece worth revisiting if S1 ever chains NROs in one process |

## C - `goal_src/`, hunks 72-85

| Hunks | What | Bucket | Decision |
|---|---|---|---|
| 72, 73 | `pc-switch-docked?` + the Jak 1 docked-60 rule | c for S1 | see B |
| 74-77 | 100 fps carousel (S2 120 Hz screen) | c | S2 only |
| 78-85 | **remove** our FIX 126 / 128 / 129 (weather cheats, jak2 + jak3) | - | **Never port.** These `-` lines are ours: the fork's base predates those fixes, so they are regressions in the fork. C contains no fork-side fix to take |

## D - build, hunks 86-104

| Hunks | What | Bucket | Decision |
|---|---|---|---|
| 86, 91, 99, 100 | launcher build (`SWITCH_GAME=all`, NRO title/icon) | c | parked with E |
| uam half of 87, 88, 93-97 | `DK_UAM_EXE` parametrization (`--uam` for `_TF3_PY`) — a CMake cache variable, not a deko3d API name <!-- dk-lint: ok --> | c | only `Switch2.cmake` overrides it; on S1 it is the same `uam` path |
| EGL half of 87, 89, 90, deko3d half of 92, 101-104 | `Switch2.cmake`, `Switch2.Dockerfile`, EGL stubs, drako3d link, `game/switch2/include/deko3d.h` shim, `SWITCH2=1`, `SWITCH2_DK_DEBUG` | c | Switch 2 branch |

## E - multi-game launcher, hunks 105-111: bucket b, parked

`game/switch/game_select.cpp`, `launcher_ui.{h,cpp}`, `package-switch-launcher.sh`,
`gen_launcher_assets.py`, `launcher_preview.cpp`. Not Switch 2 specific in its API use
(`framebufferCreate` on `nwindowGetDefault()`, the pad API: all in Switch 1 libnx), but:

- the layout is hard-coded 1920x1080 (`launcher::kScreenW/H`). In Switch 1 handheld the panel
  is 1280x720 and a 1080p framebuffer is refused, so it needs a size-aware layout or a scale;
- it needs the B launcher plumbing above;
- it changes the packaging convention (one NRO in `sdmc:/switch/opengoal/` instead of one
  NRO per game folder).

That is a product decision and its own task, not a fix to cherry-pick.

## F - release, hunks 112-113: bucket c

`make-release-bundles.sh` + `.gitignore` build S2 player bundles (kaiHEN launcher NRO,
`switch2_sd_tool.py`, `build-switch2-*`). The script is a good template for a Switch 1
release later.

## What was ported (FIX 155)

With no flag file on the card the build behaves like `937f12d6d`, except three always-on
items: the "FATAL" mark on failed deko3d debug calls, `clamp_to_fb`, and the `make_fb_layouts`
refactor (no behaviour change).

| Item | Gate | Files |
|---|---|---|
| `dk_debug_cb` FATAL mark | always | `pipelines/dk_device.cpp` |
| `clamp_to_fb` in both frame blits | always | `pipelines/dk_device.cpp` |
| `make_fb_layouts` | always | `pipelines/dk_device.cpp` |
| `resize()` / `fb_width()` / `fb_height()` | `sdmc:/switch/<game>/gk_dk_resize.txt` | `pipelines/dk_device.{h,cpp}` |
| live dock/undock follow (720p <-> 1080p) | same file | `pipelines/dk_renderer.cpp` |
| `drop_dk_image` (no leaked frame-sized targets per resize) | same file | `dkgl/dkgl_texture.cpp`, `dkgl/dkgl_state.h` |
| glow frame-depth re-alias | automatic with a resize | `dkgl/dkgl_frame.cpp` |
| every-line forensic run log | `sdmc:/gk_sync_log.txt` | `switch/run_log.h` |

With the flag off, `drop_dk_image` is a no-op and nothing calls `resize()`.

Differences from the fork, on purpose:

- **opt-in.** The fork resizes by default because it was tested on its hardware. This has not
  run on a console yet: without the flag the build behaves exactly like `937f12d6d`.
- **restore fallback.** If the new size cannot be built, `resize()` rebuilds the previous one
  instead of leaving the renderer down.
- **re-alias placement.** The fork re-aliases the glow depth lazily in `glow_begin()`; here it
  is in `set_frame_size()`, which also covers a depth use before the first glow of the frame.

Why it is safe to rebuild a swapchain on the same window with deko3d 0.5.0: `Swapchain::initialize`
calls `nwindowSetDimensions` + `nwindowConfigureBuffer` per image and `~Swapchain` only calls
`nwindowReleaseBuffers` (`third-party/deko3d/source/dk_swapchain.cpp`) - the same sequence
`init()` / `shutdown()` already run.

## For the Switch 2 branch

Take the c bucket by path, not per commit (the fork's history does not cherry-pick cleanly):
`game/switch2/`, `cmake/toolchains/Switch2.cmake`, `docker/Switch2.Dockerfile`,
`scripts/build-switch.sh`, `game/CMakeLists.txt`, the root `CMakeLists.txt`,
`cmake/Deko3dShaders.cmake`, and the `__SWITCH2__` blocks in A/B. Then re-apply our FIX
126/128/129 on top (the fork dropped them).
