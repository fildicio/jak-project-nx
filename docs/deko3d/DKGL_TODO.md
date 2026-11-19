# What is left to convert to deko3d (AI-assisted)

Rule: everything below is done through the **dkgl wrapper** (see `AI_ROLES.md`,
"Wrapper rule"). Status: ⏳ todo · 🟡 code done, hardware test pending · ✅ done.

## Already on deko3d (jak1)

| Part | How |
|---|---|
| tfrag (+ alpha, ice, dirt), tie (+ wind, envmap), shrub, sky, sky-blend | native |
| eyes, sprite, direct (HUD, text, debug) | native |
| texture uploads, level loader | native |
| ocean texture + mid + near + far | dkgl |
| generic (distant mountains, 10 buckets), shadow, depth-cue | dkgl |
| merc (characters, faces, fade) + emerc shine (GL `Merc2`, 8 buckets) | dkgl (#19: eyes still black on HW 2026-10-30, gated probe added, NRO rebuild pending) |
| screen fade to black (pmode_alp) + loader "finish before fade-in" | dkgl + loader (confirmed on hardware 2026-10-27) |

## 1. Open bugs (jak1)

| # | Bug | Status | Next step |
|---|---|---|---|
| 1 | Black line at the sea horizon | ✅ | Fixed by running the far ocean through dkgl (NRO `319f35f7`, confirmed on hardware 2026-10-27) |
| 2 | Jak / objects disappear in busy scenes | ✅ | Merc limit raised 256 → 1024 (GL value), confirmed on hardware 2026-10-27 |
| 3 | Black boxes / things disappearing while a level loads | ✅ | `dk_set_pmode_alp` was a stub (no fade-to-black, no "finish loads before fade-in"). Fixed in NRO `e73e23e2`, confirmed on hardware 2026-10-27 |
| 4 | Dark vertical stripe in the sky (IMG_0108) | ✅ | Same cause as #3 (photo taken during a load). Fixed in NRO `e73e23e2`, confirmed on hardware 2026-10-27 |
| 16 | Jak's shadow looks weird; dark shade on top of Jak and Daxter (IMG_0122) | 🟡 | Cause found: draw order. GL draws the shadow (bucket 47) BEFORE the pris/water merc buckets 49 52 55 58 61 (Jak and Daxter are 55), so the shadow's darkening quad never covers them. dk recorded all 8 merc buckets before 47. Now 49..62 are recorded after the shadow in GL order (`dk_renderer.cpp` record_tfrag_and_shrub, NRO `ce1d93b3`). The native merc path (`gk_dkgl_nativemerc.txt`) still draws all merc first. Needs a hardware look |
| 17 | Geyser Rock fps drop when Jak smashes the wooden training target | 🟡 | Log `20261027-2121`: during the smash the `sprite` pass averages 5.5 ms of GPU (worst frame 31.9 ms) with a normal sprite count (~1.5k, 21 draws). Sprite vertex+fragment shaders, sizes and alpha test match the PC renderer exactly, textures are block-linear, the game does not touch clocks: it is pure overdraw (fill rate) of big smash particles. Fix 1 (NRO `a08dd132`): sprites that do not write depth use `sprite3_ez.frag` (`layout(early_fragment_tests)`), so particle pixels hidden behind the world are rejected before shading/blending; identical picture. Log line `sprite frame ... early_z=N`. Off switch: `gk_dk_spritelatez.txt`. Next option if still slow: colour compression (`DkImageFlags_HwCompression`) on the frame target (needs the 3D splash + frame-grab check) Early-Z (`a08dd132`) did not help (log `20261027-2143`: sprite 4.86 ms avg, worst 38.7 ms). Real fix: colour compression (`HwCompression`) on the frame target, which cuts the read+write bandwidth of every blended pixel (NRO `2f0eef2f`). Opt-out: `gk_dk_nofbcomp.txt` Compression did NOT help (log `20261027-2159`: sprite still 4.6 ms avg), so it is blend overdraw, not bandwidth: the smash stacks screen-sized dust sprites. GL has the same scale limits; desktop GPUs just fill faster. Fix: near-camera fade in `sprite3.vert.glsl` -- 2D world sprites fade as their screen box passes ~1/3 of the screen and are skipped once it covers the whole screen (NRO `560278b0`). Opt-out: `gk_dk_nospritefade.txt`. Check: `sprite` GPU ms during the smash |
| 18 | Language: voices stay English | ✅ (user setting) | Log `20261027-2121`: `pc-settings.gc` still has `game-language 0`, so the option was never confirmed. In Options → Game → Language pick Italiano with left/right and **press A**; X/Y (square/triangle) cancel and restore the old value. Voices follow this option (`VAGWAD.ITA` is present) |
| 19 | Characters: eyes white/green, mouth not drawn/moving | ✅ (HW 2026-10-30, NRO `875ef956`: NPC eyes drawn) | Mouth: fixed by GL `Merc2` through dkgl (NRO `9f1eb189`). Eyes (IMG_0121/0122: white or green blobs): the native eye pass rendered fine (bad=0) but the hand-off to Merc2 was wrong. Now the GL `EyeRenderer` (bucket 54) runs through dkgl too, Merc2 samples its textures, and dkgl puts a fragment barrier after it (NRO `0b04922c`). Opt-out for an A/B: `gk_dkgl_nativeeyes.txt`. Eyes then rendered BLACK on `a08dd132`: the EyeRenderer needs 40 textures + 40 FBOs, and the dkgl object tables held only 64 each, so the later eyes got id 0. Tables raised to 256 (NRO `41bbc824`). Still black on `2f0eef2f`. Host test `ScriptedEyeRendererTwoEyes` shows dkgl emits the eye draws correctly, so the cause is the texture lookup: the Switch replays ALL of a frame's texture uploads before dkgl runs, so the eye bucket saw end-of-frame VRAM (pages 57/60/65 replace the iris/pupil/lid). First fix (NRO `c290f3f7`): a VRAM copy for bucket 54 only. Replaced (NRO `ce1d93b3`) by running every dkgl bucket inside the upload replay (`dkgl::capture_begin/capture_until/capture_end`, called from `dk_upload_bucket_hook`), so eyes, Merc2 and generic look textures up in VRAM exactly as GL does at their bucket. Log: `[dkgl] ... eye draws N | interleaved M` (N > 0 = eyes drew, M = frames captured that way). HW 2026-10-30 (NRO `ce1d93b3` confirmed on the SD, md5 + `interleaved` label): eyes still BLACK; session healthy (13.5 eye draws/frame replayed, 0 skipped, dma mismatch 0, no fatal). Full static audit of the chain (pool seam id → dktexgpu render target, FBO target switch + 128×128 viewport at draw time, scissor reset, post-54 barrier, Merc2 `lookup_eye_texture_hash` binding the same id) found no break; note `glClearBufferfv` is NOT implemented in dkgl, so the GL red debug clear never runs — a never-written eye image is black, not red. Gated probe added (AI-assisted): create `gk_dkgl_eyeprobe.txt` in `/switch/jak1/` on the SD → after bucket 54 the NRO copies eye slot 0 to CPU and logs `[dkgl] eye probe #N: avg rgba r,g,b,a | black n/16384 | r0/r32/r96 …` every ~5 s, 5 rounds. all-black ⇒ the strips never land in the eye image (eye-pass bug: RT bind / vertex stream); content present ⇒ Merc2 sampling is the suspect. NRO `e54a0396` built in docker (`devkitpro/devkita64`, `scripts/ai/check-switch.sh`), deployed to SD 2026-10-30 with the probe gate file `gk_dkgl_eyeprobe.txt` already created (sd-sync keeps it armed) — next SD trip reads the `[dkgl] eye probe #N` lines. **HW verdict 2026-10-30 (log `20261030-1722`)**: slot 0 = Jak HAS content (`avg rgba 162,165,173,233 | black 789/16384` from round 3 on; rounds 1-2 all-black = face off screen) ⇒ the eye pass, the 128×128 RT and the id→image seam all work. User: only Jak + Daxter (early slots) have eyes, all NPCs black. `gk_stdout.txt` has ZERO `lookup eye failed` lines ⇒ Merc2's `lookup_eye_texture_hash` finds and binds a texture for every character — NPC slots match a hash but are (presumably) pristine black because `run_gpu` skips quads when `lookup_gpu_texture` misses the iris/pupil/lid source (Jak's sources load, NPC ones don't). Probe v2 (AI-assisted): copies ALL 40 slots in one frame and logs a per-slot map `[dkgl] eye probe #N: ##.… | # n + n . n x n | slot0 avg …` (`#` content, `+` partial, `.` black, `x` no image), and `EyeRenderer::run_gpu` logs `[dkgl] eye src slot N: iris MISS (tbp N) …` for missing source textures during the probe window. NPC slots `.` + `eye src … MISS` ⇒ texture-pool miss for those tbps (VRAM page replay); NPC slots `#` ⇒ Merc2 binding seam. **Root cause found (AI-assisted, no HW trip needed)**: jak1 `eye.gc` puts each level's NPC eyes in that level's pris-tex bucket (48 / 51, right after the page with their iris/pupil/lid); only Jak/Daxter use bucket 54. GL's `TextureUploadHandler` runs `EyeRenderer::handle_eye_dma2` when it meets that DMA (qwc 8); the Switch replay (`SpriteDk walk_tex_bucket`) only did the uploads, so NPC eye slots were never drawn (black). Note: jak1 Merc2 picks eyes by slot (`lookup_eye_texture(eye_id)`), not by hash. Fix: dkgl lists 48 / 51 run `PrisTexEyes` (TextureUploadHandler minus the uploads) inside the replay, recorded with 54 before every merc, same barrier. NRO `875ef956` (check-switch PASS). Check: NPC eyes (e.g. Samos, the farmer) drawn; `eye draws` in the `[dkgl]` line goes up near NPCs |
| 20 | Intro "Sony presents" screen: the dark fade covers only the 4:3 centre, the sides stay bright (IMG_0115) | ⏳ | The game itself is 16:9 (`aspect-ratio-auto? #t`, the window decides). Checked and ruled out: GOAL `blackout`/`letterbox` (full 512x224 sprites), the dkgl pmode fade (full-screen quad, full viewport) and the native direct renderer scissor (same maths as GL). Remaining suspect: the 2D/HUD sprite pass, which the PC port keeps at 4:3 on purpose (may be normal PC behaviour). A/B: boot once with `gk_nosprite.txt` (the `gk_opengl.txt` GL opt-out was removed 2026-10-30). 2026-10-30 user report: intro looks like "a 4:3 box with bands around a widescreen image" (old bug). **User confirmed: pillarbox, left+right bands only.** Saved `pc-settings.gc` has `(aspect-state aspect4x3 4 3 #t)` + `(letterbox #t)`: GOAL keeps the view scissor 4:3, so the world draws in the 4:3 centre and the dk present-clear shows at the sides. The dk path reports a 16:9 window (`set_native_window` 1280x720/1920x1080), so the fix is the setting itself, no code: **Options → Aspect Ratio 16:9 + Letterbox OFF (press A to confirm)** — equivalent to editing pc-settings.gc to `(aspect-state aspect16x9 16 9 #f)` / `(letterbox #f)`; I can patch the file on the SD at the next sync if the menu route doesn't stick. If 16:9 still pillarboxes after that, then `aspect-ratio-auto?` handling misparses and the fallback is a GOAL-side force-16:9 branch in `update-from-os` (needs a goalc rebuild + data redeploy) |

## 2. Missing features (jak1, via dkgl)

| # | Feature | What the GL side uses | Status |
|---|---|---|---|
| 5 | Shine on orbs / metal (merc envmap, "emerc") | GL `Merc2` runs through dkgl on the 8 merc buckets; only the `emerc` program is in the dkgl table, so normal merc stays native (MercDk) and only the envmap draws reach the GPU, replayed right after native merc. Adds `glBindBufferRange(GL_UNIFORM_BUFFER)` (bones, binding 1), Loader buffer ids via `set_external_buffer_lookup`, and a null guard for the missing EyeRenderer in `Merc2.cpp`. Opt-out: `gk_dkgl_noemerc.txt`. Seen working on hardware 2026-10-27. Since #19 the normal merc draws (`merc2` program) also run through dkgl | ✅ |
| 6 | Screen fade to black (cutscenes, loads) | GL `FullScreenDraw` + `solid_color` through dkgl (`record_blackout`, after the HUD) | ✅ |
| 7 | Brightness / contrast (PCRTC) | ✅ done for all games | jak1 GOAL never calls `pc-set-brightness-contrast` (no-op there). jak3 does (`blit-displays.gc:656`), and the dk path has had it since commit `b9e479fc8`: `pc_set_brightness_contrast` (kmachine) → `g_global_settings` → `dkdev::set_pcrtc` (`dk_renderer.cpp`) → `record_pcrtc()` inside `present_frame` (`dk_device.cpp`, `rgb = image.rgb * alpha + add` over the whole frame). This row was stale — nothing left to build |
| 8 | Dock / undock resize | works like GL: the swapchain stays at the boot size (GL FIX 73c) and the Switch scales the image when docked. A native-resolution resize is deferred (risky, costs GPU time) | ✅ same as GL |

## 3. Close the hardware checks

| # | Item | Status |
|---|---|---|
| 9 | T102 / T110: ocean hardware test (Sentinel Beach, 20 min, heap flat, fps unchanged) | ⏳ |
| 10 | T112 / T113: depth-cue + shadow hardware test | ✅ seen working on HW 2026-10-27 |

## 4. Release (jak1)

| # | Item | Status |
|---|---|---|
| 11 | Commit the pending work (fog fix, T112/T113, merc limit, far ocean via dkgl) | ✅ `815d00d6e`, PR #4 merged |
| 12 | deko3d on by default: jak1 boots on deko3d with no flag files; no OpenGL opt-out or boot fallback since 2026-10-30 (replaces `gk_dk3d`, `gk_dkload`, `gk_dktfrag`, `gk_dkshrub`, `gk_dktie`) | 🟡 needs a boot test with no flag files |
| 13 | Long soak test (1 h, several levels) + GL fallback still works | ⏳ |

## 5. jak2 and jak3 (all via dkgl)

| # | Item | Status |
|---|---|---|
| 14 | jak2: reuse the native deko3d renderers where they already work, run the remaining GL bucket renderers through dkgl, add the missing dkgl programs (`shadow2`, `tex_anim`, ...) and TextureAnimator support — full list in §7. **tex_anim + TextureAnimator landed 2026-11-06 (T125, AI-assisted): the UNCHANGED GL animator runs through dkgl** (details in §7 item 3). **Glow landed 2026-11-06 (T126, AI-assisted):** the native T124 glow port (wrong strip order, no occlusion = flares through walls) was removed; the UNCHANGED GL `GlowRenderer` (new_mode: depth copy, probe grid, downsample, additive draw) runs through dkgl from the SpriteDk jak2 glow section into a `kGlowList` side list, called right after sprite bucket 313. dkgl gained depth textures (alias of the frame depth image, no copy), Z24S8 renderbuffers, depth attachments, vertex-stage sampling; side lists no longer replay twice from the world loop. A/B: `gk_dkgl_noglow.txt` (glow ✅ HW: skull gems + dark eco fixed). **Distort T127** (scatter gun) through dkgl, y-scale fix HW pending. What is left: §7.0 | 🟡 |
| 15 | jak3: same as jak2, plus PCRTC brightness | 🟡 | PCRTC brightness was already done (see row 7 — the row was stale). What was actually missing for jak3 was the BlitDisplays end-of-frame effects (0x11 copy-back / 0x13 zoom blur / 0x14 color filter / 0x15 slow-time) — **FIX 138** wired them through dkgl behind `gk_dkgl_copyback.txt` (details: `JAK3_BRINGUP_TODO.md` J15). Remaining: the same HW checklist as jak2 (distort haze, tex_anim on-screen, shadow2, tie proto-vis) + the J7 perf bring-up |

**jak3 slow motion below 60 fps — ✅ DONE, HW verified 2026-11-04 (GAME.CGO `efb5481f`, stable 30 fps).** Original note: `pc-maybe-vsync` in
`goal_src/jak3/engine/draw/drawable.gc` (~line 2248) still returns a fixed `1.0`, so every frame
advances game time by 1/60 s and any drop below 60 fps plays in slow motion. Port the jak2 fix
(commits `c07efb959` + `5149655da`): measure the frame with `__read-ee-timer`, divide by
300 MHz / target-fps, add it to a carried time debt, step 1.0 while the debt is < 1.15 and
`fmin 4.0 debt` otherwise, then clamp the leftover debt to [-0.5, 1.0]. Rebuild with
`goalc --game jak3 --instruction-set arm64 --cmd '(make-group "iso")'` and copy
`out/jak3/iso/GAME.CGO` to the SD. jak1 has the same carry in `display-frame-start`.

## 6. When is the migration finished?

The deko3d migration is done when all of these are true:

1. jak1: rows 3–13 above are ✅ (no open bugs, all features, deko3d on by default, soak test passed).
2. jak2 and jak3: rows 14–15 are ✅ and each game boots and plays one full level on hardware with deko3d.
3. Performance is the same as or better than GL (fps and frame ms in the `[dk]` log), measured per game.
4. jak1 has no OpenGL fallback any more (`gk_opengl.txt` removed 2026-10-30); jak2/jak3 still fall back to GL until they render right.

## 7. Jak 2 bring-up — missing list (2026-10-31, AI-assisted)

Rule (user, 2026-11-07 — replaces the 2026-10-31 "reuse native if quicker" relaxation):
**never write new native deko3d code.** Every gap runs the UNCHANGED GL renderer through dkgl.
The native renderers that already work stay as they are; only fix their bugs. Sources: `OpenGLRenderer::init_bucket_renderers_jak2()`
(the full jak2 bucket list), the dkgl program table (`dkgl_uniform_table.inc`, 14 programs),
`dk_renderer.cpp` (jak1 bucket wiring) and the native renderer sources.

### 7.0 What is left for jak2 (summary, 2026-11-07, AI-assisted)

All via dkgl (GL renderer unchanged; dkgl gets the missing program / GL hook / state).

| # | Item | How (dkgl) | Status |
|---|---|---|---|
| A | Hardware checks pending: distort haze (T127), tex_anim T125 (Haven ads/gems), shadow2, tie/shrub proto-vis (perf step 1). (jak2 slow-mo GAME.CGO ✅ HW 2026-11-07, user) | one SD trip | 🟡 |
| B | Warp bucket 317 — scattergun red cone `skel-gun-red-cone` (2026-11-07 HW: `distort 0` while shooting, no cone) | T128 done: GL `Warp` registered in `init_jak2_foreground`; dkgl seam fix: pool-held dk ids rebind to their dkgl texture (`bind_texture`), so the frame copy survives the copier's resize and gets its T106 barrier | ✅ HW 2026-11-07 (user) |
| C | Title-screen "yellow": sky-tng blit-displays capture (tbp #x4c0/#x3300) has no framebuffer copy, only the sepia tint shows | GL `BlitDisplays` registered at jak2 bucket 3 in `init_jak2_foreground`; its 0x10 copy reads LAST frame's swapchain image (`dkdev::blit_prev_frame_to`), like GL's persistent render_fb. Copy-back 0x11 / zoom blur / slow-time (`do_copy_back`) not wired yet | ✅ HW 2026-11-13 (user: title yellow shows, distort T127 + perf I also confirmed same session). **jak3 extension 2026-11-18 (AI-assisted, J11):** same pattern, `blit_prev` was never enabled for jak3 — `dkgl_frame.cpp` `blit_frame_to_tex` only matched jak2 BUCKET_3, so the jak3 `BlitDisplays` (BLIT_START) replayed as `blit_frame_to` (not-yet-drawn frame) → menu backdrop sepia-only ("yellow menu background", HW NRO 9e7a86a2; log `blit bad` 1/frame). Fix: condition now also matches `s_jak3_ids` + `jak3::BLIT_START` → `blit_prev_frame_to`. Deployed NRO `30ec435f`; HW ⏳ |
| D | tfrag WATER buckets (`tfrag-w-l*-water`) not drawn | GL `TFragment` for those buckets through dkgl (`tfrag3` program in the dkgl table); native tfrag keeps NORMAL/TRANS. **Landed 2026-11-08 (AI-assisted):** one GL `TFragment` per WATER lod (`WATER` tree kind, `s_jak2_anim_slots`) registered in `init_jak2_foreground`; dkgl gained `tfrag3` (vert+frag, jak2 vert via `_JAK2_SHADERS` HEIGHT_SCALE 0.5), `glTexSubImage2D` (tod palette top-left sub-rects: lazy `upload_rgba8` on first palette, then `update_rgba8_deferred`), an `UNSIGNED_SHORT` branch in `attrib_map` (tfrag attrib 2 is 1 x u16 `IPointer`), and `no_multidraw` on the dkgl SharedRenderState (no EXT multidraw on Switch → per-draw path). Build+tests green 2026-11-08: jak2 NRO `8dd7b3c2`, jak1 NRO `47d28f5a`, goalc-test `*Dkgl*:*dkgl*` 39/39 (tfrag3 added to `test_dkgl_shader_gen` programs; the two `test_dkgl_state` probes that used "tfrag3" as the inert program now use an unmapped name). HW check pending | 🟡 code done 2026-11-08, HW check pending |
| E | tie TRANS / TRANS_ENVMAP / WATER / WATER_ENVMAP categories not drawn (`TieDk` does NORMAL + envmap only) | GL `Tie3` for those categories through dkgl (`tfrag3`/`etie` programs) **Landed 2026-11-13 (AI-assisted):** GL `Tie3AnotherCategory` on the 24 category buckets (`tie-t/etie-t-l{n}-alpha`, `tie-w/etie-w-l{n}-water`), driven by a per-level GL `Tie3` master on `TIE_L{n}_TFRAG` that runs a new `Tie3::render_setup_only` (setup, no draw — the native `TieDk` still owns NORMAL + wind + NORMAL_ENVMAP on that bucket). dkgl gained the `etie` + `etie_base` programs (vert jak2 variants HEIGHT_SCALE 0.5 via the existing `_JAK2_SHADERS` rule; TRANS/WATER base draws reuse `tfrag3`, same shared draw path as the §7.0 D tfrag water). The 30 buckets joined the sorted jak2 table (stride arithmetic = GL `GET_BUCKET_ID_FOR_LIST`), so capture walks + replay interleave them in GL order with the native splits. A/B flag: `gk_dkgl_notie.txt`; boot log line `[dkgl] jak2 tie categories: 6 masters + 24 category buckets`. NOTE the one-shot `[dkgl] program 'X' not in table` line is now `tfrag3_no_tex`-or-later in ctor order — benign (see H) | 🟡 code done 2026-11-13, HW check pending (windows/glass in Haven, Dead Town water patches). **Deployed 2026-11-13: NRO `98878af1` on the SD (`jak2.nro`); rollback = the HW-verified bug-M build `Jak 2.330b04ce.bak`. `gk_dkgl_noglow.txt` removed (bug-M A/B leftover). Expect boot log `[dkgl] jak2 tie categories: 6 masters + 24 category buckets`; look for see-through windows/glass railings in Haven city and water patches; if anything regresses, `gk_dkgl_notie.txt` turns just this off**. **HW run FAILED 2026-11-13 (AI-assisted triage): black view / gray flashes+boxes / fps decaying 60→32. Log verdict: registration PERFECT (`6 masters + 24 category buckets` at t=1.5s; buckets=172 — the 142-buckets line in the same log file is the appended older pre-E session), no fatal, no census hits, no skipped draws, native tie UNAFFECTED (`tie draws=0` matches the good 330b build; tie0/tie2 GPU cost unchanged). The single anomaly = `b266` (`ETIE_W_L1_WATER`, envmap water) burning **15.63 ms/frame GPU** (avg over 319 frames, `gpu world 13.4→29.2 ms`, `gpufence 20.9`) while its plain-water twin `b265` (`TIE_W_L1_WATER`, same geometry + visibility, `tfrag3` single pass) stayed **≤0.14 ms** — ~100× pathological, i.e. a real bug in the etie draw path, NOT workload. Static audit after (all verified CLEAN): sorted-table ids + stride math (256+9·l water / 129+10·l alpha / 9+11·l masters), jak2 vert swaps (`etie`/`etie_base` → `_jak2` HEIGHT_SCALE 0.5, gen-dksh regexes match), uniform tables `kSlots_etie{,_base}_vert` byte-offsets == the GLSL std140 blocks, attrib_map covers all 5 tie attribs (incl. `10_10_10_2`/Snorm + `2x16`/Uint + `4x8`/Unorm), `run_bucket` resets segments every run (no accumulation), frag/vert faithful ports of GL. Coverage gap: no host test runs the GL `Tie3AnotherCategory` through dkgl with real geometry. **Next: instrumented NRO (tie census + `gk_dkgl_noenvmap2.txt` A/B) — census prints per-level `m/t/et/w/ew` draws (d), indices (i), largest single draw (x) every ~600 frames; `ew` ≫ `w` on d ⇒ category-range bug; `ew` huge on i/x ⇒ vertex/index degeneracy. `gk_dkgl_noenvmap2.txt` drops only the additive etie second pass**. **HW census 2026-11-15 (NRO `c444bd01`, AI-assisted): user walked Haven waterfront + dark jak transform — 60 fps locked end-to-end (only boot/load hitches), `b266` = 0.22–2.19 ms worst window, NO 15.63 ms repro. Census verdict: `ew257/ew266` = 2400 draws/600 f (4/frame, constant), `i` 17.7–20.5 M/600 f, `x` 14.7–24.3 k — NO degeneracy; `w`/`t`/`et` = 0 draws in the city (all harbor water is envmap water, so `ew ≫ w` is expected there, not a category-range bug); cost varies with view while counts stay ~constant ⇒ pixel-bound additive 2nd pass, as designed. The 15.63 ms event lives only in the appended 98878af1 session (~10 s at 151–161 s: 31.9 fps, gpu world 29.2). LOG HYGIENE: `gk_run_log.txt` APPENDS across boots — split sessions on the `frame target compression` line before correlating anything (cross-session reads of this file produced a wrong "census=0 during burn" theory on 2026-11-15). Cannot fully close: exact bad spot unknown + the tree is uncommitted (no diff 98878af1→c444bd01 exists). Plan: keep the census in; if the burn recurs, the census + `gpu top` now discriminate on the spot (`ew` counts ~2400 while b266 explodes ⇒ pixel path → `gk_dkgl_noenvmap2.txt` confirms; `ew` i/x explode ⇒ geometry). Side-data for G from the same run: texanim dispatches live (49 k+/session), ZERO skipped draws, zero cmd overflow, zero `[dkgl]` warnings** **HW 2026-11-15 Dead Town screenshot (AI-assisted): brick wall streaked horizontally (texture clamped where it should repeat). Root cause: the census stubs make `glGenSamplers`/`glBindSampler` non-null, so `background_common.cpp` `samplers_supported()` = true and every GL background draw through dkgl (this E path + §D tfrag water) put wrap/filter into sampler objects dkgl ignored → pool default CLAMP + no mips. Fix: dkgl sampler objects (`gen/delete/bind_sampler`, `sampler_parameteri`, a bound sampler overrides the texture params like GL) + separate wrap S/T bits (dkgl sampler combos 16→32; before, `wrap_t` was ignored). Test `DkglState.SamplerObjectOverridesTextureParams`; boot log now `... samplers (4) = 82`. jak2 NRO `1ef2cd0c`, ✅ HW 2026-11-15 (user): Dead Town wall fixed, boot log `samplers (4) = 82`** |
| F | tfrag/tie anim slots (animated background textures) — was mislabelled "shrub": GL Shrub has no anim slots; GL TFragment/Tie3 bind `anim_slot_array[-(tree_tex_id+1)]` | bug fix: `TFragmentDk`/`TieDk` negative `tree_tex_id` → `dkgl::anim_slot_dk_tex()` (read-only, 1-frame lag: texanim records after tfrag/tie) | ✅ HW 2026-11-15 (user, NRO `1f1f7bc2`) |
| G | dkgl stubs: `glGenerateMipmap` (tex_anim mips empty), `glClearBufferfv`. HW 2026-11-08: skull gems are flat orange discs (no faceted skull look) -- likely these empty tex_anim mips. **`glGenerateMipmap` landed 2026-11-08 (T129a, AI-assisted):** `dkgl_glGenerateMipmap` -> `generate_mips_flush` (bound texture, mipped colour targets only -- pool ids and `tex_sub` single-level images no-op) queues a `blit_mips` blit Segment; `record_bucket` replays it as linear-filtered level L-1 -> L `dkCmdBufBlitImage`s inside the same DkImage (Full barrier before + image-invalidate after each level). `glClearBufferfv` still a stub (eye probe pristine images) | `glClearBufferfv`: implement in dkgl  **HW 2026-11-09:** gems show the orb-like base texture -- root cause: the TextureAnimator NEVER ran (`texanim 0` in every log; SpriteDk matched imm-12 only with size_bytes 16, but start-anim-array is a qwc-0 cnt) and Merc2's `s_jak2_anim_slots` never received animator outputs. Fixed 2026-11-09: match on the vifcode only + `merge_tex_anim_slots()` after each dispatch. A/B: `gk_dkgl_notexanim.txt`. | 🟡 code done 2026-11-09, HW check pending (log `texanim` > 0) **HW 2026-11-09 (NRO `4fbe9aaa`): Jak's head fixed by the tex_image_2d pixel upload (`a72a7bf42`); gems still flat/black. stdout: `draw skipped: attrib 0 size 1 type 0x1404` -- dkgl attrib_map had no GL_INT, so EVERY TextureAnimator composite draw (vertex_index IPointer) was skipped. Fixed: GL_INT -> Sint/Snorm/Sscaled (NRO `7eb4a7fc`), HW confirm ⏳.** Open: sampler stubs (glGenSamplers/glBindSampler/glSamplerParameteri implemented 2026-11-15, see E; glSamplerParameterf = anisotropy, still a no-op), glClearBufferfv (aniso/sampler only, harmless). **2026-11-13 symptom link (AI-assisted): "dark jak colors missing" == this stub, NOT §7.0 E/b266:** dark jak's skin morph is `set-darkjak-texture-morph!` → GOAL `pc-clut-blender` DMA packet (id `darkjak` 22, plus prison-jak/oracle-jak/nest-jak variants) → C++ `run_clut_blender_group`, which still returns early for ALL jak2 Switch clut groups (commit `429e36f7a` "keeps static textures") → Jak keeps his normal palette. Present on every Switch build incl. the HW-verified `330b04ce`. Re-enabling needs `ClutBlender::run` (GL composite → GLuint) + `give_texture_and_load_to_vram` working through dkgl/dktexgpu  ✅ HW 2026-11-15 (user): gems fixed |
| H | Census sweep: close every `dk_gl_census` unimplemented-call hit in a long jak2 run | dkgl. **2026-11-13 (AI-assisted):** the fresh bug-M session log (NRO `330b04ce`, turret/cannon/gunshot run) has ZERO `dk_gl_census` hits, zero skipped draws, zero `[dkgl]` warnings except one `program 'direct_basic_textured_multi_unit' not in table (inert)` — which is BENIGN: `ShaderLibrary`'s ctor eagerly links all 46 programs and dkgl logs the FIRST not-in-table name once (`g_logged_inert`); nothing in the codebase ever draws with `DIRECT_BASIC_TEXTURED_MULTI_UNIT` (no C++ references outside Shader.h/.cpp). Remaining census exposure: the new §7.0 E tie categories (etie path) — re-check the log after that HW run | 🟡 effectively clean 2026-11-13; re-run after E lands |
| I | Perf to locked 60 at 720p (tie 3-5 ms, shrub 2-4 ms) | measure with `gk_gpuprof.txt` after A. **2026-11-10 (AI-assisted, user HW, handheld Atoll):** (1) jak2 BCn levels now go to the GPU compressed WITH their mip chain (`dktexgpu::upload_bcn_mipped`; FIX 91 RGBA decode skipped on jak2 + dk) -- the decode filled the 96 MB image pool ("tex pool full", fail=133) so tfrag/shrub textures were refused and drawn untextured (white/flat sand and grass); (2) jak2 occlusion strings (bucket 2 vis packets, 2048 B/level) are read in `capture_fog_jak2` and passed to tfrag/tie `cull_check_all_slow` (`dktf::occlusion_string`, A/B `sdmc:/gk_noocc.txt`): tie at the Atoll pumps 560k -> 90-170k tris, tie2 5-7 -> 2.3-2.9 ms; (3) shrub spatial chunk culling (`build_shrub_chunks`, A/B `sdmc:/gk_noshrubcull.txt`, jak2 only): shrub 3.5-4.5 -> 1.9-2.8 ms; (4) tod-palette copies are staged before the capture and recorded in the world recorder (`begin_deferred_frame` moved to capture start + `record_deferred_updates` at the top of `record_jak2_dkgl`): -3..4.5 ms CPU/frame. Result 55-60 fps there. Still open: sky bucket ~3 ms, shrub up to 3.7 ms in other views, mountains looked half-textured: fixed by `b8f2f676c` (32 px tall BCn textures decoded to RGBA8, Atoll checkerboard), not E. Host replay tests were NOT run for these changes | 🟡 **2026-11-15 capture timers (AI-assisted, NRO `e2eef206`, HW CHECK PENDING):** log `hw-logs/f39` showed slow frames are CPU (152/166 slow = cpu) and `capture` = 7–8.5 ms/frame, with no breakdown. New log line every 10 s: `[dk] capture ms | drain world upload dkgl texanim glow distort walk end | dkgl top bNNN ms…` (`walk` = SpriteDk tex-bucket walk minus the nested parts; `dkgl top` = CPU ms per dkgl bucket GL renderer). TODO: run Haven/Dead Town ~2 min, read that line, attack the biggest part. Ideas parked: sky half-res (~2 ms GPU, ~100–150 lines), CPU/GPU overlap (gpufence 3–4 ms). **Timer result 2026-11-15 (HW, log `hw-logs/jak2-captimers-1115.txt`):** capture 4.3–8.5 ms, of which dkgl 3.5–7.3; top CPU buckets: b9 (Tie3 setup-only master L0) 1.0–2.3, b6 ocean-mid-far 1.0–1.2, b310 ocean-near 0.7, b221 merc-lcom-pris 0.55; upload 0.4, texanim 0.16, walk ≤0.7. Busy spots also GPU-bound (gpu world 16–17.7, tie0 2–4, sh0 3, sky 3). **Fix 1 (NRO `916cb88a`, HW pending):** the setup-only `Tie3` master now skips trees with no TRANS/WATER draws and leaves NORMAL* draws out of its index list (`draw_enabled` arg of `make_index_list_*`; native TieDk owns those). ✅ HW 2026-11-15 (log `hw-logs/jak2-tie3skip-1115.txt`): b9 gone from the dkgl CPU top 10 (was 1.0–2.3 ms); fps avg 59.4→60.0, worst 10 s window 55.4→59.7; capture 6.3→5.7 ms, dkgl 5.4→4.5 ms (route not identical). Next CPU targets: b205 merc-l2-pris (0.5–4.1 ms, characters), ocean b6/b310; GPU: sky b5 2.9, particles b313 1.5–2.2. | 🟡 → locked 60 confirmed by user 2026-11-13; sky/shrubs micro-opts optional **2026-11-16 (AI-assisted, HW log hw-logs/jak2-1116-deadtown.txt):** fps windows mostly 60, dips 56.7-58.5. Slow frames are CPU-bound (`slow N: cpu N`): capture 6-9.7 ms, dkgl 4.8-7.7 ms. Top CPU buckets: b201 MERC_L1_PRIS ~5 ms, b205 MERC_L2_PRIS 1.9-5 ms, b229 0.7, b221 0.4, ocean b6/b310 0.5-1.1. gpufence wait 2-5.4 ms. GPU: sky b5 ~3.1 ms, shrub up to 2.85, tie up to 2.9, particles b313 ~1.8. NEXT (GLM): profile Merc2 through dkgl per draw (draw count, glUniform calls, bone UBO upload size, `set_int_attrib` skips: `int` counter is ~44k per 10 s) and cut the per-draw CPU cost in b201/b205 first; it is the biggest item. **2026-11-16 fix (AI-assisted):** the per-draw dkgl cost was self-inflicted: (1) `write_uniform` strcmp-scanned BOTH stage tables on every `glUniform*` and ALWAYS set `uniform_dirty`; (2) T106 step 10 then snapshotted a FRESH UBO slice (memcpy of the whole shadow + 2 `bind_ubo`) on EVERY draw. Now the name->offset mapping resolves once per (program, location), a write that changes no bytes is a memcmp no-op that does not dirty the program, and a clean program re-BINDS the slice it last uploaded in the CURRENT frame slot (safe: `ring_take` never wraps within a frame; a dirty shadow or slot change forces a fresh snapshot). New `[dkgl] perf` line every 10 s (deltas since the previous line): `glUniform +N (changed M) | bone UBO +KB | int skip +D | merc draws b201 X b205 Y` — the per-draw profile the NEXT above asked for. Built jak2 `478eec93` / jak1 `a301000d` (check-switch + check-dk PASS, warnings 3->2), deployed (`Jak 2.f49.bak`, `jak1.d5cdde24.bak`). 🟡 HW: run Dead Town ~2 min, read `[dkgl] perf` + `dkgl top` in `gk_run_log.txt`, confirm b201/b205 CPU ms drop and no merc colour regression (would look like row Q). **2026-11-16 pass 2 (AI-assisted):** the pass-1 HW log (Dead Town, 60 fps target) shows the remaining slow frames are still CPU (capture 5.6–7.4 ms: b205 1.3–3.1 ms merc, ocean b6/b310 ~2 ms, walk ≤1.3) with gpufence 5.6–6.9; the 30 fps windows in the same log were the user's own target setting (locked 30.0, CPU/GPU idle — not a regression). flush_pending now skips the DERIVATION too, not just the emit: step 7 (DkVtxAttribState/VtxBufferState rebuild per draw) is gated on the bound VAO's new `serial` (bumped on every attrib/element-buffer mutation) + a global `buffer_epoch` (bumped on every BufferObj re-spec; u64 — it moves several times a frame), and step 9's `bind_tex` (2 `dkCmdBufBindTexture` per draw per unit) is suppressed per unit when gl id + resolved dk id + the new `texture_param_epoch` (bumped on `glTexParameter*`/`glSamplerParameter*`/`glBindSampler`) are unchanged; the RT sample-barrier check stays unconditional. `[dkgl] perf` gained `| vao fast +N | tex keep +N` (10 s deltas). Test `DrawFlushScriptedOceanOrder` updated to the diffed expectations (4 UBO binds not 6, 1 tex bind not 2 — the state-identical draw reuses everything). check-dk PASS, host DkglState+DkglShaderGen 41/41 (full goalc-test stops at Jak1KernelTest boot in a no-TTY shell — pre-existing, kernel tests not GL), jak2 `2c251cf7` / jak1 `e261551f` check-switch PASS, deployed (`Jak 2.f50.bak` = pass-1 jak2, `jak1.a301000d.bak`). 🟡 HW: rerun Dead Town with the 60 fps setting, read `vao fast`/`tex keep` (expect most of the ~900 merc draws/frame to hit) + `dkgl top` b205/b201 ms; next levers if still short of locked 60: ocean b6/b310 CPU (wave sim + ~190 KB UBO/frame), sky b5 half-res (~2 ms GPU, parked), CPU/GPU overlap (gpufence 3–4 ms, parked). **2026-11-17 (AI-assisted):** glUniform dedupe already in dc9e777e8 (~4.4k calls/frame, ~7% change, memcmp only). Merc2 CPU b201/b205 ≤0.8 ms, capture 2-3 ms, vsync wait 6-8 ms → CPU has slack; Merc2 CPU work deferred, remaining wins are GPU (gpufence, sky b5). |
| J | Make deko3d the jak2 default (`gfx.cpp` still needs `gk_dk3d.txt`) | **Landed 2026-11-13 (AI-assisted):** `dk_pass_on` treats jak2 like jak1 — every dk pass file counts as present, so no `gk_dk3d.txt`/`gk_dkload.txt` opt-in anymore (`kill_switch_present` + `gfx.cpp` boot log line updated). GL stays as the init-failure fallback for jak2 in the same boot. HW check pending: delete the flag files from the SD and boot jak2 — expect `[gfx] deko3d selected (jak1/jak2 default)` | 🟡 code done 2026-11-13, HW check pending |
| K | jak2 ocean NEAR (dkgl, bucket 310): close to the camera the sea is a purple/white band (HW 2026-11-08, Haven port + slums canal screenshots); far/mid ocean OK. Not from D (ocean uses no u16 attribs / glTexSubImage2D) | check the OceanTexture wave texture + envmap the near pass samples (`notex 0`, `inert 5` in the log). **HW 2026-11-11 (user): no purple band any more** (probably the PR #9 BCn/texture fixes) | ✅ HW 2026-11-11 (user) |
| L | jak2 dark eco clusters are plain magenta dots (no bright core / glow), HW 2026-11-08 Dead Town screenshots | check the GlowRenderer (T126) + sprite3 path for those sprites | ✅ HW 2026-11-15 (user) |
| N | jak2 Dark Jak is not displayed (HW 2026-11-15, user; no screenshot yet). Dark Jak morphs Jak's mesh with blerc: GL `Merc2::model_mod_blerc_draws` / `model_mod_draws` (Merc2.cpp ~173–395) upload the morphed vertices with `glBufferData` into per-effect mod buffers from `alloc_mod_vtx_buffer` (~1043: `glGenBuffers` + `glGenVertexArrays` + `setup_merc_vao`, the pool grows and is never freed), and `do_draws` (~1229) binds `draw.mod_vtx_buffer.vao` for `MOD_VTX` draws. Already checked (Copilot, code reading, AI-assisted): dkgl `buffer_data` renames every glBufferData to a fresh ring slice and the draw reads attribs from the VAO captured at `glVertexAttribPointer` time, so the basic path looks right. Suspects to check next: (1) `kMaxVaos = 128` / `kMaxBuffers = 512` (dkgl_state.h:23-24) exhausted by the growing mod-buffer pool → `gen_vertex_arrays` returns 0 → the draw uses the default VAO; (2) `MAX_MOD_VTX` ring size per frame; (3) Dark Jak extras: lightning (Generic2 `Mode::LIGHTNING`, `effects` bucket), dark textures/tex_anim; (4) the eyes path. First step: play into Dark Jak with a log build and grep `gk_run_log.txt` for `[dkgl]`, `skipped`, `bad_attrib`, `missing_element`, VAO/buffer-full lines. **2026-11-15 diagnosis + logging pass (AI-assisted):** eliminated by code reading — (a) clut blender: `run_clut_blender_group` early-returns for ALL jak2 on Switch (TextureAnimator.cpp, "keeps static textures"), so it leaves Jak's normal palette and cannot hide the mesh; (b) GL_INT allow-list skip does not apply to merc (attrib 5 = `glVertexAttribIPointer(5,4,GL_UNSIGNED_BYTE)`); (c) MercVertex formats (float/ubyte) all supported by `attrib_map`; (d) dkgl `buffer_data` renames in place under the same buffer name and `capture_attrib` records `g_bound_array_buffer` at call time — the per-frame mod upload → VAO → draw path is correct; (e) ring capacity: data ring is 8192 KB, newest hw log (`hw-logs/jak2-tie3skip-1115.txt`) peaks at 139 KB with `fails data 0`, `skipped 0` and ZERO `[dkgl]` warnings → Dark Jak's draws are most likely never issued (or degenerate), not skipped. blerc runs through sse2neon on aarch64 (`simd_util.h`) — basic mul/add/store, low risk but unverified. **Logging build (one-shot, greppable in `gk_run_log.txt`):** Merc2.cpp (Switch-only): `[merc2] mod-blerc first upload: model %s effect %d: N verts, bbox … nan N, w0-3 …` (fires the first time the blerc morph upload runs + NaN/bbox sanity of the morphed verts), `[merc2] mod-draws (DMA unpack) first seen: model %s …`, `[merc2] mod vtx pool grew to N`, `[merc2] first MOD_VTX draw issued: vao N vtx N` (0 = gen failed → table full); TextureAnimator.cpp: `[texanim] jak2 clut-blender group N packet seen` (proves the GOAL morph packets arrive); dkgl: one-shot reasons for every silent draw skip (`[dkgl] draw skipped: attrib N buffer N dead/empty` / `element buffer N dead/empty` / `ubo N dead/unaligned` / `no enabled vertex attribs` / `GL_INT attrib N outside allow-list`) + the 10 s ocean line gains `| skip attrib N idx N ubo N int N |`; `object table full` now prints live vao/buffer counts. **HW test:** trigger Dark Jak, grep the log for `[merc2]`, `[texanim] jak2 clut-blender`, `skip attrib`, `object table full`, plus the ocean 10 s line | dkgl | ✅ → **HW 2026-11-13 (user): mesh morph WORKS** ("jak transforms but stays with jak colors") — so MOD_VTX/blerc are fine; only the palette swap is missing = the jak2 clut-blender stub (row G path). **Fix landed same day (AI-assisted): `run_clut_blender_group` re-enabled for jak2/Switch** — ClutBlender::run is pure CPU + one `opengl_upload_texture` (dkgl deferred staging, in place), darkjak group publishes via `m_private_output_slots` only (`move_to_pool=false`), the same anim-slot bind merc already uses (skull gem path); the old ban was for the fixed-anim GL draws. Log line: `[texanim] jak2 clut-blender group N packet seen; running it (Switch)`. HW re-test pending (also brings prison/oracle/nest Jak + kor groups back). **HW 2026-11-13 (user): palette confirmed — Dark Jak turns dark.** The merc2 + texanim one-shot investigation logs were trimmed after the pass; the generic dkgl skip-reason one-shots/counters and the `skip attrib/idx/ubo/int` stats fields stay permanently (those skips were invisible before) |
| O | jak2 crash at Kor (end of the first mission, level `ctykora` just loaded; HW 2026-11-15, user). Data abort in `Tie3AnotherCategory::render` -> `render_from_another` -> `glBindTexture(m_textures->at(...))` (logs `hw-logs/jak2-kor-crash-1115*`). Cause: dkgl never advanced `SharedRenderState::frame_idx` (OpenGLRenderer does `frame_idx++` per frame), so `render_from_another`'s "master ran this frame" guard always passed and the TRANS/WATER category buckets drew with a master's stale level pointers after the level unloaded | Fix (Copilot, AI-assisted): `capture_begin` does `s_srs->frame_idx++`. NRO jak2 `b0e1d23a`, jak1 `c660ad30` deployed 2026-11-15; jak2 flag files moved to `switch/jak2/flags-off/` to test row J at the same time | 🟡 HW check pending |
| P | jak2 sky: hard-edged lines/rectangles in the clouds came back (HW 2026-11-16 screenshots, Dead Town/ruins: big lighter rectangular cloud tiles with straight edges, crossing in a '+' shape). Same look as the bug fixed by f97fd4013 (cloud noise texture forced to GL_REPEAT). | **2026-11-16 (AI-assisted, Copilot):** likely regression from c155bb1b7 (dkgl sampler objects). OpenGLRenderer calls `background_sampler_unbind()` after every bucket (FIX 43, OpenGLRenderer.cpp ~1863) but dkgl `run_bucket` did not, so a tfrag/tie sampler stayed bound on unit 0 and overrode the noise texture's REPEAT in the texture animator (`texture_sampler_combo` prefers the unit sampler). Fix: `background_sampler_unbind()` after `r->render` in `dkgl_frame.cpp run_bucket`. Built jak2 5a104fcb / jak1 734c22d1, deployed (backup `Jak 2.f47.bak`). 🟡 HW: look at the Dead Town sky. If lines remain: A/B with `gk_dkgl_noclouds.txt`, then with `Jak 2.f46.bak`, and log `g_unit_sampler[0]` at `TextureAnimator::handle_clouds_and_fog`. |
| Q | jak2 breakable objects have the wrong colour (HW 2026-11-16 screenshots, Dead Town/ruins): breakable rock/wall blocks (`goal_src/jak2/levels/ruins/breakable-wall.gc`, `pillar-collapse.gc`, art groups `ruins-breakable-wall`, `ruins-pillar-collapse`) are orange/red-brown instead of grey stone like the static walls next to them. The texture detail looks right; only the colour is wrong. | **2026-11-16 (AI-assisted):** first re-test with the row P build: the same leaked sampler can change the merc texture setup. If still wrong: these are merc (foreground, b197-221 PRIS) props. Check Merc2 per-draw tint/fog/`light` uniforms and the texture they bind (log the tbp/tex id for the `ruins-breakable-wall` draw). Compare with the same object in desktop GL (it is grey there). Suspects: a CLUT/palette texture (the clut blender re-enabled for row N writes into textures every frame and may write into the wrong slot), merc `rgba` vertex colours in the wrong channel order, or a wrong texture id. A/B: `Jak 2.f46.bak` (before row P) and an NRO without the row N clut blender. **2026-11-16 update (AI-assisted, Copilot):** user: the flat beige/orange surfaces are the materials of the breakable props; row P build did not fix it. Cause: since c155bb1b7 dkgl honours sampler objects, so Merc2 samples the BCn mip chain (before: level 0 only); some small BCn mips are wrong on deko3d (same family as the 32 px tall layout bug in LoaderStages.cpp). Fix: Merc2 on Switch uses level 0 only (`use_mipmaps_for_filtering`, Merc2.cpp; `gk_dkgl_mercmips.txt` turns mips back on). jak2 aebb708b / jak1 d5cdde24 deployed (`Jak 2.f48.bak`). 🟡 HW. Follow-up for GLM: find which BCn sizes give wrong mips in `dktexgpu::upload_bcn_mipped` and extend the RGBA decode workaround to them, then turn merc mips back on. |
| M | jak2 turret/cannon hit on Jak: two tilted squares, solid green inside with a pale frame (HW 2026-11-12 20:13:29 screenshot, city). Wrong texture, not alpha: glow off (`gk_dkgl_noglow.txt`), sprite3 frag shader = GL, tex `fail=0`. Jak's own guns are fine. Same family: green squares on gunshots and near the chimney | **Root cause found (AI-assisted, code reading)**: GL's `Sprite3::flush_sprites` resolves `texture_pool->lookup(tbp)` at bucket 313 (PARTICLES), with VRAM as the upload buckets below it left it. But `SpriteDk::capture_chain` replayed ALL 47 upload buckets — including the four ABOVE the sprite bucket (`TEX_ALL_WARP` 316, `DEBUG_NO_ZBUF1` 318, `TEX_ALL_MAP` 319, `SUBTITLE` 322) — before reading the sprites, so effect-sprite pages resolved against end-of-frame VRAM. A page reused by a later upload then drew that texture instead (map tiles = green with a pale frame; warp/subtitle content too). Same class as the eye bug (entry 19): "the Switch replays ALL of a frame's texture uploads before X runs" — the dkgl buckets got the interleave fix (`dk_upload_bucket_hook`), the sprite walk never did. Fix: `capture_chain` now walks the sprite bucket when the replay passes its bucket id (before 316/318/319/322), matching GL; jak1 is unchanged (all its upload buckets are below 66). Host regression tests `SpriteReplay.SpriteWalkRunsAtItsBucketJak2` / `...Jak1` lock the hook/upload/walk order; jak1 dump replay unchanged (1792 sprites, 15 batches, golden PASS). HW check pending: turret hits, gunshots, chimney smoke, skull-gem pickups (gems looking flat is bug G's mips, not this) | ✅ **HW 2026-11-13 (user): fixed.** NRO `330b04ce` (deployed as `jak2.nro`, old build rotated to `Jak 2.f40.bak`) — turret hits, gunshots and chimney all clean |
| R | jak2 intermittent white-box flash (~1 s) when a punch connects | 🟡 probe deployed, HW repro pending (AI-assisted) | Log `gk_run_log.txt` 2026-11-15: the ONLY anomalous counter is `sprite frame ... no_tex=1` (one batch per window). Code audit (2026-11-16) cleared every suspect path as *already skipping*: `SpriteDk::build_frame` skips `tex_id==0` batches (`no_tex++` + `continue`), `dktexgpu::slot_of(0)` = `UINT32_MAX` (skipped again in the recorder), `dk_sprite_lookup` returns 0 for null/placeholder pages, upload failures (`store full / too big / pool full`) all `return 0`, and every other pass reports clean (`dkgl notex 0`, direct `no_tex=0`, eye `bad=0`, `glow/distort bad 0`). So a *visible* white quad cannot come from a sprite-pool miss — it is either a batch resolved to the WRONG texture (row-M class, page reuse against later-bucket VRAM) or content torn at snapshot. Probe shipped (NRO `152164ddf3d6f2551e38bec689bd1855`, jak2 only needs it but both NROs rebuilt): `dk_sprite_lookup` now logs `[sprtex] MISS tbp=<page> <name-or-no-page>` for every miss (first 40, then 1/600) — a miss line exactly at a punch **that coincides with a visible white box** would prove the box is drawn elsewhere (distort glow snapshot / wrong-resolve); no MISS line at the flash points at a wrong-texture resolve, whose `[sprtex] tbp=... id=...` pairing line names the texture. Next: user reproduces the flash once, grep `MISS` + the timestamps around it **HW 2026-11-16 (AI-assisted):** flash also on spin-kick hits. Log (hw-logs/jak2-20261116-0951, 27 min session): 77 MISS lines, all neon signs / title sprites (skipped, never drawn), `sprite no_tex=0` every window, glow `bad 0` → the punch box is not a sprite miss. Hits spawn glow sprites, so A/B with `gk_dkgl_noglow.txt` (flag put on the card, with nodepthcue for row S) **2026-11-17 (AI-assisted):** trigger = PC health-bars cheat (`cheats #x404b` bit 1): `enemy.gc:1590` spawns `damage-number` + health bar in b318 (debug-no-zbuf1, GL DirectRenderer, fake uploads); glow ruled out (flash with glow 0). Probe `[nozb1tex] tbp= gl= name` in `DirectRenderer::update_gl_texture` (NRO ae701e6f deployed, backup `jak2.nro.bak-152164dd`). Next: punch atoll metal heads, read probe; confirm by cheat off (`#x4049`). **2026-11-18 (AI-assisted):** whole-log census: tbp=9216 → gl=296 (`gamefont/font.24lo`) constant for the ENTIRE log, flash window included ⇒ id-recycling exonerated; the name only proves the page mapping, not the image content behind id 296. Code read: the flash draws are the fmt44 (MT4HH) font path — `lookup_mt4hh` returns the HL source's gl id and `direct_basic_textured.frag` shapes the glyph from the texture's ALPHA channel: `if (T0.w == 0.) T0.w = ta0;` then tcc modulate `color = fragment_color * T0`. **If the font texture's alpha never reaches the dktexgpu image, every glyph quad is a solid box — matching the flash exactly (damage number on screen ~1 s, punch connects).** The binding id was never the issue; the remaining unknowns are the three the logs cannot see: (1) does the font.24lo upload carry alpha (HH composed in the extracted file) and which store id it gets (`add_texture` → `upload_rgba8_mipped`, LoaderStages.cpp — fonts never matched the old `tex_probe_page` list, which is why no font give/upload lines exist), (2) the per-draw UBO/vertex inputs (ta0, tcc/decal, color/alpha_mult, alpha test, vertex rgba/stq), (3) which gl id backs the fmt44 slot at `relocate(dst, src, 44)`. Three probes shipped (NRO `1993b90886303e088633a155b42b1027`, deployed as `jak2.nro`, old rotated to `jak2.nro.bak-9d7727d7`): `[fonttex] name %ux%u id= a0=/a255=/amid= p0=` alpha census at upload in `add_texture` (all `font.*`/gamefont textures, every occurrence — id≠296 means stale store id, a0==w*h means HH never composed, mixed alpha means content fine at upload and corrupted later); `[nozb1draw] tbp v ta0 cm am atest/aref dd v0=(xyz) rgba= st= q= tcc dec uv fog` per glyph batch in the debug-no-zbuf1 flush (first 240, then 1/600); `[mt4hh] dst= src=/ gl=` at every fmt44 registration. Next: user reproduces the flash once with the health-bars cheat and greps `fonttex|mt4hh|nozb1draw` + timestamps around it — this build answers content-vs-state in one run. |
| S | jak2 Industrial Section looks washed out / grey after leaving the ammo-dump (fordump) mission (HW 2026-11-17 10:51 screenshots, hw-logs/jak2-20261116-0951/shots) | 🟡 A/B pending (AI-assisted) | Light grey haze over the ground and far streets; dark walls keep their contrast, so it looks like haze/fog, not a gamma/colour bug. Log after the mission is clean (60 fps, tex fail=0, no errors). Suspects: (1) the game's own fog/weather (ctyind is smoggy on PS2 too) — not a bug; (2) DepthCue (dkgl list 64) blending a blurred frame copy at the game's alpha everywhere (u_depth=1.0 + GEQUAL). A/B: `gk_dkgl_nodepthcue.txt` on the card. Same haze with it ⇒ game fog, compare with desktop GL on the same save. **User 2026-11-16:** the haze went away for good after entering Torn's hideout ⇒ a state that a level change/blackout load resets (stale per-level renderer state or the game's mission mood), not the city's normal look. To repro: replay the ammo-dump exit, check if the haze shows, then if a hideout trip clears it with the A/B flags too |

### 7.1 Reusable as-is (native or dkgl, already validated on jak1 — jak2 shares the code)

| jak2 renderer (bucket) | Reuse | Note |
|---|---|---|
| tfrag NORMAL + TRANS trees (`tfrag-l*-tfrag`, `tfrag-t-l*-alpha`) | `TFragmentDk` native | TRANS/LOWRES_TRANS already exist; jak2 bucket IDs + WATER tree still missing (7.2 #4) |
| tie NORMAL + wind (`tie-l*-tfrag`) | `TieDk` native | wind done (T76), same DMA wind block layout |
| etie NORMAL_ENVMAP (`etie-l*-tfrag`) | `TieDk` native | etie/etie_base dksh from T67/T68 |
| shrub (`shrub-l*-shrub`) | `ShrubDk` native | anim slots missing (7.2 #6) |
| particles (`Sprite3`) | `SpriteDk` native | ✅ code 2026-11 (AI-assisted): walks GL `render_jak2` layout (no CALL head, `SpriteFrameData`, matrix −1 glow sprites skipped), jak2 `sprite3_vert_jak2` dksh, recorded at bucket 313 in `record_jak2_dkgl`; near-fade off on jak2 (GL look). Opt-out `gk_nosprite.txt`. Glow ✅ T126 (`GlowRenderer` through dkgl, fixed skull gems + dark eco on HW). Distort ✅ T127 (AI-assisted, 2026-11): GL `Sprite3::render_distorter` non-instanced path through dkgl (`sprite_distort` program + jak2 vert AND frag variants — frag uses `SCISSOR_HEIGHT` 416, so `kJak2Frags` swaps the frag blob too): `SpriteDk`'s distort section is handed to `dk_distort` → `dkgl::distort_begin/distort_dma/distort_end` (frame snapshot via the T112 color blit + refraction strips, kDistortList 1003), played by `record_distort()` BEFORE the native sprite bucket (GL order). Opt-out `gk_dkgl_nodistort.txt`; counters `distort/bad` in the dkgl log. Jak2-only (jak1 keeps the old skip+count). **HW 2026-11-04 (NRO `bf66f569`): scatter-gun shot was a big hard-edged flat yellow/red wedge — that is the missing distorter pass (the flash sprite without its refraction). Fixed by T127. HW 2026-11-08 (NRO `f35ed4b5`): `distort 276 bad 0` but the big red see-through fan (PC reference) was missing: gen-dksh.py had no jak2 rewrite for the bare `transformed.y *= 1.0`, so the jak2 vert kept the jak1 y-scale (1.0, GL 0.5). Fixed (NRO `dc0a083b`), HW confirm ⏳** |
| eyes (jak2 `EyeRenderer`, held off-bucket, called by merc2) | dkgl `eye` | capture/interleave machinery is bucket-generic |
| merc2 (all `merc-l*-*` + `merc-lcom-*` buckets) + emerc | dkgl `merc2` / `emerc` | limits already at GL values |
| generic2 — every `gmerc-*` bucket, `effects` (Mode::LIGHTNING), `warp` (draws through Generic2, no own program) | dkgl `generic` | jak2 uses it far more than jak1 (perf check in 7.2 #8) |
| sky-draw (jak2 registers a plain `DirectRenderer`, NOT `SkyRenderer` — no SkyBlend in this fork) | dkgl `direct_basic_textured` | textures arrive via `tex-lcom-sky-pre` uploads |
| HUD / subtitle / screen-filter / debug2 / debug3 / progress-hack | dkgl `direct_basic` / `direct_basic_textured` / `solid_color` | `ProgressRenderer` activates no own shader |
| ocean mid-far + near | `dkocean` native | validate jak2 VU frames on HW (7.2 #8) |
| texture uploads + level loader | dk pool + `LoaderStages` RGBA path | jak2 fr3 is RGBA (BCn path not needed); `kDkFr3LevelCount` already has jak2 |
| pmode fade + blackout | dkgl (`record_blackout`, `dk_set_pmode_alp`) | shared, game-agnostic |

### 7.2 Missing (new work)

**Fastest sequence** (refined 2026-10-31 after a "is this quickest?" review):

- **Step 0 — log-first inventory (≈free):** boot jak2 through the existing dkgl capture with every
  unknown program left inert. The `[dkgl] program 'X' not in table` + `dk_gl_census` logs hand you
  the runtime-verified gap list in minutes and confirm the static analysis below before any code.
  **Landed 2026-10-31 (AI-assisted):** jak2 now boots through dkgl capture — `dkgl_frame.cpp`
  has a jak2 bucket table (83 buckets: ocean 6/310, 40× merc2, 39× generic2 incl. effects 315,
  sky-draw 5, screen-filter 321, blackout) built from `buckets.h` enum symbols and constructed
  with GL's `GET_BUCKET_ID_FOR_LIST` stride loops; `SpriteDk` walks all 47 jak2 upload buckets
  (sprite read off); `dk_renderer.cpp` gates it on `gk_dkload.txt` + `gk_dktfrag.txt` in
  `sdmc:/switch/jak2/` and records via `record_jak2_dkgl` (dkgl buckets in GL order + blackout).
  The jak2 `EyeRenderer` is wired through `SharedRenderState::eye_renderer` like GL. Was inert
  by design: shadow 195/314 (shadow2) landed §7.2 phase 2 (2026-10-31), and the menu/HUD tail
  (progress 320, debug2 324, debug-no-zbuf2 325, debug3 326) landed the same phase — menus,
  loading bars, pause minimap and HUD text now render through dkgl (`gk_dkgl_nomenu.txt` A/B).
  Still inert: warp 317. Particles 313: native `SpriteDk` (2026-11, HW: renders; skull gems +
  dark eco fixed by T126 glow; scatter-gun distorter ported by T127 — y-scale fix ✅ HW 2026-11-13 (user); jak1
  distorter still skipped, same as glow being jak2-only).
  **HW 2026-10-31 (build e70b2c9d):** menus/main UI text ✅ show. Three regressions reported
  and under diagnosis: (a) pause minimap blank, (b) yellow background at title→Start, (c)
  shadows inside the hideout "weirdly white". Host-side the whole Shadow2 final-pass emission
  (stencil NOTEQUAL + RevSub/Add + ONE/ONE/ONE/ZERO + per-channel color mask + mid-frame
  stencil clear) is now golden-tested (`Shadow2LightenPassEmission`/`Shadow2DarkenPassUsesRevSub`,
  DkglState 34/34) — the state layer is provably correct, so (c) is runtime data (the
  time-of-day shadow color picks the *lighten* path indoors) or GPU-side. Diagnostics in the
  next build: `[shadow2] color r g b a -> pass` lines (one per distinct color, stdout),
  `[progress] fbp N` transitions, and a `notex` counter in the `[dkgl]` stats line (draws
  whose every sampler is unbound → stale texture, the likely shape of (b)).
  Tables verified against the GL registrations by script (walk ≡ upload set 47/47, merc 40/40,
  generic 39/39). **Note:** real jak2 bucket ids differ from the old manual estimates —
  `MAX_BUCKETS` is 327, `SHADOW`=195, `SHADOW2`=314, `EFFECTS`=315, `WARP`=317, `OCEAN_NEAR`=310;
  always take ids from the enum, not from prose.
- **Phase 1 (first picture):** #1 wiring + #7 flags. Write #1 **table-driven** — mirror GL's
  `GET_BUCKET_ID_FOR_LIST` loops (chain start + stride × 5 levels + `lcom` list), not ~150
  hand-enumerated buckets. Natives + existing dkgl programs then give world, characters, HUD,
  merc, generic, sky, eyes. Missing: shadows, water trees, alpha ties, animated textures.
  **dkgl half done (see Step 0); native halves landed 2026-10-31 (AI-assisted):**
  `TFragmentDk`/`TieDk`/`ShrubDk`/`MercDk`/`DirectDk` each got a jak2 table (stride-built
  from `buckets.h` symbols) + `set_game` pick; `SpriteDk` was already parameterized.
  `record_jak2_dkgl` now splits the native world passes in at their GL bucket positions
  (`kJak2NativeSplits`: per level TFRAG_L→TIE_L→ETIE_L before MERC_L tfrag, SHRUB_L before
  MERC_L shrub, TFRAG_T_L (TRANS) before MERC_L alpha), driven by `DrawCmd::range` +
  `record_pass` slices in `dk_tfrag_record.cpp`. jak2 deltas handled: no CALL chain head,
  fog from the vis bucket (`BUCKET_2` vif0, like `VisDataHandler`), tie proto-vis packet
  instead of WindWork (wind draws at rest, like GL's zero WindWork), envmap tint /128
  without the jak1 ×2. Debug off-switch: `gk_dkgl_noworld.txt` (dkgl-only world).
  Still inert on jak2: `MercDk`/`DirectDk` (dkgl merc2/direct own those buckets, §7.1),
  particles (phase 3), shadow2/warp/tfrag-WATER/alpha-ties (phase 2). HW test pending.
  **HW 2026-10-31 (NRO `6e819bd6`, AI-assisted):** Haven City renders, much faster than GL, no
  fatal. Three bugs:
  (a) *wrong aspect* — same as jak1 #20: the SD `pc-settings.gc` had `aspect4x3 … #t` +
  `letterbox #t`; set to `(aspect-state aspect16x9 16 9 #f)` / `(letterbox #f)` (old file in
  `hw-logs/jak2-pc-settings.gc.before-aspect-fix`). Settings fix, not code.
  (b) *Jak's head missing* — dkgl Merc2 got `s_no_anim_slots` (all 0); jak2 slots 0–13
  (`texture_slots.cpp`) are Jak's face/hair/eyebrow/eyelid/finger, made by TextureAnimator
  `ClutBlender`. Stopgap: `fill_jak2_clut_slots()` in `dkgl_frame.cpp` builds each slot's
  default (`-norm`/`-start`) CLUT texture on the CPU from GAME.fr3 `index_textures` and uploads
  it with `dktexgpu::upload_rgba8_mipped` (`dkgl::set_common_level` from `dk_loader_start`).
  No dark-Jak morph until #3. Log: `[dkgl] jak2 anim slots: N/34 static clut textures`.
  (c) *violet corners* — user (photos IMG_0161/0165): missing shadows (shadow2, phase 2 #2).
  **HW 2026-10-31 #2 (NRO `de153a64`):** head OK (`anim slots: 12/32`). Game ran at 30 fps
  because the SD `pc-settings.gc` had `(fps 30)` (swap interval already 60): set to `(fps 60)`
  like jak1 (old file `hw-logs/jak2-pc-settings.gc.before-fps60`). Eyes missing on everyone
  (`eye draws 0`): jak2 `eye.gc` sends eye DMA through `vu1-bucket-map (merc-mode eye)` to the
  level's `tex-*` buckets, which dkgl did not run. Fix (NRO `4f48a65c`): `init_jak2` merges
  the 40 eye-capable tex buckets into the dkgl table as `PrisTexEyes` lists (`s_eye_list`);
  the eye barrier is only recorded when a list drew. Log: `[dkgl] jak2 eyes: 40 tex-bucket
  eye lists`, then `eye draws N` > 0 in the 10 s line.
  *Picture stretched/zoomed vertically* (user, gameplay too; jak1 fine): every dk/dkgl vertex
  shader baked the jak1 y-scale `(512/448) * 1.0`; GL uses `(512/416) * 0.5` for jak2
  (`Shader.cpp:67-81`), so jak2 y was ~1.86× too big. Fix (NRO `a92fa1df`): `gen-dksh.py
  --game jak2` rewrites the constants (fails if a `448` is left), `game/CMakeLists.txt` builds
  `*_vert_jak2_dksh` for tfrag3/shrub/etie/etie_base/merc2 + dkgl ocean_common/generic/shadow/
  direct_basic(_textured)/emerc/merc2, picked by `g_game_version` (`dk_device.cpp`,
  `use_jak2_verts()` in `dkgl_frame.cpp`). Jak1 `.dksh` bytes unchanged.
  **HW 2026-10-31 #3 (NRO `a92fa1df`): user — aspect, eyes, head all correct, nothing missing.**
  Log: `jak2 eyes: 40`, eye draws ~16/frame, fps 45.7–60 (steady 60 at 300–400 s, dips to
  46–50 in busy Haven City). Slow frames are `cpu` (render thread `build` ~6–8 ms + dkgl
  `capture` ~4–5 ms), GPU fence ~5 ms: next perf work is CPU-side (#8).
- **Phase 2 (days):** #2 `shadow2` — **landed 2026-10-31 (AI-assisted, HW pending)**; details in
  item 2 below (generated program, both bucket instances, dkgl's first mid-frame stencil clear).
  **Same session, bonus: the menus** — progress 320 (ProgressRenderer: loading bars + the
  pause-menu minimap off-screen FBO → VRAM 4032) and debug2 324 / debug-no-zbuf2 325 /
  debug3 326 (DirectRenderers, the menu/HUD-text buckets = jak1 debug 67 equivalents) wired
  through dkgl like sky-draw/screen-filter; the native DirectDk walk stays off jak2 (no CALL
  head). Upload-only neighbours (318/319/322) already ride the native sprite walk.
  A/B `gk_dkgl_nomenu.txt`. Remaining: #5 tie categories, #4 tfrag WATER.
- **Phase 3 (only multi-day subsystem, last):** #3 TextureAnimator + #6 shrub slots (rides on
  #3's plumbing). Deferring it does not block a playable-on-dk milestone — Haven City ads just
  render as static first-frame textures.
  **#3 landed 2026-11-06 (T125, AI-assisted; hardware test pending):** the UNCHANGED GL
  `TextureAnimator` now runs through dkgl on the Switch build, per the wrapper rule — no
  CPU-composite port (a first attempt, `TextureAnimDk.h`, was deleted for exactly that
  reason). Wiring: `init_jak2` constructs it from `s_srs->shaders` + the GAME.fr3 common
  level (`gk_dkgl_notexanim.txt` A/B); the SpriteDk tex-bucket walk dispatches PC_PORT
  imm-12 to `dk_texture_anim` (dk_renderer), which brackets
  `handle_texture_anim_data` in `dkgl::tex_anim_begin/end` — the animator's FBO draws
  record into a dedicated `kTexAnimList` (blackout-style pseudo bucket, called by
  `record_tex_anim()` right before the native sprite bucket 313 draws). Publish path is
  GL's own: `update_gl_texture` + `move_existing_to_vram`, with the pool seam resolving the
  dkgl texture id to a dktexgpu id, so the native sprite lookup finds the gems. Shaders:
  `tex_anim` added to the T103 generator (now supports uniform ARRAYS at std140 stride 16
  and ivec2/3/4) — `positions[4]`/`uvs[4]` spread in `uniform2fv/3fv`, `channel_scramble`
  via `uniform4i`. Still census stubs (logged, benign for skull gems):
  `glGenerateMipmap` (no mip content — sprites sample LINEAR), aniso `glTexParameterf`,
  debug-only `glGetTexImage`/`glGetTexLevelParameteriv`/`glGetFloatv`.
- #8/#9 are continuous (measure + read logs each phase), #10 runs in parallel (wiring, shadow2
  and the tex_anim shaders can be written and host-tested before extraction lands — dkgl
  compiles in the host tests).

Why not the alternatives (checked): **dkgl-everything** needs porting 7 world-geometry shader
pairs (`tfrag3`, `tfrag3_no_tex`, `shrub`, `etie`, `etie_base`, `tie_wind`, `sprite3_3d`) to
ES310+std140 with per-shader HW validation — the natives already render that same (unified
TFRAG3_VERSION) data correctly on jak1 hardware, so extending their tables/categories is much
less work. **Native replacements** for shadow2/merc2/generic would duplicate what dkgl already
does. Waiting for #10 before coding serializes work that can overlap.

1. **jak2 bucket wiring in `dk_renderer.cpp`.** `record_tfrag_and_shrub`, the sky/ocean hooks and
   `dk_upload_bucket_hook` use hardcoded jak1 bucket IDs. Need the jak2 lists (per-level
   tex→tfrag/tie/etie/merc/gmerc → shrub chain → alpha chain → pris/pris2 → water chain, plus the
   `lcom` buckets), in GL bucket order, calling the same natives + `dkgl::record_bucket` /
   `capture_until` (that machinery is bucket-generic). This is the first thing to write.
2. **dkgl program `shadow2` — LANDED 2026-10-31 (AI-assisted, HW pending).** jak2 shadows are
   `Shadow2` (buckets SHADOW 195 and SHADOW2 314 — GL registers exactly two instances, "shadow"
   and "shadow2"; the `shadow3` registration is jak3-only), program `shadow2`, not an alias of
   jak1's `shadow`. Shipped: `shadow2` added to `dkgl_gen_shaders.py PROGRAMS` (generated
   `shaders/dkgl/shadow2.{vert,frag}.glsl` from the GL sources + uniform-table entry; vert block
   7 slots/112 B, frag 1/16 B); `_DKGL_SHADERS` + `_JAK2_SHADERS` (jak2 y-scale variant,
   `dkgl_shadow2_vert_jak2_dksh`); `s_programs`/`kJak2Verts` entries; `SHADOW`/`SHADOW2` in
   `kJak2Buckets` at their GL dispatch positions; two `Shadow2` instances in `init_jak2`
   (A/B flag `gk_dkgl_noshadow2.txt`). **New dkgl capability — mid-frame stencil clear:**
   Shadow2's second instance resets the stencil buffer after the first drew its volumes
   (`glClearStencil(0)` + `glClear(GL_STENCIL_BUFFER_BIT)`, gated by `stencil_dirty` which
   `capture_begin` now resets per frame like GL). dkgl previously dropped the stencil bit of
   `glClear`. Added: `STENCIL_BUFFER_BIT` in `dkgl_gl_enums.h`, `clear_stencil` in
   `ShadowState` + `dkgl_glClearStencil` glad override, `Emitter::clear` now carries
   `stencil/mask/value`, and the Switch emitter does a stencil-only
   `dkCmdBufClearDepthStencil(cb, false, 0.f, mask, value)` with full-target scissors on the
   frame target — safe mid-frame (natives never write stencil, `clearDepth=false` leaves the
   world depth alone; the frame-begin clear still zeroes stencil at frame start). Everything
   else Shadow2 uses was already dkgl from T111/T113: stencil func/op/mask, colour mask,
   `FUNC_REVERSE_SUBTRACT`, depth `ALWAYS`/`GEQUAL`, separate alpha blend (works — repo
   deko3d is T101-patched), TRIANGLE_STRIP + primitive restart, per-frame buffer renames
   (ctor size-only `glBufferData` reserves nothing, per-frame uploads are the used counts).
   Host `Dkgl*` 36/36.
3. **TextureAnimator on the dk path** — ✅ code done 2026-11-06 (T125, AI-assisted; hardware
   test pending, see Phase 3 above for the full wiring). The dk upload replay now dispatches
   PC_PORT imm-12 to the UNCHANGED GL animator through dkgl (`tex_anim` program, kTexAnimList
   segments recorded at capture, called before sprite bucket 313). Not covered yet: shrub
   anim slots (#6) and a `glGenerateMipmap` implementation (census stub, mips stay empty —
   LINEAR sampling hides it for sprites).
4. **`TFragmentDk`: WATER tree kind** (`tfrag-w-l*-water` buckets) + the jak2 bucket table, and
   verify the recorder against the GL tfrag3-jak2 vertex format (wind / multi-draw deltas —
   the native tfrag shader has no wind today).
5. **`TieDk`: missing categories.** Only NORMAL / NORMAL_ENVMAP / NORMAL_ENVMAP_SECOND_DRAW are
   wired; jak2 also needs TRANS, TRANS_ENVMAP, WATER, WATER_ENVMAP (the etie dksh already cover
   the envmap variants).
6. **`ShrubDk`: anim slot support.** jak2 `Shrub` is constructed with `anim_slot_array()`; the
   native recorder has none (depends on #3's animator plumbing for the actual textures).
7. **Enable the path.** `gfx.cpp` hardcodes jak1 as the deko3d default; jak2 needs
   `gk_dk3d.txt` + `gk_dkload.txt` + per-pass `gk_dk*.txt` opt-ins (`dk_pass_on`) until it
   renders right, then flip the default like jak1 did.
8. **Scale validation (code expected to hold, must be measured).** Haven City is much heavier:
   VRAM/pool sizing (T57's 384 MB was sized on jak1), `dktexgpu` slot recycling under jak2
   texture churn (pitfall P8), merc2/generic2 draw volume, ocean on jak2 data, sprite fill
   (same overdraw class as bug #17).
   **Measured 2026-10-31 (NRO `a92fa1df`, handheld, STOCK clocks — no overclock):** playable
   now — avg ~55 fps, long locked-60 stretches, worst 10 s window 45.7 fps (busy Haven City),
   frame max mostly ≤ 22 ms. CPU-bound: slow frames are `cpu`, render thread `build` 6–8 ms +
   dkgl `capture` 4–5 ms, GPU fence only ~5 ms. Goal: locked 60 at stock handheld clocks (do
   not rely on overclock; docked raises only the GPU). Ideas: cache per-frame draw data that
   did not change, pin the render thread (it hops cores 0/1/2 — `gk_pin.txt` test first),
   split `build`/`capture` work across cores. Fallback: a well-paced 30 fps cap.
   **Correction 2026-11-01 (NRO `90c0eaf5` logs):** in heavy areas the frame time tracks
   `gpu world` almost exactly (22.1 vs 22.5 ms, 21.2 vs 21.5 ms) -- jak2 is GPU-bound there,
   not CPU-bound (the `slow` classifier sums every CPU part, so it always says `cpu`). Pinning
   was already tried (FIX 70b, regressed). Next: opt-in per-bucket GPU profiler
   (`switch/jak2/gk_gpuprof.txt` -> `[dk] gpu top ms/frame` line, NRO `fee755cd`), then
   optimise the top buckets.
   **Fix try 1 (NRO `1359571c`, 960x540): REVERTED** — user rejects any resolution drop below
   720p handheld. Checked and ruled out at 720p: fb HwCompression already on; TexturePool
   textures sample with GL_LINEAR (no mips on GL either); per-frame barriers (eye, tod copies)
   and the shadow2 full-screen quads are each well under 1 ms. Next fix needs the per-bucket
   `[dk] gpu top` data from the 720p profiler build `fee755cd`.
   **Profiler result (2026-11-01, NRO `fee755cd`, Haven + water station, 46 fps):** gpu world
   18-22 ms. Per unit ms/frame: sky-draw b5 **5.8-8.7** (biggest; 1.9 on the title screen),
   native tie 3-6 (one level at a time, e.g. tie2/tie6), native shrub 3-7 (sh1/sh3 ~3.5 each;
   shrub has no culling on GL either, 245k tris/frame), merc pris b201/b205 1-2, tfrag only
   ~0.5. Next: find why the sky costs ~25 full-screen layers (expected ~1-2 ms), then shrub.
   Title "yellow": sky-tng menu-mode tints the blit-displays capture (tbp #x4c0/#x3300) sepia;
   dkgl has no framebuffer copy there, so only the tint shows.
   **Sky fix (HW-confirmed 2026-11-01, NRO `2ce828bc`):** the jak2 sky double-draw (two
   alpha-test discard passes) is folded into one draw on Switch (`DirectRenderer.cpp`, only
   for `sky-direct`) and dkgl binds the discard-free `direct_basic_textured_fast` shader, so
   early-Z works: sky b5 6-9 -> ~3 ms, Haven 46 -> 53-60 fps. Remaining top: tie 3-5, shrub 2-4.
   TieDk/TFragmentDk now honour `lod-force-tie/tfrag` from pc-settings (default 0 = full
   detail; lower LOD is a visual trade-off for the user to choose).
   **2026-11-18: user chose the perf preset — card set to `(lod-force-tie 1)`** in
   `pc-settings.gc` (one tier down, tie only; tfrag/ocean/actor left at 0). Boot log should
   read `[dk] tie geom tier 1 (lod-force-tie 1)`; expected ~1-2 ms off tie in heavy Haven
   areas (tie was 3-5 ms after the sky fix). Revert to 0 any time in the settings file;
   in-game the graphics menu "background LOD: low" preset would set 2.
   (Same session: FIX 119 texture-pool raise is in every NRO from `eae0e295` on; latest
   deploy 2026-11-18 = `896798cb…` (also built via `check-switch.sh`, PASS), HW playtest
   pending — see CLINE_HANDOFF.md §FIX 119.)
   **HW data 2026-11-18 (last session of `gk_run_log.txt`, NRO `896798cb` + tier 1, ~5 min
   Haven):** `tex pool full` 0 (fix holding at 5 min; the old build logged 1023/session),
   tex live high 2057/4096, most 10-s windows 0 frames over 16.7 ms, two windows 21 and 61
   over. Top buckets: sky b5 2.7-3.1 (still #1 — the in-game low-res cloud setting only
   lowers texture detail, not fill), tie2 2.1-2.9 at tier 1, shrub ~1-2, merc b205/b201
   ~0.4-0.7. The worst window correlates with `tod 5.4/frame` (tod palette churn).
   **Half-res sky render target (2026-11-18, NRO pending HW):** the sky bucket now renders
   into a half-res (640x360) colour-only dkgl FBO instead of the 1280x720 frame target
   (~4x less 3D-engine fill for ~0.6 ms of 2D blits; expected b5 3 -> ~1-1.5 ms). Path:
   `dkgl_frame.cpp` `sky_halfres_begin/end` around `run_bucket` — downscale-blit the frame
   into the FBO first (so the sky blends over the same content as GL), half viewport (the
   direct shader maps GOAL window coords to NDC, so the same vertices fill the half-res
   target), then composite back with a linear-filtered blit (`dkdev::blit_to_frame`, new,
   the reverse of `blit_frame_to`). The sky's far-plane depth writes are dropped (no depth
   attachment): unobservable, the frame depth is still at its 0.0 clear when the sky draws
   and every later GEQUAL draw passes either way. Kill switch for A/B:
   `sdmc:/switch/jak2/gk_dkgl_fullsky.txt` (reboot to apply). Boot log:
   `[dkgl] sky half-res on (gk_dkgl_fullsky.txt to disable)`; 10-s stats line gained
   `sky halfres N fail M`. Check on HW: sky looks right (orientation/colours/alpha), b5 in
   `gpu top` ~1-1.5 ms, no regression in the gun-course haze / warp red cone (they read the
   frame the composite writes). NRO `9d7727d7` (check-switch PASS, dk-lint PASS, deployed to
   the SD 2026-11-18 with rotation; old `896798cb` → `jak2.nro.bak-896798cb`).
   **2026-11-18 static audit + zero-draw guard (AI-assisted):** full line-by-line trace of the
   record flow before the first HW boot — snapshot `blit_framebuffer` → `blit_frame_to_tex`
   segment (mirrors the HW-proven DepthCue/distort path, `DkBarrier_Full` inside
   `dkdev::blit_frame_to`), offscreen bind recorded IN the draw list by the pending-target
   flush (ocean pattern, immune to the segment `frame_target` flag), composite
   `blit_tex_to_frame` with the Full + Image-invalidate barriers inside `dkdev::blit_to_frame`
   (P3 covered), read-FBO stays 0 for every later blit caller, pool-full tex_image_2d failure
   falls back to the full-res path. One real edge case found and fixed: a frame where the sky
   bucket emits **no draws** (menus/loads/fades) still composited the snapshot back — a
   720→360→720 linear round-trip that blurs the whole frame, where GL leaves the frame
   untouched. `sky_halfres_end(bool drew)` now skips the composite when
   `draw_count() == draws_before`; the 10-s stats line gained `nodraw`
   (`sky halfres N fail M nodraw K`). NRO `120ea635` (check-switch PASS, dk-lint PASS; includes
   the jak3 agent's uncommitted J9 y-scale pick, verified behaviour-neutral for jak2:
   `g_game_version != 1` selects the same `_jak2_dksh` variants, hfrag is a jak3-only pass).
   **HW verified 2026-11-18 (deployed NRO `1cd2c518`, built from the committed tree incl. the
   zero-draw guard session's work):** `sky halfres 60/660/1260 fail 0`, `gpu top` b5 1.13-2.83 ms
   (was 5.8-8.7 full-res), most 10-s windows `0 over 16.7`. Sky render target is DONE for
   jak2 -- remaining wobble is tie/shrub (see the perf TODO below).
   **FIX 132 (2026-11-18, AI-assisted) -- the abysmal-framerate hardware regression was NOT the
   sky, it was the texture-pool probe.** The FIX 123/131 `tex_probe_page` diagnostic list
   (998/2405/1021/1325 + 853/929/2929/3090) contains hot pages: 1021/853/2929 link every
   frame, so `[pool] link` logging emitted **2.06M lines / a 91 MB `gk_run_log.txt`** in one
   session while `gpu top` stayed healthy -- render-thread snprintf + mutex + SD flushes at
   ~11k lines/s were the entire cost. `tex_probe_page` now returns false (probe retired; the
   FIX 131 slot-guard it was probing for is committed, evidence archived in
   `hw-logs/jak2-probe-flood-20261118/gk_run_log.txt.gz`, 91 MB -> 7.7 MB). Rule: never leave
   hot pages in the probe list in a deploy build.
   **FIX 133 (2026-11-18, AI-assisted) -- the walls regression was still live: the slot guard
   yielded to GEOMETRY pressure, not texture pressure.** The probe-flood log proved the
   t=168.9 s `unload 853/*` mass unload fired right after a `FIX125 pressure` line, while
   `[dk] tex ... fail=0` the whole session: `loader_under_pressure()` is the staging BUFFER
   pool (< 16 MB, routine during any level stream -- the lwidea upload at t=166 s), and the
   old wiring let it drop the FIX 131 slot guard to 3600 frames, killing draw-idle-but-linked
   page 853 (ruins) with no re-give ever following. The guard now yields ONLY when the texture
   heap itself is failing (`dktexgpu::failed_allocs()` is monotonic -> delta vs last-seen
   count; a chronically-full store keeps failing retries and stays pressured, a one-off
   failure yields for a single update); buffer pressure keeps governing only the FIX 125
   draw-stamp defer window, which cannot blacken a linked page. Diagnostics restored safely:
   `[pool] give/unload/placeholder-new/FIX124-done` now log unconditionally (rare, event-
   driven, bounded by level streams) -- only the per-frame `[pool] link` lines stay behind the
   retired probe, so give/unload pairs remain auditable without re-arming any hot page.
   **FIX 134 (2026-11-18, AI-assisted) -- the save-menu picture regression was the half-res
   sky, and the zero-draw guard could never see it.** In menu mode `sky-tng.gc` (line ~869)
   draws the frozen-frame backdrop (bucket-3 capture, tbp `#x4c0`) + tint + filter overlay
   INSIDE the SKY_DRAW bucket -- 3 full-screen quads, so the bucket always "drew"
   (`nodraw 1` the whole session) and the 720->360->720 round-trip blurred the whole menu
   backdrop (and the title screen's). The half-res path now stays off when the PREVIOUS sky
   bucket's draw count <= 8 (menu ~3, in-game sky = 20+; the decision must precede the
   renderer run, so one menu-entry frame rides at half-res behind the progress-interp
   fade-in, invisible). Stats line gained `menu K` (`sky halfres N fail M nodraw K menu L`).
   HW check: save-menu picture sharp again, `menu` counter climbs in menus/title only, b5 in
   `gpu top` still 1-3 ms during gameplay.
   **FIX 135 (2026-11-19, AI-assisted) -- the save-menu picture broke again: FIX 134's
   detector is one frame late at menu ENTRY.** The card still ran the FIX 134 NRO
   (`ea61ea4b`, never re-verified on the save screen): entering the pause/save menu from
   gameplay, the previous sky count is 20+, so the entry frame -- the exact frame whose
   frozen image the menu captures and draws as the picture -- rode the 720->360->720
   round-trip; the "invisible behind the fade-in" claim was wrong because the capture side
   (`fx-copy-buf` bucket-3 PC_PORT 0x10 + the warp `0x4c0` copy at bucket 317) bakes that
   frame's blur in for the whole menu session. GOAL gives a perfect same-frame signal:
   bucket 3 runs BEFORE the sky bucket and the fx-copy-buf capture fires only on the
   menu-entry frame (`blit-displays.gc`: menu-mode && !screen-copied), so the queued
   jak2 BUCKET_3 `blit_prev` segment now sets `s_sky_menu_entry`, which forces the entry
   frame's sky full-res too (`sky_menu = prev<=8 || s_sky_menu_entry`; cleared in
   `capture_begin`). New `late J` stat in the dkgl stats line: frames where half-res DID
   run but the sky bucket turned out menu-sized (drew <= 8) -- an exemption miss that
   still blurs (e.g. a save screen whose own sky exceeds the threshold, or the menu quads
   landing a frame after the capture). HW check: save-menu picture sharp when entered
   BOTH from gameplay and from title; `menu` climbs at every menu entry now; if `late`
   climbs while the picture is broken, raise `kSkyMenuDrawMax` / add the retro-skip.**
   **Slow-mo fix (2026-11-01, GAME.CGO `f8765700`, HW pending):** `pc-maybe-vsync` in
   `goal_src/jak2/engine/draw/drawable.gc` now returns the measured frame time as the dog-ratio
   (jitter < 1.15 -> 1.0, max 4.0), like the PS2 display-sync, so drops below 60 no longer slow
   the game down.
   **TODO — next perf step: make tie and shrub cheaper at 720p, full detail (no LOD/res drop).**
   Profiler after the sky fix: tie 3-5 ms (tie2/tie4/tie6), shrub 2-4 ms (sh1/sh2/sh3),
   sky ~3 ms; target total < 16.7 ms with room for particles/warp/texture animator.
   Ideas to measure one at a time with `gk_gpuprof.txt`: (a) shrub has no frustum culling
   (~245k tris/frame) — add per-instance/per-group culling like TieDk; (b) check tie/shrub
   shaders for `discard` (alpha test) on opaque draws, which disables early-Z (same cause as the
   sky); (c) draw order front-to-back / depth pre-pass for the heaviest tie trees; (d) vertex
   format/size and fragment shader cost (fog, tod blend done per pixel vs per vertex).
   **Step 1 (2026-11-03, NRO `b33c1147`, HW pending):** TieDk read and discarded the jak2
   proto-vis mask and ShrubDk never read it, so both drew every proto the game had hidden (GL
   skips them). Both now apply it (`dk_proto_vis.h`, host test `DkProtoVis`). Measure tie/shrub
   ms with `gk_gpuprof.txt` in Haven City before trying (a)-(d).
9. **Census sweep.** Run jak2 on the dk build and close every `dk_gl_census` unimplemented-call
   hit the same way #19 did; `glClearBufferfv` is a known miss already.
10. **Data prerequisite (not renderer work).** jak2 fr3 extraction (v43 RGBA) +
    `jak2_texture_remap` per JAK2_JAK3_ROLLOUT_PLAN §6.1 — nothing above can be tested without it.

### 7.3 Checked and NOT jak2 gaps

`depth_cue` (jak1-only registration), `SkyRenderer`/`SkyBlendHandler` (jak1-only in this fork —
jak2's sky-draw is a plain DirectRenderer), `hfrag` (jak3), `glow_*` (no GlowRenderer is
registered anywhere), `direct2`/`DirectRenderer2` (registered nowhere), movie playback (no
MoviePlayer in this fork at all), PCRTC (jak3-only), `CollideMeshRenderer` (not registered).
