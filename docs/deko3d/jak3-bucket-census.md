# Jak 3 bucket census — GL registration walk (AI-assisted, J1)

Written 2026-11-18. **Method: pure code census, no hardware.** Sources, all verified today:

- `game/graphics/opengl_renderer/OpenGLRenderer.cpp:511-786` — `init_bucket_renderers_jak3()`
  (the whole registration list; everything not registered gets an `EmptyBucketRenderer` at
  :774-782).
- `game/graphics/opengl_renderer/buckets.h:415-657` — `jak3::BucketId`, `MAX_BUCKETS = 587`.
- `common/goal_constants.h:69` — `jak3::LEVEL_MAX = 10` (per-level loops are ×10).
- jak2 reference implementation: `game/graphics/deko3d_renderer/dkgl/dkgl_frame.cpp`
  (`kJak2Buckets` :261, `init_jak2` :1537, `init_jak2_foreground` :1312, `dkgl::init` :1664).

This is the input spec for J2 (`init_jak3`) — see `JAK3_BRINGUP_TODO.md`.

## 1. Constants

| | value | note |
|---|---|---|
| `MAX_BUCKETS` | 587 | bucket sanity cap; GL profiler cap already raised (`kBucketProfSanityCap`) |
| `LEVEL_MAX` | 10 | jak2 was 6 — every per-level block is bigger |
| per-level stride (tfrag block) | 12 | `TEX_L0_TFRAG` 10 → `TEX_L1_TFRAG` 22 |
| per-level stride (shrub) | 10 | `TEX_L0_SHRUB` 130 → `TEX_L1_SHRUB` 140 |
| per-level stride (alpha) | 11 | `TEX_L0_ALPHA` 230 → `TEX_L1_ALPHA` 241 |
| per-level stride (pris / pris2) | 5 | `TEX_L0_PRIS` 351 → 356; `TEX_L0_PRIS2` 406 → 411 |
| per-level stride (water) | 10 | `TEX_L0_WATER` 463 → 473 |
| vertex y-scale | shared with jak2 | non-Jak1 constants (`Shader.cpp`); `use_jak2_verts()` applies — `dkgl_frame.cpp:1715` must become "not Jak1" |

Always compute ids with the `GET_BUCKET_ID_FOR_LIST` pair arithmetic exactly like GL
(`(int)a + ((int)b - (int)a) * i`) — never hardcode, ids cannot drift from
`init_bucket_renderers_jak3` then.

## 2. Registration census (bucket → GL renderer → planned jak3 owner)

Owner key: **CAP** = dkgl captured list (renderer runs unchanged through the glad overrides,
jak2 phase-1 pattern); **NAT** = native deko3d pass replaces it (jak2 later-phase pattern);
**TEX** = TextureUploadHandler — merged into the sorted ids like jak2's PrisTexEyes merge
(capture ordering + eye draws); **—** = EmptyBucketRenderer, nothing to do.

### Fixed (non-leveled)

| bucket | name | GL renderer | owner | jak2 precedent |
|---|---|---|---|---|
| 2 | BUCKET_2 | VisDataHandler | NAT | fog/occlusion source — but arrays are sized 6 (`TFragmentDk.cpp:505`), jak3 needs **10** |
| 3 | BLIT_START | BlitDisplays | CAP | jak2 BUCKET_3 "blit" (`blit_prev` copy) |
| 4 | TEX_LCOM_SKY_PRE | TextureUploadHandler | TEX | jak2 tex merge |
| 5 | SKY | DirectRenderer "sky" (1024×8) | CAP | jak2 sky-draw 5 |
| 6 | OCEAN_MID_FAR | OceanMidAndFar | CAP | identical |
| 7 | PROGRESS_HACK | DirectRenderer (0x8000) | CAP | jak2 has no twin — new, but DirectRenderer capture is proven |
| 8 | HFRAG | Hfrag | **NAT** | HfragDk (jak1 bucket 8) — needs `set_game(Jak3)` + bucket id |
| 340/341/343 | TEX_LCOM_TFRAG / MERC / GMERC | TEX / CAP / CAP | | 342 EMERC empty |
| 345/346 | TEX_LCOM_SHRUB / MERC_LCOM_SHRUB | TEX / CAP | | 347 EMERC, 348 GMERC empty |
| 350 | SHADOW | Shadow2 "shadow" | CAP | jak2 §7.2 phase 2 |
| 401/402/404/405 | lcom-pris TEX/MERC/GMERC/GMERC2 | TEX / CAP / CAP / CAP | | 403 EMERC empty |
| 456/457/458/459/460 | lcom-pris2 ids | *(not registered)* | — | **verified by grep 2026-11-18**: `lcom-pris2` / `LCOM_PRIS2` appear nowhere in OpenGLRenderer.cpp — all five lcom-pris2 ids are EmptyBucket |
| 461 | TEX_LCOM_SKY_POST | TextureUploadHandler | TEX | |
| 462 | OCEAN_NEAR | OceanNear | CAP | identical |
| 563/564/566 | lcom-water TEX / MERC / GMERC2(PRIM) | TEX / CAP / CAP | | 565 GMERC empty; note GL puts generic2-lcom-water on **GMERC2**_LCOM_WATER with PRIM |
| 568 | TEX_SPRITE | TextureUploadHandler | TEX | |
| 569 | GENERIC_SPRITE_1 | Generic2 PRIM | CAP | |
| 570 | PARTICLES | Sprite3 | **NAT (or gap)** | see §4 gap G1 — sprite3 program not in the dkgl T103 table; jak2 runs native SpriteDk |
| 571 | GENERIC_SPRITE_2 | Generic2 PRIM | CAP | |
| 572/573 | SHADOW2 / SHADOW3 | Shadow2 ×2 | CAP | two instances like jak2's 195/314; stencil reset between |
| 574 | GENERIC_SPRITE_3 | Generic2 LIGHTNING | CAP | jak2 effects 315 |
| 575/576 | TEX_WARP / GENERIC_WARP | TextureUploadHandler / Warp | TEX / CAP | T128 pattern (blit_framebuffer + Generic2 WARP) |
| 577 | DEBUG_NO_ZBUF1 | TextureUploadHandler (true) | TEX | jak2 318 boot logo |
| 578/579 | TEX_HUD_HUD_ALPHA / HUD_DRAW_HUD_ALPHA | TEX / ProgressRenderer | TEX / CAP | |
| 580/581 | TEX_HUD_PRIS2 / HUD_DRAW_PRIS2 | TEX / ProgressRenderer | TEX / CAP | |
| 582 | BUCKET582 | ProgressRenderer "progress" | CAP | jak2 320 |
| 583/584/585 | DEBUG / DEBUG_NO_ZBUF2 / DEBUG_MENU | DirectRenderer ×3 | CAP | jak2 debug2/no-zbuf2/debug3 |
| — | eyes | EyeRenderer (not bucket-driven) | NAT | #19 eye pass; reached through `render_state->eye_renderer` from Merc2 |

### Per level (×10), tfrag block (stride 12)

| offset from `TEX_L{n}_TFRAG` | name | GL renderer | owner |
|---|---|---|---|
| +0 | tex-l{n}-tfrag | TextureUploadHandler | TEX |
| +1 | tfrag-l{n}-tfrag | TFragment NORMAL | **NAT** (dktf) |
| +2 | tie-l{n}-tfrag (master) | Tie3 | merged (chain holds per-frame setup, jak2 §7.0 E) |
| +3 | etie-l{n}-tfrag | Tie3AnotherCategory NORMAL_ENVMAP | merged |
| +4/+5/+6 | tfrag/tie/etie scissor | *(not registered)* | — |
| +7 | merc-l{n}-tfrag | Merc2BucketRenderer | CAP |
| +8 | emerc | *(not registered)* | — |
| +9 | gmerc-l{n}-tfrag | Generic2 NORMAL | CAP |
| +10 | tie-vanish | *(not registered)* | — |
| +11 | gmerc2 | *(not registered)* | — |

### Per level, shrub block (stride 10, base `TEX_L0_SHRUB` 130)

tex-l{n}-shrub TEX · shrub-l{n} **NAT** (dkshrub) · merc-l{n}-shrub CAP · gmerc-l{n}-shrub CAP.

### Per level, alpha block (stride 11, base `TEX_L0_ALPHA` 230)

tex-l{n}-alpha TEX · tfrag-t-l{n}-alpha **NAT** (dktf TRANS) · tie-t-l{n}-alpha merged ·
etie-l{n}-alpha merged (TRANS_ENVMAP) · merc-l{n}-alpha CAP · gmerc-l{n}-alpha CAP.
(The water variants of the tie categories — tie-w/etie-l{n}-water — are registered on the
water-block ids `TIE_L0_WATER` / `ETIE_L0_WATER`, see below.)

### Per level, pris / pris2 blocks (stride 5, bases 351 / 406)

tex-l{n}-pris TEX · tex-l{n}-pris2 TEX · merc/gmerc/gmerc2-l{n}-pris CAP ×3 ·
merc/gmerc/gmerc2-l{n}-pris2 CAP ×3. (8 registrations per block per level.)

### Per level, water block (stride 10, base `TEX_L0_WATER` 463)

tex-l{n}-water TEX · merc/gmerc/gmerc2-l{n}-water CAP ×3 · tfrag-l{n}-water **CAP**
(jak2 keeps its TFRAG_W buckets captured — `kJak2Buckets` §7.0 D — follow that) ·
tie-w-l{n}-water + etie-w-l{n}-water merged.

## 3. Proposed table composition (input to `init_jak3`)

Mirrors `init_jak2`'s split (`kJak2Buckets` + merged tex + merged tie, then `std::sort` —
the sorted walk is GL dispatch order):

- **`kJak3Buckets`** (always-draw captured): 27 fixed (§2 fixed table, CAP rows +
  `DEBUG_NO_ZBUF1`; the lcom-pris2 ids are NOT registered — nothing to do) + 16 per level
  (merc/gmerc × tfrag,shrub,alpha; merc/gmerc/gmerc2 × pris, pris2; merc/gmerc/gmerc2/tfrag-w
  × water) ×10 = 160 → **187**, plus the four special lists
  `kBlackoutList` / `kTexAnimList` / `kGlowList` / `kDistortList` (T125-127 pattern) → **191**.
- **Merged tex ids** (PrisTexEyes equivalent, TextureUploadHandler rows): 6 per-level lists
  (tfrag/shrub/alpha/pris/pris2/water) ×10 = 60, + lcom TEX 340/345/401/563 = 4
  (TEX_LCOM_PRIS2 456 is unregistered, see §2), + singles 4 / 461 / 568 / 575 / 578 / 580 = 6
  → **70**.
- **Merged tie ids**: per level ×6 (TIE master, ETIE_Ln_TFRAG, TIE_Ln_ALPHA, ETIE_Ln_ALPHA,
  TIE_Ln_WATER, ETIE_Ln_WATER) ×10 = **60**.
- **`kJak3TableMax` = 191 + 70 + 60 = 321** (+1 in FIX 138: `kCopyBackList`, the
  BlitDisplays `do_copy_back` side list — **322**). This becomes the new `kMaxBuckets`
  (`dkgl_frame.cpp:352`) — every `s_lists` / census / renderer array must fit it.
- **Not in the dkgl table** (native passes own them, drawn at their sorted position by
  `record_jak3_dkgl`): tfrag NORMAL/TRANS per level, shrub per level, HFRAG 8, PARTICLES 570,
  vis 2 (fog/occlusion source only).

Exact membership is finalized when `init_jak3` lands (J2) — keep the census log line
(`[dkgl] jak3: bucket %d (%s) missing from kJak3Buckets`) so hardware runs verify the table
against reality, like jak2's.

## 4. Gaps and risks (each one needs a decision or a check in J2-J6)

- **G1 — Sprite3 / particles 570.** The sprite3 program is not in the dkgl T103 program
  table (jak2 ran particles natively via SpriteDk instead). J3 decides: extend the dkgl
  program table with sprite3, or give SpriteDk a jak3 bucket set. Either way the
  distort/glow/texanim list order (T125-127) must wrap bucket 570 the same way jak2 wraps 313.
- **G2 — occlusion/fog sizing.** `capture_fog_jak2` + `s_occ[6][128*16]`
  (`TFragmentDk.cpp:505-530`) walk 6 levels; jak3 has 10. Per-game sizing + the
  `jak3::BucketId::BUCKET_2` base for the DMA walk.
- **G3 — HfragDk.** Fully implemented for jak1 (bucket 8). Needs `set_game(Jak3)` +
  the jak3 HFRAG id (also 8 — trivial, but the pass tables must be checked for jak1-only
  assumptions, e.g. tree kinds the jak3 Hfrag chains use).
- **G4 — `kMaxBuckets` growth.** 26 (jak1) / ~100 (jak2 merged) → 325. Static arrays:
  `s_lists`, `s_eye_list`, census arrays, `s_generic_buckets`, `s_merc_buckets`,
  `s_renderers` (`dkgl_frame.cpp:352-380, 898, 906, 1157`). All are per-list-index, so 325
  entries is fine memory-wise (vectors/pointers), but double-check nothing indexes them by
  *bucket id*.
- **G5 — shadow3.** Second Shadow2 instance on 573 right after shadow2 572 (jak2's phase-2
  stencil reset pattern). Also note jak3 has NO shadow between pris blocks — only 350 + the
  572/573 pair.
- **G6 — capture misses.** `glClearBufferfv` (GL clear path) is a known miss — expect it in
  the first census log; anything else logged `[dkgl] unhandled` on the first HW run (J6)
  feeds back into this census.
- **G7 — perf.** 587 buckets, 10 live levels, biggest textures of the trilogy; FIX 63
  GPU-OOM history. Watch `s_persist.used`, `*_fail` counters, and start handheld at 432p
  (`JAK3_CARRYOVER_FROM_JAK2.md` §6).
- **G8 — deploy rule.** NRO + all 274 fr3 together (v44), `Jak 3.fNN.bak` backup first
  (`JAK3_CARRYOVER_FROM_JAK2.md` §0.2 / §8).
