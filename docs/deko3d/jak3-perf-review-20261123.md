# Jak 3 performance review — 2026-11-23 (AI-assisted)

Sources: `/Volumes/SWITCH SD/switch/jak3/` logs from the Nov-20 run of the
FIX 142-144 build (`jak3.nro md5 3455b421`), plus the Nov-19 fix152 session in
`hw-logs/`. No code was changed for this review (parallel agent has FIX
136-146 in flight — see `CLINE_HANDOFF.md`).

## Measured state (Nov-20 session, desert/Haven)

- 60.0 fps most of the session; dips to ~53 fps for a few windows
  (`slow 399: gpu ...` in one window, `cpu 7` in another).
- Per-frame CPU: `chainwait ~0 | capture 7.85 | build 1.2-1.7 | record 0.9`,
  capture split `dkgl 3.3-4.3 upload 0.5 walk 0.55 texanim 0.4`.
- GPU world: avg 10-12.7 ms, max 75 ms (dips are GPU-driven).
- GPU top: tie6/18/24/30 1.4-2.9 ms, b570 **1.2-3.7 ms**, b5 1.5, b579 0.6.
- Loader: `stage texture took 6-28 ms`, `slow setup up to 13.1 ms`
  (budget 8 ms) → visible hitches while streaming; Jak 3 holds up to 11
  live levels. BCn decode bursts: 326 tex / 31 ms and 738 tex / 48 ms.
- Memory: pools 414 MB, live tex 138 MB / 2630 textures, heap free flat —
  no leak.
- Nov-19 session (different area): **EE-bound** — `chainwait 11.3 ms`,
  `slow 399: ee 397` ≈ 52 fps limited by the GOAL thread, not the renderer.

## Bucket id

`b570 = jak3::BucketId::PARTICLES` (`SpriteDk.h:117`). The #1/2 GPU cost in
Jak 3 is the sprite/particle pass. `#17` early-Z + near-camera fade are
already ON by default for jak3 (flag files only turn them OFF), so the
remaining cost is genuine blended fill-rate, like the jak1 Geyser case.

## Ranked recommendations

1. **Keep BCn compressed on the jak3 dk path (like jak2).** In
   `LoaderStages.cpp:751` `decode_level_bcn_to_rgba` returns early only for
   `GameVersion::Jak2` on the dk path — Jak 3 still CPU-decodes every BCn
   texture to RGBA (4-8x more VRAM + sampling bandwidth, plus the 31-48 ms
   loader bursts and inflated 414 MB pools). The upload side
   (`LoaderStages.cpp:1037-1063`) is also `Jak2`-gated, including the
   32-px-tall deko3d layout-bug decode fallback. Fix = extend BOTH gates to
   jak3 (or drop the version check when `dkdev::is_up()`).
   ⚠️ **`sdmc:/gk_nodecode.txt` is NOT a safe A/B for jak3 today**: it skips
   the decode but the upload branch is still Jak2-only, so level textures
   hit "malformed BCn texture skipped" → world renders untextured. The gate
   must be changed in code first, then A/B on HW (visual regression: any
   32-px-tall BCn sampled as b/w blocks; the fallback handles it).
   Expected: smaller pools (VRAM headroom), lower GPU sampling bandwidth on
   the tie/tfrag-heavy buckets (the actual GPU limit), faster `stage
   texture`, fewer loader hitches.
   **STATUS: implemented 2026-11-23 as FIX 154** (reviewer-direct, <40-line
   change per the AI_ROLES cost law): the decode-skip early-return in
   `decode_level_bcn_to_rgba` and BOTH upload gates (32-px fallback +
   `upload_bcn_mipped`) now accept `GameVersion::Jak3` via `dk_bcn_game`;
   one-shot log `[texfmt] FIX 154 jak3 dk path: BCn kept compressed (no CPU
   decode)`. With this in place `gk_nodecode.txt` IS a safe jak3 A/B (the
   upload then sees RGBA and never reaches the BCn branch). Jak1 and the
   desktop/GL paths unchanged. Build + HW test pending.
2. **Loader hitch spread.** `slow setup` 13 ms > 8 ms budget happens while
   streaming (11 live levels). After (1), re-measure; if still over, split
   the texture stage across more loader ticks (per-mip upload call count is
   the cost — FIX 90 lesson) or move decode off the loader thread.
3. **GPU dips (max 75 ms) around particles.** b570 spikes with the dips.
   Ideas: tune `set_near_fade` threshold for jak3 (currently 1.0 = fade only
   at full-screen size — `dk_renderer.cpp:938`), and add a b570
   sprite-count/draw-count probe on the worst frames to confirm it's the
   same storm-of-blended-quads class as jak1 #17. Verify fade applies to
   the rendermodes that dominate jak3 (mode-1 2D world; mode-3 beams are
   NOT faded — `sprite3.vert.glsl` fade lives in the mode-1 branch only).
4. **EE-bound scenes (~52 fps, Nov-19 log).** GOAL-thread limit; renderer
   can't fix it directly. Enable the GOAL profiler in one such scene to see
   what the game code is doing. `tod sync 268` is only the boot-time
   fallback count, not a per-frame cost.
5. **Log hygiene (cheap, certain win).** `gk_boot_log.txt` spam
   (`[RpcSync] entering busy-wait` x100s, `[sceOpen]` repeats) and the
   12 MB/section `gk_run_log.txt` are written to FAT32 during gameplay;
   frame-time includes SD I/O. Buffer/trim these (also DKGL_TODO row:
   split run log per boot).
6. **dkgl capture 3.3-4.3 ms/frame CPU** — render-thread cost, currently
   not the limiter (chainwait/gpu are). Park until the GPU side is at
   budget.

## What NOT to chase

- `capture 7.85` total is not one fixable thing: the split shows dkgl
  emission ~4 ms + walk/upload/texanim each < 1 ms; it only matters if the
  render thread becomes the limit.
- Pools/heap: no leak, flat heap free — nothing to fix.
- `tod 1.7-6.8/frame` deferred palette copies: sub-ms, fine.
