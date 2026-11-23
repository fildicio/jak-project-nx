# J18 handoff for GLM (AI-assisted)

Read AI_ROLES.md, then the J18 row in docs/deko3d/JAK3_BRINGUP_TODO.md. Work on jak3 only.

## Bug
On Switch hardware, jak3 wasteland enemy cars (the `interceptor` model) are black when close
and correctly coloured when far. On the close car the spike horns ARE coloured, the body is
black. Screenshots: SD `emuMMC/RAW1/Nintendo/Album/2026/11/23/2026112316520700-*.jpg` (close,
black) and `...16520101-*.jpg` (far). Log: `switch/jak3/gk_run_log.txt` (latest boot = last
`glad overrides` line).

## Already ruled out (do NOT retest)
fog colour (FIX 156), Shadow2, dkgl UBO reuse/emit cache (`gk_dkgl_nouboreuse.txt`, no change),
merc2 dkgl shaders (= desktop), the game's light values (`gk_dkgl_merclightfix.txt` forced
neutral lights: still black), merc fade/w1, LOD swap (only `*-lod0` drawn), wreck colour fade
(only after destruction), bind probe (0 dead binds, 0 mismatch), FIX 157 mips level 0,
FIX 161 (stop BCn mip chain before a 32-px-tall level, in `dk_texture_gpu.cpp`
`upload_bcn_mipped`: no change; revert it unless it turns out useful).

## Strongest lead
`[mercprobe]` for interceptor-lod0: body textures come from tpage `desrescg-pris`
(intcept-pipe01, intcept-base-green01, intcept-base-patern02, all BC3 64x64/128x64).
The horns and the far look use tpage `desrescg-water` (`intcept-lorez-spike01` etc.).
So it is probably per TPAGE, and the distance dependence is the merc effect switching to
the "lorez" effect far away, not mips. Check, in this order:
1. Use `build-host/tools/level_tools/merc_dump <desrescg fr3> interceptor-lod0 --tex` to list each
   effect, its textures and when the lorez effect is used. Confirm which effect draws close.
2. Texture animation / tex remap on Switch: does any TextureAnimator / dkgl texanim output (log
   counts `texanim` are high) or tpage remap write into the `desrescg-pris` texture ids?
   Compare with GL `TextureAnimator` for jak3.
3. Upload of `desrescg-pris` textures in `LoaderStages.cpp` add_texture (~1022) vs `-water`: any
   difference in format, size or mip table. Check the decoded fr3 pixels on the host
   (`merc_dump --tex`) to see if level 0 really holds colour.
4. Also black on Switch: wasteland gate posts (no envmap). Check whether they also use a `*-pris` tpage.

## Rules
Use only APIs found in docs/deko3d-ref/include. Make one change per HW trip, gated by a flag file.
Run `scripts/ai/check-switch.sh jak3` + `python3 scripts/ai/check-dk.py` before deploy. Deploy
to `/Volumes/SWITCH SD/switch/jak3/jak3.nro`. Do not commit without asking. Write what you
find into the J18 row and end with "Your next commands".

## Progress from GLM session 1795439154634_l7hc3 (died 17:14 on an image upload, AI-assisted)
- Lead 2 checked: texanim clut/anim slots are separate named textures (jakc-face...), nothing
  found writing into `desrescg-pris` ids.
- Lead 3 checked: `add_texture` dk upload path is identical for `-pris` and `-water` (same BC3
  sizes/mips). `pool=` in `merc_dump --tex` = `load_to_pool`.
- NEW: `[mercprobe]` shows interceptor-lod0's body binds `desrescg-pris` ALWAYS, also far
  (no runtime tpage remap). So the "far = lorez tpage" idea is not confirmed; re-check lead 1.
- Still open: lead 1 (which effect draws close vs far), lead 3 pixels at level 0, lead 4.
- Do NOT read screenshots (.jpg) into the model: the API rejects images (`invalid_request`).
