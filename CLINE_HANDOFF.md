# Handoff from Cline session (AI-assisted) — 2026-10-03, FIX 76d deployed (area prefetch hardened; f76c hardware results + 3 fixes)

Repo fildicio/jak-project-switch. Branch: **optmissation-openGoal-NX**.
`main` = F69. Working through PERF_PLAN_NEXT_AGENT.md — **step 1 closed**:
F70 (always-on core pinning) hardware-tested, then F70b (pinning opt-in via
`sdmc:/gk_pin.txt`) A/B-tested on the same workload: **pinning is NEUTRAL**
(98.7 vs 106.3 hitches/min = noise; see plan §"Step 1 FINAL VERDICT").
F70b was live until today; **FIX 71b** (PERF_PLAN step 2:
`appletSetCpuBoostMode(FastLoad)` scoped to blackout loads + `update_blocking`)
is now live on all three games — needs the hardware test below. F71 lasted
about an hour on the card: its watchdog thread `_exit(1)`'d every game at the
first boot blackout (the FIX 34c wall: no runtime `std::thread`, ~4 MB heap
free of 3.2 GB — see kmemcard.cpp FIX 35 notes); 71b is the same boost logic
with the thread removed. Steps 3 (pacing) and 4 (FSR) still queued.

## Card + naming convention (player-defined, standing)
- Games live in `sdmc:/switch/jak1/`, `sdmc:/switch/jak2/`, `sdmc:/switch/jak3/`.
  The live NRO is **`jakN.nro`** (lowercase, no space). Always refer to the
  games as jak1/jak2/jak3.
- Build artifact is `build-switch-jakN/game/gk.nro` — deploy it to the card
  **as `jakN.nro`** (rename at deploy time), after rotating the old live NRO
  to `Jak N.fNN.bak`.
- FAT32 is case-insensitive: `jak2.nro` and `Jak 2.nro` are the SAME file.
- Flag files: `sdmc:/gk_nopbo.txt` at CARD ROOT; **`sdmc:/gk_pin.txt` at CARD
  ROOT (NEW, FIX 70b)** — create it to turn core pinning ON, absent = all
  threads float (pre-F70 behavior). `sdmc:/switch/jakN/gk_no_vag.txt`.
- Desktop copies of every deployed build: `~/Desktop/jak bakcups/jakN.fNN.nro`.

## FIX 75 REJECTED (hardware, user) → reverted; FIX 76 area prefetch deployed
- **f75 (catchup-burst) is unplayable**: the assumption that stream-in loads
  happen while the old level is FROZEN was FALSE. jak1 area transitions
  (bridges/midsections) are PLAYABLE — EMA 33.3 ms during loads is live
  gameplay rendering, so the burst's 16-66 ms frames hit the player directly
  (user: "10 fps"). Loads finished in 7-8.7 s but at unacceptable cost.
  Reverted (commit `4c6f9a1cd`); f74c NRO restored to card before rebuilding.
- **Standing rule**: with the locked-30fps constraint the loader gets ~7 ms
  of vsync slack per frame (the f74c `catchup` rate, user-validated). A 3.5 s
  upload CANNOT be made faster once started — it must START EARLIER.
- **FIX 76 (area prefetch)**: while the game plays normally and the loader is
  idle, quietly prefetch the most likely NEXT area at the proven catchup rate.
  When the player crosses, `set_want_levels` finds the level already in
  `m_loaded_tfrag3_levels` → nothing queued → `pending=0` → zero loader cost
  at the crossing and no LoadBoost dip: instant area, locked 30 fps. GOAL
  never reads loader state back (only pushes `__pc-set-levels`), so the extra
  cached level is invisible to the game — verified in
  goal_src/jak1/engine/level/level.gc (only `__pc-set-levels`, no poll).
- Prediction = runtime-LEARNED transitions (every observed desired-set change
  records an edge; works for all games, heals bad guesses) over a static
  jak1 overworld table (`kJak1LevelAdjacency` in Loader.cpp).
- Safety: prefetch cancelled within one frame if the game asks for anything
  else (FIX 63 unload path / loader_thread post-read abandon); never starts
  during blackout, under buffer-pool pressure, above live=5, or past 2 cached
  prefetches; every target is `fs::exists`-checked (missing fr3 = fatal).
  Evicted prefetched levels are session-retired (no thrash loop). Pure-prefetch
  work does NOT trigger LoadBoost streaming (`game_pending` vs `pending` in
  update_frame_budget) — a hidden load must never show as a resolution dip.
- Log signatures: `[loader] prefetch: <name> cached (budget catchup)`,
  `prefetch: dropped ... mid-staging`, `prefetch: abandoned ... after file
  read`, and `| pf loading <name>` / `pf N cached` in the [loader] telemetry.
  Success on hardware = crossing a prefetched area shows NO `ready in` line at
  all (level was resident) and ema stays ~33.3 ms throughout.
- jak1 f76 = f74c + revert + FIX 76. jak2/jak3 still on f74c-less line —
  carryover: extract BCn fr3 + rebuild + deploy for both.


## Card state — LIVE: FIX 76d (area prefetch hardened), jak1 only
| game | live `jakN.nro` md5 | rollbacks on card |
|---|---|---|
| jak1 | `2189e2178eb4a381a860ac9612446ff9` (f76e) | `Jak 1.f76d.bak`=`118ef137`, `Jak 1.f76c.bak`=`3fd59674` (prefetch works, purge/dispatch bugs), `Jak 1.f76.bak`=`196d47f0`, `Jak 1.f74c.bak`=`047309a3` (known good, pre-f75), plus older f62..f71b chain |
| jak2 | `3de120596b0a3851c6a9206686285ce7` | `Jak 2.f71b.bak`=`95921de8` (boost, good), `Jak 2.f71.bak`=`e7202346` (**BAD**), `Jak 2.f70b.bak`=`d56a98bb`, `Jak 2.f70.bak`=`576599fd`, `Jak 2.f69.bak`=`59041473` |
| jak3 | `eacc630632f343b5a4c48fad1764fedf` | `Jak 3.f71b.bak`=`49012ce4` (boost, good), `Jak 3.f71.bak`=`bd064d8f` (**BAD**), `Jak 3.f70b.bak`=`6b17e8c2`, `Jak 3.f70.bak`=`97910d0d`, `Jak 3.f68.bak`=`9e6e3b1d` |

Desktop copies: `~/Desktop/jak bakcups/jak1.f76.nro` (md5 == live, verified). Also `jak1.f75.nro` (rejected) and prior fNN chain.
F72 = F71b + PERF_PLAN step 3. `pc_get_display_mode()` reports `fullscreen`
on Switch (kmachine.cpp) so GOAL's `set-frame-rate!` stops force-clearing
`vsync?` (it had persisted `(vsync #f)` into pc-settings.gc — flipped back to
`#t` on card for jak1/jak3, backups `pc-settings.gc.f72.bak`), and
`pc_set_vsync()` clamps to true on Switch (one-shot log `[vsync] Switch:
vsync is the pacing mechanism, ignoring request to disable`). Also fixed the
`[vsync]` log printing garbage for target_fps (missing `(int)` cast on a
float, opengl.cpp). Expected log signature: `[vsync] requested=2 set_ok=1
actual=2` and **no later `requested=0`**; `[cam] dt` should quantize to
33/50/67 ms (the 36-49 ms limiter-overshoot band must vanish). GOAL-side
vsync menu entry is inert on Switch now (cosmetic, documented in FIX 72).
F71b log signature: `[boost] cpu boost ON ...` then `[boost] cpu boost OFF
after N ms ...` around every blackout load. Standing rule re-learned 2026-10-03:
**never create a runtime `std::thread` in this port** — the GOAL heap leaves
~4 MB free, pthread stack allocation fails, and the game `_exit(1)`s.

F70b = F68/F69 texture path + `[cores]` diagnostics always-on, pinning only
if `sdmc:/gk_pin.txt` exists (see game/switch/platform.cpp FIX 70b comment
for the hardware numbers that forced the default-off).
Log signature when pinning is OFF (expected on the card now): one line
`[cores] pinning disabled (create sdmc:/gk_pin.txt to enable)` and per-thread
`[cores]` reports showing wherever the scheduler floats them.

## Key metrics + baselines (corrected after F70b A/B, 2026-10-02)
- **Hitch accounting** (primary felt-metric): `[cam] ... HITCH dt=` lines in
  `gk_run_log.txt`. Per-minute rates MISLEAD across different session lengths;
  always report duration + total + max-in-15 s, same-area sessions only.
- jak3 Haven-streaming reality on EVERY build F62..F70b: ~250-275 hitches
  per streaming session (45-50 ms band), plus 1.3-3 s level-load freezes
  (pre-existing since FIX 16 era). Pinning on/off changes none of it
  (A/B: F70 98.7/min vs F70b 106.3/min same workload = noise).
- Loader ema p50/p90 (gk_stdout `[loader] ... ema` lines): jak3 baseline
  36.4/42.3 ms; "slow setup" lines ~10-12 ms vs 8 ms budget during streams.
- gk_stdout.txt keeps only the last boot (truncates); gk_run_log.txt APPENDS
  across boots — segment by `session start 7x` lines; F70+ boots have
  `[cores]` lines, F70b boots say `pinning disabled`.
- /Users/filippo/hitchrate.py prints the full per-boot hitch table for a game's
  gk_run_log.txt (python3, edit path at top).

## Next queue (PERF_PLAN_NEXT_AGENT.md order)
- **Step 2 DEPLOYED (F71b), HARDWARE-VALIDATED 2026-10-03 (AI-assisted)**: CPU boost during loads —
  `appletSetCpuBoostMode(FastLoad)` while a blackout load / `update_blocking`
  is active, `Normal` right after (see plan §Step 2 DONE note for the hooks).
  Results (pos-matched HITCH dt, F71b session vs old sessions in same logs):
  jak1 clean (worst 293 ms); jak2 boot freeze ~3.6-4.2 s -> **3.15 s**, forest
  2.5-3.5 s -> **1.57 s** (~20-45% cut); jak3 boot freeze ~3.3 -> 3.12 s
  (marginal). Verdict: keep — real win on jak2, no regressions, but the
  remaining freeze time is GPU-upload + SD-IO bound (FastLoad clamps GPU to
  min clock while the loader is still uploading textures — jak3 Haven City is
  the most upload-heavy), so don't iterate further here; step 3 (pacing) and
  step 4 (FSR) attack the actual bottleneck.
- **Step 3 DEPLOYED (F72), HARDWARE-VALIDATED 2026-10-03 (AI-assisted)**: even 30 fps
  pacing via vsync interval 2. Root cause found in logs: C++ defaults vsync ON
  and boot reaches `requested=2 actual=2`, but GOAL's `set-frame-rate!`
  (pckernel-common.gc:81) force-clears `vsync?` because Switch reported
  `windowed` while refresh 60 != target 30 — persisted `(vsync #f)` into
  pc-settings.gc — and `update-to-os` then pushed vsync off every frame
  (`[vsync] requested=0` at t~11-16 s), leaving the sleeping limiter to pace
  (45-55 ms spread in every [cam] histogram). Fix is C++-only (no GOAL
  recompile): report `fullscreen` from `pc_get_display_mode()` + clamp
  `pc_set_vsync()` true on Switch + `(vsync #t)` re-flip on card (jak1/jak3).
  Test = run each game a few minutes; verify `[vsync] requested=2 ... actual=2`
  persists (no `requested=0` line later in the session) and `[cam] dt` bins
  move to 33/50/67 ms with the 36-49 ms band gone. jak2/jak3 heavy areas may
  still show 50/67 ms slips (GPU-bound — that is step 4 FSR's job, not pacing).
- Step 4: FSR (jak2's main win); Step 5: city traffic
  density (jak3); Step 6: LOD preset; Step 7: precompressed textures.

## Operational reminders
- Build the three games SERIALLY, never concurrently. Docker
  `devkitpro/devkita64`, mount repo at **/work** (`-v "$PWD:/work" -w /work`),
  e.g.: `docker run --rm -v "$PWD:/work" -w /work devkitpro/devkita64:latest
  env SWITCH_GAME=jak3 BUILD_DIR=/work/build-switch-jak3 bash scripts/build-switch.sh`
  (local `cmake --build` fails: caches were created under /work in-container).
- The Switch's clock is ~1 day ahead of the Mac; date FATAL/ASSERT entries by
  their position relative to the first `[cores]`-bearing boot, or by file
  mtimes, not by trusting "today".
- Deploy md5-verify (card md5 == build md5) and rotate `.bak`s BEFORE copy;
  sync; eject via Finder if `umount` says Operation not permitted.
- AI usage disclosure: append `(AI-assisted)` to every commit message.

## 2026-10-03 addendum — FIX 73c deployed (branch **upscale**, FSR follow-up)

F73/73b (FSR EASU upscaler + panel-height menu labels) verified working on
hardware. **F73c removes the handheld double-resample**: with the old
always-1080p swapchain, a 540p frame did EASU 540→720, bilinear 720→1080,
then vi 1080→720 (two wasted resamples). Changes:

- `SWITCH_RES_OVERRIDE` back to **1** (platform.h): window/swapchain is
  created at the operation-mode size (720 handheld / 1080 docked). This is
  the FIX 7f A/B toggle; the *other* 7f mitigation (capped probe logging)
  stays, and the real 7c killer (kernel-thread SD write storm) is long
  fixed. If a dispatch-start fatalThrow returns, flip it back to 0 —
  `__wrap_nwindowQueueBuffer` logs the rejected vi op in gk_fatal.txt.
- `DisplayManager::set_display_mode` / `set_window_size` are **no-ops on
  Switch**: the swapchain size is decided exactly once at window creation.
  This closes the 2026-09-11 resize-crash class AND stops GOAL's boot-time
  application of saved "fullscreen 1920x1080" from resizing the 720p
  handheld window right back to 1080p (that call really does run at boot:
  "[DISPLAY] Setting to display mode: 1, with window_size: 1920,1080").
- `pc_get_active_display_size` reports the size the window was **created**
  with (not live opmode), so docking mid-session cannot desync GOAL's draw
  region from the framebuffer (mid-session dock = vi upscales 720→1080
  until the next boot; correct but soft).
- FSR renderer logic unchanged: with a 720p handheld draw region the
  mid-FBO path is skipped automatically — EASU 540→720 lands 1:1 on the
  panel, single pass. Docked 540p keeps EASU→720 FBO + bilinear→1080.

Deployed to all three games (md5-verified, previous live NROs rotated to
`Jak N.f73b.bak`, desktop copies `~/Desktop/jak bakcups/jakN.f73c.nro`):
jak1 `466528d998692e1f3a870b18cf52f539`, jak2 `52b0b444055e3a217b59fe4060062ab7`,
jak3 `051753bcb7c30aea4590b9021e63505c`. CGOs untouched (no GOAL changes).

Hardware test for the user: boot each game handheld — expect
`[disp] create_window 1280x720 (res_override=1)` and
`[fsr] EASU 960x540 -> 1280x720` (no "then bilinear", panel 1280x720);
docked should log `create_window 1920x1080` and the mid-FBO path for 540p.

## 2026-10-01+ — F73c hardware-confirmed; next step chosen: STEP 7 (AI-assisted)

User confirmed the f73c NROs work on hardware (handheld + docked). The user's
remaining #1 issue is the **new-area streaming slow motion**. Analysis of the
FIX 9/33/42/48/68/69 history (see `STEP7_COMPRESSED_TEXTURES_DESIGN.md` §1)
shows every cheaper lever is exhausted — FIX 69 (PBO) was hardware-rejected —
so **step 7 (pre-compressed BCn fr3 textures, FIX 74)** was selected.
Design note written: `STEP7_COMPRESSED_TEXTURES_DESIGN.md` (repo root),
**awaiting user approval before implementation** per the plan's step-7 rule.
Steps 5/6 (Haven traffic / LOD) remain queued but address constant city load,
not streaming. Also pending: push the `upscale` branch (cf995cb33).


## 2026-10-02 — FIX 74 implemented, jak1 deployed (AI-assisted)

User approved step 7. FIX 74 (commit 8990981ee, branch `compression-ecc`):
BC1/BC3 fr3 textures with file mips, TFRAG3_VERSION 44.

Deployed on the card, md5-verified, `._` files purged:
- jak1 NRO `a2236f70...`
- all jak1 fr3 files
- old NRO kept as `Jak 1.f73c.bak`
- full pre-FIX74 data backup at `~/Desktop/jak bakcups/2026-10-01-pre-FIX74-full/`

jak2/jak3 are untouched (F73c).
**Next: user hardware test of jak1.** Do NOT convert jak2/jak3 until the user reports back.

## 2026-10-03 addendum 2 — f76c hardware verdict + FIX 76d (AI-assisted)

User tested f76c: crossings WAY better ("way better than before"), but (a) a
small fps hiccup still perceptible while a prefetch stages, (b) after warping
to Geyser Rock the game crawled ~10-15 fps EVERYWHERE for a while, and (c)
the prefetch stopped working afterwards ("the forbidden forest wasn't loaded
like it did at the beginning").

Log forensics (gk_stdout.txt + gk_run_log.txt, session 7x):
1. **Feature death at the warp exit**: `blackout purge: recycling 3 retired
   level(s)` at every warp — purge takes everything not active/desired, and
   caches are NEVER desired, so it ate the beach+jungle caches AND
   session-retired them (76b rule). Next beach crossing: `ready in 22.47s`, no
   re-prefetch ever. FIX 76d: purge skips `m_prefetch_resident` names; the
   FIX 63 reclaim-on-real-allocation-failure path remains the memory valve.
2. **The crawl**: warp loads training (blackout) + village1 as an
   "additional level" that staged DURING play at catchup-floor. The texture
   stage's dispatch-count clamp has a floor of 4 checked AFTER dispatch, and
   one dispatch costs 1-15 ms of nouveau driver time → every frame 60-100 ms
   (ema 55-67 ms sustained, 43 hitches ≥100 ms in run log). FIX 76d: new
   `g_loader_budget.dispatch_cap` retuned per frame — 1 while staging a pure
   prefetch (plus skip staging entirely when frame ema >34.5 ms), 2 when a
   game load pushes ema >45 ms (non-blackout), 20 otherwise; blackout
   uncapped. FIX 38 floor untouched (progress ≥1 dispatch/frame always).
3. **Prediction noise**: after the Geyser Rock trip the static picker fell
   through to rank-3 `training` (twice: `dropped training mid-staging` ×2,
   wasted staging + hitches). FIX 76d: static table now priority-only
   (each area's #1 entry; learned graph decides otherwise) + 60 s cooldown
   on cancelled targets (`m_prefetch_cooldown`).

Commits: `e8bad800f` (code). NRO f76d = `118ef137a7c4e816214069ec9936dd04`,
verified on card; rollback `Jak 1.f76c.bak`=`3fd59674`.

**f76d hardware validation checklist** (expect):
- `pf N cached` still appears right after boot in village1; crossings to
  cached areas = no `ready in` line.
- Geyser Rock warp: arrival crawl replaced by at most brief dips — no ema
  55-67 stretch, no 60-100 ms hitch cluster in `[cam] HITCH`.
- Warp EXIT keeps caches: next crossing after a warp should still be instant
  (no `ready in 20+s` after `blackout purge`).
- No repeated `dropped X mid-staging` for the same X within a minute.
- Prefetches take ~2-4x longer to cache (dispatch_cap 1) — that is intended.

## FIX 76e — prefetch must never cost a frame (AI-assisted)

Problem: f76d hardware showed "terrible fps in Geyser Rock". The prefetch staged at catchup-pace (7-8 ms) on frames with ~0 slack, and the 34.5 ms EMA gate produced a sawtooth.

Fixes:
- **pf-lean budget.** When the ONLY thing in flight is the prefetch, the budget is 2 ms / 256 KB.
- **Miss backoff.** A real missed frame (raw gap > 40 ms) pauses prefetch staging for 1 s, doubling per repeat up to 8 s (240 frames). It resets after 60 clean staged frames.
- **Big-texture deferral.** During a pure prefetch, textures over 384 KB are deferred. If the same texture has blocked for 90 frames, it is pushed through anyway.
- **Static prediction.** The table now takes the top-2 entries (76d's #1-only never prefetched the jungle).
- **Guard.** `m_in_update_blocking` keeps all of these throttles out of the post-blackout sync sweep, where m_blackout is already false.

Build and deploy:
- NRO f76e = `2189e2178eb4a381a860ac9612446ff9`, verified on card.
- Rollback: `Jak 1.f76d.bak`=`118ef137`.
- jak1 only. jak2/jak3 are untouched and still have no prefetch / BCn.

**f76e checklist:**
- Geyser Rock and village1 dwell: no 36-41 ms budget oscillation; budget lines show `mode=pf-lean`.
- `pf N cached` still appears; it just takes longer.
- Crossing village1 -> jungle on a fresh boot is instant once cached.
- No regression in the warp/blackout load times (`ready in`).

## FIX 76f — Geyser Rock crash + island prefetch (AI-assisted)
- Crash: stack overflow in `joint-exploder-method-28` (recursive bbox split). A joint that moved >20480 units in one long frame never changes side, so it recursed on the same list forever -> null pc. Fix: depth cap 32 (`*joint-exploder-split-depth*`), in GAME.CGO.
- Loader: no prefetch while on training/misty, and they are never prefetch targets (static table + learned). The f76e log showed a beach prefetch on Geyser Rock with 6-11 ms texture stages at ema 39 ms.
- Host goalc build fix: `make_texture(..., true, false)` in goalc/build_level (FIX 74 signature).
- SD: jak1.nro md5 791051f05ccfaa078780f1e6667505dd (rollback `Jak 1.f76e.bak`), GAME.CGO d29d23c9676693f6e92d028f176468a8 (old one in ~/Desktop/jak bakcups/GAME.CGO.f76e.bak).

## FIX 77 — Geyser Rock is GOAL-bound + [goal] profiler (AI-assisted)
- f76f capture (R3+Minus on Geyser Rock): render only 10-14 ms, but `wait_dma` 35-40 ms and `starved=25-37` => the GOAL engine, not the GPU, is over budget. Worst near the warp-gate start area (x≈-5.37M z≈4.37M), even standing still; east side is locked 30. Loader idle (no prefetch).
- Added `[goal]` lines to gk_run_log (Switch, only while R3+Minus diag is on): aggregates the existing jak1 pc-prof events (per process type + display-loop phases) and logs top self-times every 2 s. kmachine.cpp `pc_prof`.
- SD: jak1.nro md5 59dbea88afff5f7f4b2ba8da72820993 (rollback `Jak 1.f76f.bak`). f76f logs saved in ~/Desktop/jak bakcups/logs-f76f-geyser/.
- Next: read `[goal]` from a Geyser Rock capture and fix the named process.

## FIX 78 (AI-assisted) — Geyser Rock particle lag

- Cause: `check-drop-level-training-spout-rain` launches 2 splash particles per dying drop from inside
  particle processing, so they go through `*sp-launch-queue*` (32 entries). At 30fps it overflowed
  constantly; each overflow did `format 0 "ERROR: ... queue is full"`, which goes to the SD game log
  (data/log/jak1.*.log) with an fflush per line. 13,157 such lines in one session; FIX 77 `[goal]`
  showed process-particles at 40-50ms/frame (max 130ms) vs ~1.6ms normally.
- Fix (goal_src/jak1/engine/gfx/sprite/sparticle/sparticle-launcher.gc): SPARTICLE_QUEUE_SIZE 32 -> 64
  (PS2 launches-per-second at 30fps), overflow now silent.
- Deployed ENGINE.CGO 4996e971 + GAME.CGO 6ea857fb (backups ~/Desktop/jak bakcups/*.f77.bak). NRO unchanged.
- Note: any other per-frame `format 0` spam is equally expensive on Switch; check data/log/*.log.

## Next: jak2/jak3 rollout (AI-assisted)
See `JAK2_JAK3_ROLLOUT_PLAN.md`. Blocker: TextureAnimator (jak2/3 only) reads RGBA pixels from GAME.fr3, so BCn extraction must keep those textures RGBA first.

## FIX 79: jak2 kid-escort crash + jak3 artifact-race crash (AI-assisted)
Crash dumps were symbolized against `build-switch-jakN/game/gk` (it matches the deployed F73c NROs).
Only the dumps with svcBreak `lr_off=0x1138` come from the current NROs; the older entries are from earlier builds.

**jak2: crash when the kid/crocadog escort mission loads (`lkiddoge`).** Two sessions (Oct 4 and Oct 5):
`dgo file header kid-escort has overrun heap by 12016 bytes`, followed by a crash in `klink.cpp:604` (registers hold "kid-escort").
- Cause: `lkiddoge` borrows ctywide slot 0 = `#x17c` KB × BORROW_MULT 1.1 = 418 KB. The arm64 DGO is 525 KB (the PS2 original is 262 KB).
  `lerlchal` (520 KB) has the same problem; `lmeetbrt` fits with only 33 KB to spare.
- Fix: in `goal_src/jak2/engine/level/level-info.gc`, ctywide `:borrow-size` goes from `#x17c #x82f` to `#x1fe #x7ad`.
  Slot 0 becomes 561 KB; slot 1 becomes 2161 KB (lwidea peak is 2127 KB). The total is unchanged, so ctywide's own heap is unchanged
  (that heap has only about 68 KB free, so don't take room from it).
- Deployed: jak2 `GAME.CGO` 6f4e27f7…

**jak3: GOAL crash in the desert artifact race (`desrace1`).** pc = EE+0x2431470, which is inside `artifact-race.o`.
- far = garbage root (0xc4001a17) + 0x9c, which is `root-prim` (offset 0xa0, minus 4 for the basic tag).
- Cause: `was-artifact::check-pickup` dereferences `(handle->process (-> *target* pilot vehicle))` without a null check.
- Fix: a guard on all 5 copies of this pattern: artifact-race, desert-chase, desert-jump, des-bush, forest-ring-chase.
- Deployed: 18 jak3 DGOs (DESRACE1/2, DESCHASE, DESJUMP, FRSTA, LBB*).

Backups: `~/Desktop/jak bakcups/f79/`. NROs are unchanged.

**Not fixed (need more data):**
- jak3 `DmaFollower` `tag.addr == 0` assert (Oct 4, desert-artifact-race-2 intro / warpcast).
- jak3 jump to a garbage PC from `nouveau_fence_trigger_work` (Oct 5, Haven ctysluma, 57 minutes in).
Both look like memory corruption in the render path. There is no root cause yet.

## FIX 80: jak3 audio — repeated sounds and music cutting out (AI-assisted)
User report:
- The intro cutscene's first sound repeats several times before the cutscene starts.
- Haven City music sometimes stops.
- Zoomer sounds sometimes don't trigger.
- jak1/2 don't have these problems.

Cause: jak3's overlord refills streamed audio only from the IOP vblank thread
(`VBlankThread` → `CheckVagStreamsProgress` → `ProcessStreamData`).
- Our IOP vblank came from `Gfx::register_vsync_callback`, i.e. once per *rendered frame*, and it collapsed to a single bool.
- During a loading freeze (jak3 has 1–3 s freezes), no refills happened.
- The SPU mixer looped the old 0x2000-byte stream halves, which is the repeated audio.
- `CheckVAGStreamProgress` then saw the play position behind the data, returned 0 and `StopVagStream`, which is the music stopping.
- jak1/2 refill their streams differently.

Fix:
- `IOP_Kernel` gets a 60 Hz timer vblank (`set_timer_vblank`), polled at the top of `dispatch()` and between threads. The IOP loop already wakes every ≤1 ms.
- It is enabled for Jak3/JakX in `runtime.cpp`, and the render vsync signal is ignored in that mode.
- In `vblank_handler.cpp`, the per-tick stream clock divides by 60 instead of `g_nFPS`, because ticks are no longer per frame.

Deployed NRO = **F73c (cf995cb33) + this patch only**, built in a worktree, so it stays compatible with the v43 fr3 files on the card.
- md5 `a7a778db…`; ELF in `~/Desktop/jak bakcups/f80/gk-f80.elf`.
- Rollback: `Jak 3.f73c.bak` on the card (also in `~/Desktop/jak bakcups/f80/`).
- The zoomer-sound symptom is not proven to have the same cause. Re-check it after the user tests.

## FIX 81 — Free secrets / cheats, level select from start (jak2 + jak3) (AI-assisted)

User wanted to skip missions without the debug menu (debug segment costs perf). GOAL-only, GAME.CGO only.

- jak2 `progress.gc` `menu-update-purchase-secrets`: every secret auto-purchased (no task/orb gate) except hero-mode.
  `'auto` items (gungame-blue/dark, reverse-races) get enabled as before; toggleable ones stay off.
- jak2 `progress-draw-pc.gc`: names shown for purchased secrets (not "????????").
- jak2 `pckernel.gc` `update-cheats`: PC cheats always revealed/purchased/unlocked; toggles stay off.
- jak2 `progress-draw.gc` `memcard-unlocked-secrets?`: title Secrets menu always unlocked (level select,
  scene players, scrapbooks = #x0fe0, no hero-mode). Level Select = mission select from the title screen.
- jak3 `progress-draw.gc`: method-12 no story lock (except hero-mode); shown cost 0; title Secrets menu always
  unlocked (level select 1-3, scene players, scrapbooks, model viewers; not commentary).
- jak3 `progress.gc`: buy always allowed, orbs not deducted. Player still chooses what to buy.
- Deployed GAME.CGO for jak2 (ebbf6ba9) and jak3 (27f26752). Backups: ~/Desktop/jak bakcups/f81/.

## FIX 82 — In-game "skip to mission" keeping the save (jak2 + jak3) (AI-assisted)

Pause menu -> Secrets -> Level Select (jak3: Level Select Act 1/2/3, buy free first) opens the select-start list.
When `starting-state != 'title`, confirm calls `play-task idx #f 'play` (no `play-clean` -> `initialize! 'game`
reset), so orbs/gems/purchases/perms are kept; `task-node-open!` closes the prerequisite nodes (and runs their
eval-add item grants). Missions whose play-node is already closed are blocked (low beep) = forward-only.
Title-screen Level Select unchanged (fresh game). Files: jak2 progress.gc + pc/progress/progress-pc.gc,
jak3 progress.gc. GAME.CGO jak2 239c83fb, jak3 ba802585; backups ~/Desktop/jak bakcups/f82/.
Risk: skipping while inside an active mission/race; untested on hardware.

## FIX 83 — jak3 crash C: DirectRenderer vertex ring buffer (AI-assisted)

Three jak3 crashes (Spargus/wascity return, ~19 min) died inside nouveau: `nouveau_fence_emit` jumping to a garbage
PC via `nouveau_fence_wait/next` <- `nouveau_scratch_more` <- `st_bufferobj_data` <- `DirectRenderer::update_gl_*`.
Every DirectRenderer flush did `glBufferData` with a new size -> fresh pipe buffer + old one released through fence
work, hundreds of times per frame. Switch-only now: fixed-size buffer used as a ring, upload with
`glMapBufferRange(WRITE|UNSYNCHRONIZED)` (fallback glBufferSubData), `glDrawArrays` first = ring offset, orphan
with same-size `glBufferData(nullptr)` only on wrap. Safe: regions are never rewritten before the orphan.
Note: `gk_fatal.txt` pc_off/lr_off are relative to `get_memory_info`; add 0x9e510 (f80 ELF) for the gk offset.
Deployed jak3.nro = cf995cb33 + FIX 80 + FIX 83 (md5 02b7d29f). Backups ~/Desktop/jak bakcups/f83/ (+ gk-f83.elf),
card `Jak 3.f80.bak`. jak1/jak2 NROs not rebuilt yet (same code path, untouched until jak3 is confirmed).

## FIX 84 — jak2/jak3: DirectRenderer ring never re-specified, fence-synchronized (AI-assisted)

FIX 83 did not fix the crash. Evidence from the card on 2026-10-06 (f83 NRO):
- jak3 21:46 session died at 1122 s with **erpt 2520-0000 = GPU MMU fault** (GR unit, read, VA 0x6bff4ce000, fault
  type 3). This was the first GPU fault ever recorded on the card. It is in `atmosphere/erpt_reports`, and there is no
  crash_report or gk_fatal for it. Silent session ends: check erpt_reports first.
- jak3 23:03 session crashed at 1135 s: far=0x8 in libdrm_nouveau `bo_map_hash`. The call path is `pushbuf_kref` <-
  `pushbuf_validate` <- `nvc0_draw_vbo` <- `DirectRenderer::flush_pending` (glDrawArrays). The bufctx held a **NULL bo**.
- Cause: FIX 83 orphaned on wrap with a same-size `glBufferData(nullptr)`. Mesa turns that into `invalidate_resource` ->
  `nouveau_buffer_reallocate`, which swaps the BO under the same pipe_resource. Stale vertex state then references the
  freed BO, giving the GPU fault and the NULL/dead bo on the CPU side. The pre-83 code (new-size glBufferData per flush)
  died in the deferred-free fence work (crash C). Both crashes are the same family: buffer storage is reallocated and
  freed while it is still referenced.
- Fix (`DirectRenderer.cpp/.h`, Switch only): storage is allocated once and never re-specified. The ring is split into
  segments (segment = min(buffer, 1 MB), at least 4 segments, so the ring is at most ~4 MB larger than before).
  Leaving a segment drops a `glFenceSync`. Entering a segment waits on its fence (glClientWaitSync, falls back to
  glFinish). An upload larger than a segment drains the whole ring and restarts at 0. Writes still use an
  unsynchronized map.
- jak2 had an "error screen" at ~22:14 in mincan, 10 s after an auto-save. It left no dump and no erpt, so there is no
  hard evidence. jak2 got the same fix, because it still ran the pre-83 per-flush glBufferData path.
- Built in worktree `../jak-f84-wt` = cf995cb33 (F73c) + FIX 80 game diff + DirectRenderer from HEAD.
  - jak3.nro `10649c6a170ad756be73878429af7304`, rollback `Jak 3.f83.bak` (02b7d29f).
  - jak2.nro `e079129aed20183069471f6c400dc9fb`, rollback `Jak 2.f73c.bak` (52b0b444).
  - ELFs and backups are in `~/Desktop/jak bakcups/f84/`.
  - gk_fatal pc_off is relative to get_memory_info. In the f84 jak3 ELF, look up its offset with `nm`.
- If it still crashes, read `atmosphere/erpt_reports` (GpuError*) as well as crash_reports and gk_fatal.

## FIX 85 — jak2 BCn textures (v44 fr3) + prefetch + GOAL fixes; desktop "boot crash" ROOT-CAUSED (AI-assisted)

Scope (source already in tree at HEAD): `decompiler/level_extractor/extract_level.cpp` (BCn fr3,
TFRAG3_VERSION 44), `game/graphics/opengl_renderer/loader/Loader.cpp` (BCn upload path + kill
switch `#ifdef __SWITCH__` — dead on macOS), `goal_src/jak2/engine/anim/joint-exploder.gc`,
`goal_src/jak2/engine/gfx/sprite/particles/sparticle-launcher.gc`.

### The desktop jak2 boot crash was NOT a regression — it was a missing build flag

After rebuilding all 2683 jak2 GOAL targets, desktop jak2 died during kernel boot (EE thread at a
bad PC in unsymbolized GOAL heap code, right after RPC/IOP-CD init). Everything was exonerated in
turn (texture-name errors, gk binary, FIX 79–85 C++, GAME.CGO bisect, clean-vs-incremental cache).
The smoking gun: KERNEL.CGO shrank 119024 -> 101760 bytes with zero kernel source changes, and
**every** object file differed from the Sept backup.

**Root cause: the rebuild ran `(make-group "iso")` WITHOUT `--instruction-set arm64`.** goalc
defaulted to x86-64 and emitted x86-64 GOAL machine code. The host `gk` on this arm64 Mac (and the
Switch NRO) execute GOAL code natively as **aarch64** — so the kernel crashed on first execution.
Not Xcode, not a goalc miscompile, not FIX 85.

Proof chain:
- Old (Sept, boots) iso + ONLY the new KERNEL.CGO -> still crashes (T1 bisect).
- Rebuild with `--instruction-set arm64` -> KERNEL.CGO is **119024 bytes again (exact Sept size)**,
  all 2683 targets in ~18 s.
- Boot test: title -> `[texfmt] FIX 74 BCn compressed texture path active` -> ctywide/lwidea/ctysluma
  streaming, 2200 draws/frame, stable >60 s. Log `/tmp/gk_jak2_arm64c.log`. Only errors are the
  pre-existing "Multiple textures named" ones (byte-identical counts in Oct 1 card logs).

**Standing rule: every GOAL compile for this port MUST pass `--instruction-set arm64`** (see
PERF_PLAN_NEXT_AGENT.md): `./build-host/goalc/goalc --game jak2 --instruction-set arm64 --cmd '(make-group "iso")'`.
jak1 boots at HEAD only because its data was built with the flag. A stale obj cache built without
the flag is poison — wipe `out/jakN/obj` before rebuilding if unsure (build is deterministic:
incremental == clean, byte-for-byte).

### State / leftovers from the debug session
- `out/jak2/obj` + `out/jak2/iso` = clean arm64 FIX 85 build (good; boots).
- `out/jak2/iso.f85` = the **bad x86-64** build — delete it, do not deploy.
- /tmp: `GAME.CGO.f85-clean.bak`, `GAME.CGO.mixed.bak` (both x86-64), `goalc_arm64_build.log`,
  `gk_jak2_arm64*.log`, earlier bisect logs.

### Deployed (2026-10-08, all md5-verified)
- NRO `jak2.nro` = fdd60cef1 build, md5 `49451ab136ce96122152f5a6fc2b87f4` (15,160,616 B);
  f84 rotated to `Jak 2.f84.bak`; desktop copy `~/Desktop/jak bakcups/jak2.f85.nro`.
- Data sync (copy-if-differs): **163 copied, 2711 already identical** — all 148 fr3 (v43→v44 BCn,
  incl. `test-zone.fr3`: leaving it v43 against a v44 NRO is a version-assert landmine),
  iso only `GAME.CGO` + `TSZ.DGO` differed (arm64 rebuild is byte-identical to the working Sept
  data everywhere else — goalc/Xcode fully exonerated), 13 obj (joint-exploder, sparticle-launcher
  + dependents). Full re-verify: **2874 files, 0 mismatches**.
- fr3 shrank 633 MB -> 501 MB (-21%); total game data ~5.07 GB -> ~4.89 GB. The BCn win is GPU
  upload/VRAM, not disk.
- **Rollback caveat**: `Jak 2.f84.bak` NRO alone will NOT run against v44 fr3 (version assert);
  no v43 fr3 backup exists on card or desktop. If f85 misbehaves: fix forward (BCn path is
  loader-side), or re-extract v43 (TFRAG3_VERSION 43) — not a quick revert. Prefetch A/B kill
  switch: create `sdmc:/switch/jak2/gk_no_pf.txt`.
- Awaiting user hardware test: expect `[texfmt] FIX 74 BCn compressed texture path active` in
  `gk_boot_log.txt`, 30 fps target, BCn textures. jak3 rollout ONLY after jak2 confirmed.

## FIX 86 — jak2: area prefetch was structurally OFF in Haven City; now memory-gated + jak2 seed table (AI-assisted)

### FIX 85 hardware verdict (user session 2026-10-09, gk_run_log/stdout on card)
- BCn **confirmed live**: `[texfmt] FIX 74 BCn compressed texture path active` + `FIX 74c
  glTexStorage2D accepted - immutable BCn upload`. User reports clear improvement.
- Remaining complaints: lag between areas + small slowdowns. Log evidence:
  - **`pf 0 cached` in ALL 85 `[loader]` telemetry lines — the FIX 76 prefetch never cached a
    single level all session.** No `[pf] DISABLED` (no kill file). Root cause below.
  - Haven City holds `live 8/9` constantly; districts `ready in 2-15 s` (hiphog 14.8 s,
    gungame 12.7 s, ctypal 7.0 s) at catchup budget; pool had 67.8 MB free.
  - 3091 `Loader::update slow setup` frames (9-18 ms vs 2-8 ms budget) = the visible micro-dips;
    residual runtime mipgen bursts (`mip rate` did=8/16 frames). That is FIX 87 territory.

### Root cause (Loader.cpp set_want_levels idle branch)
Old gate: `loaded + 1 >= min(max_live_levels(), 5)` -> with jak2's max_live 9, city at 8 live,
prefetch could NEVER start exactly where the player crosses districts. The `5` was a jak1-shaped
proxy for memory. Additionally `pick_prefetch_target_locked` had a static table for jak1 only;
jak2 relied on the per-session learned graph (empty at boot).

### Changes (Loader.cpp only, all under `#ifdef __SWITCH__`; no GOAL, no fr3, v44 data untouched)
- Gate replaced with honest signals: live-level hard cap (`max_live_levels()`), FIX 76c's
  `failed_allocations() > 0`, and pool free bytes >= `kPfPressureFreeBytes` (32 MB = 2x the
  budget logic's 16 MB pressure line; f85 session measured 67.8 MB free in-city).
- `kJak2LevelAdjacency` hub-and-spoke seed (ctywide -> districts/outdoors; siblings paired;
  `l*` sub-levels and blackout-only destinations deliberately excluded). Learned transitions
  still outrank it; jak3 remains learned-graph-only.
- Diagnostics: throttled `[pf] idle, not caching: <reason>` on every reason change (<= 1/30 s)
  and `[pf] start: background-caching <name>` — the next hardware log states the prefetch's
  situation instead of us inferring it from `pf 0 cached`.

### Deployed (2026-10-08 build, md5-verified)
- NRO `jak2.nro` md5 `6b3fe8ae5940cb985aab86298f0c6be6` (15,164,712 B); f85 rotated to
  `Jak 2.f85.bak`; desktop copy `~/Desktop/jak bakcups/jak2.f86.nro`.
- **Rollback is trivial this time**: restore `Jak 2.f85.bak` (fr3/CGO unchanged, no version
  assert). Kill switch unchanged: `sdmc:/switch/jak2/gk_no_pf.txt`.

### Hardware test expectations
- In-city idle: `[pf] start: background-caching <district>` lines; `pf 1-2 cached` in telemetry.
- District crossings on a learned/seeded route: `ready in` should drop or vanish (level already
  resident); watch for cancel churn (`[pf]` cancels + 60 s cooldowns = wrong guesses, acceptable).
- Watch `gk_fatal.txt` / crash reports: prefetch into the 9th live slot exercises the eviction
  path more; FIX 63 OOM history means any GPU-memory fatal = report immediately.

## FIX 87 — jak2: prefetch must also ask TIME health, not just bytes (AI-assisted)

### FIX 86 hardware verdict (user session 2026-10-09, ~140 s; user: "worse than before")
- BCn still live (`[texfmt] FIX 74 BCn compressed texture path active`) — the shrink is NOT the
  regression. The regression is the FIX 86 gate: **bytes-free was treated as health.**
- Gate opened exactly once (55.9 s, `[pf] start: background-caching`) — into a loader running
  catchup the WHOLE session (`catchup (ema 4.9->17.2)`, `catchup-pace (ema 36.4)`,
  `catchup-floor (ema 39.9)` with 69.8 MB pool free — memory-rich, time-poor).
- Cost, all measured: `atollext ready in 33.15s` (same load class = 9.97 s in the f85 log,
  3.3x worse); `gc 746 tex` reclaimed the volunteer (live 5 -> 4); **`pf 0 cached` in all 33
  telemetry lines** (zero benefit); **238 `[cam]` hitches in 140 s** vs 205 hitches in f85's
  68-minute session. Session was NOT in Haven City (live 0-5, want 2-4, atoll/wasteland).
- Secondary bug found: the FIX 86 `[pf]` diag lines used `{}` fmt syntax but `switch_run_logf`
  is printf-style (`__attribute__((format(printf,1,2)))`) — logs printed a literal `{}` and
  passed a `std::string` through varargs (UB). We were blind by construction.

### Changes (Loader.cpp only, `#ifdef __SWITCH__`; no GOAL, no data changes)
- New gate arm BEFORE the bytes check: `m_frame_gap_ema_ms > kPfMaxFrameEmaMs` (25.0) blocks
  with reason `frames are too slow to volunteer work`. At the gate nothing is in flight, so
  EMA <= 25 is exactly the `idle-healthy` budget tier: prefetch only ever starts on frames
  that genuinely have room. Pool-bytes check (32 MB) stays — the gate now asks BOTH questions.
  The FIX 76d/e mid-flight skip (34.5 ms EMA + pause-on-miss) stays as the backstop; the
  start gate prevents the un-skippable part (the SD read burst of the volunteer level) from
  ever landing in a starved stream.
- Fixed all three `[pf]` log lines to `%s` printf style (DISABLED/idle/start). Next session's
  log will name the target and the blocking reason for real.

### Deployed (build 2026-10-09, md5-verified)
- NRO `jak2.nro` md5 `1159733eb062f93a820c7c7ddd21b036` (15,164,712 B); f86 rotated to
  `Jak 2.f86.bak`; desktop copy `~/Desktop/jak bakcups/jak2.f87.nro`.
- Rollback trivial: `Jak 2.f86.bak` (data untouched). Kill switch unchanged:
  `sdmc:/switch/jak2/gk_no_pf.txt`.

### Hardware test expectations
- Heavy outdoor areas (atoll/wasteland): expect `[pf] idle, not caching: frames are too slow
  to volunteer work` — that is the FIX working, not failing (f86 proved volunteering there
  is a 3.3x regression on wanted loads). `ready in` for atoll-class loads should return to
  ~10 s, hitch count to f85 levels.
- Haven City calm moments (idle-healthy): `[pf] start: background-caching <district>` with a
  real name in the log (finally, thanks to `%s`), then `pf 1 cached`. If the city NEVER reaches
  idle-healthy EMA, the idle lines will say so — that data decides whether the threshold
  should be 30 (idle-lean allowed) or the city simply can't afford a prefetch.
- A/B via `gk_no_pf.txt` if in doubt.

## FIX 88 — BCn PBO ring staging (the real "before compression" fix)
Date: 2026-10-08 (AI-assisted)

### f87 hardware verdict (gate WORKED; the real bottleneck finally measured)
- The FIX 87 gate + `%s` logs behaved exactly as designed: `[pf] start: background-caching
  ctysluma` at 49.7 s (a real name, in Haven City), honest idle reasons elsewhere
  (`frames are too slow`, `blackout load in progress`, `a level is still staging`). Atoll-class
  `ready in` back to 1-10 s (`title 1.04s`, `lwidea 9.93s`, `ctykora 9.03s`). Gate verdict: KEEP.
- But the user reported performance "kinda like before the compression" — and the data agrees:
  **176 `[cam]` hitches (session 41 of gk_run_log) vs 177 in f85's session 39.** The prefetch
  was never the constant drag; f86 added hitches (238), f87 removed them again (176), the
  baseline never moved. Why, finally measured:
- **`[loader] tex stage: 1222 textures, upload 9762.9ms` (8.0 ms/texture) and
  `738 textures, upload 8844.1ms` (12.0 ms/texture)** — the BCn path costs 4.5-12.7 ms
  PER TEXTURE, vs 1.2-1.4 ms/texture for the RGBA path it replaced (f73c), despite moving
  4-8x fewer bytes. Boot log confirms `glTexStorage2D accepted`, so this is NOT the FIX 74c
  fallback loop — it is the per-mip `glCompressedTexSubImage2D` calls themselves: each hands
  nouveau a CLIENT pointer, and the driver pays validation + transient staging alloc +
  synchronous copy (~1 ms) per call, 5-11 calls per texture. ~18.6 s of render-thread staging
  in one city session. BCn shrank the bytes; the CALLS are the cost. This also explains
  `slow setup 11-18ms` with `mip rate=3 did=0, gpu=0` (2,460 frames — submit cost, not mipgen).
- Slow-setup trend across builds (never the prefetch's fault): f85 3,091 -> f86 2,638 ->
  f87 2,460.

### Changes (LoaderStages.cpp only, `#ifdef __SWITCH__`; no GOAL, no data changes)
- FIX 88: **PBO RING staging for the BCn mip chain, default ON.** Stage the whole `bcn_data`
  into one of 16 ring PBO slots via one orphan+refill `glBufferData(GL_STREAM_DRAW)`, then run
  the per-mip `glCompressedTexSubImage2D` calls from buffer OFFSETS (PBO stays bound through
  the loop, unbound after). bo-relative uploads have no client pointer to stage/copy per call.
- Why this does not repeat the FIX 69 hardware rejection (56.5 ms implicit-sync stalls): FIX 69
  used ONE PBO orphaned on EVERY texture, so every refill waited on the still-in-flight upload.
  A 16-slot ring reuses a slot 16 textures later (2+ frames under vsync) — the GPU has drained.
  Worst case (blackout burst wrapping the ring in one frame) the orphan path allocates fresh
  storage; a hitch behind a black screen is invisible. Oversized textures (>512 KB) fall back
  to the client-pointer loop; `sdmc:/gk_nopbo.txt` (same file FIX 69 honors) kills the whole
  path, same build, one file moved.
- First-BCn-texture log line: `[texfmt] FIX 88 BCn PBO ring staging active (16 slots, max 512 KB)`
  (or DISABLED with the kill-switch reason). Desktop path untouched.

### Deployed (build 2026-10-08, md5-verified)
- NRO `jak2.nro` md5 `8bd2b1974c46cfd6fda0056f366c1ae2` (15,168,808 B); f87 rotated to
  `Jak 2.f87.bak`; desktop copy `~/Desktop/jak bakcups/jak2.f88.nro` (same md5).
- Kill switch: `sdmc:/gk_nopbo.txt` (BCn PBO off -> f87 behaviour). Prefetch kill switch
  unchanged: `sdmc:/switch/jak2/gk_no_pf.txt`.

### Hardware test expectations (one line settles it)
- `gk_boot_log.txt`/`gk_stdout.txt` early: `[texfmt] FIX 88 BCn PBO ring staging active`.
- The money metric — `tex stage:` lines. f87 baseline: 8.0-12.0 ms/texture in cities. If FIX 88
  works, expect **1-2.5 ms/texture** (same texture counts, upload totals ~4-8x smaller, e.g.
  the 1222-texture level ~2-3 s instead of 9.8 s). If it prints ~8+ ms/texture still, nouveau
  is syncing the ring too -> revert via `gk_nopbo.txt` and record the numbers (next idea:
  per-texture dedicated PBOs, as the FIX 69 note already mused).
- Felt effect: the in-area streaming stutter (176-hitch class) should drop substantially —
  this is the first fix that attacks the per-frame submit cost itself rather than re-pacing it.
  `slow setup` frames should collapse toward the 7 ms budget (overshoot was one 8-12 ms texture).
- Failure modes to watch in `gk_fatal.txt`/logs: nouveau_mm_allocate aborts (FIX 33 class —
  ring allocs are small, 16x<=512 KB, and fewer than the per-call staging allocs they replace,
  so risk is lower than status quo, but the class is known). Visual corruption of BCn textures
  would mean a bo-relative copy misread — `gk_nopbo.txt` restores the old path.

### Numbering
- The previously planned "FIX 88" (~3,091 over-budget loader frames retune + residual runtime
  mipgen) is now **FIX 89**. Much of its premise just dissolved: "runtime mipgen" is already
  dead (`did=0` on 2,413/2,460 slow frames, `0 mip chains deferred` in every city tex-stage
  line) — the slow setups were BCn submit cost, which FIX 88 removes at the source. Reassess
  the staging-budget retune AFTER f88's numbers are in; the admission-pacing idea (EMA-based
  "will this texture fit" check before dispatch) remains the follow-up if slow setups persist.

### 2026-10-09 postscript — first f88 hardware run was silently DISABLED (trap to remember)
- The first f88 session looked unchanged ("works like before", tex stage still 8.0/11.7
  ms/tex, 1222 tex @ 9776 ms) because stdout line 43 said it all:
  `[texfmt] FIX 88 BCn PBO ring DISABLED (sdmc:/gk_nopbo.txt present)`. A zero-byte
  `gk_nopbo.txt` from the FIX 69 hardware rejection (Oct 1) was still sitting at the card
  root and toggled the fix off — the kill switch worked perfectly, against us. Session 42:
  148 hitches vs 176 (f87) = area-mix variance, not a fix.
- **Always grep for the `FIX 88 ... staging active` line before judging a session.**
- File deleted from the card 2026-10-09; next boot runs the ring for real. Lesson banked:
  any default-ON fix with an sdmc: kill switch must have its switch-file state checked
  against the log line, not assumed.


## FIX 89 — S3TC storage probe + optional CPU BCn→RGBA8 decode (jak2, Switch)

- Context: FIX 88 PBO ring did not reduce BCn upload cost (8.8–13.1 ms/texture) → now
  default OFF (`sdmc:/gk_pbo.txt` opts in, `gk_nopbo.txt` hard off).
- Hypothesis: driver may be decompressing S3TC on upload. At first BCn upload a 4x4 DXT1
  probe texture is created and `GL_TEXTURE_COMPRESSED` / `GL_TEXTURE_INTERNAL_FORMAT` are
  queried. Log line: `[texfmt] FIX 89 S3TC probe: ...`. If storage is not compressed,
  BC1/BC3 mips are decoded on CPU (`fix89_decode_bcn_mip`, verified bit-exact against a
  Python reference incl. BC1 3-colour mode, BC3 6/8-alpha modes, non-multiple-of-4 edges)
  and uploaded as RGBA8; byte accounting uses w*h*4 when decode is active.
- Caveat: Mesa answers `GL_TEXTURE_COMPRESSED` from the logical format, so the probe will
  probably report "keeps S3TC" even if the driver does a fallback. Use the overrides for a
  real A/B: `sdmc:/gk_decode.txt` = force decode, `sdmc:/gk_nodecode.txt` = force off.
  Compare `tex stage:` ms/texture against f88 (8.8–13.1). Decoding raises VRAM 4–8x for
  those textures — watch for OOM/GPU faults (erpt) in long sessions.
- NRO `jak2.nro` md5 `7cc8d7b574dadd442088fc89774ba64b` (15,172,904 B); f88 rotated to
  `Jak 2.f88.bak` (8bd2b197…). Copy in `~/Desktop/jak bakcups/f89/`. jak3 unchanged.

## FIX 90 — decode is default; ONE upload call per texture (jak2, Switch)

- f89 verdict: probe said `compressed=1 internal=0x83f0 -> driver keeps S3TC`. A/B with
  `gk_decode.txt` (per-mip RGBA uploads) cost the SAME as the per-mip BCn path:
  1222 tex 10231 ms vs 9975 ms, 738 tex 8869 vs 8789. So the cost is the NUMBER of
  glTex*Image calls (~1 ms each on nouveau), not bytes/format. User: intro, save load and
  area loads all slower since BCn (f85).
- FIX 90: decode path uploads level 0 only (one atomic RGBA glTexImage2D, exactly the f73c
  path at 1.2 ms/tex) and defers the chain to the FIX 42 mipq. Decode is now DEFAULT
  ON; `sdmc:/gk_nodecode.txt` restores the f88 BCn path. SD files stay BCn (smaller reads).
- Expect: `[texfmt] FIX 90 decode path: level 0 only + deferred mipgen`, `tex stage:`
  ~1-2 ms/tex (1222-tex level ~1.5-2.5 s), `mipmaps: N deferred chains left` lines return.
  VRAM back to f73c (RGBA) level.
- NRO md5 `97aa0e405d7528e5f40a374e8caa852d`; f89 rotated to `Jak 2.f89.bak`;
  `gk_decode.txt` removed from SD root (no longer needed). Copy in `~/Desktop/jak bakcups/f90/`.

## FIX 91 — BCn decode moved to the loader thread (jak2, Switch)

- f90 verdict: tex stage 3.5-4x faster (1222 tex 10.2 s -> 2.76 s; 738 tex 8.9 -> 2.2 s), but
  `stage texture took` p50 8.6 ms / `slow setup` p50 12.1 ms per streaming frame, and the
  user still sees zoomer slow motion. Note budget EMA is 25-44 ms even with `pending 0`:
  the city is over 33 ms/frame without any loading -> needs an `[fps]` capture
  (hold R3+Minus; diag is opt-in since FIX 41) to split wait_dma / render / swap.
- FIX 91: `decode_level_bcn_to_rgba()` runs in `Loader::loader_thread` after unpack (and in
  `load_common`), converts BC1/BC3 level 0 to `tex.data` RGBA, sets format RGBA, frees
  bcn_data. Render thread now runs the exact f73c RGBA path (glTexImage2D + mipq).
  `sdmc:/gk_nodecode.txt` keeps BCn. Log: `[texfmt] FIX 91 loader-thread decode: N ...`.
- NRO md5 `97f49e6c49fcbcf0d03244295982aada`; f90 rotated to `Jak 2.f90.bak`.

## FIX 92 — CPU floor 1785 MHz + TOD budget 1 (jak2, Switch)

- f91 R3+Minus capture (Haven City, zoomer, 82-141 s): `wait_dma 0.00`, `swap ~1 ms`,
  `render 32 ms` of 33.3 -> RENDER-THREAD CPU bound, zero slack. Streaming adds
  `loader 13-17 ms` -> 40-60 ms frames (22-25 fps) = the slow motion. Top buckets avg:
  blit 4.55 (0 draws), merc-l2 3.7, tie-l0 2.8, sky-pre 2.4, ocean-mid-far 2.2; ~860
  draws/frame. `[tod] trees 3/9 recomputed 5.18 ms (upload 5.01)`. FIX 91 decode costs
  only 43 ms per 1222 textures on the loader thread (KEEP).
- FIX 92a `switch_clock_tick()` (platform.cpp, called per frame from
  `update_frame_budget`): clkrst CPU floor 1785 MHz (stock boost clock), GPU untouched,
  never lowers a higher horizon-oc/sys-clk profile, re-checked every 2 s and right after a
  FastLoad window. Logs `[clk] now cpu=.. gpu=.. mem=.. MHz`. Kill: `sdmc:/gk_noclk.txt`.
- FIX 92b `kTodRecomputeBudget` 3 -> 1 (~3.4 ms/frame in the city).
- Loader tiers deliberately NOT tightened (FIX 38 lesson); the clock creates the slack.
- NRO md5 `83367d6bdacf7abf9ec5e34e7360dd8b`; f91 rotated to `Jak 2.f91.bak`.

## FIX 93 (jak2 Switch) — render-thread trims (AI-assisted)

The f91 diag capture showed the render thread at about 32 ms of a 33.3 ms budget. Three changes; the loader budget is intentionally unchanged at the user's request.

1. **Blit stall.** `BlitDisplays::render` cleared fb 0 at frame start, which made the driver acquire the swapchain image early (`blit` bucket 4.55 ms, 0 draws). On Switch it now sets `SharedRenderState::deferred_window_clear`, and `do_pcrtc_effects` performs the clear just before the final window blit. This is the jak2 equivalent of FIX 13.
2. **Clouds every other frame.** `TextureAnimator::handle_clouds_and_fog` skips `run_clouds` on alternate frames and re-binds the previous texture.
3. **Ocean envmap every other frame.** `OceanTexture::handle_ocean_texture_jak2` still consumes the full DMA but skips the VU emulation, draw and mip chain on alternate frames.

NRO md5 aac3f9115cd6a48b450ee8985c78bd1a. Rollback file: `Jak 2.f92.bak`.

## After FIX 93 — hardware verdict and load-speed analysis (AI-assisted)

- User: "way better". Jak 3 carry-over notes are written in `JAK3_CARRYOVER_FROM_JAK2.md`.
- `[clk] FIX 92 CPU 1428 -> 1785` repeats every 2 s, so sys-clk is resetting the CPU to 1428 MHz. The user must set the sys-clk profile for the hbmenu host title to at least 1785, or remove it.
- Area loads are bound by texture upload calls, not by the SD card.
  - atollext: file load, decompress and unpack took about 0.3 s in total, and the decode took 167 ms.
  - Its 848 textures took about 2.3 ms each, about 3 per frame at the 7 ms tier, so 219 frames and 10.3 s until ready.
- After loads, the mip drain (`mip rate=16`) costs 5–8 ms per frame against a 2 ms idle budget.
- Proposals:
  - (a) Raise the blackout budget (intro, save load and warps are black screens).
  - (b) Time-cap the mip drain.
  - (c) Recycle same-size GL textures from evicted levels.
  - (d) Upload on the loader thread through a shared EGL context. This could take uploads off the render thread entirely, but it is high risk on nouveau and must be gated by a file.

## FIX 94 — faster frozen loads + time-capped mip drain (jak2, Switch) (AI-assisted)

- **Blocking sweep.** Save loads and warps call `update()` back to back while the game is frozen, but they used the gameplay tiers (`catchup-floor` 8 ms, plus a 2-dispatch crawl cap because the EMA was above 45 ms). The new `blocking` tier gives 40 ms, 32 MB and 64 dispatches per call, and the mip drain is deferred until after the fade-in.
- **Black-screen frames.** The `m_blackout` tier goes from 12 to 24 ms and from 4 to 8 MB, with 40 dispatches. When no uploads are waiting, the mips drain at up to 64 chains or 16 ms per frame.
- **Mip drain cap.** `mipq_process(rate, max_ms)` now checks a wall-clock cap after each chain, and always runs at least one. In idle play it gets 2 ms (EMA above 30) or 4 ms, instead of the measured 5–8 ms. This is not the reverted FIX 49, which clamped against the upload's time.
- **Not done:**
  - A second-thread GL context: the devkitPro Mesa is 20.1, and nouveau there is not thread-safe across contexts.
  - Texture recycling: FIX 88 showed the ~1 ms per call cost remains even with the storage already allocated.
- NRO md5 cd926b16acfeb26a626298e5a5ae1999. Rollback file: `Jak 2.f93.bak`.
- **Expect** `mode=blocking` lines after `coming out of blackout`, and a shorter `[boost] cpu boost OFF after N ms` on save loads (f93: 3341 ms).

## FIX 94b (AI-assisted) — partial revert of FIX 94

User report: f94 felt worse than f93. f94 log: loads faster (lwidea 3.02→1.82 s, ctykora 2.26→1.60 s), but the
blocking sweep ran mip rate 0, so the backlog grew to 1463 chains (f93 peak ~650). It then drained during gameplay
at 4-8 chains/frame (5-15 ms "slow setup" lines), with unfiltered textures meanwhile. There were also 1.6-2.1 s hitches right after sweeps.

Changes:
- update_blocking sweep: mip rate 16 with a 10 ms cap, instead of 0 (chains are built while frozen, so it's invisible).
- blackout tier back to 12 ms / 4 MB (f93). Removed the 24 ms tier, dispatch cap 40, and the blackout rate-64 burst (and `m_budget_pending`).
- Kept: 40 ms blocking tier, mipq time cap, and the idle-path 2/4 ms cap.
NRO md5 d6ebc49a138c450b8c6dd518770d32ea. Rollback: `Jak 2.f93.bak` (best known) or `Jak 2.f94.bak`.

## FIX 95 (AI-assisted) — city slowdown without touching clocks

- f94b city log: while an 848-texture level streamed during play, each frame spent 10-15 ms in "slow setup":
  the texture stage took ~9 ms against a 7-8 ms tier, plus 2-4 mip chains. EMA was 37-38 ms, which is the slow motion.
- f93 [phase]: buckets ~20 ms plus pcrtc ~11 ms, mostly the swapchain-acquire wait. That wait is free time.
- OpenGLRenderer publishes `g_switch_frame_free_ms = ph_loader + ph_pcrtc`. The loader takes the minimum over 4 frames minus a
  4 ms margin and uses it to cap the gameplay streaming line (between 1 ms and the tier value). The mip drain gets whatever is left; if
  less than 0.8 ms is left, it does 1 chain every 4th frame. The stage still always dispatches at least 1 texture per frame.
- The game no longer changes clocks: FIX 71 FastLoad boost and FIX 92 CPU floor are now no-ops (user request:
  the horizon-oc profile owns the clocks).
- Log: `[loader] FIX 95 free X ms -> stream budget Y ms` every 60 frames while streaming.
NRO md5 b44c85e8eeaadb622eecc4c13f26919d. Rollback: `Jak 2.f94b.bak`, `Jak 2.f93.bak`.

## Jak 1 rebuilt at FIX 95 (AI-assisted)

- The jak1 card data was already current: v44 BCn fr3 since FIX 74, and the CGOs match local `out/jak1/iso` (FIX 78). FIX 85's extractor change
  only affects jak2/3, so no re-extract was needed. Only the NRO was replaced.
- NRO built from 230e71cdf, md5 868719c435ea6f15658cb4a20b41b50b. The old de7a2f7b1 NRO (59dbea88…) is now `Jak 1.pre-f95.bak`.
  Desktop copy is in `~/Desktop/jak bakcups/f95-jak1/`.
- Jak 1 gets: FIX 84, 86/87 prefetch, 90/91 loader-thread decode, 94/94b, 95 free-time streaming, and no clock changes.
  It does not get FIX 93 (BlitDisplays, clouds and jak2 ocean are jak2/3-only paths).

## Jak 3 rolled out to FIX 95 (AI-assisted)

Executed `JAK3_CARRYOVER_FROM_JAK2.md` §8 in order.

- **GOAL ports (the plan's "to check/port" row): needed and done.** jak3's `engine/anim/joint-exploder.gc` and
  `engine/gfx/sprite/particles/sparticle-launcher.gc` were the untouched Sep 9 decompiles — ported the FIX 85 jak2 changes
  exactly: recursion-depth cap 32 on `adjust-bbox-for-limits` (stack-overflow crash) and the silent `sp-queue-launch` overflow
  (per-particle `format 0` = SD-log fflush cost).
- **Extraction (v44 BCn)**: `task` CLI not on PATH — wrote `scripts/tasks/.env` (GAME=jak3) by hand and invoked the decompiler
  directly with the `extract` task's exact arguments. Binary: `build-host/decompiler/decompiler` (Oct 8 11:04; no extractor
  commits since = current). GAME.fr3 stays RGBA (TextureAnimator rule — 1024 common textures), levels go BCn
  (e.g. waschase 33.5% of old texture bytes). fr3 876 MB -> 701 MB (-20%).
- **GOAL rebuild**: wiped `out/jak3/obj` (stale-cache poison rule), `./build-host/goalc/goalc --game jak3 --instruction-set arm64
  --cmd '(make-group "iso")'` — 3066 targets in 16.5 s. Sanity: pre-rebuild GAME.CGO md5 `ba802585a12e90b0d4c1d7e93c663d16` =
  the card's live CGO, proving the Sept build was arm64. Only `GAME.CGO` (`219a1cafffd3ba4a94a42b58073c3fd2`) and `TSZ.DGO`
  (`31b3b192c319674a816045b04a62bf64`) changed; the other 761 iso files are byte-identical to the card.
- **NRO**: docker `devkitpro/devkita64:latest`, `SWITCH_GAME=jak3 BUILD_DIR=/work/build-switch-jak3 JOBS=2` — 75/75, clean.
  md5 `ec1526ba179fb7d10a7c158863edc8cd` (15,188,931 B). Deployed as `sdmc:/switch/jak3/jak3.nro`; desktop copy
  `~/Desktop/jak bakcups/jak3.f95.nro`.
- **Deploy verification**: all 274 fr3 `cmp`-identical repo↔card; the two changed CGOs md5-verified on card. The only card extras
  in `data/out/jak3/iso/` are the old `GAME.CGO.f72.bak` / `f73.bak` (left in place). SD root and `switch/jak3/` have **no**
  `gk_*.txt` kill switches (checked — the FIX 88 trap).
- **Rollback (jak2 had none; jak3 does)**: `Jak 3.f83.bak` on card (md5 `10649c6a170ad756be73878429af7304`) plus the v43 fr3
  set saved to `~/Desktop/jak bakcups/jak3-v43-fr3/` (274 files, 867 MB). Restore **both together** (f83 NRO + v43 fr3 is a
  consistent pair). Fix-forward is still preferred — the BCn path is loader-side.
- **Awaiting hardware test**: expect `[texfmt] FIX 74 BCn compressed texture path active`, `[texfmt] FIX 91 loader-thread decode`,
  `[loader] FIX 95 free X ms -> stream budget Y ms` (X ~8-11 ms busy), no `[clk]`/`[boost]` lines. jak3 gets FIX 84-95 + both GOAL
  ports, learned-graph-only prefetch (no seed table), and FIX 93 (jak2 render paths). Jak 3 holds up to 11 live levels — if the
  FIX 95 free time is tighter than jak2's, start at 432p.

## FIX 96 (AI-assisted) — f95 hardware verdict + jak3 prefetch, crash guard, zoomer sound

**FIX 95 hardware verdict (user session 2026-10-09, 27 min, card logs): the slowdowns and frame
drops are GONE.** All expected lines present (`FIX 74 BCn path active`, `FIX 91 loader-thread
decode`, `FIX 95 free 2.8-10.5 ms -> stream budget`), zero `[clk]`/`[boost]`. Remaining: area
arrival still cold, zoomer engine sound drops mid-ride, and the recurring crash. All three root-caused
from the card logs:

1. **Prefetch was structurally OFF in jak3.** `pf 0 cached` in every telemetry line for 28 minutes;
   every `[pf]` reason was `frames are too slow to volunteer work`. Cause: FIX 87's start gate is
   `frame-gap EMA > 25.0`, but jak3 pins to 30 fps — the EMA floor is ~33.3 ms with zero correlation
   to headroom (FIX 95 measured 2.8-10.5 ms genuinely free on those same frames). The 25.0 line was
   calibrated on jak2, where sub-25 windows exist (the f87 jak2 test did prefetch ctysluma). And the
   learned graph is empty at boot, so even an open gate had nothing to pick.
   - Fix (Loader.cpp, `#ifdef __SWITCH__`): per-game gate line — `kPfMaxFrameEmaMsJak3 = 34.0`
     (just under the 34.5 mid-flight pause so a started prefetch can keep streaming; the earlier
     gates — in flight / staging / blackout / pool bytes >= 32 MB — still run first, and dispatch
     stays capped at 1 during a prefetch).
   - `kJak3LevelAdjacency` seed table, built from the player's own observed `GAMEPLAY: enter`
     transitions across the five card sessions (not guesswork): desert/Wasteland hub -> outposts,
     Spargus (wascitya), Haven web (ctyinda/ctyport/ctyslum*), sewers, factory/forest, temple
     cluster. `l*` sub-levels and blackout destinations excluded, same policy as jak2's table.
   - Latent bug fixed: the seed-table scan compared `sit == kJak1LevelAdjacency.end()` regardless
     of the selected table (worked only by accident — iterators from different maps never compare
     equal). Now `seed_table->end()`.
   - Kill switch unchanged: `sdmc:/switch/jak3/gk_no_pf.txt`.
2. **The recurring crash (gk_fatal blocks 5/6, 2026-10-07, both identical).** Symbolized against
   `~/Desktop/jak bakcups/f83/gk-f83.elf` (anchor `get_memory_info` = 0x9e510): pc =
   `link_control::jak3_finish+0x1e0`, stack `jak3::link_begin+0x128` / `_stack_call_arm64`. The
   faulting read is `*((entry - 4).cast<u32>())` with **m_entry.offset == 0**: both fatals have
   far = rw_base + 0xfffffffc exactly (entry 0, minus 4, zero-extended), and the DGO name
   ("lfaccar" family — vehicle-h ships in LFACCAR) sits in the argument registers from
   `basename_goal`. So the v5 work loop finished without ever setting the top-level segment
   (`m_entry = m_object_data + 4`) and finish() called through a null entry. Root cause of the
   null entry is still unknown (suspect: link state/heap corruption — see the nouveau family
   below); the fix makes it non-fatal and loud:
   - All four klinks (jak1/2/3/x, same latent pattern) now guard the top-level call: null or
     out-of-arena entry -> skip the call + `[klink] FIX 96 object <name> has invalid top-level
     entry …` naming the object, version and flags, so the next occurrence pinpoints the source.
   - The other fatal blocks: #2 is the pre-FIX-79 artifact-race crash (fixed), #1/#3/#4 are the
     documented nouveau/fence GPU-corruption family (crash C, still open).
3. **Zoomer engine sound going away mid-ride.** The engine/tire loops are bank voices
   (`sound-play-by-name`, vehicle-sound-info), and jak3 swaps the district soundbank when crossing
   Haven City boundaries (`Unload soundbank ctyslmbh / Load ctyslmah` in the data log).
   `snd::Player::UnloadBank` erased the outgoing bank's handlers **without calling Stop()** —
   voices were never keyed off and their grains kept referencing a handler destroyed a moment
   later. Riding across a district boundary = engine loop silently killed.
   - Fix: `Stop()` each handler (the same path `Player::StopSound` uses) before erasing, plus an
     `[aud] FIX 96 bank unload stopped N live sound handler(s)` breadcrumb. If the sound still
     drops WITHOUT that line, the stop comes from the GOAL side or voice stealing — next lead.
   - Note: audio-device re-init (cubeb) was ruled out — all 44 inits are boot-time only.

### Deployed (2026-10-08 build, md5-verified)
- NRO `jak3.nro` md5 `cb9ce08b51c0fa480feb5aff261233ea` (15,201,219 B); f95 rotated to
  `Jak 3.f95.bak` (md5 `ec1526ba179fb7d10a7c158863edc8cd`); desktop copy
  `~/Desktop/jak bakcups/jak3.f96.nro`. CGOs/fr3 unchanged from the FIX 95 rollout — **rollback is
  the f95 NRO alone** (no data pairing this time).
- Card-reader note: the first deploy attempt hit a transient write failure (truncated .bak,
  stale-looking md5s). Redone stepwise; final state verified by md5 on card and desktop.

### Hardware test expectations
- In Haven/wasteland idle: `[pf] start: background-caching <level>` lines and `pf 1-2 cached` in
  telemetry; first cross-district arrivals should feel instant after the prefetch lands.
  Performance must stay at f95 level — the EMA still gates during streams (37-41 ms idle reasons
  stay honest), and `gk_no_pf.txt` A/Bs it if anything regresses.
- No more lfaccar-style hard crash; if the condition recurs it logs `[klink] FIX 96 …` instead —
  send that line back, it names the object.
- Zoomer across district borders keeps its sound; `[aud] FIX 96 bank unload stopped N …` lines
  appearing at swaps is the fix working.


## FIX 97 (AI-assisted) — per-load soundbank accumulation, prefetch churn gate (documented after the fact)
Built and deployed by a previous agent session (commit dda1cc5dd), this section was missing.
- `sndshim.cpp`: `snd_BankLoadFromIOPPartialEx_{Start,,Completion}` now take a `load_token`
  (the ISO command's `SoundBankInfo*`) and accumulate per token, so interleaved jak3 LOAD_SOUNDBANKs
  (WASCITY1/2/3) no longer mix bytes into one global vector. A completion with no data returns
  handle 0 instead of ASSERT-killing the ISO thread. Callers: `overlord/jak3/iso.cpp`,
  `overlord/jakx/iso.cpp` only (jak1/jak2 unaffected).
- `Loader.cpp`: prefetch only volunteers work after the want-set has been stable for
  `kPfWantStableSec`; desert sub-area seed table trimmed.
- Deployed jak3 NRO md5 `5e429b4811fff769835d73c105232ea2` (stdout says "4a17e8ee9" because it
  was built before its own commit). Now kept as `Jak 3.f97.bak` / `~/Desktop/jak bakcups/jak3.f97.nro`.

## FIX 98 (AI-assisted) — jak3 f97 crash: driver staging ran out of heap during a merc upload
### Crash analysis (f97 session, 228 s, desertb streaming in)
- `gk_fatal.txt` real block: `esr=0x92000045` (write, translation fault) `far=0x2`, X00=0x2,
  non-main-thread sp. Symbolized against the f97 ELF (`offset + nm(get_memory_info)`):
  `memcpy` <- `u_default_buffer_subdata` (Mesa) <- `MercLoaderStage::run`
  (LoaderStages.cpp:1857, glBufferSubData of merc indices) <- `Loader::update`.
- Mesa 20.1 NULL-checks the transfer map, so the driver handed back a bogus near-NULL mapping:
  the nouveau buffer/GART staging allocation failed silently. Telemetry right before:
  `live=10 init=1 want=5`, pool 35.5 MB free, **0 failed** allocations. On nouveau
  `glBufferData` never reports OOM, so the FIX 63 "evict on failed allocation" path never ran and
  5 retired levels stayed resident until the heap ran dry.
- The last `gk_fatal.txt` block (pc_off 0x15f8, far = stack top) was the exception handler
  faulting on its own fixed 768-word stack scan (the thread had only ~6 KB above sp).
- Not the sound code: no `[snd]` warnings; FIX 97's accumulator is fine.

### Fix
- `switch_platform::heap_headroom_bytes()` = unclaimed sbrk tail (`fake_heap_end - sbrk(0)`) +
  `mallinfo().fordblks`. (`get_memory_info().used` is flat: libnx claims the whole heap at boot.)
- `Loader::heap_guard()` (Switch, top of `update()`): samples every frame while staging, every 15
  otherwise. Below **96 MB**: purge retired (not active, not desired) levels, return pooled
  buffers, flush texture garbage, block prefetch (`[pf] ... heap headroom is low (FIX 98)`).
  Below **48 MB**: also drop prefetch caches, delete garbage buffers, and pause staging for up to
  60 frames so fenced frees land (never hangs a load). Rate-limited (30 / 10 frames).
- Telemetry line now ends with `| heap NNNMB`; reclaim logs `[loader] FIX 98 heap low|CRITICAL
  X MB -> Y MB` to stdout and `gk_run_log.txt`.
- Exception handler bounds the stack scan with `svcQueryMemory(sp)`.

### Deployed (md5-verified)
- `jak3.nro` md5 `6fc00ac252c73485784897ea86dc84b9`; f97 rotated to `Jak 3.f97.bak`; desktop
  `~/Desktop/jak bakcups/f98/jak3.f98.nro`. Rollback: `Jak 3.f97.bak` (or `Jak 3.f96.bak`).
- jak1/jak2 not rebuilt (shared code, compiles the same; carry over at their next build).

### Hardware test expectations
- Normal play: `heap` in the telemetry stays well above 96 MB → nothing changes, no perf cost.
- Long wasteland/desert sessions: occasional `FIX 98 heap low` lines instead of a crash; a brief
  stream slowdown is possible right after one (retired areas reload from scratch).
- If it still crashes: send `gk_fatal.txt` + the last `heap` values — the thresholds may need
  raising, and the stack dump will now be complete.

## FIX 98b (AI-assisted) — FIX 98 heap guard REVERTED (it caused the slowdown + slow streaming)
- f98 hardware log: `heap 1.7-1.8 GB` free in every telemetry line, so heap exhaustion was NOT the
  f97 crash cause. Worse, `mallinfo()` in `heap_guard()` cost **~12 ms per call** (newlib walks
  every free chunk of a huge fragmented arena): every staging frame and every 15th idle frame
  showed `slow setup: 12-14ms (did=0)`. Frame EMA climbed to 40+ ms, the FIX 95 free-time clamp
  then cut the stream budget to 1 ms -> areas took 15-17 s to stream in.
- Loader.cpp/.h and platform.h restored to the f97 (dda1cc5dd) state. Kept only the
  `svcQueryMemory`-bounded exception stack scan in platform.cpp.
- **Never call mallinfo() per frame on Switch.**
- The f97 merc-upload crash (memcpy to 0x2 inside Mesa glBufferSubData) stays open; the next
  lead is nouveau GART/nvmap exhaustion, not the newlib heap. Need another crash dump to tell.
- Deployed `jak3.nro` md5 see below; f98 kept as `Jak 3.f98.bak`, desktop `f98b/jak3.f98b.nro`.
- jak3 f98b md5 `96b49e261237fa43049fb432685f02ed`.

## FIX 99 (AI-assisted) — jak3 streaming: time-boxed texture garbage + no stage chaining over budget
### f98b hardware verdict (city / gungame / hiphog session, no crash)
- EMA ~33 ms idle (f98 regression gone). Gameplay streams 4-10 s (gungame 10.7 s, hiphog 8.5 s).
- Every `PC unloading X` was followed by one 28-40 ms frame and then **15-20 frames of
  `slow setup: 10-12ms (mip did=0)`** with no stage over 5 ms: the garbage drain deleting 20
  textures/frame (~0.5 ms per glDeleteTextures on nouveau). It overlapped the next area's
  stream, so FIX 95 measured 2-5 ms free -> stream budget 1 ms -> slow loads + hitches.
- Per-texture upload cost stays ~1-1.5 ms per glTexImage2D call (FIX 90 finding), so the
  remaining limit is free frame time; this fix gives that time back to the stream.
### Fix (Loader.cpp, Switch only)
- Garbage texture drain: gameplay caps at ~1.5 ms/frame (3 ms when >400 queued), min 1;
  blackout / update_blocking keep 20/frame uncapped.
- Stage loop: after a stage that did real work (>0.2 ms) finishes, don't start the next stage
  on a gameplay frame whose budget is already spent. Finished stages return instantly, so
  there is no stall.
### Deployed
- jak3 `jak3.nro` md5 `ec64fb00a165bc49a23bf45ebfd34dfd`; f98b kept as `Jak 3.f98b.bak`;
  desktop `~/Desktop/jak bakcups/f99/`.
- Shared code: jak2/jak1 get it at their next rebuild (not deployed: jak2 is "almost perfect").
### Expect
- No 10 ms frames after unloads; `FIX 95 free` values higher during streams -> bigger stream
  budgets -> shorter `ready in` times; fewer `[cam] HITCH` lines around area changes.

## FIX 100 — f99 crash: stale mip queue, prefetch re-fetch churn, handler scans (AI-assisted)

f99 jak3 session: city loads improved (1.8-5.3 s streams), wasteland 6.7-11.6 s.
Crash at 422 s (12 s AFTER the player went back from 60 to 30 fps, so not the
fps toggle itself): `[pf] start: background-caching warpcast` right after
`PC unloading warpcast`, then SIGSEGV in Mesa `_mesa_format_has_color_component`
<- `_mesa_Clear` <- `OpenGLRenderer::do_pcrtc_effects` (FIX 13 deferred window
clear), far=0x13e6b64dd0 — a garbage renderbuffer pointer (Mesa heap object
clobbered; same class as the f97 merc memcpy crash). Root cause not proven.
The last gk_fatal block was the handler faulting again: the FIX 98 bound only
covered the module scan; the GOAL backtrace + FIX 28 scans still read 768 words.

Changes (shared code, built for jak1/2/3):
- `mipq_forget()` (LoaderStages): `unload_level_gpu_objects` drops the level's
  textures from the deferred-mipmap queue. Unloaded textures linger in garbage
  (FIX 99 time-box) and pass `glIsTexture`, so mipq_process wasted 3-5 ms
  glGenerateMipmap per dead texture; after deletion a recycled GL name could make
  a stale entry touch another level's texture.
- Evicted levels get a 90 s prefetch cooldown (no more unload -> immediate re-cache).
- platform.cpp: all three stack scans use the svcQueryMemory-bounded `scan_words`.

Deployed: jak3 f0ed3dbe1e8e79786d27c2a232577c6a (prev `Jak 3.f99.bak`),
jak2 a856d849492b6b5fe4bfba8a07d73452 (prev `Jak 2.f95.bak`),
jak1 fdbbe06eb81c1cd7af85868478055cb5 (prev `Jak 1.f95.bak`). Desktop: `f100/`.
jak1/jak2 also gain FIX 96-99 (soundbank accumulator, prefetch churn gate,
garbage time-box, stage-chain break). Note: never build two games in parallel —
draco's configure writes `draco_features.h` into the source tree (race corrupts it).

**jak1 REVERTED to f95 (868719c4...)** — user reported f100 jak1 brought back slow loads and slowdowns. f100 jak1 kept on SD as `Jak 1.f100.bak`. Do not ship FIX 96-100 to jak1 again without a hardware A/B. (AI-assisted)

## FIX 101 — jak3-only loader rules (AI-assisted)
Root cause of the f100 jak1 regression: the FIX 97 want-set churn gate. jak1/jak2
rewrite __pc-set-levels constantly while walking, so `[pf] idle: the game is streaming
levels (want-set churn)` blocked the prefetch for the whole session -> beach 15.6 s /
jungle 11.8 s visible streams with 9-13 ms loader frames. Now gated to GameVersion::Jak3:
churn gate, FIX 99 stage-chain break, FIX 99 garbage time-box, FIX 100 eviction cooldown.
jak1/jak2 loader = f95 behaviour (+ mipq_forget, crash guards, bounded handler scans).
Deployed jak1 f1159fe5f30ded7ee5295371ad19cbe5 (prev `Jak 1.f95.bak`, `Jak 1.f100.bak`),
jak2 00aa88d333bfdc8761d7efe97ecf58c4 (prev `Jak 2.f100.bak`, `Jak 2.f95.bak`). jak3 unchanged (f100).
RULE: any new loader/prefetch tuning made from jak3 logs must be gated to Jak3.

## FIX 102 — jak1/jak2 prefetch was dead at 30 fps (AI-assisted)
f101 logs: jak1 `[pf] idle: frames are too slow to volunteer work` all session (+ `buffer pool
low on free bytes`), `pf 0 cached`, beach 13.8 s / jungle 12.6 s visible streams, ~45 cam
hitches per 10 s (good f77 session 25: ~1 per 10 s outside area changes). jak2 same reason
(125x). Cause: FIX 87's 25 ms EMA line is a 60 fps line; at target_fps=30 the EMA floor is
33.3. The "perfect" jak1 was f77 (59dbea88, de7a2f7b1), which had neither gate.
Fix: EMA line is 34.0 (FIX 96 jak3 line) whenever target_fps <= 35, for all games; the
pooled-bytes gate is skipped for jak1 (0 MB recycled is its steady state). jak3 unchanged.
Deployed jak1 adf0d2f1ebbb03baa9aa5ab5a9dcbbd9 (prev `Jak 1.f101.bak`), jak2
4e3ec42ccc71b722af38ea7eadb4fa76 (prev `Jak 2.f101.bak`). Desktop `f102/`.
If jak1 is still worse than f77: rebuild de7a2f7b1 in a worktree (59dbea88 binary is lost).

---

## FIX 103 — TODO FOR NEXT AGENT: texture dedup / sharing (AI-assisted)

**Status: designed and measured; NO CODE WRITTEN YET.** SD state is unchanged: jak1 and jak2 = f102,
jak3 = f100. f102 has not yet been hardware-tested by the user.

### Why (the "creative" fix for slow area loads, all 3 games)
On Switch the live texture path is FIX 91 (decode BCn to RGBA on the loader thread) plus FIX 90 (one
`glTexImage2D` per texture, deferred mipgen). nouveau charges about 1.2 ms per call **regardless of
size**, so load time = number of textures × 1.2 ms. Measured offline on `out/<game>/fr3/*.fr3`:

| game | uploads | byte-identical unique | wasted |
|---|---|---|---|
| jak1 (26 lv) | 10070 | 3891 | **62%** |
| jak2 (148 lv) | 32506 | 14332 | **56%** |
| jak3 (274 lv) | 43916 | 19438 | **56%** |

For example, jak1 beach has 670 textures but only 257 are unique, so about 0.5 s of GL time per area
is wasted. Duplicates are also shared **across** neighbouring levels. jak2 examples: ctymarkb is 89%
identical to ctymarka, ctyslumb is 76% identical to ctysluma, ctyslumc is 60% identical to ctyslumb.
Jak1 neighbours share about 17–22%. The duplicates usually have **different combo_ids**, so matching
by combo_id finds almost nothing. They must be matched **by content**.

The extra GL calls are pure waste, and removing them has no quality cost.

### Design (Switch-only `#ifdef __SWITCH__`, all games; kill switch `sdmc:/gk_nodedup.txt`)
Add a content-keyed, refcounted registry in `game/graphics/opengl_renderer/loader/LoaderStages.cpp`.
It is used only on the render thread.

```
struct TexShareEntry { GLuint gl; std::vector<const tfrag3::Texture*> users; };
std::unordered_map<u64 /*hash*/, std::vector<TexShareEntry>> g_texshare;   // collisions -> vector
std::unordered_map<GLuint, u64> g_texshare_by_gl;
```
- **Hash:** 64-bit FNV-1a/mix over w, h, format, `data` (u32 words), `bcn_data` and `mip_offsets`.
  About 2–3 ms per level on the render thread is OK (it saves about 1.2 ms per duplicate). Moving the
  hash into the loader thread next to `decode_level_bcn_to_rgba` (Loader.cpp around line 1325) is
  optional.
- **Equality must be exact:** compare w, h, format, then `memcmp` of `data`, `bcn_data` and
  `mip_offsets` against `users.front()`. Never trust the hash alone.
- `texshare_acquire(tex)`: return the matching entry's gl and push `&tex` into `users`; otherwise
  return 0.
- `texshare_register(hash, gl, tex)` after a real `add_texture`.
- `texshare_release(gl, tex)`: remove `&tex` from `users`. Return true (the caller should delete the
  texture) only when `users` is empty; then erase both map entries. If gl is not in the registry
  (dedup disabled, or a common texture), return true. `users.front()` always stays a live pointer,
  because each level's `Texture` objects live until that level releases them.

**Hook 1: `TextureLoaderStage::run`** (LoaderStages.cpp around line 1050, Switch branch).
- Before calling `add_texture`, try `texshare_acquire`.
- On a hit, push the shared gl into `ld.textures` without any GL call:
  - Do **not** add to `bytes_this_run`, `tex_this_run`, `g_tex_uploaded` or
    `g_loader_gpu_submits_this_frame`.
  - Do not `mipq_defer` it again.
  - Do not apply the FIX 76e big-texture deferral to it.
  - If `tex.load_to_pool`, still call `pool.give_texture(...)` with the shared gl. Copy the
    `TextureInput` fill from `add_texture` (around lines 899–1005): `src_data` = `tex.data.data()`,
    or nullptr if `data` is empty. The pool lock is already held in that loop.
    `TexturePool::unload_texture` erases one `{gl}` entry per call, so duplicate entries are
    symmetric and safe.
- On a miss, call `add_texture` as today, then `texshare_register`.
- Log once per level:
  `[loader] FIX 103 tex dedup: N shared of M (saved ~X ms)`.

**Hook 2: `Loader::unload_level_gpu_objects`** (Loader.cpp around line 1652).
- Keep the pool-unload loop exactly as it is.
- Replace `for (auto tex : lev.textures) m_garbage_textures.push_back(tex);` with a loop over `i`:
  if `texshare_release(lev.textures[i], lev.level->textures[i])`, push the texture to garbage and to
  a local `freed` vector.
- Call `mipq_forget(freed)`, not `lev.textures`. Otherwise a still-shared texture would lose its
  pending mips.
- Guard the indices: a partially staged level has `lev.textures.size() <= level->textures.size()`.
- **Critical:** never push the same gl to `m_garbage_textures` twice. The FIX 99 time-boxed drain can
  interleave `glGenTextures`, which recycles names, so a double delete would destroy a live texture.

Everything else is unchanged:
- `flush_texture_garbage`, `reclaim_gpu_memory` and the garbage drain go through hook 2 already.
- The common level (`m_common_level`, around lines 1383 and 2469) stays **not** deduped.
- The desktop path stays unchanged.

### Build / deploy / verify
- Build sequentially, never in parallel (draco race), using the commands in the "Build" notes above:
  jak1 and jak2 mount `/work`, jak3 mounts `/src`.
- Deploy each game as `switch/jakN/jakN.nro` with `.f102.bak` rotation (jak3: `.f100.bak`), md5 checks,
  and a copy in `~/Desktop/jak bakcups/f103/`. Delete the `._jak*.nro` files.
- Hardware check in `gk_stdout.txt`:
  - The `[loader] tex stage: N textures, upload X ms` totals should drop by about 55–60%.
  - The new FIX 103 line should appear.
  - Texture-swap visual glitches would mean a broken memcmp/refcount; test `gk_nodedup.txt` to compare.
- Commit with "(AI-assisted)" and the Copilot co-author trailer.

### Offline measurement script (re-run to verify numbers)
fr3 = 8-byte LE uncompressed size + zstd. Level fields: u16 version (44), str (u64 len + bytes),
u64 n_textures. Per texture: u16 w, u16 h, u32 combo, vec<u32> data, str, str, u8 pool, u8 fmt,
vec<u8> bcn, vec<u32> mips. (`pip3 install zstandard`; skip `test-zone.fr3`, which is v43.)

### Next creative item after FIX 103 (slow motion)
`goal_src/<game>/engine/draw/drawable.gc`: `time-ratio` uses `float-time-ratio`, which is real
elapsed time (see SWITCH_PORT_SESSION_NOTES.md around lines 1720–1780, jak1). Check that jak2 and
jak3 got the same float-ratio fix and that the `fmin 4.0` cap is in place, so a dropped frame
advances game time instead of producing slow motion. This needs a GOAL CGO rebuild, not only the NRO.

### FIX 103 — REVISED SCOPE (supersedes the cross-level registry above) (AI-assisted)
The offline counts above (62% / 56% / 56%) were computed **per level file**, so they come entirely
from duplicates within a single level. Build the **intra-level** version first. It has no lifetime
risk because every copy is loaded and unloaded with its own level.
- In the Switch branch of `TextureLoaderStage::run`, keep a per-level map
  `hash -> index of first texture` (store it in `LevelData` and clear it when staging starts). On a
  verified match (hash plus memcmp), push `ld.textures[first]` and skip the GL call. Pool
  `give_texture` stays as described above.
- `unload_level_gpu_objects`: put the names in an `unordered_set` before pushing them to
  `m_garbage_textures` and `mipq_forget`, so each GL name is deleted exactly once.
- Safety net: before queueing a name for deletion, skip it and log
  `FIX 103 LEAK-GUARD` if any other entry in `m_loaded_tfrag3_levels` or `m_common_level.textures`
  holds it. The worst case becomes a leak, not a texture being deleted while still in use.
- No refcounts and no global registry. Cross-level sharing (jak2 city chunks, 60–89%) is a later
  step, only after the intra-level version is proven on hardware.

### FIX 103 — IMPLEMENTED (all 3 games) (AI-assisted)
**103a, in-level texture dedup.**
- `hash_level_textures()` runs on the loader thread right after the FIX 91 decode and stores
  `Texture::pc_dedup_hash`. This field is runtime-only and is not serialized.
- `TextureLoaderStage` keeps a `LevelData::tex_dedup` map (hash → first index). A verified
  byte-identical duplicate reuses that GL name: no GL call and no budget is charged, and it still gets
  `give_texture`.
- On unload, each GL name is queued for deletion exactly once (`unordered_set`).
- Log line: `[loader] FIX 103 tex dedup: N of M textures shared (~X ms saved)`.
- Kill switch: `sdmc:/gk_nodedup.txt`.

**103b, warm areas.** The f102 jak1 log showed beach being prefetched and wanted, then dropped by
the game (jak1 holds 2 levels). About 300 frames later the always-true `low_mem` evicted it, and
walking back cost a **13.96 s** visible re-stream, with 166 of the session's 216 hitches in that
window. `pick_eviction_victim` now keeps game-dropped levels that only `low_mem` would evict, as
long as their count is within `slots - prefetch_resident` (jak1 2, jak2/jak3 1). Peak residency
therefore never exceeds what FIX 76 prefetching already runs with. Behaviour falls back to
eviction in four cases:
- `at_cap` evicts as before;
- any `failed_allocations() > 0` restores the old rule;
- `reclaim_gpu_memory` can still take warm levels;
- the kill switch `sdmc:/gk_nowarm.txt`.

Deployed: jak1 17e490df54b699cfb39bcaf2364be1b7 (prev `Jak 1.f102.bak`), jak2
90173d52147a456bd0ef7dabd3388633 (prev `Jak 2.f102.bak`), jak3 d8e669453b3e588aa0b5e6bdc0b99242
(prev `Jak 3.f100.bak`). Desktop `f103/`.

To verify on hardware:
- The FIX 103 dedup lines appear, and `tex stage: N textures` drops to about the unique counts.
- Walking back to an area just left (beach ↔ village1 ↔ jungle) shows no `PC unloading` and no
  `ready in` re-stream.
- Watch for any `failed` > 0 in the `[loader] live=` line.

### FIX 103c — dedup identity bug (AI-assisted)
The f103 hardware logs showed few textures being shared (jak2 atollext: 32 of 848 instead of
about 611). The extractor stores most textures once with `load_to_pool=1` and 1–3 more times with
`0` (same pixels and same combo id, one copy per renderer), and the f103 equality check required
matching flags. The flag is no longer part of the comparison. This is still safe: pool registration
and unregistration stay per texture entry, so each `give_texture` still has a matching
`unload_texture`.

Expected texture uploads after the fix: jak3 ctypepa 348 → 43 (was 8.85 s), ctycara 296 → 16
(was 10.05 s), ctygenb 248 → 168; jak2 atollext 848 → 237; jak1 beach 670 → 257.

f103 hardware result: jak1 had 12 hitches in 105 s (f102: 216 in 192 s), with no failed
allocations in any game.

Deployed: jak1 3eb9f1585fe9b52b19a07301c723b63b, jak2 d9fd59e925735dd63c13ce764dff3d2b, jak3
6b4be49d3f30a5221381e9bcd950182a. The previous build is `Jak N.f103.bak`. Desktop `f103c/`.

## FIX 104 — jak2 pause-menu Level Select: any mission selectable, replay completed ones (AI-assisted)

User report after the SD-card incident: "can't select the mission from the secrets menu anymore".
Investigation: NOT a regression. The card's `GAME.CGO` (bf5c0c66, the 7-Nov arm64 build at then-HEAD)
still contains FIX 81/82; nothing GOAL-side was lost with the SD restore. The cause was FIX 82's
deliberate forward-only design: from pause → Secrets → Level Select, confirm was blocked with a
low-pitch `generic-beep` when the mission's play-node was closed (= already completed) or had no
`play-continue`. The user's save now has most early missions completed, so most rows beeped.

- `goal_src/jak2/engine/ui/progress/progress.gc` (menu-select-scene-option confirm, ~line 2937):
  block condition `(or (not play-continue) (task-node-closed? play-node))` → `(not play-continue)`.
  Completed missions now take the normal path: `play-task ... #f 'play` calls `task-node-open!`,
  which un-closes the node (and un-marks the task complete if it had close-task), so the mission
  replays with orbs/gems/purchases kept. Rows without a play start point still beep (nothing to
  start from). Unreached-but-available missions were already selectable; behavior unchanged there.
- GOAL-only: no C++, no NRO rebuild. Rebuilt with the standing rule
  `./build-host/goalc/goalc --game jak2 --instruction-set arm64 --cmd '(make-group "iso")'`;
  933 targets, only `GAME.CGO` + `TSZ.DGO` changed in iso/ (usual relink pair). Determinism check:
  a comment-only rebuild reproduced GAME.CGO md5 byte-for-byte.
- Deployed + md5-verified to card: `GAME.CGO` c1f1245bab6a0b84e9726c3a88ce6369,
  `TSZ.DGO` 0f85f66b5c1c84838a39815accda4f35. Backups of the previous files:
  `~/Desktop/jak bakcups/f100/` (old GAME.CGO was bf5c0c66…).
- Known effects (by design): replaying a completed mission marks it not-complete until redone
  (`open!` clears the complete status and bumps task-counter); the mission's parent nodes get
  closed by `task-node-open!` (standard skip semantics). Skipping while inside an active
  mission/race is still untested on hardware (pre-existing FIX 82 caveat).
- jak3 still has the FIX 82 forward-only block; lift it the same way if the user wants it.

## 2026-11-16 — EN/IT language trim (both games) + punch white-box probe deployed (AI-assisted)

Two jobs, both live on the card as of this session.

### 1. Languages stripped to English + Italian (jak1 AND jak2)
**Menus (GOAL, rebuilt via `./build-host/goalc/goalc --game jakN --instruction-set arm64 --cmd '(make-group "iso")'`):**
- jak2 `goal_src/jak2/pc/progress/progress-static-pc.gc`: the three carousels
  (`subtitle-language`, `sound-language`, `text-language`) now have 2 items
  (English, Italiano) with explicit index↔enum translation — `english=0`,
  `italian=4` in BOTH `pc-language` and `language-enum`. Old saved settings
  outside EN/IT show as English; one confirm snaps them to EN/IT.
- jak1 `goal_src/jak1/pc/progress-pc.gc`: `*text-languages*` / `*subtitle-languages*`
  trimmed to `(english italian)` — safe because jak1's confirm path is NAME-based
  (`assoc` into `*language-remap-info-pc*`), not positional. Sound language
  (the `game-option-type language` cycler) remapped to EN(0)/IT(4): back/forward
  toggles between 0 and 4, and the sliding display's next/prev/next2/prev2 are
  computed for 2 items. **Careful:** the file has TWO copies of the neighbour
  arithmetic — the GENERIC `progress-draw-carousell-from-string-list` helper
  (~line 1400, must stay `mod` arithmetic, it serves every string-list carousel)
  and the language one (~line 3169). Only the second was patched.

**Files kept (local `out/jakN/iso` + SD `switch/jakN/data/out/jakN/iso`):**
- jak1: `{0,4,6}COMMON.TXT` + `{0,4,6}SUBTIT.TXT` (6=uk-english for the SCEE
  first-boot remap), `SCREEN1.{EUR,USA,ITA}`, `VAGWAD.{ENG,ITA}`.
- jak2: `{0,4,7}COMMON.TXT` + `{0,4,7}SUBTI2.TXT`, `SCREEN1.{EUR,USA,ITA,DEE,DEJ,DEM}`,
  `VAGWAD.{ENG,ITA}`.
- Deleted everything else: ~**3.0 GB freed on the card** (jak1 data 1.90→1.21 GB,
  jak2 5.68→3.26 GB) and ~3.0 GB locally. Missing-language fallbacks are graceful
  (text loader returns 0, VO `find_in` null).

**TRAP:** `(make-group "iso")` RE-CREATES the deleted language files from the
extraction data. Always re-delete after a rebuild (or the next SD sync resurrects
them). This session re-deleted after the rebuild; lists are in the commands above.

**Deployed (md5 verified local == SD):**
- `jak1.nro` `eb13960baee29addd2b34f528f9ad195`, `jak2.nro` `152164ddf3d6f2551e38bec689bd1855`
- `jak1/iso/ENGINE.CGO` `406060def2ec853d1c02373ad393fa69`, `jak1/iso/GAME.CGO` `f64b25666f251deebd216fe5b287817d`
- `jak2/iso/GAME.CGO` `2d4123d3ba4c318e529fdef6e0dfc814`, `jak2/iso/TSZ.DGO` `b501235c65e659b35468761902c44b1e`
- All other CGO/DGO were byte-identical after the rebuild (checked 179 files) — not redeployed.
- Backups rotated into `~/Desktop/jak bakcups/`: `jak1.e261551f.bak`, `jak2.2c251cf7.bak`,
  `jak1-GAME.CGO.c1f1245b.bak`, `jak2-GAME.CGO.3000efb0.bak`, `jak1-ENGINE.CGO.<md5>.bak`,
  `jak2-TSZ.DGO.<md5>.bak`.

### 2. jak2 punch white-box flash — probe deployed (DKGL_TODO row R)
Audit result: every miss path ALREADY skips (SpriteDk build_frame, slot_of(0),
dk_sprite_lookup null/placeholder, all upload failure paths return 0) and every
stats counter in the user's log is clean except `sprite no_tex=1` — so the visible
white quad is NOT a drawn miss. Probe added in `dk_sprite_lookup`
(game/graphics/pipelines/dk_renderer.cpp): every texture miss now logs
`[sprtex] MISS tbp=<page> <texture-name-or-no-page>` (first 40, then every 600th).
**HW test:** play jak2, reproduce the punch flash once, then grep `MISS` in
`gk_run_log.txt` — a MISS at the flash ⇒ the box is drawn elsewhere (distort/glow
snapshot or wrong-resolve); no MISS ⇒ look at the `[sprtex] tbp=... id=...` pairing
lines at that timestamp for a wrong texture (row-M class).

### 3. VO language never switched in-session (jak2) — FIXED, deploy pending (AI-assisted)
**User report:** playing in Italian, switching Speech Language to English in Options kept Italian voices.
- **Mechanism:** VO is streamed per dialog line from `VAGWAD.<lang>`; the overlord's
  `gLanguage` picks the suffix and is only changed by the `set-language` RPC
  (jak2 `srpc.cpp` `case set_language`). That RPC is sent by `setting-control`'s
  per-frame `update()` ONLY when it sees `user-current != user-target`
  (`apply-settings` first copies `user-default → user-target` every frame).
- **Bug (inherited from upstream):** `game-options-pc-sound-language`'s on-confirm
  wrote BOTH `user-default` AND `user-current` — so current == target, the diff never
  tripped, the RPC never fired, and `gLanguage` stayed on the boot language. The
  choice was still saved (`game-language` in `pc-settings.gc`), which is why a full
  game restart DID apply it ("VO after reload" above).
- **Fix** (`goal_src/jak2/pc/progress/progress-static-pc.gc`): on-confirm now also
  calls `(set-language lang)` directly (sends the RPC immediately; the suppressed
  diff avoids a duplicate). Voices switch from the NEXT spoken line — no restart.
- **jak1 is NOT affected:** its cycler writes only `default.language` through the
  `value-to-modify` pointer, so the diff path fires and the RPC goes out live
  (overlord prints `IOP language: <lang>` in the log each switch — use that to
  verify on HW).
- **Rebuilt** (`/tmp/goalc_jak2_fix.log`, 0 errors): `GAME.CGO` `2d4123d3…` →
  `8da29cac1f0894e084f4c6c7fa9536db` (deterministic), `TSZ.DGO` →
  `0efa34e036f2206d69fb744af1025bec`. NOTE: `TSZ.DGO` is NOT build-deterministic
  (two no-change builds differ; GAME.CGO reproduces byte-for-byte) — don't md5-bisect it.
- **SD was UNMOUNTED this session** — deploy when the card is back (no NRO change,
  GOAL data only):
  ```
  SD="/Volumes/SWITCH SD/switch/jak2/data/out/jak2/iso"   # adjust to real path
  B="$HOME/Desktop/jak bakcups"; ISO=out/jak2/iso
  for f in GAME.CGO TSZ.DGO; do cp "$SD/$f" "$B/jak2-$f.$(md5 -q "$SD/$f" | cut -c1-8).bak"; cp "$ISO/$f" "$SD/$f"; done
  md5 -q "$SD/GAME.CGO" "$SD/TSZ.DGO"   # expect 8da29cac… / 0efa34e0…
  ```
- Cleanup trap hit twice this session: the rebuild resurrects ALL language files.
  This time `rm VAGWAD.*` also killed ENG/ITA locally (restored via rebuild, md5s
  unchanged `f7b3ae73…`/`5de6270f…`). Always delete with an EXPLICIT name list.
  Final local state = keep-list exactly (555 files).

### User HW checklist
1. jak2 + jak1 menus: language options show English/Italiano only; pick Italian,
   confirm text/subtitles switch. VO: jak2 switches from the next spoken line
   (after deploying §3); jak1 switches live already. Log check: `IOP language:` lines.
2. Reproduce the punch white box once in jak2; send `gk_run_log.txt`.
3. Optional carried items: DKGL pass-2 60 fps perf test (row I), jak3 level-select lift.

---

## FIX 118 — jak2 mission-blackout ZSTD crash root-caused & fixed (2026-11-18, AI-assisted)

**The recurring crash:** deterministic `ZSTD_decompress` "Src size is incorrect" ASSERT
during jak2 mission-blackout loads (both symbolized stacks ended in the one-shot fr3
decompressor, failing instruction 0x951864; crash 2 = blackout re-request of `consiteb.fr3`,
a 3.58 MB file with a game-queued load in flight).

**Root cause (proven by elimination):** the jak2 overlord streamed its fake-CD data with
**no `SWITCH_FS_LOCK`** while the loader thread held the lock and read fr3s through the
same newlib fsdev layer:
- `/tmp/ztest/zshot` one-shot harness PASSED on both SD and local `consiteb.fr3` → file
  content + algorithm clean, corruption is runtime, on-device.
- malloc thread-safety verified (`__malloc_lock` = real recursive lock); `read_binary_file`
  checks fread; FsLock.h sound; Loader.cpp crash site clean.
- **The gap:** jak1 overlord + common fake_iso take SWITCH_FS_LOCK (fake_iso.cpp:163,191),
  but **nothing in `game/overlord/jak2/` did**. The only raw stdio there is `iso_cd.cpp`:
  `sceCdRead` (thpool task: fseek+fread), `read_remaining_pages_inline` (FIX 65 made page
  runs back-to-back freads on the ISO thread — intensified the window), and a `fclose`.
  A mission blackout is exactly when heavy page streaming (DGO/VO via `sceCdReadPages`)
  overlaps the loader's big fr3 read. newlib's devoptab (descriptor table, shared fsp-srv
  session) is unsynchronized → the loader's in-flight fread comes back with garbage bytes
  → one-shot zstd sees a bogus frame → assert. Normal gameplay races the same way but
  with far less overlap; the loader is the one reader that VALIDATES content, so it's the
  one that crashes.

**Fix (`game/overlord/jak2/iso_cd.cpp`):** added `#include "common/util/FsLock.h"` and
`SWITCH_FS_LOCK()` in three places: (1) `sceCdRead` do_read (covers fseek+fread, scope =
lambda), (2) per-page inside `read_remaining_pages_inline` (fine-grained so loader/kernel
interleave between pages), (3) the `fclose` before swapping `gFakeCd.fp`. `open_fr` was
already covered (FileUtil `open_file` takes the lock). Note: `read_remaining_pages_inline`'s
"position is right after the last page" no-fseek trick is unaffected — the lock is released
between pages and other lock-takers use their own FILE*s, not `gFakeCd.fp`.

**Deployed:** rebuilt in docker (clean, 743/743, 0 errors — CMake re-ran so it was a full
rebuild, ~9 min). NRO `jak2.nro` md5 `14739204dcd041390a31d68b52d45a04` (16,463,467 B);
old live NRO (md5 `ae701e6f…`) rotated to `jak2.nro.bak-ae701e6f`; desktop copy
`~/Desktop/jak bakcups/jak2.f118.nro`. **VO language fix (GAME.CGO `8da29cac…`, TSZ.DGO
`0efa34e0…`) verified already deployed on the card** — nothing left to ship there.

**HW test:** play jak2 through a mission blackout (the crash repro, ideally the exact
mission that always hit it) with dialogue/VO active; confirm no zstd assert. If a crash
still occurs, grab `gk_stdout.txt`/`gk_run_log.txt` + `gk_fatal.txt` — but the fsdev race
was the only remaining unlocked I/O in the jak2 overlord (word-boundary stdio grep over
`game/overlord/jak2/` + `game/overlord/common/` = iso_cd.cpp and common/fake_iso.cpp only,
and the latter is locked).

---

## FIX 119 — texture pool exhaustion (black/no-color models after ~30-40 min; Sig in the crew bar) (AI-assisted)

**Status 2026-11-18 (evening):** the last session in `gk_run_log.txt` (NRO `896798cb` =
FIX 119 + `(lod-force-tie 1)`, ~5 min Haven) shows the fix working short-term: `tex pool
full` **0**, tex live high 2057/4096, mem 79 MB of 256. Still needs the 60+ min playtest.
**Same deploy added the half-res sky render target** (NRO `9d7727d7`, see DKGL_TODO §7.2
"Half-res sky render target"): boot log must show `[dkgl] sky half-res on …`; kill switch
`sdmc:/switch/jak2/gk_dkgl_fullsky.txt`. Expected sky b5 3 → ~1-1.5 ms in `gpu top`.

**Symptom (user):** first crew-bar entry shows Jak/Sig as black untextured silhouettes;
the same "polygons have no colors" state appears ~30-40 min into any session; exiting the
mission or restarting restores colors. No crash.

**Root cause (proven from `gk_run_log.txt`):** the dktexgpu image pool filled. At t≈3084 s
(51 min) the telemetry reads `tex live=1972 high=2048 fail=2870585 freed=15933 mem=98304KB
holes=36KB` with 1023 `[dk] tex pool full (95 MB carved, …)` lines — the 96 MB image
memblock AND the 2048-slot TextureStore cap were both exhausted. Every upload after that
returns id 0 → those textures render black; mission exit releases level textures back into
the holes (T87 free-range reuse) → later re-upload attempts succeed → colors return. Same
failure class as the T55/T57 buffer-pool fill (128→384 MB) — long-session streaming simply
outgrew the carve-out. (Crew-bar entry is late enough in a session to hit the same wall.)

**Fix (3 constants + 8 bindings):**
- `dk_texture_gpu.cpp`: `kImgPoolBytes` 96 MB (`0x6000000`) → 256 MB (`0x10000000`); new
  `kMaxTexSlots = 4096` used for `s_images[]`/`s_slotOff[]`/`s_slotSize[]` and
  `s_store.init()`; memblock-fail log now prints the size; stale 48 MB header comments fixed.
- `dk_device.cpp`: `kMaxTexImages` 2048 → 4096 (descriptor region grows to `0x20100`;
  kOffUbo/kOffSamplers/kDataPool all derive automatically → data memblock `0x2C5000`;
  static_asserts still hold). Must stay in sync with `kMaxTexSlots`.
- 8 draw recorders (`HfragDk`, `dk_eye_record`, `dk_sprite_record`, `dk_direct_record`,
  `dkgl/dkgl_frame`, `dk_tfrag_record`, `dk_ocean_flush`, `dk_merc_record`):
  `kTexImages` 2048 → 4096 — the count passed to `dkCmdBufBindImageDescriptorSet` MUST
  cover the store, or slots ≥2048 sample garbage.
- `dk_renderer.cpp`: init log now says "256 MB image memblock, 4096 slots".
- Memory check: `[heap] dk after init` on this console = 2698 MB free; +160 MB is safe.
- `scripts/ai/check-dk.py`: PASS (the 1 dk_eye_record warning is pre-existing heuristic
  noise, surfaced only because the file was touched).

**Deployed:** docker build clean (incremental 23/23, 0 errors). NRO md5
`eae0e2959af435003f76227f92ef6a75` (16,463,467 B) on card as `jak2.nro`; f118
(`14739204…`) rotated to `jak2.nro.bak-14739204`; desktop copy
`~/Desktop/jak bakcups/jak2.f119.nro`. **Includes FIX 118** (same tree), so the blackout
zstd test continues to be covered by this NRO.

**HW test:** play 60+ min continuously (roam Haven City, several areas + one mission);
watch `[dk] tex live=… mem=…` telemetry — mem should plateau below 256 MB, `fail=` must
stay 0, and no `[dk] tex pool full` lines. Crew bar should show Sig with full colors on
first entry. If it still fills after hours, the next lever is BCn direct upload
(`dk_texture_pool.h` already reserves Fmt::Bc1-7; RGBA8 decode is the 4-8× memory
multiplier) or LRU eviction, not another size bump.

## FIX 120 — jak2 ship-to-nest silent exit root-caused: corrupt `nest.fr3` (2026-11-18, AI-assisted)

**Symptom:** taking the ship to the Metal Head nest exits the game silently — every
build, every session. Three `decompress_zstd` asserts in `gk_run_log.txt` (34187/35292/
36310) all read `ZSTD error: Src size is incorrect`.

**Correction of the FIX 118 record:** the two crashes at `0x864` (S1/S2) were the OLD
pre-f118 `ae701e6f` NRO — symbolized by fingerprinting every backup NRO's
`decompress_zstd` line-43 call site (`scripts/ai/fingerprint_nro.py`); only S3 (`0x104`
→ resolves to `private_assert_failed+0x24` at base `0x376b6da000`) ran f119. The
"FIX 118 didn't fix it" conclusion was half-right: it didn't, but not because of locking.

**Root cause (proven, not theorized):** `out/jak2/fr3/nest.fr3` was 9,508,205 bytes =
[8-byte header][complete valid 9,505,915-byte zstd frame][**2,282 stale trailing
bytes**] — the tail of an older, longer copy left behind by a non-truncating file
replace. One-shot `ZSTD_decompress` requires the frame to consume the whole source →
`Src size is incorrect` → assert → `_exit(1)`. Card and Mac copies were md5-identical
(bad at the source). All 148 card fr3s are md5-clean vs the extraction; a full
`zstd -t` sweep found exactly one bad file: `nest.fr3`. `scripts/ai/zrepro <file>`
(`clang … -lzstd`) reproduces the game's exact error locally in 1 s — use it first on
any future `compress.cpp:43` assert; do NOT start from threading theories.

**Fixes:**
1. **Data:** truncated `nest.fr3` to 9,505,923 B (frame ends exactly; decompresses to
   the declared 32,360,888 B). Repaired on the Mac AND copied to
   `sdmc:/switch/jak2/data/out/jak2/fr3/nest.fr3` (md5 `b137c5aceb999251a71a784a5e33ccab`,
   corrupt copies kept as `nest.fr3.corrupt-backup` on both). **No rebuild needed** —
   the current f119 NRO can play the mission with just the repaired file.
2. **Code:** `common/util/compress.cpp decompress_zstd` now trims the source to
   `ZSTD_findFrameCompressedSize(...)` when the file carries trailing bytes (logs
   `lg::warn("decompress_zstd: ignoring N trailing bytes…")` and loads instead of
   dying). Truncated frames (no complete frame) still assert exactly as before.
   jak1/jak3/jak1-pal fr3 sweeps: all clean.

**Also noted, not fixed here:** `game/kernel/jak2/kdgo.cpp` (and jak1/jak3 variants)
does unlocked `fread`s — boot-only (`load_and_link_dgo_from_c_fast`, GAME.CGO at
DiskBoot), so it cannot explain mission crashes, but it is the last unlocked stdio user
and worth a `SWITCH_FS_LOCK` in a future hygiene pass.

**HW test:** load the save, take the ship to the Metal Head nest — expect the level to
load and no `[ASSERT] compress.cpp:43` in the log. With the FIX 120 NRO, even a
re-corrupted file would log `ignoring N trailing bytes` and keep playing.

## FIX 121 — audio desync (gun course) + black destructible walls/moving textures (2026-11-18, AI-assisted)

Two user reports, both root-caused from `gk_run_log.txt` + code. NRO f121 md5
`13bfa1de5908f976182b8320b3689c14` (16,463,467 B); f120 rotated to
`jak2.nro.bak-26585e19` on card, `~/Desktop/jak bakcups/jak2.f121.nro` saved.

**1. Audio desync, worst during the scatter gun course.** The `[aud] 10s plays` lines are
logged by the audio thread once per 10 *audio*-seconds (48 000 samples mixed), and their
log timestamps are wall-clock: quiet windows are exactly +10.00 s apart, busy windows run
+10.06…**+10.72** s (192.486→203.201, 224.213→234.605). That is the 240 Hz sound clock
(`Player::mTick`, counted inside the SDL callback) permanently falling behind the frame
clock — everything scheduled on it (MIDI course music, announcer, grain delays) drifts
out of sync with the vblank-driven on-screen timer. Three causes, all fixed:
- `Player::PlaySound` built a **full BlockSoundHandler before CheckInstanceLimit discarded
  it** (the ctor even executes immediate grains = voice allocation). GOAL retriggers limited
  sounds at up to 4190 plays / 4019 rejected per 10 s window, all under `mTickLock` — the
  same lock the ~21 ms audio callback needs. FIX 121 adds a decision-identical pre-check
  (`player.cpp`) that counts same-sound instances off the bank's SFX def and returns 0
  without constructing anything; the steal branch (tick/vol flags) still runs the original
  path.
- `Player::LoadBank` held `mTickLock` for the **entire multi-MB bank parse/memcpy**
  (`Loader::BankLoad`) — every district bank swap (see the `[aud] bank unload` lines)
  blew the callback deadline. `Loader::BankLoad` is split into `ParseBank` (no shared
  state, runs unlocked; `mBanks` is GOAL-thread-only, the audio Tick never reads it) +
  `AddBank` (cheap locked insert). `SFXFlags` accessors made `const` for the pre-check.
- Only 1024 frames (~21 ms) of SDL device buffer (`cubeb_shim.h`). Now 2048 (~43 ms):
  a late callback is a buffer the sound clock never gets back, so the tolerance doubled.
  (Pin status unchanged — FIX 70b opt-in.)
**Verify on HW:** consecutive `[aud] 10s plays` timestamps should stay within ~±0.1 s of
10 s apart even during the gun course; gunfire-heavy windows should no longer lose
hundreds of ms per 10 s.

**2. Destructible walls / moving objects displayed black.** The run log names the
victims as placeholder-forever textures: 1776 `[sprtex] MISS` lines for
`ctywide-spriteticker-a/b` (the moving ticker tapes), `ctyport-sprite/sign-krew-international`,
`ctywide-spritebaron-propoganda-logo`, title sprites. Root cause:
`TextureAnimator.cpp` banned ALL jak2 DMA-driven publishing on Switch
(`publish_dma_textures = false`, SKULL_GEM-only fixed arrays, slime off — commit 429e36f7a)
because the composites "drew empty under dkgl". The empty draws were not a mystery:
**dkgl skips GL_INT vertex-attrib quads outside the allow-list** (`dkgl_draw.cpp`, the
`set_int_attrib_draws` gate added for the skull gem, HW-proven since f97fd4013 — clouds
use it too). FIX 121:
- `dk_texture_anim` (`dk_renderer.cpp`) now wraps the whole animator run in
  `dkgl::set_int_attrib_draws(true)` — every GL draw in that capture context is an
  animator composite; nothing else uses GL_INT attribs there.
- The DMA publish loop, the fixed anim arrays (bomb, cas-conveyor, security, waterfalls,
  lava, stadiumb, fort-pris/warp, metkor, shield, krew-holo) and slime are re-enabled for
  jak2, replacing the black placeholders with real composited pixels.
- Kill switches for A/B on HW (pattern of `gk_dkgl_noclouds.txt`):
  `sdmc:/switch/jak2/gk_dkgl_nodmatex.txt` restores the DMA publish ban;
  `sdmc:/switch/jak2/gk_dkgl_noanim.txt` restores the fixed-arrays+slime ban
  (skull gem always runs).
- New diagnostic: `[texanim] publish tbp=<tbp> <w>x<h>` logged once per published tbp
  (cap 60) — on a good HW run these appear for the tickers' tbps (1670/1672/1448/1982)
  and the `[sprtex] MISS` lines for them stop.

**HW test:** roam Haven City + the gun course. Expect tickers scrolling with real art,
Krew/baron signs colored, destructible walls with proper damage colors, and the gun
course music/announcer staying in sync with its timer. Watch `[texanim] publish` lines
appearing, `[sprtex] MISS` dropping, `[aud] 10s` deltas ≈ 10.0 s, and the SDL open line
now reading `samples=2048 size=8192`. If any re-enabled array misbehaves visually, drop
the matching kill-switch file on the card and re-test — then that array gets its own
opt-out instead of the blanket ban.


## FIX 122 — HW verdict on FIX 121 + targeted probes for the survivors (2026-11-18, AI-assisted)

**HW verdict (2026-11-18, user, f121 = `13bfa1de...`):** the moving city textures
(ticker tapes etc.) now show real colours; the audio **desync is cured** — every
`[aud] 10s` delta in the f121 session is 9.98–10.03 s wall time, even during gunfire
(pre-fix it drifted up to +10.72 s per 10 s). Still broken: the destructible walls /
dead-town falling bridge (black), and some shot sounds missing in the gun course.

**What the f121 log actually says (session starts at line 39414, samples=2048):**
- Tickers/Krew sign/propaganda logo: **zero MISS** — fixed by the fixed-arrays path
  (`[texanim] fixed` codes: skull-gem, bomb, cas-conveyor, security, waterfall, lava,
  stdmb-energy-wall, krew-holo, ...). The old "expect publishes at tbps
  1670/1672/1448/1982" prediction was wrong: the generic DMA publish loop only ever
  published the two sky tbps (0/128, at 6.8–7.0 s, boot).
- Still MISSing all session: `gungame-sprite/gen-01..04`, `meta-01..04` (gun course,
  tbps 2034–2298), `hiphog-spritehiphog-neon-*` letters, `ctyport-...hiphog-exterior-*`,
  `ctysluma-spritebaron-neon-*`. These are NOT in any C++ fixed def and NOT in the
  extractor's `animated_textures` list (`decompiler/config/jak2/ntsc_v1/inputs.jsonc`,
  100 names) — they are plain static textures inside `gungame.fr3` / `hiphog.fr3`
  (fr3 files are 8-byte-header + **zstd**; `strings` on the compressed file finds
  nothing — decompress first: `tail -c +9 X.fr3 > x.zst && zstd -d`).
- Audio: the desync is gone but there is a **retrigger storm**: `plays 2730, limit 2468`
  per 10 s, `top rejected: bank ctywid1 sound 16 x2210` (~220 plays/s of one sound,
  also x599 cut-off) while `live` stays 0–19. During the gun course window itself
  (169–227 s) the storm stops (`limit` 0–6), so the missing shot sounds are NOT the
  handler limit there — a different mechanism, unproven. This storm predates f121
  (same sound 16 storming in the 2026-11-16 log).

**FIX 122 (this build, NRO md5 `7ff7b42a47c084603ebd3e04bcdf572b`, deployed as
`jak2.nro`, f121 rotated to `jak2.nro.bak-13bfa1de...`): pure diagnostic pass, no
behaviour change.**
1. `player.cpp`: the `[aud] 10s` line gains `instant N` — handlers that finished
   less than 24 ticks (~0.1 s) after starting, counted per bank/sound, printed as
   `[aud]   top instant-death: bank %s sound %u (NAME) x%d`. Every `top rejected` /
   `top cut-off` line now also prints the sound NAME (reverse lookup in
   `SFXBlock::Names`). If the storm sounds die at birth this names them; if gunshot
   handlers die at birth we'll see the gun bank there instead.
2. `TextureAnimator.cpp`: once-per-tbp/cbp traces (cap 60 each) of the whole generic
   DMA path, all to `gk_run_log.txt`: `[texanim] upload tbp fmt f2g`
   (handle_generic_upload), `[texanim] clut cbp` (clut-16-16), `[texanim] erase tbp
   WxH` (handle_erase_dest), `[texanim] draw dst` (handle_draw), `[texanim] nongpu
   tbp kind` (publish-loop entries that stay CPU-side — the CLUT16 conversion HACK),
   and `[texanim] fixed NAME` (which fixed arrays GOAL actually requests, cap 20).
   Cross-referencing these against the surviving `[sprtex] MISS tbp` lines shows
   exactly which stage each still-black page dies at: never uploaded / uploaded but
   never erased+drawn / drawn but stuck non-GPU / never requested by GOAL at all.

**Next HW test (same card swap):** boot, roam the city past the destructible walls,
dead town (falling bridge), then the scatter gun course firing often. Then read
`gk_run_log.txt`'s LAST session: (a) does any `[texanim] upload/erase/draw` line show
tbp 2034–2298 or the hiphog pages? (b) does `top instant-death` name the ctywid
storm sound and/or the gunshot? (c) does `[texanim] fixed stdmb-energy-wall` appear
(shooting the gun-course energy walls)? The answers pick the next real fix: GOAL
never sends those pages (loader/upload-bucket issue), sends them but the animator
drops them (publish HACK to relax), or the sounds die at birth (grain/voice issue).

## FIX 123 — f122 HW verdict + id/forensics probes (2026-11-18, AI-assisted)

**HW verdict (2026-11-18, user, f122 = `7ff7b42a...`, session at log line 40844):**
1. **GOAL never sends any texanim DMA for the still-black pages.** All session the
   generic path only ever sees the two sky pages (`upload tbp=0`, `tbp=128`,
   `publish tbp=128 256x1`, one clut at 192) and the fixed arrays requested are just
   skull-gem/bomb/security. The black pages are plain pooled textures: their tbp
   slots are expected to be linked by `handle_upload_now` (the runtime
   upload-now!/chain-replay path), not by the animator. That part of the old
   hypothesis is confirmed dead.
2. **Every MISS line prints a texture NAME (not "no-page")** — so `handle_upload_now`
   DID run for those tbps and linked them, but to a `GpuTexture` that is a
   **permanent placeholder** (`give_texture` clears placeholders, `TexturePool.cpp:138`,
   so a surviving placeholder means the loader never gave that PcTextureId, or gave it
   under a different id, or unloaded it again). Timeline proof: the gungame level load
   finishes at 190.253 ("loader finished pending loads in 302.2 ms") and gen-02/meta-01
   MISS *continuously* 190.356 → 202+ — loading the owning fr3 does NOT heal it.
3. **Extraction is provably fine** (parsed the fr3s offline, serializer-faithful):
   `gungame.fr3` `gungame-sprite` page **2405**, tex 0..16 complete, no gaps
   (cita 0-5, gen 6-8, gold 9-12, meta 13-16), all pool=1 fmt=BC3. `hiphog-sprite`
   page **998** (neon letters tex 36-67), `ctysluma-sprite` page **1021**,
   `ctyport-sprite` page **1325**. `load_to_pool=false` only exists for
   animated_textures in the common path (`extract_level.cpp:414`), so the loader
   gives all of these when the level file loads. Remaining suspects: the runtime
   tpage that draws them has a **different id or tex index** than the extraction
   (link ≠ give), or the give/unload ordering kills it.
4. **Audio: positional/ambient sounds die within 24 ticks of starting.** In the
   gun-course window the plays are there (`plays 278`/10 s) but `instant 190`:
   gunshots, `WATER_DROP` x159/10 s (course), `STEAM_VENT` (city), `RUINS_AMB_LOW`
   x117 (dead town), footsteps NORMAL_HEEL/GIRL_HEEL x90-98. The retrigger storms
   are a *consequence* (GOAL ambient processes are play→wait→replay loops; each play
   dies at birth, so they re-fire every frame). Music/MIDI (different handler class)
   plays fine. Not the instance limit (limit stays 0-9 in the course).

**FIX 123 (this build, NRO md5 `68b325d21c0551716d21faee9d5164e9`, deployed as
`jak2.nro`, f122 rotated to `jak2.nro.bak-7ff7b42a`): pure diagnostics, no
behaviour change.**
1. `blocksound_handler.{h,cpp}` + `sfxgrain.cpp`: every handler records
   `m_last_grain_type` (last GrainType executed; **STOP=24** means the sound program
   branched to its own stop grain — a bank-data cull decision) and `m_had_voice`
   (whether a VAG voice ever started).
2. `player.cpp`: per instant-death up to 2 full snapshots:
   `[aud] instant BANK#id (NAME): ticks= grains= next= last= voice= paused= appvol=
   curvol= pan= countdown= reg=`. `top instant-death` is now top-3 (f122 had several
   distinct populations and top-1 hid all but the biggest).
3. `dk_renderer.cpp`: `[sprtex] MISS tbp=… pc=PAGE/TEX` — the runtime-linked
   PcTextureId, to compare directly with the fr3 combo ids (gen-02=2405/7,
   meta-01=2405/13, hiphog-neon-on=998/64, ctyport signs=1325/xx). The upload-now
   queue now LOGS drops (`[dk] tex op queue FULL`, cap 512) instead of dropping
   silently — a blackout load without chains flowing could unlink a level's tbps
   invisibly.
4. `TexturePool.cpp`, only for pages 998/2405/1021/1325 (so the log stays small):
   `[pool] link tbp= -> pc P/T` (handle_upload_now, runtime side), `[pool] give P/T`
   (loader side), `[pool] placeholder-new P/T` (link before give), `[pool] unload
   P/T` (entry going placeholder again).

**Next HW test:** city → hip hog bar (interior NEON letters) → baron sluma signs →
gun course firing often → dead-town bridge. Then read the LAST `gk_run_log.txt`
session and match every `[sprtex] MISS pc=2405/x` against `[pool] give 2405/x`, and
every `[aud] instant … last=24` against its appvol. Verdict table:
(a) give 2405/x present + MISS pc=2405/x persists → placeholder healed but slot
stale (refresh_links seam) or give→unload churn; (b) link shows a different page/tex
than the fr3 → runtime tpage id/index mismatch, fix the mapping (name-based link or
follow the GOAL upload-now recreation); (c) no give at all → loader never loaded the
file/texture (want-levels list); (d) `[aud] instant … last=24 appvol≈0` → plays come
in at ~zero volume (GOAL ear/distance math); last=24 with normal appvol → find what
stops the handler; last=1 voice=0 → VAG voice never survived (synth path).

## 2026-11-18 — UPSTREAM SYNC (AI-assisted, Cline): 10 upstream fixes cherry-picked; NOT yet deployed

Branch `deko3d-migration`. Added `upstream` remote (open-goal/jak-project); fork
history is fully rewritten so there is **no merge-base** — picks are by SHA from
identified base `a42c07199` (2026-09-10). Delta was 38 commits; **10 picked**:

- `6206e74ca` draco cmake install path · `dcf31a96e` extraction workers capped to
  hw concurrency · `b1bd559bc`/`4075e9095` goalc/extractor default arm64 on ARM
  (Switch pipeline unaffected: `package-switch.sh` passes `--instruction-set arm64`)
- `97002122a` Windows REPL WSA_IO_PENDING · `da7fc00dc` GlowRenderer probe depth
  format (GL-only, dead on deko3d — but note our T126 glow-probe port may share
  the same depth-format-lost-on-resize bug class; worth an eyeball someday)
- GOAL fixes (**land in `data/out/jakN/obj`, NOT in the NRO**): `6059ab859` jak1
  hint subtitles (`game_subtitle.gp` is v1; goalc marks it V1, converter now sets
  `merge=false`) · `7eb72fd2f` jak3 speedruns `logior!` (resolved conflict: kept
  OUR category set, fixed our two hero-mode blocks) · `75a4211f1` jak3 Onin Tent
  minimap icon · `8355ed9a1` jak3 glider/sniper-turret relocate crash (duplicate
  commented out; original verified in `engine/common-obs/generic-obs.gc:3319`).

Skipped on purpose: `31c37336` jak2 language menu (our `set-language` rpc in
`progress-static-pc.gc` is the same fix, better), CI/docs/dep bumps, jakx decomp
batches + decompx tooling, SFX extraction feature, TOD GLB feature, `-nosound`
removal (touches our Switch-modified kmachine).

**Deploy state: none of these are on the card.** Card NROs predate the picks:
jak1.nro 16/11, jak2.nro today (f122 diag build, before the picks), jak3.nro
10/10. To ship: rebuild host tools (done, `build-host` goalc+gk clean), re-run
the per-game ARM64 data compile (new binaries — subtitle fix is in the compiler),
`GAME=jakN scripts/package-switch.sh`, deploy whole `switch/jakN/` tree (NRO
**and** `data/` — the GOAL fixes are data-side; NRO-only swap ships nothing
user-visible). Users must keep `switch/jakN/OpenGOAL/` saves when swapping.




## 2026-11-19 — FIX 123 HW VERDICT (AI-assisted, Cline): black walls = OUR Loader evicts pages still drawn; audio FIXED; perf NOT worse; white-box Jak = same root cause

Card md5 confirmed f123 (`68b325d21c0551716d21faee9d5164e9`). Log session
starts at line 42374 of `gk_run_log.txt` (379 s tour). User: audio FIXED,
black walls still there, new transient white-box-Jak in dead city / pumping
station, "did probes make perf worse?"

**Audio: fixed, case closed unless it regresses.** The 80 `[aud] instant`
snapshots that remain are benign populations: 57/80 have `appvol=0` (GOAL
ear/distance cull — the sound program stops itself, `last=-1`, normal) and the
rest are voice-limit rejections during gungame retrigger storms
(`plays 2033 limit 1797 instant 234` per 10 s — the limit counter is high but
sounds survive; pre-f121 the same storms showed `instant 888` with silence).
The f121 audio-clock fix is the healed path (see memory entity).

**Black walls: verdict (a), second half — give→unload churn, and the unload
is OURS, not GOAL's.** Per-page timeline in the f123 session:
- ctysluma 1021: `placeholder-new` t≈9.4 → `give` t≈10.8 → works → **mass
  `unload` t=35.619 (78 pages)** → `MISS tbp=930 pc=1021/9` at t=105.9+.
- hiphog 998 / ctyport 1325: `give` t≈35.5 → works → unload t=177.8.
- gungame 2405: `give` t=56.6 → **walls textured, zero MISS for 120 s** →
  user re-enters the course at t=176.9 (blackout load; `[pool] link
  tbp=20xx -> pc 2405/x` re-link + GOAL upload-now at 176.884) → 0.9 s later
  **t=177.799 the Loader unloads 2405/0-16 + 1325 + 998 (111 pages)** →
  `MISS 2405/7, 2405/13` continuously from t=178.7 to the end of the log.
  So the pages load fine, survive the whole first visit, and are then killed
  by a level-cache eviction and never re-given ("no give follows a same-level
  unload", TexturePool.cpp:243).

`pc=` values match the fr3 combo ids exactly (2405/7, 2405/13, 998/xx,
1021/x, 1325/19) — runtime id/index mapping is CORRECT; verdicts (b) and (c)
are dead. `[dk] tex op queue FULL`: 0 hits — the upload-now queue never
dropped anything.

Who unloads: `Loader::unload_level_gpu_objects` (Loader.cpp:1703) calls
`tex_pool.unload_texture` for every pool texture of an evicted cached level.
The Switch branch of the level-cache picker (Loader.cpp ~1650) evicts
prefetch-resident levels that are no longer active/desired — this fires often
under the Switch memory cap, while the desktop policy (180-frame grace, only
over m_max_levels) almost never does. On the PS2 none of this matters because
texture pages are streamed per frame by the proximity masks
(`upload-textures`, texture.gc:1768) regardless of level-object lifetime —
"unloading" a page on PS2 just frees the VRAM slot bookkeeping; the data
stays and is re-uploaded whenever in view. Our port gave the pool static
per-level data, so when the owner level is evicted the tbps still drawn by
the ACTIVE level's sprites go placeholder forever.

**White-box Jak = same root cause, different renderer.**
`unload_texture` flips the entry back to `is_placeholder` and
`refresh_links` re-points every linked tbp at `m_placeholder_texture_id`
(TexturePool.cpp:223/263/408) — a 16×16 gray/white **checkerboard**
(`0xa0303030`/`0xa0e0e0e0`, TexturePool.cpp:444-452). The sprite path
skips on placeholder (`dk_sprite_lookup` → 0 → batch skipped → **black
walls**), but the merc path (dkgl bind → pool resolve → placeholder id)
*draws* it — Jak's model textured with the stretched checkerboard reads as a
**white box**. Dead city / pumping station are the heaviest streaming scenes,
so their level evictions catch Jak's art pages; the punch makes the
placeholder page visible for ~1 s until the animator/clut blender republishes
(also matches the dk_renderer.cpp:216 comment "the punch-spark suspect").
Not a new bug — fix the eviction and both symptoms should go together.
**Probe gap:** `[pool]` probes only cover 998/2405/1021/1325 and there is no
MISS probe on the merc/dkgl resolve seam, so Jak's page wasn't traced — this
session never visited dead city/pump station anyway.

**Performance: did NOT get worse** (f123 vs all earlier sessions in the same
log; different builds, rough scene mix):
- gpu world avg 11.24 ms (max 15.35) vs 11.47 (max 23.6)
- slow frames ~12 per 600-frame pass vs ~17.6
- frame components: capture 4.17 vs 4.71, build 1.25 vs 1.45 (both BETTER);
  gpufence 4.74 vs 4.16, inwait 0.49 vs 0.26 (both idle waits, not cost);
  total 16.16 vs 16.05 ms of the 16.7 budget.
- The [sprtex] MISS probe is rate-limited in code (first 40 + every 600th)
  and per-frame cost is a lookup + counter; 2126 log lines in 261k. Negligible.

**FIX 124 directions (pick next session):**
1. **Simplest:** on the dk path skip the `tex_pool.unload_texture` loop in
   `unload_level_gpu_objects` — keep evicted levels' pool textures alive.
   Cost: pool memory stops recycling (`tex live=1849 mem=79.5 MB freed=1726`
   at session end — freed=1726 is how much the current behavior recycles;
   dktexgpu P8 means image-bump memory never returns, so a long session would
   grow until the img memblock fails — needs a cap or real LRU as follow-up).
2. **PS2-semantics:** draw-stamp each tbp at lookup time (dk_sprite_lookup +
   dkgl resolve seam); `unload_texture` defers pages stamped within the last
   N frames and retries them once idle. Evictions then only land on pages
   truly not drawn — exactly what the PS2 did with VRAM.
3. **Loader policy:** refuse to evict a cached level whose pool pages were
   drawn recently (same draw-stamp set, checked in the Switch picker).
Also add the merc-side placeholder probe (rate-limited `[merctex]` naming
tbp/page at the dkgl resolve seam) and `[pool] unload` logging for any page
while still linked, so the white-box events become visible in the log.

**Still open:** intro FMV low fps — grep `MoviePlayer|bink|mpeg|avcodec`
found nothing; next attempt: GOAL-side `*movie*` handlers, `play-movie`,
streaming/decoder in `goal_src/jak2/`.

## FIX 124 — IMPLEMENTED: draw-stamp deferred unload (option 2, PS2 semantics) (2026-11-19, AI-assisted, Cline)

**Root cause recap (FIX 123 HW verdict):** black walls / white-box Jak after
area transitions = `Loader::unload_level_gpu_objects` calls
`TexturePool::unload_texture` on pages the *active* level still draws; nothing
re-gives them, so the tbp renders placeholder/black forever. The PS2 never did
this: it only evicted far levels' pages and the EE re-uploaded what it needed.

**What landed (option 2, not the option-1 "never unload" — that hits P8
unbounded image-memory growth):**

1. `TexturePool.h/.cpp` — `GpuTexture::last_draw_frame` (u64 draw stamp) +
   FIX 124 API: `tick_frame()` (clock, render thread, once/frame),
   `note_draw_id()` (per-id stamps, fixed `[4096]` array = dktexgpu store
   kMaxTexImages), `unload_texture_if_idle()` (= `unload_texture` unless the
   page was drawn within `m_defer_frames`; then queue + dedup by gpu_id),
   `retry_deferred_unloads()` (mirrors unload_texture's teardown:
   find_if-by-gl, erase, empty→placeholder, `refresh_links`). Stamps are
   written in `lookup()` + `lookup_mt4hh()` (lockless render-thread u64
   stores, same pattern the header already documents).
2. dkgl bind seam — merc/Generic2 bind raw `lev->textures` dktexgpu ids with
   NO tbp lookup, so they'd never be stamped. Added
   `dkgl::set_pool_draw_stamp_hook` / `note_pool_draw` (dkgl_state.h/.cpp),
   installed in `dkgl::init` (dkgl_frame.cpp, raw pool ptr, dk_renderer
   outlives dkgl), called from the dkgl_draw.cpp unit loop pool-path branch
   (`dk_id = gl_id; else` arm). Null hook on host/stub = no-op.
3. `Loader.cpp` — `unload_level_gpu_objects` now builds a `freed_now` list:
   pool ids released *now* (idle) go to the garbage queue as before; deferred
   ids are NOT queued (that was the actual bug). `Loader::update` drains
   `retry_deferred_unloads` each frame (pool mutex) and pushes released ids
   into `m_garbage_textures` → existing paced delete. Desktop (defer 0) is
   byte-for-byte the old behavior.
4. `dk_renderer.cpp` — `dk_loader_start` sets defer window **120 frames**
   (~2 s at 60 fps); **kill switch `gk_dkgl_nopooldefer.txt`** in
   `sdmc:/switch/<game>/` restores the old behavior (same access() flag
   pattern as gk_dkgl_noworld.txt). `tick_frame()` runs once per frame in the
   render loop, before the loader update/`update_blocking` block.

**Probes:** `[pool] FIX124 defer unload page/tex (drawn N frames ago, M
queued)` — rate-limited first-40 + every-600th (sprtex pattern);
`[pool] FIX124 deferred unload done` on probe pages;
`[loader] FIX124 pool defer: freed N tex, M still held` while work is
outstanding. On hardware with the bug, the black-wall/white-box pages should
now appear in the `defer unload` log instead of going black.

**Threading (verified before writing):** stamps + tick + deferral checks +
retries all run on the render thread (lookups/binds happen during bucket
recording; `Loader::update` is called from the dk render loop, T49 comment).
`handle_upload_now` (game EE thread) touches different fields under the same
mutex; a torn aligned u64 is impossible. So no new locks.

**Gate:** `check-dk.py` PASS (44 files, 0 errors, 1 pre-existing warning);
docker jak2 NRO build clean 69/69, **md5 80ff1eb38c2eb8eb4c613a369a8f5d81**
(build-switch-jak2/game/gk.nro). No new warnings (the 4 grep hits are
pre-existing lines: Loader.cpp:188 reorder, dkgl_draw.cpp:556/557,
dkgl_frame.cpp:2640).

**Deploy:** DONE (2026-11-19) — SD card was mounted. `build-switch-jak2/game/gk.nro`
(16,520,811 B, md5 80ff1eb38c2eb8eb4c613a369a8f5d81) copied to
`/Volumes/SWITCH SD/switch/jak2/jak2.nro`, card md5 re-verified identical; prior
build rotated to `jak2.nro.bak-1993b908` BEFORE the copy, then `sync`. A/B test =
create `gk_dkgl_nopooldefer.txt` on the card to fall back without redeploying.
Expect: during a real area transition the defer log shows a handful of pages held
~120 frames then released — if it shows hundreds held continuously or `tex live`
keeps climbing over a long session, shrink the window or move to option 3.
## 2026-11-19 — FIX 127: debug-menu cheat toggles persist (jak2+jak3) (AI-assisted, Cline)

**User report (follow-up to FIX 126):** "Bad Weather" toggled OFF in the OpenGOAL
Cheats debug menu comes back ON (rain) after a game restart.

**Root cause (GOAL, persistence — not rendering):**
`dm-game-opengoal-cheat-toggle-pick-func` flips the cheat bit in
`(-> *pc-settings* cheats)` in memory only. The one auto-persister,
`update-cheats` (jak2 `pckernel.gc:704` / jak3 `pckernel.gc:597`), calls
`pc-settings-save` only when the cheat bitfields change *within a single frame's*
loop (`old` captured at the top of the same call) — a debug-menu toggle happens
*between* frames, so the OFF is never detected and the file is never rewritten.
Any earlier settings save (frame rate, pause-menu cheat page, PC Settings Save,
boot-time default write) that captured the cheat ON stays in `pc-settings.gc`;
`load-settings` at boot (`pckernel-common.gc:675`) restores it → rain back.
Serialization itself is fine: `cheats` is written (`pckernel.gc:837`) and parsed
(`pckernel.gc:780`). FIX 126 fixed the visual snap-back only.

**Fix (GOAL only, no C++):** in both
`goal_src/jak2/engine/debug/default-menu.gc` and
`goal_src/jak3/engine/debug/default-menu.gc`,
`dm-game-opengoal-cheat-toggle-pick-func` now calls `(pc-settings-save)` right
after the `logxor!`, guarded by `(not (-> *pc-settings* speedrunner-mode?))` to
match `update-cheats`'s rule that speedrun cheat modifications are never
persisted. All 19 cheats on that page (weather-bad/good, turbo-board, etc.) now
persist on toggle.

**Build/deploy (GOAL arm64, `make-group "iso"`, no NRO change):**
jak2 933 targets clean → GAME.CGO `80a3f9f9a62d3c34ab937dda23b6d61e`
(was `c18420bb…`); jak3 1164 targets clean → GAME.CGO
`325dff0f8c40a0395914754f26c55831` (was `8a1ade2d…`). Both copied to
card `data/out/<game>/iso/GAME.CGO`, md5 re-verified after `sync`. Pre-FIX127
pairs backed up in `~/Desktop/jak bakcups/2026-11-19-fix127-cheatpersist/`
(jak2 `c18420bb…`, jak3 `8a1ade2d…`). `check-dk.py` PASS.

**HW test:** toggle weather-bad OFF → exit → power off → boot → weather stays
clear; `pc-settings.gc` on card should show `(cheats …)` without the
weather-bad bit. Same spot-check for weather-good ON.

## 2026-11-19 — FIX 125 + FIX 126: pool window widened 60 s; weather cheat unstick (AI-assisted, Cline)

**FIX 124 hardware verdict (gk_run_log, Nov 19):** the deferral fired exactly as
designed — transition at 42.6 s deferred 40 pages (drawn 1–87 frames ago), idle
ones released at 43.7–48.7 s, and "47 still held" are pages the active level
keeps drawing continuously (correct: they stay alive and render). BUT the black
walls still come back: the same pages get drawn again ~35 s after release
(`[sprtex] MISS tbp=930 pc=1021/9` at 85.3 s after `deferred unload done` at
48.7 s) → placeholder → black. 120 frames (2 s) of idle protection is too short
for pages the active level draws intermittently (camera/fog/streaming sectors).

**FIX 125 (C++):** idle window 120 → `TexturePool::kPoolDeferFrames` = 3600
frames (~60 s), still set at pool creation (`dk_renderer.cpp`, kill-switch
`gk_dkgl_nopooldefer.txt` unchanged). `Loader::update` (FIX 125 block) drops
the window to `kPoolDeferFramesPressure` (120) while
`loader_under_pressure() || dktexgpu::failed_allocs() > 0`, so a tight dk
texture heap still reclaims in seconds; one-shot probe
`[loader] FIX125 pressure: pool defer window 3600 -> 120 frames`. Boot log now
reads `[dk] pool defer unload ON (FIX124/125, 3600 frames)`. Desktop/kill-switch
behavior byte-identical to before. Expect after a transition: pages held up to
~60 s then released; if `tex live`/`mem` climbs unbounded over a long session,
the pressure fallback is the lever to watch.

**FIX 126 (GOAL, jak2 only — user-reported "OpenGOAL cheat Bad Weather stuck"):**
root cause in `mood.gc`: the weather-cheat cond pins `current-interp`
cloud/fog directly (1.0 bad / 0.0 good) every frame, but nothing restores them
when the cheat is toggled OFF — recovery relies on `seek!` toward
`target-interp` at `1/speed-interp`, speed 30–120 → up to 2 minutes of lingering
storm. The debug-menu toggle itself is correct (the `/ 8` in
`dm-game-opengoal-cheat-toggle-pick-func` is fixnum untagging, bit 17/18 match
`pc-cheats`). Fix: new `mood-control` field `weather-cheat-state` (`mood-h.gc`,
0 = none / 1 = bad / 2 = good); the mood process tracks it and on the OFF
transition snaps `current-interp` cloud/fog back to `target-interp`
(instant). Cheats still persist via the settings file (`cheats` u64) — toggling
off then saving is the way to clear it across boots, same as before.


## 2026-11-18 — MENU PARITY: jak3 debug-menu Sizes drop (J10) (AI-assisted, Cline)

Follow-up to the menu-parity verification (jak2↔jak3 pause Graphics pages
byte-identical; jak1 still on the old PC-style `*graphic-options-pc*`; debug menus
jak1/jak2 drop "Sizes" on Switch, jak3 kept it + added the Resolution W/H readout).

**Why jak3's Sizes picker is not cosmetic:** picking a size pushes a display-mode
change that can resize the nwindow swapchain (`display_manager.cpp:224`) — the
hazard the jak1/jak2 drop closes. deko3d renders at the native panel size
(720p handheld / 1080p docked); no size option can take effect.

**Change (GOAL only, no C++):** ported the byte-equivalent of jak2
`goal_src/jak2/pc/debug/default-menu-pc.gc:1096` into jak3
`goal_src/jak3/pc/debug/default-menu-pc.gc` (top-level `when` after the menu
build, line ~1021): on Switch (`(= (pc-get-os) 'switch)`) find the "PC Settings"
menu, `delete!` the "Sizes" item, `debug-menu-rebuild`. The read-only
"Resolution W/H" readout above it stays (jak3-only nicety).

**Build:** incremental `./build-host/goalc/goalc --game jak3 --instruction-set arm64 --cmd '(make-group "iso")'` — 1164 targets, 0 errors
(`/tmp/goalc_jak3_dmfix.log`), `default-menu-pc.gc` recompiled (38% mark).

**Artifact:** `out/jak3/iso/GAME.CGO` — 16,325,936 B, md5
`8a1ade2d1c56d08362b93a3ee1a85275`. **Supersedes the undeployed `72fc92e6`**
(last session's readout-only build); still pairs with the undeployed J7 NRO
`c561d1b664e672bb4a08655281f9860e` — deploy the two together when the SD card
is back. TRAP: `(make-group "iso")` re-creates any pruned language files —
re-check before the SD sync (jak1/jak2 lists in the f100 entry; jak3 pruning
status unknown — verify on the card).

**Awaiting user go/no-go (proposed, not started):** (a) jak1 Graphics page →
Switch-appropriate options + window-size readout — a real port into jak1's old
menu system, not copy-paste; (b) Resolution W/H int-vars + `pc-get-window-size`
31-frame refresh into jak1/jak2 debug menus; (c) nothing more for jak3's menus.
If approved, all three GAME.CGOs rebuild (jak1's first ever). J9 (jak3 vertical
stretch) still awaits the user's location/handheld-vs-docked answer.

**Not done / follow-ups:** `[merctex]` placeholder probe deferred (the defer
log already names suspect pages via tex_probe_page); option 3 (evict far
levels only) not needed unless the 120-frame window proves too generous.


## 2026-11-18 — MENU PARITY: jak3 debug-menu Sizes drop (J10) (AI-assisted, Cline)

Follow-up to the menu-parity verification (context: jak2↔jak3 pause Graphics pages
byte-identical; jak1 still on the old PC-style `*graphic-options-pc*`; debug menus
jak1/jak2 drop "Sizes" on Switch, jak3 kept it + added the Resolution W/H readout).

**Why jak3's Sizes picker is not cosmetic:** picking a size pushes a display-mode
change that can resize the nwindow swapchain (`display_manager.cpp:224`) — the
hazard the jak1/jak2 drop closes. deko3d renders at the native panel size
(720p handheld / 1080p docked); no size option can take effect.

**Change (GOAL only, no C++):** ported the byte-equivalent of jak2
`goal_src/jak2/pc/debug/default-menu-pc.gc:1096` into jak3
`goal_src/jak3/pc/debug/default-menu-pc.gc` (top-level `when` after the menu
build, line ~1021): on Switch (`(= (pc-get-os) 'switch)`) find the "PC Settings"
menu, `delete!` the "Sizes" item, `debug-menu-rebuild`. The read-only
"Resolution W/H" readout above it stays (jak3-only nicety).

**Build:** incremental `./build-host/goalc/goalc --game jak3 --instruction-set
arm64 --cmd '(make-group "iso")'` — 1164 targets, 0 errors
(`/tmp/goalc_jak3_dmfix.log`), `default-menu-pc.gc` recompiled (38% mark).

**Artifact:** `out/jak3/iso/GAME.CGO` — 16,325,936 B, md5
`8a1ade2d1c56d08362b93a3ee1a85275`. **Supersedes the undeployed `72fc92e6`**
(last session's readout-only build); still pairs with the undeployed J7 NRO
`c561d1b664e672bb4a08655281f9860e` — deploy the two together when the SD card
is back. TRAP: `(make-group "iso")` re-creates any pruned language files —
re-check before the SD sync (jak1/jak2 lists in the f100 entry; jak3 pruning
status unknown — verify on the card).

**Awaiting user go/no-go (proposed, not started):** (a) jak1 Graphics page →
Switch-appropriate options + window-size readout — a real port into jak1's old
menu system, not copy-paste; (b) Resolution W/H int-vars + `pc-get-window-size`
31-frame refresh into jak1/jak2 debug menus; (c) nothing more for jak3's menus.
If approved, all three GAME.CGOs rebuild (jak1's first ever). J9 (jak3 vertical
stretch) still awaits the user's location/handheld-vs-docked answer.


## 2026-11-18 (16:00–16:50) — SD BACK: FULL DEPLOY J11 (both games) + jak3 yellow-menu fix (AI-assisted, Cline)

User report on the OLD card build (jak3 NRO `9e7a86a2`, GAME.CGO `2e84919a`):
(1) menu shows a **yellow background** when pressed (title→menu backdrop),
(2) a **white box** during first-start loading. Diagnosis from the card's
`gk_run_log.txt` (log kept, timestamps Nov 19 = Switch clock):

**Yellow menu = jak3 blit_prev never enabled.** `dkgl_frame.cpp:851` set
`seg.blit_prev` only for jak2 (`s_bucket_ids == s_jak2_ids && BUCKET_3`); the
jak3 `BlitDisplays` at `BLIT_START` (registered in `init_jak3`, comment already
said "blit_prev: reads last frame's image") always replayed as
`dkdev::blit_frame_to` = the not-yet-drawn current frame target → backdrop
empty, sepia tint only. Log corroboration: `[dkgl] … blit bad` 1/frame from
title on (60 at 60 frames, 6314 by 4860). **Fix:** the condition now also
matches `s_bucket_ids == s_jak3_ids && s_cur_bucket id == jak3::BLIT_START` →
replays via `dkdev::blit_prev_frame_to` (game-agnostic, jak2-proven HW path).
`check-dk.py` PASS. dk-lint PASS.

**White box during first-start loading:** prime suspect = FIX 124/125 class
(Loader evicts pages still drawn; the card NRO predated FIX124) — the pending
NRO includes it. Sprites never missed (`sprite frame … no_tex=0` all session),
so not a skipped sprite batch; if it persists on the new build, next probe =
name the dkgl bucket behind the `notex` (~2–13/frame) stale-binding draws, and
check the intro `PC-WARP` entry (`[sprtex] tbp=1216 id=0 … PC-WARP/PC-WARP`,
15.9 s — pc-port texture with no dktexgpu id, warp quad untextured) and the
`introcst-spritethrust-glob` placeholder misses at tbp=1152 (transient, data
lands late). **Awaiting HW re-test.**

**Deployed (all md5s verified on card after copy):**
- jak3: NRO `30ec435fd1bf9865edfc1754b4ce1b63` (docker clean 69/69; supersedes
  `c561d1b6` — adds the blit_prev fix on top of FIX124/125 + J7) + GAME.CGO
  `8a1ade2d1c56d08362b93a3ee1a85275` (J10). Whole iso tree verified
  byte-identical local↔card, KERNEL.CGO `2cab49e7` — only the NRO changed.
- jak2: NRO `65ff6b7e336dc4f7a65a5800ce45f3bd` + GAME.CGO
  `c18420bb05d7d2088e8dcd48a2ab7da7` + TSZ.DGO `c69c0e0a56626818fcc3c1ef2756400a`
  (relink pair, KERNEL.CGO `f6698f65` already matched).
- Backups (6 files) in `~/Desktop/jak bakcups/2026-11-18-j11-blitfix/`
  (old jak3 9e7a86a2/2e84919a, jak2 80ff1eb3/8da29cac/0efa34e0).

**ANOMALY — concurrent jak2 build:** the local jak2 pair was rebuilt at
16:42–16:44 by something other than this session (no docker/build process of
mine; my build was jak3-only and build-switch.sh never touches `out/*/iso`).
The artifacts match the current tree (FIX124/125/126 + J7 + the blit_prev edit,
behaviour-neutral for jak2), so they were deployed. If the user has a second
agent/session running, coordinate before the next build; the summary's
`d2b7a47d`/`e5ff3989` md5s were stale — the 16:42/16:44 build supersedes them.
Language files recreated by the jak2 `make-group iso` were NOT copied
(card stays pruned; verified `NOT-ON-CARD` set is exactly VAGWAD.*/SCREEN1.*
languages + `*COMMON.TXT`).

**HW test checklist (jak3 first):** (1) boot sanity into city; (2) title →
press Start: menu backdrop should now show the captured frame, not yellow;
(3) pause menu same check; (4) first-start loading: white box gone? (if not,
note WHEN it appears and keep the log — probe next); (5) debug PC Settings:
Sizes gone, Resolution W/H readout correct handheld/docked; (6) pause Graphics
regression; (7) J7: no `kickoff failed res=0xd5c`, `[deko3d] gpfifo 1s:` count
down ~600; (8) J9 stretch observation (where + handheld/docked). jak2 quick
pass: warp/blackout OK, Bad Weather cheat off now clears instantly (FIX 126),
gpfifo check.


## 2026-11-18 (17:00) — ADDENDUM (FIX 125/126 session, Cline)

Cross-reference for the J11 entry above: the "concurrent jak2 build" it noticed was
**this session** — the 16:42 docker NRO build (64/64 clean, /tmp/fix125_build.log) and
the 16:44 goalc arm64 `make-group iso` (933 targets, 11.5 s, **mood.gc/mood-h.gc
compiled clean** → GAME.CGO `c18420bb…`). Both artifacts were then independently
deployed twice (two sessions racing on the card at ~16:45) — final card state
re-verified serially: jak2.nro `65ff6b7e336dc4f7a65a5800ce45f3bd`, GAME.CGO
`c18420bb05d7d2088e8dcd48a2ab7da7`.

Note on the `GAME.CGO.bak-8da29cac` anomaly: the backup rotation misfired because
the J11 whole-iso copy had already written the new GAME.CGO to the card; the true
pre-FIX126 rollback copy is in `~/Desktop/jak bakcups/2026-11-18-j11-blitfix/`
(md5 verified `8da29cac…`). The redundant duplicate created here was removed from
the card; card now holds live GAME.CGO + f01/f02 baks only. Rollback order for jak2
GAME.CGO: Desktop `8da29cac` (pre-FIX126) → card `f02.bak` (Nov 4).

`check-dk.py` PASS after the final tree state. Committed: TexturePool.h, Loader.cpp,
dk_renderer.cpp (FIX 125) + mood.gc/mood-h.gc (FIX 126). This handoff file is left
uncommitted (shared with the in-progress jak3 session's J11 entry).

## 2026-11-18 — BLACK WALLS DEEP-DIVE: full log re-analysis of jak2-nozb1 (FIX 123 was only 1 of 3 mechanisms) (AI-assisted, Cline)

**Why:** user asked to dig into the Jak 2 black-wall issue "we didn't
understand". The nozb1 capture (`hw-logs/jak2-nozb1-20261118-1416/`,
gk_run_log 528k lines / 18 sessions + gk_stdout 39 boots) had NEVER been
fully analyzed — FIX 123 looked at one session only. Re-correlated every
MISS against its pool entry's give/link/unload history (state simulation,
correct event ordering) + aligned the stdout purge/reclaim/ready stream.

**Data (sessions 16/17/18 = instrumented builds; s16 IS the f123 tour):**
- s16 (379 s): gungame 2405 given 56.6 → re-linked 176.884 (GOAL upload-now)
  → **purged 177.799** with 6 other levels → MISS 178.2→379.1 (end), never
  re-given. ctysluma 1021 given 10.8 → purged 35.6 (player left slums) →
  misses to 107.6, never re-given. hiphog 998/ctyport 1325 startup misses
  at ~35.5 heal in <0.1 s (give was 50 ms later — benign).
- s17 (371 s): 998/1325 purged 59.4 (exit purge entering ruins) → re-given
  +5–9 s (reload) = brief flash; a later purge left 998 black for **+171–219 s**
  until the player's return reloaded hiphog. NEW page class: **853
  ruins-vis** (14 misses 69.6→261.8) — NOT in the probe set, so its
  give/unload telemetry is unknown; fr3 says pool=1 (should be given at
  ruins load; no purge/reclaim of ruins in that boot — suspect texture-stage
  streaming lag after "ready" or repeated re-miss; NEEDS 853/929/2929/3090
  added to tex_probe_page).
- s18 (191 s): **boot purge at 31.3 recycled title/lwidea/ctywide/ctysluma
  while the intro still draws them** → title-sprite 3090 (12 misses) +
  lwidea-pris 2929 (34) black 33.3→~45 s, healed by lwidea reload.
  ctysluma 1021 black 33.3→93.1 (~83 s) until the slums reload.

**The mechanism (all three classes, one root):** `purge_retired_levels`
(Loader.cpp:1878, called from `update_blocking` at blackout-exit) unloads
pool pages of every level not in `m_active_levels`/`m_desired_levels` at
that instant. GOAL keeps drawing tbps linked to those pages (HUD/sprites of
other areas, revisits, intro), `dk_sprite_lookup` finds `is_placeholder` →
SpriteDk SKIPS the batch → black. **Nothing ever re-gives a purged page**
unless the owning level file happens to reload at a later area transition
(GOAL doesn't re-request levels its own cache thinks are resident; the
loader does not auto-load desired-but-missing levels). Eviction
(`pick_eviction_victim`/`reclaim`) does the same via a second door.

**Verdict on FIX 124/125:** deferral treats the symptom. Measured
black-window (unload→re-give) durations: 5–9 s, 82.6 s, up to 218.8 s —
**the 60 s (3600-frame) window does not cover them**; the pressure
fallback (120 frames) makes it worse exactly when memory is tight. Pages
drawn continuously (the s16 gungame HUD case) should now survive f125
(stamps keep refreshing — needs HW confirm), but "left area, came back
>60 s later" walls will persist.

**Fix directions for next session (my recommendation, not implemented):**
1. *Slot-linked protection (PS2 semantics, cheapest correct fix):* in
   `unload_texture_if_idle`, also defer/refuse when the entry still has
   linked slots (`entry->slots` / `m_textures[tbp].source == entry`).
   Slots are only re-pointed by a new `handle_upload_now` — i.e. a page
   lives until GOAL overwrites it, exactly like PS2 VRAM. Memory is
   bounded by the existing pressure path. Verify first whether
   `unload_texture` already detaches slots (it calls refresh_links — read
   it before implementing).
2. *Re-give on miss:* keep `src_data` alive on the entry after unload and
   lazily re-upload (budgeted); heavier, only if (1) is insufficient.
3. Add 853/929/2929/3090 to `tex_probe_page` for the next HW run so the
   never-given class gets telemetry (853 ruins-vis is pool=1 in fr3 and
   still missed for 200 s — unexplained).
4. Optional: `purge_retired_levels` should skip levels retired < ~2 s ago
   (the s18 boot purge killed pages the intro was still drawing; the
   gungame purge fired 1 s after GOAL re-uploaded the course tpages).

Artifacts: `/tmp/j2_events.pkl` (parsed event stream), this analysis only —
no code changed. check-dk.py untouched (no dk edits).

## 2026-11-18 (18:00) — FIX 131: slot-linked protection — the structural black-wall fix, IMPLEMENTED + BUILT (AI-assisted, Cline)

Follows the BLACK WALLS DEEP-DIVE above. Implemented recommendation #1
(slot-linked protection, PS2 VRAM semantics) — note **FIX 126/127/128/129/130
were already taken** by the mood/cheats/settings series, so this is **FIX 131**.

**What changed (4 files, all unload doors covered — every door funnels through
`Loader::unload_level_gpu_objects` → `TexturePool::unload_texture_if_idle`):**
- `TexturePool.h/.cpp` — FIX 131: an unload that would remove the **last GPU
  copy** of an entry that **still has linked slots** (`slots` / `mt4hh_slots`;
  `unload_texture` never unlinks them — that was the bug) is held for a new
  slot-guard window instead of the draw-stamp window:
  - normal = `kSlotGuardFrames` (0xFFFFFFFF = hold while linked, PS2 semantics);
  - pressure = `kSlotGuardFramesPressure` = 3600 frames (~60 s idle) — an order
    of magnitude above FIX 125's 120-frame pressure window, which is what let
    the s16 gungame purge kill course tpages GOAL uploaded 0.9 s earlier;
  - non-last copies unload exactly as before (can never flip an entry to
    placeholder, so can never blacken anything);
  - mirrored in `retry_deferred_unloads` (the guard is re-evaluated every
    frame, so a held page releases via the normal idle window once a later
    `handle_upload_now` re-points its slots — PS2 VRAM turnover, which is what
    bounds memory);
  - desktop / `gk_dkgl_nopooldefer.txt`: `defer_frames()==0` bypasses FIX 131
    exactly like FIX 124 (old behavior, byte-identical).
  - New telemetry: `[pool] FIX131 slot-guard hold %u/%u (%zu tbp + %zu mt4hh
    links)` (rate-limited like the FIX124 line) so hardware logs can tell the
    two defer reasons apart.
- `dk_renderer.cpp` (`dk_loader_start`) — sets the slot guard at pool creation,
  same `gk_dkgl_nopooldefer.txt` kill switch; boot log now reads
  `[dk] pool defer unload ON (FIX124/125/131: 3600 frames, slot-linked held)`.
- `Loader.cpp` (`update`, FIX 125 block) — pressure toggles the slot guard too
  (held-while-linked ↔ 3600 frames), log line extended.
- `tex_probe_page` — added the never-given miss class from the nozb1 log:
  **853 ruins-vis, 929 ruins, 2929 lwidea-pris, 3090 title-sprite** (telemetry
  only; their give/purge history is still unknown).

**Why this is structural and FIX 124/125 wasn't:** draw stamps idle during
blackouts, so the age check expired exactly when the blackout-exit purge fired,
and the game re-drew the purged tbps for minutes after (measured 82–219 s).
The slot link is the game's own declaration "this VRAM address holds this
texture" — the only signal that survives a blackout. On PS2 the page would
simply still be in VRAM.

**Left alone deliberately:** `do_reload_common` still calls `unload_texture`
directly (explicit REPL dev command that unconditionally deletes ids right
after — deferring there would break its delete loop). Handoff rec #4 (purge
skip <2 s) not needed: FIX 131 protects the textures regardless of purge age.

**Status:** check-dk PASS · docker Switch build clean 66/66, no new warnings ·
`build-switch-jak2/game/gk.nro` rebuilt 18:12+ (AI-assisted). **DEPLOY BLOCKED:
SD card unmounted** (`/Volumes/` shows only Macintosh HD). When remounted:
deploy as `jak2.nro` per the usual rotate-old-build procedure. **HW verify:**
expect `[pool] FIX131 slot-guard hold` lines at purges/evictions, and the
s16/s18/s17 scenarios (gungame course, boot intro logos, ruins revisit)
staying textured; the never-given class (853/929/2929/3090) needs the new
probe lines to diagnose — FIX 131 does NOT fix that class (no unload is
involved; page 853 is pool=1 in ruins.fr3 yet never given — still unexplained).
Desktop regression safety: FIX 131 is inert when defer==0 (desktop default).


## 2026-11-18 (Mac clock) — FIX 128: weather selection persists (jak2, sun survives reboot) (AI-assisted, Cline)

**User report:** "I select the sun, save, reboot — it rains again."

**Root cause (from the card's own settings file):**
`/Volumes/SWITCH SD/switch/jak2/OpenGOAL/jak2/settings/pc-settings.gc` had
`cheats #x2404b` = weather-bad (0x20000) ON + weather-good (0x40000) OFF.
Serialization + boot restore were both CORRECT (FIX 127 works — the other bits
prove it); the saved state itself was wrong. Only the in-level mood code
(`mood.gc`, FIX 126) cleared the opposite weather bit, and only AFTER
unpausing — so a lingering weather-bad bit survived a "weather-good"
selection made while paused, and every boot restored the storm.

**Fix (GOAL, jak2 only; jak3 deferred by user):**
- `engine/debug/default-menu.gc` `dm-game-opengoal-cheat-toggle-pick-func`:
  the two weather flags are now mutually exclusive AT TOGGLE TIME — toggling
  weather-bad ON clears weather-good and vice versa (the newly selected one
  wins), then FIX 127's immediate `pc-settings-save` persists exactly that.
- `pc/progress/progress-pc.gc` pause-menu Cheats page: same mutual exclusion
  after its `logxor!`, plus the same guarded immediate save (speedrunner mode
  skipped) — pause-menu toggles previously relied on update-cheats' frame
  diff only.
- **Card settings fixed directly:** `cheats #x2404b` → `cheats #x4404b`
(weather-bad OFF, weather-good ON = sun selected). The very next boot is
sunny with no user action needed. All other bits preserved.

**Build/deploy (GOAL arm64, `make-group "iso"`, 933 targets, 0 errors; no NRO
change needed — pure GOAL):**
- Deployed jak2 GAME.CGO `9083cc5878545fd3c81ca4d8f284c9f6` (card md5
  verified after copy + sync, `._GAME.CGO` purged). KERNEL.CGO unchanged
  `f6698f65`. NRO unchanged `65ff6b7e`.
- **Anomaly:** the card's pre-deploy GAME.CGO was `80a3f9f9a62d3c34ab937dda23b6d61e`,
  NOT the `c18420bb…` recorded in the FIX 127 entry — some other build was
  deployed after that handoff. The `80a3f9f9` copy (rollback) is saved at
  `~/Desktop/jak bakcups/2026-11-18-weather-fix128/GAME.CGO.pre-f128.bak`
  (md5 verified). The new GAME.CGO is built from the current tree, so it is
  a superset of everything committed here.

**HW test:** boot jak2 → should be SUNNY immediately (settings already set).
Then: toggle weather-bad ON (rain) → power off → boot → still rain; toggle
weather-good ON (sun) → boot → sun. The last selection always wins and the
menu shows exactly one weather flag ON at a time.

**jak3 deferred (user: "then we think about jak 3"):** jak3's toggle func and
SD file are fine (FIX 127 present, `cheats 0x0` on card), but jak3's
`mood.gc` never consumes the weather cheat bits — they are dead cheats
(no `pc-cheats` reference in `goal_src/jak3/engine/gfx/mood/`). A jak3 fix
needs the FIX-126-style mood port (pin cloud/fog + rain on bad, clear on
good, plus the lightning guard) in jak3's mood code, which has a DIFFERENT
structure from jak2's (different locals/seek pattern — not a blind copy).

Not committed yet (held for user approval per workflow).

## 2026-11-18 (Mac clock) — FIX 129 + FIX 130: jak1 debug-cheats persistence, jak2 duplicate settings line (AI-assisted, Cline)

**User ask:** "in the meantime spot bugs in jak1 and jak2, analyse everything, do bugfixes if necessary, write what you do and list the issues."

**Scope:** sweep of the settings/cheats persistence code paths (same area as FIX 127/128): pc-settings writer/parser pairs, debug + pause cheat menus, cheat consumers, and the shared-file reuse across games.

**FIX 129 — jak1: debug-menu cheat toggles never persisted (same class as jak2's FIX 127).**
- `dm-pc-cheats-pick-func` and the five inline eco-blue/red/green/yellow + invincibility
  lambdas in `pc/debug/default-menu-pc.gc` "PC cheats" page only XOR'd the bit. jak1's
  `update-cheats` is a no-op stub (jak2's does the within-frame diff save), so nothing
  ever wrote pc-settings.gc after a debug-menu toggle — the last previously-saved
  value came back on every boot.
- Second trap: even when the file got written by any other settings commit,
  `handle-input-settings` masks the saved cheats to `cheats-known` on load
  (anti-cheat: you only keep cheats you've unlocked), so unearned toggles silently
  reverted at boot anyway.
- Fix: new `dm-pc-cheat-save!` helper — ORs the cheat into `cheats-known` (same
  grant the debug-mode l1+r1 unlock combo already makes, progress-pc.gc:2618) and
  commits the file, guarded by `speedrunner-mode?` (matches jak2's FIX 127 rule
  that speedrun cheat changes are never persisted). Called from the pick-func
  and all five inline lambdas.

**FIX 130 — jak2: every saved pc-settings.gc contained a duplicate `(subtitle-speaker ...)` line.**
- `handle-output-settings` calls the SHARED parent writer first
  (`(method-of-type pc-settings handle-output-settings)` — that is jak1's
  `pckernel-common.gc`, which jak2 compiles directly; see the "hardcoded cases to
  grab from jak1" rule in `goal_src/jak2/lib/project-lib.gp:221`), which already
  writes `(subtitle-speaker ...)`, then wrote it AGAIN at pckernel.gc:842. Both
  parse into the same field, so harmless — but real dead duplication (visible in
  the card's own settings file). Removed the second write. jak2's own redundant
  `subtitle-speaker` parser case (787) left alone — harmless, keeps the diff
  minimal.

**Verified non-bugs (no changes made):**
- FALSE LEAD from the earlier session: "jak1 handle-output-settings omits cheats" —
  wrong. The shared BASE writer omits it, but jak1's `pc-settings-jak1` child writer
  persists `(cheats known current)` in the `(secrets ...)` group (pckernel.gc:508)
  and parses it back (466-468). jak1 cheat serialization was never broken; only the
  debug-menu save trigger was missing (FIX 129).
- jak1: every cheat has a consumer (sky pckernel.gc:136, tunes flava-hack:135,
  no-tex gfx-hack:141, mirror, eco, invinc, heads, sidekick-blue); the eco cheats
  are already mutually exclusive in the debug menu (logclear! pattern); the
  cheats-known masking itself is intentional anti-cheat and is kept.
- jak2: writer↔parser key pairs complete (checked both directions); the
  `(cheats-backup ...)` parse case is a deliberate legacy no-op.
- The shared base writer/parser (compiled into ALL THREE games) is key-complete.

**Build (validation only — NOT deployed):** `./build-host/goalc/goalc --game jakN --instruction-set arm64 --cmd '(make-group "iso")'`
- jak1: 546 targets, 0 errors (log `/tmp/goalc_jak1_f129.log`)
- jak2: 933 targets, 0 errors (log `/tmp/goalc_jak2_f130.log`)
- New local CGOs (supersets of all prior fixes; the on-card jak2 GAME.CGO is still
  the FIX 128 one, md5 `9083cc58…`):
  - `out/jak1/iso/GAME.CGO` md5 `939ffde7e1da806de6bd7d6edf4936c2`
  - `out/jak2/iso/GAME.CGO` md5 `104e34cc97470c2e30589415c656c393`

**HW test (jak1, after a deploy):** debug menu → PC cheats → toggle e.g. "Big head
jak" ON → power off → boot → still ON (and it shows unlocked in the in-game secrets
menu too). Toggle OFF → reboot → OFF.

**jak3 note (for the pending jak3 patch):** jak3 has the SAME duplicate
subtitle-speaker write at `goal_src/jak3/pc/pckernel.gc:734` (parent call at :716) —
added to the jak3 patch list. No jak3 files were touched (other agent's build).

Not committed; not deployed (both held for user approval per workflow).


## Full-codebase audit — jak1/jak2 GOAL (2026-11-18, AI-assisted)

Scope: every Switch-touched hunk in `goal_src/jak1` + `goal_src/jak2` — all 11 `FIX 1xx`
markers (104, 126, 127 ×4, 128 ×2, 129, 130), all 15 `(pc-get-os) 'switch` branch hunks,
the whole pc-settings persistence chain (writers, parsers, commit triggers, cheat-bit
mutation sites), the speedrun gating, and the debug/progress cheat menus.
The 1100+ `og:preserve-this`/`PC_PORT` markers are the upstream PC-port scaffolding,
not Switch changes; the Switch-specific surface above was audited exhaustively.

**Result: no new actionable bugs found.** Everything verified:

- All FIX 104/126/127/128/129/130 hunks re-reviewed line-by-line — logic sound.
  - FIX 126 OFF-transition: `weather-cheat-state` starts zeroed (GOAL zero-init),
    snap-to-target only fires on cheat→OFF, good/bad branches and the rain/
    lightning gates are consistent with it.
  - FIX 129: all 5 eco/invinc lambdas call `dm-pc-cheat-save!`; helper ORs into
    `cheats-known` (survives the load-time mask), skips commit in speedrunner mode.
    Invinc lambda's raw `16` = `(state-flags invulnerable)` bit 4 ✓.
  - FIX 128: mutual exclusion at BOTH toggle sites (debug menu + secrets menu),
    each followed by a speedrun-gated `pc-settings-save`.
- Writer↔parser key parity re-checked mechanically (both games, both directions);
  remaining deltas are intentional legacy parse stubs (`cheats-backup`,
  `hint-subtitles?`, old audio/gfx keys) kept because the parser can't skip
  unknown keys. `subtitle-speaker` is written once by the shared
  `pckernel-common.gc` writer (the jak2 copy at :842 was the duplicate, FIX 130).
- jak1 `(cheats known current)` write order matches parser read order.
- jak2 `pc-cheats` enum ↔ debug-menu flag numbers (1…524288) match exactly;
  `(/ arg0 8)` is the binteger untag, consistent everywhere.
- All cheat-bit mutation sites either save immediately or are covered by
  jak2's frame-diff `update-cheats` save; speedrun paths correctly never persist.
- `scripts/ai/check-dk.py`: `dk-lint: PASS` (0 files — nothing uncommitted to lint).

**Notes (not fixed — judged intentional/by-design, flag if you disagree):**
- N1: jak1 debug-menu PC Settings booleans (Letterbox, Skip movies, Money starburst,
  Subtitles, Hinttitles, subtitle-speaker) flip `*pc-settings*` without a commit;
  that menu has an explicit "Save" item (default-menu-pc.gc:696), so this matches
  upstream behavior. Cheats were the deliberate exception (FIX 129, user request).
- N2: jak1 options-menu L1+R1 `cheats-known` grant (progress-pc.gc:2616-2619)
  commits only on a later options action; upstream code, harmless in practice.
- N3: `give-cheat!`/`lock-cheat!` (jak2 pckernel-impl.gc:308/313) are dead macros —
  zero callers. Upstream; left alone.
- N4: jak2 mood.gc still `logclear!`s the opposite weather bit every frame while a
  weather cheat is active — now redundant after FIX 128's toggle-time clearing, but
  harmless (never triggers a save on its own).
- jak2 mood/weather, jak3-inherited FIX 130 site and the rest of the jak3 patch
  list remain deferred until the other agent's jak3 build completes.

Still held for user approval: commit of FIX 128/129/130; SD deploy of the new
jak1/jak2 CGOs (+ HW tests: jak2 weather sun/rain reboot, jak1 debug-cheat reboot).



## 2026-11-18 (evening) — JAK3 SESSION: FIX 128/129 weather cheat shipped; J9 aspect root-caused + FIXED (AI-assisted, Cline)

User directive (going out): finish the weather cheat, then do everything that
doesn't need the user (J7-J10 + any missing rendering/bug work).

### FIX 128/129 — jak3 weather cheat COMPLETE (code + build; commit f67db853)
mood-h.gc: weather-cheat-state field on mood-control (after clouds). mood.gc
FIX 129, all inside the og:preserve-this guard: cheat consumption block after
the game-secrets bad-weather branch (pins current-interp cloud/fog — bad = 1.0
storm, good = 0.0 clear, restores target-interp on OFF, logclear!s the
opposite bit), rain pin (good → f30-3 0.0 / bad → unclamped f0-125), lightning
suppressed while weather-good. default-menu.gc FIX 128: the cheat toggle
clears the opposite weather bit at toggle time (the persistence killer).
Full arm64 make-group: 1164 targets, 0 errors.

Also committed in f67db853 (prior session's reviewed work): jak2 FIX 127/128
mirrors (default-menu.gc + progress-pc.gc), the J11 dkgl blit_prev fix for
jak3 BLIT_START (menu backdrop), J10 Sizes drop, all handoff/TODO doc entries.

### J9 — "stretched in height" ROOT-CAUSED + FIXED (no user info needed)
The same bug jak2 had on HW 2026-10-31 (DKGL_TODO #203): dk_device.cpp:684
picked the non-jak1 y-scale vert blobs only for `g_game_version == Jak2`, so
jak3's NATIVE world passes (tfrag/tie/shrub/sprite3) ran with the JAK1
constants (HEIGHT_SCALE 1.0 / SCISSOR 448) and drew ~1.86x too tall, while the
dkgl-captured buckets (merc2/ocean/shadow — use_jak2_verts() covers every
non-jak1 game) had the correct 0.5/416 scale — geometry towering over the
characters reads as "everything stretched". GL-PC (Shader.cpp:68-70) and dkgl
both treat jak3 as jak2; the native pick now does too (jak2_style = non-jak1,
Jak1=1 via the forward-declared enum). hfrag — a jak3-ONLY native pass — was
missing from _JAK2_SHADERS entirely: added (hfrag.vert -> hfrag_vert_jak2_dksh,
CMake _JAK2_SHADERS + the load pick). jak1/jak2 behavior byte-identical.
Eliminated on the way (all byte-identical jak2<->jak3, jak2-dk HW-correct):
settings force, update-video-hacks, math-camera PC block, draw-raw-image
pillarbox, progress/HUD pages. For the record jak3's authored camera space
differs (math-camera init x-clip 512/y-pix 208/y-clip 416 vs jak1/2
1024/112/448) — GL-PC handles jak3 with the shared jak2 constants, so the
dk pick must match that; if any stretch remains after this fix it will be
UNIFORM (world+characters together) and the frame composition is next.
**jak3 NRO: docker 7/7 clean, md5 a346eaa3c02af4cb4793c577f8cbe701**
(supersedes deployed 30ec435f; the `c561d1b6` note in the J7 row was stale —
J11 already deployed 30ec435f/65ff6b7e). NOTE: NRO relinks are nondeterministic
— my re-build of the UNCHANGED tree earlier gave bbb9a871 != 30ec435f; don't
panic at tree-identical md5 drift, deploy the recorded artifact.

### FIX 130 — jak3 mirror applied
Same duplicate (subtitle-speaker ...) write at goal_src/jak3/pc/pckernel.gc:734
(parent writer at :716 already writes it; parser case kept at :676).
**jak3 GAME.CGO rebuilt: md5 3861ef8b28e891505cdf90a9e1921695** (16,327,504 B;
FIX 128/129 + J10 + FIX 130) — supersedes 3274cebc AND the deployed 8a1ade2d.

### CONCURRENT SESSION NOTE + FIX-numbering collision
The jak1/jak2 session is live in this tree (its own handoff sections; its
jak1 FIX 129 + jak2 FIX 130 source edits are UNCOMMITTED and left alone).
"FIX 129" now names two different fixes: this session's jak3 weather-mood
cheat (commit f67db853) and their jak1 debug-cheat persistence (their section
below). Disambiguate as FIX 129-jak3 / FIX 129-jak1 in future references.
Their CGO builds superseded mine mid-session: jak2 GAME.CGO is now THEIR
104e34cc (mine 9083cc58 was transient; their build includes FIX 130-jak2 +
all committed work).

### DEPLOY CHECKLIST — SD card currently NOT MOUNTED (everything below waits for it)
1. jak3 NRO build-switch-jak3/game/gk.nro md5 a346eaa3c02af4cb4793c577f8cbe701
   -> sdmc:/switch/jak3/jak3.nro (rotate old 30ec435f to .bak-30ec435f;
   desktop copy per convention).
2. jak3 GAME.CGO out/jak3/iso/GAME.CGO md5 3861ef8b28e891505cdf90a9e1921695
   -> card data iso dir (rotate 8a1ade2d). Copy ONLY this file — the
   make-group iso re-created pruned language files; the card jak3 pruning
   set is still unverified.
3. jak2 GAME.CGO out/jak2/iso/GAME.CGO md5 104e34cc97470c2e30589415c656c393
   + TSZ.DGO out/jak2/iso/TSZ.DGO md5 533f0ad04635d423c93577cd511f3283
   (relink PAIR, rotate c18420bb/c69c0e0a). KERNEL.CGO f6698f65 unchanged.
   jak2 NRO 65ff6b7e already current on the card.
4. (Other session, coordinate) jak1 GAME.CGO out/jak1/iso/GAME.CGO
   md5 939ffde7e1da806de6bd7d6edf4936c2 — their FIX 129-jak1 build, first
   jak3-side CGO change for jak1; they held deploy for user approval.
MD5-verify after every copy, remove ._* sidecars, eject clean.

### HW TEST CHECKLIST (user, one sitting)
(1) jak3 weather cheats: bad ON -> storm/rain/lightning; good ON -> clear sky,
    rain stops, no lightning; OFF -> normal weather resumes; reboot -> last
    choice persists; exactly one weather bit on at a time. jak2 same.
(2) J9: world proportions correct — character vs geometry now consistent
    (they already matched merc2/ocean). Note handheld vs docked anyway.
(3) J7: no kickoff-failed 0xd5c spam; [deko3d] gpfifo 1s entries down ~600.
(4) J11 menu backdrop shows the frame (not sepia); J10 Sizes gone, Resolution
    readout correct.
(5) G-3 intro white box: still open (data-lands-late hypothesis) — note when
    it appears/resolve and keep the log.
(6) jak1 debug-cheat reboot persistence (other session's FIX 129-jak1).

Left open on purpose: J8 (jak2 soak probes — needs HW), G-3 probe (needs the
new log), the white-box-during-loading probe (same), jak1 Graphics page port
(J10 follow-up, awaits user go/no-go).

## Half-res sky: pre-boot static audit + zero-draw guard (AI-assisted, 2026-11-18)

User green-lit the "make the sky cheap" rebuild. Status: the §7.2 half-res sky
render target (640x360 dkgl FBO, ~4x less 3D fill, composite blit back) is
already on the card — current jak2 NRO 65ff6b7e includes it (c70467a6c chain),
deployed by the jak3 session. It has still NEVER run on HW.

This session: full static trace of the record flow (snapshot blit_framebuffer ->
blit_frame_to_tex segment = HW-proven DepthCue shape; offscreen bind recorded
in-list by the pending-target flush, ocean pattern; composite blit_to_frame
carries its own DkBarrier_Full + Image invalidate = P3 covered; read-FBO left
at 0; tex_image_2d pool-full falls back to full-res). One real edge case found
and FIXED: a zero-draw sky bucket frame (menus/loads/fades) still composited
the snapshot back = a 720->360->720 linear round-trip, i.e. whole-frame blur
where GL is a no-op. sky_halfres_end(bool drew) now skips the composite when
the bucket emitted no draws; the 10-s stats line gained "nodraw K".
dk-lint PASS (5 files), check-switch jak2 PASS -> new NRO
120ea63517bf5fbb9a2e006c22f1d109 (built from the shared tree, so it also
carries the jak3 agent's uncommitted J9 pick — verified behaviour-neutral for
jak2: same _jak2_dksh selection, hfrag is a jak3-only pass).

Deploy: SD not mounted during this session — when next inserted, rotate
jak2.nro 65ff6b7e -> jak2.nro.bak-65ff6b7e, copy
build-switch-jak2/game/gk.nro (md5 120ea635…), MD5-verify, purge ._* sidecars.
Then the HW check list for the half-res sky: boot log shows
"[dkgl] sky half-res on"; Dead Town/Haven sky looks right (orientation,
colours, alpha, no blur); gpu top b5 ~1-1.5 ms (was ~3); no gun-course-haze /
warp-red-cone regression (they read the frame the composite writes); 10-s
line "sky halfres N fail 0 nodraw K" (nodraw > 0 on menus is expected and
fine). A/B: sdmc:/switch/jak2/gk_dkgl_fullsky.txt (reboot).

Next perf levers if still short of locked 60 after the sky lands: gpufence
3-4 ms (CPU/GPU overlap, parked), ocean b6/b310 CPU (parked).

## 2026-11-18 19:30 — FULL THREE-GAME REDEPLOY (user: "build again and push all the new ones") (AI-assisted, Cline)

Rebuilt EVERYTHING with the working tree as it stands (both sessions' work:
commits 0e115874..17f78a48 PLUS the still-uncommitted other-session WIP —
FIX 131 TexturePool slot-guard + probe additions 853/929/2929/3090,
Loader.cpp/dk_renderer.cpp/dkgl_frame.cpp follow-ups, jak1 FIX 129
default-menu-pc.gc, jak2 FIX 130 pckernel.gc).
- GOAL arm64 make-group iso: all 3 OK, 0 errors (deterministic: jak1
  GAME 939ffde7, jak2 GAME 104e34cc, jak3 GAME 3861ef8b match prior builds).
- Docker NROs all OK, 0 errors (jak1 was a FULL 744-object rebuild — its
  build dir was cold): jak1 e516a69b3e790ec48c5c6db4f96b06a9,
  jak2 1cd2c518357eeea471a336844530fa31, jak3 6426abe090fe252956db6f9cf24464d6.
  NRO md5s differ from prior records = relink nondeterminism + new content,
  as expected.
- Full-tree md5 diff out/jakN/iso vs card = jak1: ENGINE.CGO+GAME.CGO;
  jak2: GAME.CGO+TSZ.DGO (pair, KERNEL.CGO f6698f65 still identical);
  jak3: GAME.CGO only. TSZ.DGO relinked again (9be8c134 — the pair shipped
  together).
DEPLOYED to SD (rotated old NROs to .bak-eb13960b/.bak-65ff6b7e/
.bak-30ec435f), md5-verified every file ON the card post-copy, no ._*
sidecars, ejected clean. Card now carries:
  jak1.nro e516a69b + ENGINE d2947045 + GAME 939ffde7
  jak2.nro 1cd2c518 + GAME 104e34cc + TSZ 9be8c134
  jak3.nro 6426abe0 + GAME 3861ef8b
The other session's WIP is STILL UNCOMMITTED (deliberately — its session
owns the commit decision).
HW test order (one sitting): jak3 weather cheats + J9 aspect + intro
white-box watch; jak2 weather + FIX 131 black-walls (watch gk_run_log for
FIX131 slot-guard-hold lines + the new probe pages); jak1 debug-cheat
persistence (FIX 129-jak1).
