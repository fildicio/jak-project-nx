/*!
 * @file dk_renderer.cpp
 * deko3d migration Phase 1 (AI-assisted): the GfxRendererModule for the native deko3d
 * backend. See dk_renderer.h for scope. libnx/deko3d calls live in dk_device.cpp; this
 * TU uses the project's normal headers (SDL compat for input, run_log for telemetry).
 *
 * Engine handshake: identical contract to the GL module (gl_vsync / gl_sync_path /
 * gl_send_chain, incl. the FIX 11 single-mutex rule and the FIX 19 "frame_idx must
 * advance every iteration" rule), so the GOAL engine runs unchanged. Received DMA chains
 * are acknowledged but not rendered in Phase 1 -- the screen shows the boot splash, then
 * black. Level/texture entry points are accepted and ignored for now (P2/P3).
 */

#include "game/graphics/pipelines/dk_renderer.h"

#if defined(__SWITCH__)

#include <algorithm>
#include <array>  // J4: kJak3NativeSplits is a constexpr-built std::array
#include <atomic>  // T49: s_dk_loader_ptr
#include <chrono>
#include <cstdio>
#include <cstring>  // T24: strlen() while assembling the soak telemetry line
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>  // std::this_thread::sleep_for when out of focus (T10)
#include <unordered_set>  // [sprtex] probe
#include <unistd.h>

#include "common/goal_constants.h"  // T49: jakN::LEVEL_TOTAL for the dk-path Loader
#include "common/log/log.h"
#include "common/util/FileUtil.h"     // T49: file_util::get_jak_project_dir
#include "common/util/Timer.h"

#include "game/graphics/display.h"
#include "game/graphics/opengl_renderer/buckets.h"  // §7.2 phase 1: jak2::BucketId splits
#include "game/graphics/pipelines/dk_device.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"  // T24: P2.3 dktexgpu telemetry
#include "game/graphics/deko3d_renderer/TFragmentDk.h"     // T60: tfrag world pass
#include "game/graphics/deko3d_renderer/HfragDk.h"          // T113: hfrag (mountains)
#include "game/graphics/deko3d_renderer/ShrubDk.h"         // T72: shrub world pass
#include "game/graphics/deko3d_renderer/TieDk.h"           // T76: tie world pass
#include "game/graphics/deko3d_renderer/MercDk.h"          // T78: merc2 pass
#include "game/graphics/deko3d_renderer/SpriteDk.h"        // T79: sprite3 pass
#include "game/graphics/deko3d_renderer/DirectDk.h"        // T80: menus / HUD text
#include "game/graphics/deko3d_renderer/SkyDk.h"           // T88: jak1 sky CPU blend
#include "game/graphics/deko3d_renderer/dk_ocean_driver.h"  // T95: jak1 ocean
#include "game/graphics/deko3d_renderer/dkgl/dkgl_frame.h"  // T107: GL ocean through dkgl
// FIX 121: dkgl_state.h declaration, kept local like TextureAnimator.cpp does.
namespace dkgl {
void set_int_attrib_draws(bool on);
}
#include "common/dma/dma_chain_read.h"  // T125: DmaFollower/VifCode (texture animator)
#include "game/graphics/opengl_renderer/TextureAnimator.h"  // T125: handle_texture_anim_data
#include "game/graphics/deko3d_renderer/EyeDk.h"           // T82: render-to-texture eyes
#include "game/graphics/deko3d_renderer/dk_gl_census.h"   // T49: no-op glad stubs + census
#include "game/graphics/opengl_renderer/loader/Loader.h"   // T49: opt-in dk-path Loader
#include "game/graphics/texture/TexturePool.h"             // T49
#include "game/kernel/common/kboot.h"
#include "game/kernel/common/kmachine.h"  // T79: offset_of_s7 for texture uploads
#include "game/runtime.h"
#include "game/switch/platform.h"
#include "game/switch/run_log.h"
#include "game/switch/sdl3_compat.h"
#include "game/system/hid/display_manager.h"
#include "game/system/hid/input_manager.h"

namespace {

// T83 (AI-assisted): frame-time split, render thread only. Hardware tick counter (same
// clock as dk_device present_frame); no hooks on the EE (GOAL) side.
inline u64 dk_tick() {
  return dkdev::tick_now();
}
inline double dk_ms(u64 a, u64 b) {
  return dkdev::tick_ms(a, b);
}

// §7.0 I (AI-assisted): where the `capture` ms of the split line go (jak2 path). `walk` is
// the SpriteDk tex-bucket walk minus the parts that run inside it (upload..distort).
enum CapPart {
  kCapDrain,
  kCapWorld,
  kCapUpload,
  kCapDkgl,
  kCapTexAnim,
  kCapGlow,
  kCapDistort,
  kCapWalk,
  kCapEnd,
  kCapCount
};
const char* const kCapName[kCapCount] = {"drain", "world",   "upload", "dkgl", "texanim",
                                         "glow",  "distort", "walk",   "end"};
double s_cap_ms[kCapCount] = {};
struct CapLap {
  int part;
  u64 t0;
  explicit CapLap(int p) : part(p), t0(dk_tick()) {}
  ~CapLap() { s_cap_ms[part] += dk_ms(t0, dk_tick()); }
};

struct DkSyncData {
  std::mutex sync_mutex;
  std::condition_variable sync_cv;
  std::mutex dma_mutex;
  std::condition_variable dma_cv;
  u64 frame_idx = 0;
  u64 frame_idx_of_input_data = 0;
  bool has_data_to_render = false;
  u64 chains_received = 0;
  // T60: the last chain from dk_send_chain. Only valid until has_data_to_render is cleared.
  const void* chain_data = nullptr;
  u32 chain_offset = 0;
};
std::unique_ptr<DkSyncData> g_dk;

int dk_init(GfxGlobalSettings& /*settings*/) {
  // Gamepads only. SDL's Switch video driver would claim the default native window for
  // EGL, which deko3d needs -- so SDL_INIT_VIDEO is never called on this path (plan P1.2).
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!SDL_Init(SDL_INIT_EVENTS)) {
    switch_run_logf("[dk] SDL_Init(EVENTS) failed: %s", SDL_GetError());
  }
  g_dk = std::make_unique<DkSyncData>();
  return 0;
}

// T49 (AI-assisted, P2.3 bring-up): dk-path Loader (jak1/jak2: always on with the
// deko3d default; jak3: still opt-in via gk_dkload.txt on top of gk_dk3d.txt). The Loader still
// runs its GL staging code; dk_gl_census stubs every glad pointer (no context here) so
// those calls are no-ops, and the run log names each GL function the Loader reaches —
// the exact list of seams still to write. Level textures go to dktexgpu via the T43
// seam. Created/updated on the render thread only (T47 owner-thread rule).
constexpr int kDkFr3LevelCount[] = {jak1::LEVEL_TOTAL, jak2::LEVEL_TOTAL, jak3::LEVEL_TOTAL,
                                    jakx::LEVEL_TOTAL};
std::unique_ptr<TexturePool> s_dk_tex_pool;
std::unique_ptr<Loader> s_dk_loader;
std::atomic<Loader*> s_dk_loader_ptr{nullptr};  // read by the EE thread (set_levels)
std::atomic<float> s_dk_pmode_alp{1.f};  // GOAL set_pmode_alp (EE thread), read per frame

// TODO #12 (AI-assisted): jak1 and jak2 run on deko3d by default (jak2 since 2026-11-13,
// after the dkgl rows A-I passed on HW), so every opt-in pass file counts as present for
// them and there is no OpenGL opt-out (gfx.cpp). jak3 keeps the per-pass opt-in files
// until it renders right.
bool dk_pass_on(GameVersion version, const char* file) {
  if (version == GameVersion::Jak1 || version == GameVersion::Jak2) {
    return true;
  }
  char path[96];
  snprintf(path, sizeof(path), "sdmc:/switch/%s/%s", game_version_names[version], file);
  return access(path, F_OK) == 0;
}

bool dkload_present() {
  return dk_pass_on(g_game_version, "gk_dkload.txt");
}

// T60 (AI-assisted): OPT-IN P3 tfrag world pass, only with sdmc:/switch/jak1/gk_dktfrag.txt
// on top of gk_dk3d.txt + gk_dkload.txt. jak1 only (bucket ids are jak1's).
bool s_dk_tfrag = false;
// §7.2 phase 1 (AI-assisted, DKGL_TODO #14): jak2 world through dkgl capture only -- no
// native passes yet (their bucket tables are jak1's). Set when dkgl::init succeeds.
bool s_dk_jak2_dkgl = false;
// §7.2 phase 1 (AI-assisted): the native tfrag/tie/shrub passes inside record_jak2_dkgl,
// split at their GL bucket positions. Debug off-switch gk_dkgl_noworld.txt (the world
// then stays dkgl-only = black where the natives own buckets; useful to bisect a pass).
bool s_dk_jak2_world = false;
// §7.2 particles (AI-assisted): SpriteDk reads + draws jak2 bucket 313 (GL "particles"
// Sprite3, glow not ported). Opt-out: gk_nosprite.txt (same file as jak1).
bool s_dk_jak2_sprite = false;
// J4 (AI-assisted, jak3 bringup -- JAK3_BRINGUP_TODO J4): the jak3 twins of the three jak2
// flags above. dkgl capture ON (init_jak3, J2); native tfrag/tie/shrub/hfrag world passes
// split into record_jak3_dkgl; SpriteDk particles on bucket 570 (G1: native, like jak2).
bool s_dk_jak3_dkgl = false;
bool s_dk_jak3_world = false;
bool s_dk_jak3_sprite = false;
// J4: the gen-2 dkgl path is two games now. These pick the ACTIVE game's flags so the
// capture / build / report sites below stay game-agnostic; the per-game parts (bucket
// tables, hfrag) branch on g_game_version inside those blocks.
bool dk_gen2_dkgl() {
  return s_dk_jak2_dkgl || s_dk_jak3_dkgl;
}
bool dk_gen2_world() {
  return (s_dk_jak2_dkgl && s_dk_jak2_world) || (s_dk_jak3_dkgl && s_dk_jak3_world);
}
bool dk_gen2_sprite() {
  return (s_dk_jak2_dkgl && s_dk_jak2_sprite) || (s_dk_jak3_dkgl && s_dk_jak3_sprite);
}
// T72 (AI-assisted): OPT-IN shrub pass, gk_dkshrub.txt on top of gk_dktfrag.txt (jak1 only).
bool s_dk_shrub = false;
// T76 (AI-assisted): OPT-IN tie pass, gk_dktie.txt on top of gk_dktfrag.txt (jak1 only).
bool s_dk_tie = false;
// T113/FIX 117 (AI-assisted): the GL Hfrag renderer is a JAK3 renderer (jak3's HFRAG
// bucket is 8). Jak1's bucket 8 is TIE_NEAR_LEVEL0, which the game leaves empty (GL
// registers nothing there either) and no jak1 fr3 level carries hfrag data -- the T113
// port could never draw on jak1. Off until the jak3 port flips this in its own init.
bool s_dk_hfrag = false;
// T78 (AI-assisted): merc2 (characters) rides on the tie flag (no extra SD file);
// gk_nomerc.txt turns it off again.
bool s_dk_merc = false;
// T79 (AI-assisted): sprite3 also rides on the tie flag; gk_nosprite.txt turns it off.
bool s_dk_sprite = false;
// T80 (AI-assisted): direct buckets (menus, HUD text, subtitles) ride on the sprite flag;
// gk_nodirect.txt turns them off.
bool s_dk_direct = false;
// T88 (AI-assisted): OPT-IN jak1 sky pass (GL SkyRenderer bucket 3 + SkyBlendHandler
// buckets 32/39) riding on the direct flag; gk_nosky.txt turns it off again.
bool s_dk_sky = false;
// T95 (AI-assisted): OPT-IN jak1 ocean pass (GL ocean-mid-far bucket 4 +
// ocean-near bucket 63) on top of tfrag; gk_noocean.txt turns it off again.
bool s_dk_ocean = false;
void dk_drain_tex_ops();  // T80: queued GOAL upload-now / relocate, defined below

// T79: TextureUploadHandler::flush_uploads on the dk pool (render thread, chain time).
const u8* s_dk_upload_base = nullptr;
void dk_sprite_upload(uint32_t page, int64_t mode) {
  CapLap lap(kCapUpload);
  if (s_dk_tex_pool && s_dk_upload_base && page < (uint32_t)EE_MAIN_MEM_SIZE) {
    s_dk_tex_pool->handle_upload_now(s_dk_upload_base + page, (int)mode, s_dk_upload_base,
                                     offset_of_s7(), false);
  }
}
// T79: TexturePool::lookup -> dktexgpu id; 0 when empty or still a placeholder.
uint32_t dk_sprite_lookup(uint32_t tbp) {
  if (!s_dk_tex_pool) {
    return 0;
  }
  GpuTexture* src = s_dk_tex_pool->lookup_gpu_texture(tbp);
  if (src == nullptr || src->is_placeholder) {
    // AI-assisted (white-box probe, DKGL_TODO): a missed batch is SKIPPED by SpriteDk
    // (no_tex counter), so a miss cannot draw anything itself -- but naming each miss
    // tells us which page the game asked for at that moment. "placeholder" = a named
    // texture whose data had not landed yet (the punch-spark suspect).
    static uint32_t s_miss_logged = 0;
    if (s_miss_logged < 40 || (s_miss_logged % 600) == 0) {
      // FIX 123 (AI-assisted): pc= is the PcTextureId the slot was linked to by
      // handle_upload_now (runtime tpage id / runtime tex index). Compare with the
      // fr3 combo ids: gungame gen-02 is 2405/7, meta-01 2405/13; hiphog neon-on
      // 998/64; baron-neon pages 1021/xx; ctyport signs 1325/xx. A mismatch here
      // means the runtime tpage draws with a different id/index than the
      // extraction assigned, so the loader's give never fills this entry.
      switch_run_logf("[sprtex] MISS tbp=%u pc=%u/%u %s", tbp,
                      src ? (unsigned)src->tex_id.page : 0, src ? (unsigned)src->tex_id.tex : 0,
                      src ? s_dk_tex_pool->get_debug_texture_name(src->tex_id).c_str() : "no-page");
    }
    s_miss_logged++;
    return 0;
  }
  auto id = s_dk_tex_pool->lookup(tbp);
  // AI-assisted probe (turret-hit green squares): log each new tbp -> texture pairing once so
  // the run log names the texture a sprite asked for and the id it got.
  static std::unordered_set<uint64_t> s_seen;
  const uint64_t key = ((uint64_t)tbp << 32) | (id ? (uint32_t)*id : 0u);
  if (s_seen.size() < 3000 && s_seen.insert(key).second) {
    switch_run_logf("[sprtex] tbp=%u id=%u slot=%u %ux%u common=%d %s", tbp,
                    id ? (unsigned)*id : 0u, id ? dktexgpu::slot_of((uint32_t)*id) : 0u,
                    (unsigned)src->w, (unsigned)src->h, (int)src->is_common,
                    s_dk_tex_pool->get_debug_texture_name(src->tex_id).c_str());
  }
  return id ? (uint32_t)*id : 0;
}

// T80: GL DirectRenderer::update_gl_texture lookup (mt4hh for the font); 0 = missing.
uint32_t dk_direct_lookup(uint32_t tbp, bool mt4hh) {
  if (!s_dk_tex_pool) {
    return 0;
  }
  std::optional<u64> id;
  if (mt4hh) {
    id = s_dk_tex_pool->lookup_mt4hh(tbp);
  } else {
    GpuTexture* src = s_dk_tex_pool->lookup_gpu_texture(tbp);
    if (src == nullptr || src->is_placeholder) {
      return 0;
    }
    id = s_dk_tex_pool->lookup(tbp);
  }
  return id ? (uint32_t)*id : 0;
}

// T125 (AI-assisted): PC_PORT imm-12 dispatch. GL TextureUploadHandler::render hands the
// following transfers to the (UNCHANGED, GL) TextureAnimator; here it runs through dkgl,
// exactly like the ocean/Merc2 renderers (AI_ROLES.md wrapper rule). Its FBO composites
// record into dkgl's kTexAnimList segments at capture time; record_jak2_dkgl calls them
// right before the native sprite bucket 313 draws, which samples the textures the animator
// published into the pool (update_gl_texture -> resolve_texture_id -> dktexgpu id).
// ee_mem = the chain/EE snapshot base, same base the uploads use (GL: ee_main_memory).
// jak2 PcTextureAnimCodesJak2::FINISH_ARRAY = 13 (TextureAnimator.cpp).
void dk_skip_texture_anim(DmaFollower& dma) {
  for (int guard = 0; guard < 100000; guard++) {
    auto t = dma.read_and_advance();
    if (t.vifcode0().kind == VifCode::Kind::PC_PORT && t.vifcode0().immediate == 13) {
      return;
    }
  }
}
void dk_texture_anim(DmaFollower& dma, const void* ee_mem) {
  CapLap lap(kCapTexAnim);
  if (dkgl::tex_anim_begin()) {
    // FIX 121 (AI-assisted): the jak2 animator composites -- the DMA DRAW path, the fixed
    // anim arrays, clouds and slime -- all draw their quads with GL_INT vertex attribs,
    // which dkgl skips unless allow-listed (dkgl_draw.cpp "outside the allow-list"). The
    // missing allow-list HERE is why the composites used to come out empty and publishing
    // was banned in TextureAnimator.cpp (black tickers / Krew sign / propaganda logo /
    // animated damage textures ever since). The skull gem and clouds have run this exact
    // way on hardware since f97fd4013; now the whole animator run is allow-listed, and the
    // publish loop + fixed arrays are re-enabled behind the gk_dkgl_nodmatex.txt /
    // gk_dkgl_noanim.txt kill switches there.
    dkgl::set_int_attrib_draws(true);
    dkgl::texture_animator()->handle_texture_anim_data(dma, (const u8*)ee_mem,
                                                       s_dk_tex_pool.get(), g_dk->frame_idx);
    dkgl::set_int_attrib_draws(false);
    dkgl::tex_anim_end();
  } else {
    dk_skip_texture_anim(dma);  // consume to FINISH_ARRAY; later uploads continue cleanly
  }
}

// T126 (AI-assisted): jak2 glow section (after the HUD sprites in bucket 313) -> the
// unchanged GL GlowRenderer through dkgl. Without a capture it is skipped (no glow drawn).
void dk_glow(DmaFollower& dma) {
  CapLap lap(kCapGlow);
  if (dkgl::glow_begin()) {
    dkgl::glow_dma(dma);
    dkgl::glow_end();
  }
}

// T127 (AI-assisted): jak2 distort section (at the START of bucket 313, before the sprites)
// -> the GL Sprite3 distort walk + draw through dkgl (scattergun heat haze). Without a
// capture distort_dma still consumes the section, so the walk continues cleanly.
void dk_distort(DmaFollower& dma) {
  CapLap lap(kCapDistort);
  if (dkgl::distort_begin()) {
    dkgl::distort_dma(dma);
    dkgl::distort_end();
  } else {
    dkgl::distort_dma(dma);  // walk-only (no segment open)
  }
}

// T88 (AI-assisted): GL SkyBlendCPU::init_textures for the dk path. The two blended
// outputs (32x32 sky, 64x64 clouds) are dktexgpu textures registered in the pool at the
// fixed VRAM slots the sky-draw bucket (3) samples, SKY_TEXTURE_VRAM_ADDRS; the pool's
// `gpu_texture` field holds the dktexgpu id, exactly like every other dk texture.
// Allocated once and never released (P8: image memory is never reclaimed anyway).
GpuTexture* s_sky_gpu[2] = {nullptr, nullptr};
uint32_t s_sky_tex_id[2] = {0, 0};
uint32_t s_sky_store_fails = 0;

void dk_sky_init_textures() {
  static const uint8_t zeros[4 * 64 * 64] = {0};
  if (!s_dk_tex_pool) {
    return;
  }
  for (int i = 0; i < 2; i++) {
    const int size = (i == 0) ? 32 : 64;  // GL SkyBlendCPU m_sizes
    s_sky_tex_id[i] = dktexgpu::upload_rgba8(size, size, zeros);
    if (s_sky_tex_id[i] == 0) {
      switch_run_logf("[dk] sky tex %d alloc failed (store full / memblock)", i);
      continue;
    }
    TextureInput in;
    in.gpu_texture = s_sky_tex_id[i];
    in.w = size;
    in.h = size;
    in.debug_name = (i == 0) ? "PC-SKY-CPU-0" : "PC-SKY-CPU-1";
    in.id = s_dk_tex_pool->allocate_pc_port_texture(g_game_version);
    s_sky_gpu[i] = s_dk_tex_pool->give_texture_and_load_to_vram(in, SKY_TEXTURE_VRAM_ADDRS[i]);
  }
  switch_run_logf("[dk] sky textures at tbp %d/%d: ids %u %u", SKY_TEXTURE_VRAM_ADDRS[0],
                  SKY_TEXTURE_VRAM_ADDRS[1], s_sky_tex_id[0], s_sky_tex_id[1]);
}

// T88: SourceFn -- GL do_sky_blends' pool lookup. RGBA level textures keep a CPU pixel
// pointer in the pool (LoaderStages sets TextureInput::src_data for exactly this reader);
// BCn entries have none and count as no_src in the blend stats.
dksky::SkySource dk_sky_source(uint32_t tbp) {
  dksky::SkySource out;
  if (!s_dk_tex_pool) {
    return out;
  }
  GpuTexture* tex = s_dk_tex_pool->lookup_gpu_texture(tbp);
  if (tex == nullptr) {
    return out;
  }
  if (tex->is_placeholder) {
    // FIX 117: distinguish "not uploaded yet" from "placeholder" in the sky stats.
    out.placeholder = true;
    return out;
  }
  out.data = tex->get_data_ptr();
  out.bytes = tex->data_size();
  return out;
}

// T88: SinkFn -- push one blended buffer to its texture. Build time, right after
// begin_deferred_frame(): the copy is staged now and recorded by
// record_deferred_updates() at the top of the world recorder, before the sky draws
// sample the texture (same T86 path as the tod palettes).
void dk_sky_store(int index, const uint8_t* rgba, uint32_t /*size*/) {
  if (index < 0 || index > 1 || s_sky_tex_id[index] == 0) {
    return;
  }
  const int size = (index == 0) ? 32 : 64;
  if (!dktexgpu::update_rgba8_deferred(s_sky_tex_id[index], size, size, rgba)) {
    s_sky_store_fails++;
  }
  // FIX 112: GL SkyBlendCPU re-pins after every upload; level loads can take the slot.
  if (s_dk_tex_pool && s_sky_gpu[index]) {
    s_dk_tex_pool->move_existing_to_vram(s_sky_gpu[index], SKY_TEXTURE_VRAM_ADDRS[index]);
  }
}

// FIX 112: dksprite::BucketHookFn. Called before each replayed texture-upload bucket; runs
// a level's sky blend once the replay has passed its bucket (32 / 39), i.e. with the VRAM
// state GL's SkyBlendHandler sees. Later uploads (pris, water...) reuse those addresses.
const void* s_sky_hook_data = nullptr;
uint32_t s_sky_hook_offset = 0;
int s_sky_hook_next = 0;  // next sky level to blend this frame (0, 1, 2 = done)
void dk_sky_bucket_hook(int bucket) {
  while (s_sky_hook_data && s_sky_hook_next < 2 &&
         bucket > dksky::kBlendBucket[s_sky_hook_next]) {
    dksky::capture_blend_level(s_sky_hook_data, s_sky_hook_offset, dk_sky_source,
                               s_sky_hook_next);
    s_sky_hook_next++;
  }
}

// #16/#19 (AI-assisted): GL runs every bucket in order, so a renderer sees VRAM as the
// uploads before it left it. Here the upload replay walks the tex buckets, and the dkgl
// buckets (eyes 54 = iris/pupil/lid by tbp, Merc2, generic, ...) run between them, before
// the next page replaces their textures.
bool s_dkgl_interleave = false;
void dk_upload_bucket_hook(int bucket) {
  dk_sky_bucket_hook(bucket);
  if (s_dkgl_interleave) {
    CapLap lap(kCapDkgl);
    dkgl::capture_until(bucket);
  }
}

// T84: pass order for the per-pass timing (GPU marks 1..11, CPU build slots). Must match the
// record order. #17 (AI-assisted): the dkgl passes after merc get their own marks
// (glmerc = Merc2/emerc buckets, alpha = generic + shadow 46..62, oceann = ocean-near 63,
// dcue = depth-cue 64) so "sprite" is only the sprites.
enum DkPass {
  kPTfrag,
  kPTie,
  kPShrub,
  kPEye,
  kPMerc,
  kPGlMerc,
  kPAlpha,
  kPOceanNear,
  kPDepthCue,
  kPSprite,
  kPDirect,
  kPassCount
};
const char* const kPassName[kPassCount] = {"tfrag", "tie",  "shrub",  "eye",  "merc",  "glmerc",
                                           "alpha", "oceann", "dcue", "sprite", "direct"};
static_assert(1 + kPassCount < dkdev::kGpuMarks, "pass marks must stay below the end mark");

void record_tfrag_and_shrub(int fb_w, int fb_h) {
  dktexgpu::record_deferred_updates();  // T86: tod palette copies + T88 sky uploads first
  if (s_dk_sky) {
    dkdirect::record_sky_world(fb_w, fb_h);  // T88: GL bucket order -- sky 3 before tfrag 6
  }
  if (s_dk_ocean) {
    // T95: GL bucket order -- ocean-mid-far 4 after sky 3, before tfrag 6
    // (texture gen -> far draws -> mid body flush).
    if (dkgl::active()) {
      dkgl::set_frame_size(fb_w, fb_h);
      // T107: the GL ocean, recorded at capture time. The far ocean (GL DirectRenderer,
      // direct_basic_textured) runs through dkgl too (AI-assisted).
      dkgl::record_bucket(4);
    } else {
      dkocean::record_mid_far(fb_w, fb_h);
    }
  }
  dktf::record_world(fb_w, fb_h);
  dkdev::gpu_mark(1 + kPTfrag);
  if (s_dk_hfrag) {
    dkhfrag::record_world(fb_w, fb_h);  // T113: GL bucket order -- hfrag 8 after tfrag 6/7
  }
  if (s_dk_tie) {
    dktie::record_world(fb_w, fb_h);  // GL bucket order: tfrag 6, tie 9, shrub 20
    dkdev::gpu_mark(1 + kPTie);
  }
  if (s_dk_shrub) {
    dkshrub::record_world(fb_w, fb_h);
    dkdev::gpu_mark(1 + kPShrub);
  }
  if (dkgl::active()) {
    // T111: Generic2 tfrag/shrub buckets (11 18 24 30) after the opaque native world
    for (int b : {11, 18, 24, 30}) {
      dkgl::record_bucket(b);
    }
  }
  if (dkgl::eyes_active()) {
    // #19 (AI-assisted): GL EyeRenderer (bucket 54) before every merc bucket; record_bucket
    // adds the render-to-texture barrier Merc2 needs to sample the eye textures.
    // 48 / 51 = the level NPC eyes, drawn inside the pris-tex buckets (dkgl PrisTexEyes).
    // They only write eye textures, so recording them here keeps them before every merc.
    for (int b : {48, 51, 54}) {
      dkgl::record_bucket(b);
    }
    dkdev::gpu_mark(1 + kPEye);
  } else if (s_dk_merc && s_dk_direct) {
    dkeye::record_eyes();  // T82: eye textures first, merc samples them (rebinds the frame target)
    dkdev::gpu_mark(1 + kPEye);
  }
  if (s_dk_merc) {
    if (!dkgl::merc_active()) {
      dkmerc::record_world(fb_w, fb_h);  // GL: merc buckets 10+ come after the opaque world
    }
    dkdev::gpu_mark(1 + kPMerc);
    if (dkgl::active()) {
      // TODO #5 / #19 (AI-assisted): the GL Merc2 through dkgl -- characters (blerc faces,
      // fade) + emerc shine, or emerc only on top of the native merc (gk_dkgl_nativemerc.txt).
      // Only the merc buckets before the shadow (47) here; the rest follow below.
      for (int b : {10, 17, 45}) {
        dkgl::record_bucket(b);
      }
      dkdev::gpu_mark(1 + kPGlMerc);
    }
  }
  if (dkgl::active()) {
    // T111/T113: Generic2 alpha / pris / water (46 50 53 56 59 62) and the shadow (47) in GL
    // bucket order. #16 (AI-assisted): the pris / water merc buckets (49 52 55 58 61, Jak and
    // Daxter are 55) come AFTER the shadow, as in GL -- recorded before it, the shadow's
    // darkening quad landed on the characters.
    for (int b : {46, 47, 49, 50, 52, 53, 55, 56, 58, 59, 61, 62}) {
      const bool merc = b == 49 || b == 52 || b == 55 || b == 58 || b == 61;
      if (!merc || s_dk_merc) {
        dkgl::record_bucket(b);
      }
    }
    dkdev::gpu_mark(1 + kPAlpha);
  }
  if (s_dk_ocean) {
    // T95: GL bucket order -- ocean-near 63 after the water buckets, before
    // depth-cue/sprite 66.
    if (dkgl::active()) {
      dkgl::record_bucket(63);  // T107
    } else {
      dkocean::record_near(fb_w, fb_h);
    }
    dkdev::gpu_mark(1 + kPOceanNear);
  }
  if (dkgl::active()) {
    dkgl::record_bucket(64);  // T112: depth-cue blurs the finished world, before sprite 66
    dkdev::gpu_mark(1 + kPDepthCue);
  }
  if (s_dk_sprite) {
    dksprite::record_world(fb_w, fb_h);  // GL: sprite bucket 66 after merc/water
    dkdev::gpu_mark(1 + kPSprite);
  }
  if (s_dk_direct) {
    dkdirect::record_world(fb_w, fb_h);  // GL: debug 67, debug-no-zbuf 68, subtitle 69
    dkdev::gpu_mark(1 + kPDirect);
  }
  if (dkgl::active()) {
    // GL do_pcrtc_effects: the pmode_alp fade to black goes over everything, HUD included
    dkgl::record_blackout();
  }
  dkgl::record_frame_fence();  // J7: the frame's single slot-fence signal
}

// §7.2 phase 1 (AI-assisted): where the native world passes slot into the dkgl bucket
// order. Bucket ids ascending = GL dispatch order (dispatch_buckets_jak2/jak3 walk ids),
// with GL's GET_BUCKET_ID_FOR_LIST stride arithmetic so they cannot drift from
// init_bucket_renderers_jak2/jak3. module: 0 = dktf, 1 = dkshrub, 2 = dktie, 3 = dkhfrag
// (J4); range = the module's record_pass range (tfrag: pass index of kJak2Passes /
// kJak3Passes; tie: ranges_per_pass()*pass + category; shrub: pass index; hfrag: 0).
struct Gen2NativeSplit {
  int bucket;
  uint8_t module;
  uint8_t range;
};
constexpr int jak2_split_lvl(jak2::BucketId a, jak2::BucketId b, int i) {
  return (int)a + ((int)b - (int)a) * i;
}
constexpr Gen2NativeSplit kJak2NativeSplits[] = {
    // tfrag group: TFRAG_L{n} (native tfrag NORMAL), TIE_L{n} (tie normal + wind),
    // ETIE_L{n} (tie envmap) -- all before MERC_L{n}_TFRAG (dkgl merc2).
    {jak2_split_lvl(jak2::BucketId::TFRAG_L0_TFRAG, jak2::BucketId::TFRAG_L1_TFRAG, 0), 0, 0},
    {jak2_split_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 0), 2, 0},
    {jak2_split_lvl(jak2::BucketId::ETIE_L0_TFRAG, jak2::BucketId::ETIE_L1_TFRAG, 0), 2, 1},
    {jak2_split_lvl(jak2::BucketId::TFRAG_L0_TFRAG, jak2::BucketId::TFRAG_L1_TFRAG, 1), 0, 1},
    {jak2_split_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 1), 2, 2},
    {jak2_split_lvl(jak2::BucketId::ETIE_L0_TFRAG, jak2::BucketId::ETIE_L1_TFRAG, 1), 2, 3},
    {jak2_split_lvl(jak2::BucketId::TFRAG_L0_TFRAG, jak2::BucketId::TFRAG_L1_TFRAG, 2), 0, 2},
    {jak2_split_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 2), 2, 4},
    {jak2_split_lvl(jak2::BucketId::ETIE_L0_TFRAG, jak2::BucketId::ETIE_L1_TFRAG, 2), 2, 5},
    {jak2_split_lvl(jak2::BucketId::TFRAG_L0_TFRAG, jak2::BucketId::TFRAG_L1_TFRAG, 3), 0, 3},
    {jak2_split_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 3), 2, 6},
    {jak2_split_lvl(jak2::BucketId::ETIE_L0_TFRAG, jak2::BucketId::ETIE_L1_TFRAG, 3), 2, 7},
    {jak2_split_lvl(jak2::BucketId::TFRAG_L0_TFRAG, jak2::BucketId::TFRAG_L1_TFRAG, 4), 0, 4},
    {jak2_split_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 4), 2, 8},
    {jak2_split_lvl(jak2::BucketId::ETIE_L0_TFRAG, jak2::BucketId::ETIE_L1_TFRAG, 4), 2, 9},
    {jak2_split_lvl(jak2::BucketId::TFRAG_L0_TFRAG, jak2::BucketId::TFRAG_L1_TFRAG, 5), 0, 5},
    {jak2_split_lvl(jak2::BucketId::TIE_L0_TFRAG, jak2::BucketId::TIE_L1_TFRAG, 5), 2, 10},
    {jak2_split_lvl(jak2::BucketId::ETIE_L0_TFRAG, jak2::BucketId::ETIE_L1_TFRAG, 5), 2, 11},
    // shrub group: SHRUB_L{n} before MERC_L{n}_SHRUB (dkgl).
    {jak2_split_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 0), 1, 0},
    {jak2_split_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 1), 1, 1},
    {jak2_split_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 2), 1, 2},
    {jak2_split_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 3), 1, 3},
    {jak2_split_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 4), 1, 4},
    {jak2_split_lvl(jak2::BucketId::SHRUB_L0_SHRUB, jak2::BucketId::SHRUB_L1_SHRUB, 5), 1, 5},
    // alpha group: TFRAG_T_L{n} (tfrag TRANS trees) before MERC_L{n}_ALPHA (dkgl).
    {jak2_split_lvl(jak2::BucketId::TFRAG_T_L0_ALPHA, jak2::BucketId::TFRAG_T_L1_ALPHA, 0), 0, 6},
    {jak2_split_lvl(jak2::BucketId::TFRAG_T_L0_ALPHA, jak2::BucketId::TFRAG_T_L1_ALPHA, 1), 0, 7},
    {jak2_split_lvl(jak2::BucketId::TFRAG_T_L0_ALPHA, jak2::BucketId::TFRAG_T_L1_ALPHA, 2), 0, 8},
    {jak2_split_lvl(jak2::BucketId::TFRAG_T_L0_ALPHA, jak2::BucketId::TFRAG_T_L1_ALPHA, 3), 0, 9},
    {jak2_split_lvl(jak2::BucketId::TFRAG_T_L0_ALPHA, jak2::BucketId::TFRAG_T_L1_ALPHA, 4), 0, 10},
    {jak2_split_lvl(jak2::BucketId::TFRAG_T_L0_ALPHA, jak2::BucketId::TFRAG_T_L1_ALPHA, 5), 0, 11},
};
constexpr int kJak2SplitCount =
    (int)(sizeof(kJak2NativeSplits) / sizeof(kJak2NativeSplits[0]));

// J4 (AI-assisted, jak3 bringup -- docs/deko3d/jak3-bucket-census.md §2/§3): the jak3
// native splits, 91 entries in ascending bucket order (= GL dispatch order):
//   HFRAG 8 (dkhfrag, J3) · per level n=0..9 the TFRAG_L{n}_TFRAG + TIE/ETIE_L{n}_TFRAG
//   trio (tfrag NORMAL range n, tie ranges 6n+0/6n+1) · SHRUB_L{n}_SHRUB (range n) ·
//   per level the TFRAG_L{n}_ALPHA + TIE/ETIE_L{n}_ALPHA trio (tfrag TRANS range 10+n,
//   tie 6n+2/6n+3) · per level TIE/ETIE_L{n}_WATER (tie 6n+4/6n+5). The water TFRAG
//   buckets stay dkgl-captured (init_jak3, like jak2's tfrag-w), and PARTICLES 570 is
//   driven by the record_sprite mechanism below, not a split. jak3's enum drops jak2's
//   _T_/_W_ prefixes (TFRAG_L{n}_ALPHA, not TFRAG_T_L{n}_ALPHA).
constexpr int jak3_split_lvl(jak3::BucketId a, jak3::BucketId b, int i) {
  return (int)a + ((int)b - (int)a) * i;
}
constexpr int kJak3SplitCount = 91;  // 1 hfrag + 3*10 tfrag group + 10 shrub + 3*10 alpha + 2*10 water
constexpr std::array<Gen2NativeSplit, kJak3SplitCount> build_jak3_splits() {
  std::array<Gen2NativeSplit, kJak3SplitCount> t{};
  int k = 0;
  auto put = [&t, &k](int bucket, int module, int range) {
    t[k++] = {bucket, (uint8_t)module, (uint8_t)range};
  };
  put((int)jak3::BucketId::HFRAG, 3, 0);
  for (int n = 0; n < 10; n++) {  // tfrag group (ids 11..121, before MERC_L{n}_TFRAG)
    put(jak3_split_lvl(jak3::BucketId::TFRAG_L0_TFRAG, jak3::BucketId::TFRAG_L1_TFRAG, n),
        0, n);
    put(jak3_split_lvl(jak3::BucketId::TIE_L0_TFRAG, jak3::BucketId::TIE_L1_TFRAG, n), 2,
        6 * n + 0);
    put(jak3_split_lvl(jak3::BucketId::ETIE_L0_TFRAG, jak3::BucketId::ETIE_L1_TFRAG, n), 2,
        6 * n + 1);
  }
  for (int n = 0; n < 10; n++) {  // shrub group (ids 131..221, before MERC_L{n}_SHRUB)
    put(jak3_split_lvl(jak3::BucketId::SHRUB_L0_SHRUB, jak3::BucketId::SHRUB_L1_SHRUB, n),
        1, n);
  }
  for (int n = 0; n < 10; n++) {  // alpha group (ids 231..332, before MERC_L{n}_ALPHA)
    put(jak3_split_lvl(jak3::BucketId::TFRAG_L0_ALPHA, jak3::BucketId::TFRAG_L1_ALPHA, n),
        0, 10 + n);
    put(jak3_split_lvl(jak3::BucketId::TIE_L0_ALPHA, jak3::BucketId::TIE_L1_ALPHA, n), 2,
        6 * n + 2);
    put(jak3_split_lvl(jak3::BucketId::ETIE_L0_ALPHA, jak3::BucketId::ETIE_L1_ALPHA, n), 2,
        6 * n + 3);
  }
  for (int n = 0; n < 10; n++) {  // water ties (ids 467..558, before MERC_L{n}_WATER)
    put(jak3_split_lvl(jak3::BucketId::TIE_L0_WATER, jak3::BucketId::TIE_L1_WATER, n), 2,
        6 * n + 4);
    put(jak3_split_lvl(jak3::BucketId::ETIE_L0_WATER, jak3::BucketId::ETIE_L1_WATER, n), 2,
        6 * n + 5);
  }
  return t;
}
constexpr std::array<Gen2NativeSplit, kJak3SplitCount> kJak3NativeSplits =
    build_jak3_splits();
// Layout sentinels (a stray put() above shifts these and fails the build):
static_assert(kJak3NativeSplits[0].bucket == 8 && kJak3NativeSplits[0].module == 3);
static_assert(kJak3NativeSplits[1].bucket == 11 && kJak3NativeSplits[1].module == 0 &&
              kJak3NativeSplits[1].range == 0);
static_assert(kJak3NativeSplits[3].bucket == 13 && kJak3NativeSplits[3].module == 2 &&
              kJak3NativeSplits[3].range == 1);
static_assert(kJak3NativeSplits[31].bucket == 131 && kJak3NativeSplits[31].module == 1);
static_assert(kJak3NativeSplits[41].bucket == 231 && kJak3NativeSplits[41].range == 10);
static_assert(kJak3NativeSplits[71].bucket == 467 && kJak3NativeSplits[71].range == 4);
static_assert(kJak3NativeSplits[90].bucket == 558 && kJak3NativeSplits[90].range == 59);

// Opt-in GPU profiler (AI-assisted, DKGL_TODO 7.2 #8): sdmc:/switch/jak2/gk_gpuprof.txt puts
// a timestamp before every unit record_jak2_dkgl records (each dkgl bucket, each native
// tfrag/shrub/tie range) and logs the most expensive units every report window. The unit
// order is fixed per frame, so unit k of the frame read back (N-2) is unit k of this one.
static bool s_gpu_prof = false;
static int s_prof_label[dkdev::kGpuProfMax];  // dkgl bucket id, or 10000 + module*100 + range
static int s_prof_units = 0;
static double s_prof_ms[dkdev::kGpuProfMax];
static int s_prof_frames = 0;
static int s_prof_over = 0;  // frames whose whole world pass took > 16.7 ms on the GPU

static void prof_unit(int& k, int label) {
  if (s_gpu_prof && k < dkdev::kGpuProfMax - 1) {
    s_prof_label[k] = label;
    dkdev::gpu_mark(dkdev::kGpuProfBase + k);
    k++;
  }
}

static void prof_accumulate() {
  if (!s_gpu_prof || s_prof_units == 0) {
    return;
  }
  const double end = dkdev::gpu_mark_ms(dkdev::kGpuMarks - 1);
  if (end < 0) {
    return;
  }
  for (int k = 0; k < s_prof_units; k++) {
    const double a = dkdev::gpu_mark_ms(dkdev::kGpuProfBase + k);
    const double b = dkdev::gpu_mark_ms(dkdev::kGpuProfBase + k + 1);
    if (a >= 0 && b >= a) {
      s_prof_ms[k] += b - a;
    }
  }
  s_prof_frames++;
  s_prof_over += end > 16.7 ? 1 : 0;
}

static void prof_report() {
  if (!s_gpu_prof || s_prof_frames == 0) {
    return;
  }
  int order[dkdev::kGpuProfMax];
  for (int k = 0; k < s_prof_units; k++) {
    order[k] = k;
  }
  const int top = std::min(12, s_prof_units);
  std::partial_sort(order, order + top, order + s_prof_units,
                    [](int x, int y) { return s_prof_ms[x] > s_prof_ms[y]; });
  char line[512];
  int pos = snprintf(line, sizeof(line), "[dk] gpu top ms/frame (%d frames, %d over 16.7) |",
                     s_prof_frames, s_prof_over);
  static const char* kMod[4] = {"tf", "sh", "tie", "hf"};  // J4: module 3 = hfrag
  for (int i = 0; i < top && pos < (int)sizeof(line) - 24; i++) {
    const int k = order[i];
    const int l = s_prof_label[k];
    const double ms = s_prof_ms[k] / s_prof_frames;
    if (l >= 10000) {
      pos += snprintf(line + pos, sizeof(line) - pos, " %s%d %.2f", kMod[((l - 10000) / 100) % 4],
                      (l - 10000) % 100, ms);
    } else {
      pos += snprintf(line + pos, sizeof(line) - pos, " b%d %.2f", l, ms);
    }
  }
  switch_run_logf("%s", line);
  for (double& m : s_prof_ms) {
    m = 0;
  }
  s_prof_frames = 0;
  s_prof_over = 0;
}

// §7.2 phase 1 (AI-assisted) / J4 (jak3): the gen-2 world record -- every dkgl bucket in
// GL dispatch order (dkgl's own sorted table) with the active game's native passes split
// in at their GL bucket positions (kJak2NativeSplits / kJak3NativeSplits), then the
// pmode_alp blackout. Shared by record_jak2_dkgl / record_jak3_dkgl; the sprite bucket
// wraps the texanim (T125) / distort (T127) / glow (T126) side lists exactly like GL
// Sprite3 draws them around its own bucket.
void record_gen2_dkgl(const Gen2NativeSplit* splits,
                      int split_count,
                      bool world,
                      bool sprite,
                      int sprite_bucket,
                      int fb_w,
                      int fb_h) {
  // AI-assisted: record the staged tod-palette copies (and sky uploads) into THIS frame, like
  // record_tfrag_and_shrub does. Without it begin_deferred_frame() pushed them with a blocking
  // submit every frame (3-5 ms of CPU wait on jak2).
  int unit = 0;
  // perf (AI-assisted): own profiler unit "b9999" for these copies, so their GPU time is not
  // billed to the first bucket (sky b5 showed a constant ~3 ms).
  prof_unit(unit, 9999);
  dktexgpu::record_deferred_updates();
  dkgl::set_frame_size(fb_w, fb_h);
  int next_split = 0;
  auto flush_natives = [&](int before_bucket) {
    // every native whose bucket id comes before the next dkgl bucket records first
    while (world && next_split < split_count && splits[next_split].bucket < before_bucket) {
      const Gen2NativeSplit& s = splits[next_split++];
      prof_unit(unit, 10000 + s.module * 100 + s.range);
      switch (s.module) {
        case 0:
          dktf::record_pass(s.range, fb_w, fb_h);
          break;
        case 1:
          dkshrub::record_pass(s.range, fb_w, fb_h);
          break;
        case 3:
          dkhfrag::record_world(fb_w, fb_h);  // J4: jak3 mountains (bucket 8, no range)
          break;
        default:
          dktie::record_pass(s.range, fb_w, fb_h);
          break;
      }
    }
  };
  // §7.2 particles: SpriteDk records at its GL slot (jak2 bucket 313, after ocean-near 310;
  // jak3 bucket 570, before shadow2 572). dkgl re-binds its fixed state per bucket.
  bool sprite_done = !sprite;
  auto record_sprite = [&]() {
    flush_natives(sprite_bucket);
    prof_unit(unit, sprite_bucket);
    dkgl::record_tex_anim();  // T125: gem composites exist before the sprite bucket samples them
    dkgl::record_distort();   // T127: GL draws the distorter before the sprites (it samples
                              // the frame as it is BEFORE the sprite draws)
    dksprite::record_world(fb_w, fb_h);
    dkgl::record_glow();  // T126: GL render_jak2/jak3 draws the glow right after the HUD sprites
    sprite_done = true;
  };
  for (int i = 0; i < dkgl::bucket_count(); ++i) {
    const int b = dkgl::bucket_at(i);
    if (!sprite_done && b > sprite_bucket) {
      record_sprite();
    }
    flush_natives(b);
    prof_unit(unit, b);
    dkgl::record_bucket(b);
  }
  if (!sprite_done) {
    record_sprite();
  }
  flush_natives(1 << 30);  // leftovers (none expected: all splits sit below screen-filter)
  if (s_gpu_prof && unit > 0) {
    dkdev::gpu_mark(dkdev::kGpuProfBase + unit);  // end of the last unit
  }
  s_prof_units = unit;
  dkgl::record_copy_back();  // FIX 138: GL blit_display/do_copy_back runs after every bucket
  dkgl::record_blackout();
  dkgl::record_frame_fence();  // J7: the frame's single slot-fence signal
}

void record_jak2_dkgl(int fb_w, int fb_h) {
  record_gen2_dkgl(kJak2NativeSplits, kJak2SplitCount, s_dk_jak2_world, s_dk_jak2_sprite,
                   dksprite::kJak2SpriteBucket, fb_w, fb_h);
}

// J4 (AI-assisted, jak3 bringup): the jak3 world record -- same walk as jak2 with the
// jak3 split table (hfrag 8 + per level tfrag/tie/shrub/alpha/water groups, census §2/§3)
// and the jak3 sprite bucket (PARTICLES 570).
void record_jak3_dkgl(int fb_w, int fb_h) {
  record_gen2_dkgl(kJak3NativeSplits.data(), kJak3SplitCount, s_dk_jak3_world,
                   s_dk_jak3_sprite, dksprite::kJak3SpriteBucket, fb_w, fb_h);
}

bool dktfrag_present() {
  return dk_pass_on(g_game_version, "gk_dktfrag.txt");
}

void dk_loader_start() {
  dkglcensus::install();
  // T51 breadcrumbs: the T49 run froze silently inside this function.
  switch_run_logf("[dk] dk-loader step 1/3: TexturePool (placeholder upload)");
  s_dk_tex_pool = std::make_unique<TexturePool>(g_game_version);
  // FIX 124 + FIX 125 (AI-assisted): draw-stamp eviction guard (PS2 semantics,
  // see the TexturePool.h FIX 124/125 block): a page drawn within the idle
  // window keeps its GPU id while its level unloads, and the unload retries
  // once it goes idle. The window is kPoolDeferFrames (~60 s) -- 120 frames
  // was proven too short on hardware ([sprtex] MISS ~35 s after release).
  // gk_dkgl_nopooldefer.txt turns it off (old behavior).
  {
    char defer_path[96];
    snprintf(defer_path, sizeof(defer_path), "sdmc:/switch/%s/gk_dkgl_nopooldefer.txt",
             game_version_names[g_game_version]);
    const bool defer_on = access(defer_path, F_OK) != 0;
    s_dk_tex_pool->set_defer_frames(
        defer_on ? TexturePool::kPoolDeferFrames : 0);
    // FIX 131 (AI-assisted): slot-linked protection -- the last GPU copy of an
    // entry GOAL still has tbp/mt4hh links to is held while linked (PS2 VRAM
    // semantics) instead of being released after the draw-stamp idle window;
    // see the TexturePool.h FIX 131 block. Same kill switch as FIX 124/125.
    s_dk_tex_pool->set_slot_guard_frames(
        defer_on ? TexturePool::kSlotGuardFrames : 0);
    switch_run_logf("[dk] pool defer unload %s",
                    defer_on ? "ON (FIX124/125/131: 3600 frames, slot-linked held)"
                             : "OFF (gk_dkgl_nopooldefer.txt)");
  }
  const auto fr3 =
      file_util::get_jak_project_dir() / "out" / game_version_names[g_game_version] / "fr3";
  switch_run_logf("[dk] dk-loader step 2/3: Loader(%s)", fr3.string().c_str());
  s_dk_loader = std::make_unique<Loader>(fr3, kDkFr3LevelCount[(int)g_game_version],
                                         g_game_version);
  switch_run_logf("[dk] dk-loader step 3/3: load_common GAME.fr3");
  dkgl::set_common_level(&s_dk_loader->load_common(*s_dk_tex_pool, "GAME"));
  s_dk_loader_ptr.store(s_dk_loader.get());
  switch_run_logf("[dk] dk-loader up: common loaded, tex live=%u fail=%d",
                  dktexgpu::live_count(), dktexgpu::failed_allocs());
  if (g_game_version == GameVersion::Jak1 && dktfrag_present()) {
    s_dk_tfrag = true;
    dkdev::set_world_recorder(dktf::record_world);
    switch_run_logf("[dk] tfrag world pass ON (gk_dktfrag.txt)");
    // R1 (AI-assisted): one-shot replay dump for the host test. The file may hold the
    // number of tfrag frames to wait (default 300 = ~10 s at 30 fps).
    char path[96];
    snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_dkdump.txt",
             game_version_names[g_game_version]);
    if (FILE* f = fopen(path, "r")) {
      int frames = 300;
      if (fscanf(f, "%d", &frames) != 1 || frames < 0) {
        frames = 300;
      }
      fclose(f);
      char dir[64];
      snprintf(dir, sizeof(dir), "sdmc:/switch/%s", game_version_names[g_game_version]);
      dktf::arm_replay_dump(dir, frames);
    }
    if (dk_pass_on(g_game_version, "gk_dkshrub.txt")) {
      s_dk_shrub = true;
      dkdev::set_world_recorder(record_tfrag_and_shrub);
      switch_run_logf("[dk] shrub world pass ON (gk_dkshrub.txt)");
    }
    if (dk_pass_on(g_game_version, "gk_dktie.txt")) {
      s_dk_tie = true;
      dkdev::set_world_recorder(record_tfrag_and_shrub);
      switch_run_logf("[dk] tie world pass ON (gk_dktie.txt)");
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_nomerc.txt",
               game_version_names[g_game_version]);
      s_dk_merc = access(path, F_OK) != 0;
      switch_run_logf("[dk] merc pass %s", s_dk_merc ? "ON (with tie)" : "OFF (gk_nomerc.txt)");
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_nosprite.txt",
               game_version_names[g_game_version]);
      s_dk_sprite = access(path, F_OK) != 0;
      switch_run_logf("[dk] sprite pass %s",
                      s_dk_sprite ? "ON (with tie)" : "OFF (gk_nosprite.txt)");
      // #17 (AI-assisted): early-Z sprites (Geyser smash overdraw); gk_dk_spritelatez.txt = off.
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_dk_spritelatez.txt",
               game_version_names[g_game_version]);
      dksprite::set_early_z(access(path, F_OK) != 0);
      // #17 (AI-assisted): near-camera sprite fade (Geyser smash); gk_dk_nospritefade.txt = off.
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_dk_nospritefade.txt",
               game_version_names[g_game_version]);
      const bool sprite_fade = access(path, F_OK) != 0;
      dksprite::set_near_fade(sprite_fade ? 1.0f : 0.f);
      switch_run_logf("[dk] sprite near fade %s",
                      sprite_fade ? "ON" : "OFF (gk_dk_nospritefade.txt)");
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_nodirect.txt",
               game_version_names[g_game_version]);
      s_dk_direct = s_dk_sprite && g_game_version == GameVersion::Jak1 && access(path, F_OK) != 0;
      switch_run_logf("[dk] direct pass %s",
                      s_dk_direct ? "ON (with sprite)" : "OFF (gk_nodirect.txt / not jak1)");
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_nosky.txt",
               game_version_names[g_game_version]);
      s_dk_sky = s_dk_direct && access(path, F_OK) != 0;
      switch_run_logf("[dk] sky pass %s",
                      s_dk_sky ? "ON (with direct)" : "OFF (gk_nosky.txt / no direct)");
      if (s_dk_sky) {
        dk_sky_init_textures();  // T88: register the blend outputs at SKY_TEXTURE_VRAM_ADDRS
      }
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_noocean.txt",
               game_version_names[g_game_version]);
      s_dk_ocean = s_dk_tfrag && g_game_version == GameVersion::Jak1 &&
                   access(path, F_OK) != 0;
      switch_run_logf("[dk] ocean pass %s",
                      s_dk_ocean ? "ON (with tfrag)" : "OFF (gk_noocean.txt / not jak1)");
      if (s_dk_ocean && !dkgl::init(s_dk_tex_pool.get(), s_dk_loader.get())) {
        // T107: dkgl runs the unchanged GL ocean; this native path is the fallback
        // (gk_dkocean_native.txt, or any dkgl init failure -- logged as [dkgl]).
        // T95: ocean-texture render target in the pool at OCEAN_TEX_TBP_JAK1.
        dkocean::init_textures(s_dk_tex_pool.get(), g_game_version);
        dkocean::init_driver();
        // OCEAN_VISUAL_BUG (AI-assisted): gk_oceannomip.txt / gk_oceanlevels.txt.
        dkocean::init_debug_flags();
      }
    }
  } else if (g_game_version == GameVersion::Jak2 && dktfrag_present()) {
    // §7.2 phase 1 (AI-assisted, DKGL_TODO #14): jak2 boots through dkgl capture -- ocean,
    // Merc2, Generic2 (+ effects), sky-draw, screen-filter; shadow2 / warp stay inert until
    // phase 2. Opt-in like jak1: gk_dkload.txt + gk_dktfrag.txt in sdmc:/switch/jak2/.
    // The native world passes (tfrag/tie/shrub) now run too, interleaved into the dkgl
    // order (see record_jak2_dkgl): their jak2 bucket tables are picked here, BEFORE the
    // first capture. MercDk/DirectDk get the pick as well but stay unwired on jak2 (dkgl
    // merc2 / direct own those buckets, §7.1).
    dktf::set_game(g_game_version);
    dktie::set_game(g_game_version);
    dkshrub::set_game(g_game_version);
    dkmerc::set_game(g_game_version);
    dkdirect::set_game(g_game_version);
    s_dk_jak2_dkgl = dkgl::init(s_dk_tex_pool.get(), s_dk_loader.get());
    if (s_dk_jak2_dkgl) {
      char path[96];
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_dkgl_noworld.txt",
               game_version_names[g_game_version]);
      s_dk_jak2_world = access(path, F_OK) != 0;
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_gpuprof.txt",
               game_version_names[g_game_version]);
      s_gpu_prof = access(path, F_OK) == 0;
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_nosprite.txt",
               game_version_names[g_game_version]);
      s_dk_jak2_sprite = access(path, F_OK) != 0;  // record_world checks the shaders
      dkdev::set_world_recorder(record_jak2_dkgl);
      switch_run_logf("[dk] jak2 dkgl world ON (%d buckets, native world %s, gpu profiler %s, "
                      "particles %s)",
                      dkgl::bucket_count(),
                      s_dk_jak2_world ? "ON" : "OFF (gk_dkgl_noworld.txt)",
                      s_gpu_prof ? "ON" : "off",
                      s_dk_jak2_sprite ? "ON" : "OFF (gk_nosprite.txt)");
    }
  } else if (g_game_version == GameVersion::Jak3 && dktfrag_present()) {
    // J4 (AI-assisted, jak3 bringup -- JAK3_BRINGUP_TODO J4): jak3 boots through dkgl
    // capture like jak2 (init_jak3, J2 -- merc2/generic2/ocean/sky/shadow/warp/debug/HUD),
    // with the native world passes (tfrag/tie/shrub/hfrag) and particles (SpriteDk, G1)
    // split in at their GL bucket positions by record_jak3_dkgl. Opt-in like the jak2
    // boot was: gk_dk3d.txt + gk_dkload.txt + gk_dktfrag.txt in sdmc:/switch/jak3/.
    // The set_game picks install the jak3 tables (J3) BEFORE the first capture.
    // MercDk/DirectDk stay unwired on jak3 (dkgl merc2/direct own those buckets, §7.1);
    // their set_game keeps the default tables until they need jak3's own.
    dktf::set_game(g_game_version);
    dktie::set_game(g_game_version);
    dkshrub::set_game(g_game_version);
    dkhfrag::set_game(g_game_version);  // J3: bucket 8, no-CALL-head chain
    dksprite::set_game(g_game_version);  // J3: flag&0x30 corner order
    dkmerc::set_game(g_game_version);
    dkdirect::set_game(g_game_version);
    s_dk_jak3_dkgl = dkgl::init(s_dk_tex_pool.get(), s_dk_loader.get());
    if (s_dk_jak3_dkgl) {
      char path[96];
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_dkgl_noworld.txt",
               game_version_names[g_game_version]);
      s_dk_jak3_world = access(path, F_OK) != 0;
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_gpuprof.txt",
               game_version_names[g_game_version]);
      s_gpu_prof = access(path, F_OK) == 0;
      snprintf(path, sizeof(path), "sdmc:/switch/%s/gk_nosprite.txt",
               game_version_names[g_game_version]);
      s_dk_jak3_sprite = access(path, F_OK) != 0;  // record_world checks the shaders
      dkdev::set_world_recorder(record_jak3_dkgl);
      switch_run_logf("[dk] jak3 dkgl world ON (%d buckets, native world %s, gpu profiler %s, "
                      "particles %s)",
                      dkgl::bucket_count(),
                      s_dk_jak3_world ? "ON" : "OFF (gk_dkgl_noworld.txt)",
                      s_gpu_prof ? "ON" : "off",
                      s_dk_jak3_sprite ? "ON" : "OFF (gk_nosprite.txt)");
    }
  }
}

void dk_exit() {
  // T60: stop recording the world before the Loader (and its level data) goes away.
  dkdev::set_world_recorder(nullptr);
  s_dk_tfrag = false;
  s_dk_jak2_dkgl = false;
  s_dk_jak2_world = false;
  s_dk_jak2_sprite = false;
  s_dk_jak3_dkgl = false;
  s_dk_jak3_world = false;
  s_dk_jak3_sprite = false;
  s_dk_shrub = false;
  s_dk_tie = false;
  s_dk_hfrag = false;
  s_dk_merc = false;
  s_dk_sprite = false;
  s_dk_direct = false;
  s_dk_sky = false;
  s_dk_ocean = false;
  for (int i = 0; i < 2; i++) {  // T88: the pool is going away with them
    s_sky_gpu[i] = nullptr;
    s_sky_tex_id[i] = 0;
  }
  // T49: stop the opt-in Loader first (joins its file thread) while the device is up.
  s_dk_loader_ptr.store(nullptr);
  s_dk_loader.reset();
  s_dk_tex_pool.reset();
  // T48 (AI-assisted): free the texture image pool BEFORE the device goes away; drain the
  // queue first so no in-flight upload still reads the memblock being destroyed.
  dkdev::wait_idle();
  dktexgpu::teardown();
  dkdev::shutdown();
  g_dk.reset();
}

class DkDisplay : public GfxDisplay {
 public:
  explicit DkDisplay(bool is_main)
      : m_display_manager(std::make_shared<DisplayManager>(nullptr)),
        m_input_manager(std::make_shared<InputManager>(nullptr)) {
    m_main = is_main;
    m_display_manager->set_input_manager(m_input_manager);
  }
  ~DkDisplay() override {
    m_display_manager.reset();
    m_input_manager.reset();
    SDL_Quit();
    if (m_main) {
      dk_exit();
    }
  }

  std::shared_ptr<DisplayManager> get_display_manager() const override {
    return m_display_manager;
  }
  std::shared_ptr<InputManager> get_input_manager() const override { return m_input_manager; }

  void render() override;
  void init_splash() override;
  void draw_splash(int, int) override {}

 private:
  std::shared_ptr<DisplayManager> m_display_manager;
  std::shared_ptr<InputManager> m_input_manager;
  bool m_should_quit = false;
  bool m_splash_done = false;
  // splash review (AI-assisted): the dk device is up ~3 s after boot, so the 3 s SplashTimer
  // (started at InitVideo) had nearly run out when the splash first appeared, then the screen
  // stayed black until the game's first frame (~18-20 s). Count from the first show and keep
  // it up until the first chain arrives.
  Timer m_splash_shown;
  float m_last_pmode_alp = 1.f;  // GL OpenGLRenderer::m_last_pmode_alp
  bool m_seen_chain = false;
  unsigned m_swap_interval = ~0u;  // force the first frame to apply it
  int m_op_mode = -1;             // docked/handheld state for P1.2b change logging (T09)
  bool m_focused = true;          // pause present while out of focus (T10 / P1.2b)

  struct Stats {
    int frames = 0, chains = 0;
    double wait = 0, record = 0, submit = 0, total = 0, max_total = 0;
    // T83: render-thread split + which part was largest in each slow (> 18 ms) frame
    double chain_wait = 0, capture = 0, loader = 0, build = 0, fence = 0, acquire = 0;
    int slow = 0, slow_ee = 0, slow_gpu = 0, slow_vsync = 0, slow_loader = 0, slow_cpu = 0;
    // T84: per-pass CPU build ms and GPU ms (sum, frames with data), max GPU frame
    double build_pass[kPassCount] = {}, gpu_pass[kPassCount] = {}, gpu_total = 0, gpu_max = 0;
    int gpu_frames = 0;
  } m_st;
  Timer m_report;
  Timer m_period;
  bool m_first = true;

  void process_events();
};

void DkDisplay::init_splash() {
  if (m_splash_done || !Gfx::g_splash.ready.load() || Gfx::g_splash.data.empty()) {
    return;
  }
  m_splash_done = true;
  m_splash_shown.start();
  dkdev::upload_splash(Gfx::g_splash.data.data(), Gfx::g_splash.width, Gfx::g_splash.height);
  Gfx::g_splash.data.clear();
  Gfx::g_splash.data.shrink_to_fit();
}

namespace {
// Wifi frame grab combo (AI-assisted): R3 + Plus held together arms a one-shot readback
// of the next presented frame, which present_frame streams to the wifi log host two
// frames later (dkdev::request_frame_grab -> switch_net_send_frame -> the
// frame-receiver.py listener on the dev machine). Same edge-triggered shape as the
// R3+Minus diagnostics combo in opengl.cpp, so holding the combo cannot re-arm it.
// This is the opt-in replacement for the removed FIX 115/117 SD-shot recorder.
void process_switch_grab_combo(SDL_GamepadButton button, bool down) {
  static unsigned s_held = 0;
  const unsigned kStart = 1u << (unsigned)SDL_GAMEPAD_BUTTON_START;
  const unsigned kRStick = 1u << (unsigned)SDL_GAMEPAD_BUTTON_RIGHT_STICK;
  const unsigned kCombo = kStart | kRStick;
  const unsigned bit = 1u << (unsigned)button;
  if ((bit & kCombo) == 0) {
    return;
  }
  if (down) {
    const bool was_complete = (s_held == kCombo);
    s_held |= bit;
    if (s_held == kCombo && !was_complete) {
      dkdev::request_frame_grab();
      switch_run_logf("[grab] R3+Plus: frame grab armed (streams to the wifi host)");
    }
  } else {
    s_held &= ~bit;
  }
}
}  // namespace

void DkDisplay::process_events() {
  if (!switch_platform::applet_pump()) {  // FIX 7p: acknowledge applet messages every frame
    m_should_quit = true;
  }
  SDL_Event evt;
  while (SDL_PollEvent(&evt) != 0) {
    if (evt.type == SDL_EVENT_QUIT) {
      switch_run_logf("[dk] SDL_EVENT_QUIT received");
      m_should_quit = true;
    }
    // Watch gamepad buttons for the frame grab combo (same hook point as the GL path's
    // diagnostics toggle, opengl.cpp process_sdl_events).
    if (evt.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || evt.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
      process_switch_grab_combo((SDL_GamepadButton)evt.gbutton.button,
                                evt.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    }
    m_display_manager->process_sdl_event(evt);
    m_input_manager->process_sdl_event(evt);
  }
  const int mode = dkdev::operation_mode();
  if (mode != m_op_mode) {
    switch_run_logf("[dk] operation mode: %s (swapchain stays at boot size until P1.2b resize)",
                    mode ? "docked" : "handheld");
    m_op_mode = mode;
  }
}

void DkDisplay::render() {
  if (m_first) {
    m_first = false;
    m_report.start();
    m_period.start();
    switch_run_logf("[dk] first render; gamepads seen by SDL: %d (no SDL video on this path)",
                    m_input_manager->get_num_controllers());
    // T48 (AI-assisted, P2.3): bring up the texture image pool (FIX 119: 256 MB) HERE, on
    // the render thread, so the T47 owner-thread guard records this thread as the only uploader.
    const bool tex_ok = dktexgpu::init();
    switch_run_logf("[dk] tex pool init: %s",
                    tex_ok ? "ok (256 MB image memblock, 4096 slots, render thread owns uploads)"
                           : "FAILED (telemetry stays 'tex -')");
    if (tex_ok && dkload_present()) {
      dk_loader_start();  // T49
    }
  }
  m_input_manager->poll_keyboard_data();
  m_input_manager->poll_mouse_data();
  m_input_manager->finish_polling();
  process_events();
  m_display_manager->process_ee_events();
  m_input_manager->process_ee_events();
  m_display_manager->set_game_size(Gfx::g_global_settings.lbox_w, Gfx::g_global_settings.lbox_h);

  if (SplashScreen && DiskBoot && !m_splash_done) {
    init_splash();
  }
  const bool show_splash = SplashScreen && m_splash_done &&
                           (m_splash_shown.getSeconds() < SPLASH_SCREEN_TIME || !m_seen_chain);

  // Engine handshake, same contract as render_game_frame() on the GL path: wait up to
  // 40 ms for a chain, then mark it consumed under dma_mutex (FIX 11). Phase 1 does not
  // draw it yet.
  bool got_chain = false;
  const u64 t_chain0 = dk_tick();
  {
    std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
    got_chain = g_dk->dma_cv.wait_for(lock, std::chrono::milliseconds(40),
                                      [] { return g_dk->has_data_to_render; });
  }
  if (got_chain && !m_seen_chain) {
    m_seen_chain = true;  // splash review: the game draws from now on
    switch_run_logf("[dk] first chain: splash off after %.1f s on screen",
                    m_splash_shown.getSeconds());
  }
  const u64 t_cap0 = dk_tick();
  if (got_chain) {
    {
      std::unique_lock<std::mutex> lock(g_dk->sync_mutex);
      g_dk->frame_idx_of_input_data = g_dk->frame_idx;
    }
    std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
    CapLap lap_drain(kCapDrain);
    if (dk_gen2_world()) {
      // AI-assisted: new deferred-copy staging slot BEFORE the capture stages this frame's
      // texture-anim / sky copies (they used to be flushed with a blocking submit when the
      // build called begin_deferred_frame later: 3-5 ms/frame). World recorder records them.
      dktexgpu::begin_deferred_frame();
    }
    dk_drain_tex_ops();  // T80: before any capture looks textures up
    if (s_dk_tfrag) {
      // T60: read the chain BEFORE the ack below -- after it the EE rewrites that memory.
      dktf::capture_chain(g_dk->chain_data, g_dk->chain_offset);
      if (s_dk_hfrag) {
        dkhfrag::capture_chain(g_dk->chain_data, g_dk->chain_offset);  // T113/FIX 117
      }
      if (s_dk_shrub) {
        dkshrub::capture_chain(g_dk->chain_data, g_dk->chain_offset);  // T72
      }
      if (s_dk_tie) {
        dktie::capture_chain(g_dk->chain_data, g_dk->chain_offset);  // T76
      }
      if (s_dk_merc && !dkgl::merc_active()) {  // #19: dkgl Merc2 draws the characters
        // T78: bones are read at their GOAL addresses, so pass the EE memory size.
        dkmerc::capture_chain(g_dk->chain_data, g_dk->chain_offset, EE_MAIN_MEM_SIZE);
      }
      if (s_dk_sprite) {
        // T79: replays the texture uploads (bucket order), then reads the sprite bucket.
        // FIX 112: the sky blend runs inside that replay (dk_sky_bucket_hook) so its
        // source textures are still in VRAM, like GL's bucket order.
        s_dk_upload_base = (const u8*)g_dk->chain_data;
        s_sky_hook_data = s_dk_sky ? g_dk->chain_data : nullptr;
        s_sky_hook_offset = g_dk->chain_offset;
        s_sky_hook_next = 0;
        // #16/#19: dkgl buckets run inside the replay (dk_upload_bucket_hook)
        s_dkgl_interleave = s_dk_ocean && dkgl::active() &&
                            dkgl::capture_begin(g_dk->chain_data, g_dk->chain_offset);
        dksprite::capture_chain(g_dk->chain_data, g_dk->chain_offset, dk_sprite_upload,
                                dk_sprite_lookup, dk_upload_bucket_hook);
        if (s_dkgl_interleave) {
          dkgl::capture_end();
          dkgl::note_interleaved_frame();
        }
        s_sky_hook_data = nullptr;
      }
      if (s_dk_sky) {
        if (!s_dk_sprite) {
          // no upload replay to hook into: blend against end-of-frame VRAM (old T88 path)
          dksky::capture_blend_chain(g_dk->chain_data, g_dk->chain_offset, dk_sky_source);
        }
        for (int i = 0; i < 2; i++) {
          // GL re-points the VRAM slots after every blend step (move_existing_to_vram);
          // once per frame, before the sky-draw capture resolves them, is enough here.
          if (s_sky_gpu[i] && s_dk_tex_pool) {
            s_dk_tex_pool->move_existing_to_vram(s_sky_gpu[i], SKY_TEXTURE_VRAM_ADDRS[i]);
          }
        }
        dkdirect::capture_sky_chain(g_dk->chain_data, g_dk->chain_offset, dk_direct_lookup);
      }
      if (s_dk_direct) {
        // T80: after sprite, so the pool holds this frame's uploads (font, HUD icons).
        dkdirect::capture_chain(g_dk->chain_data, g_dk->chain_offset, dk_direct_lookup);
      }
      if (s_dk_ocean) {
        // T95: after sprite + direct captures (the envmap tbp lookup happens at
        // build time; the far walk needs the dkdirect capture state ready).
        if (dkgl::active()) {
          // T107: the GL ocean renderers walk the chain now (it is acked right after).
          if (!s_dkgl_interleave) {
            dkgl::capture_frame(g_dk->chain_data, g_dk->chain_offset);  // no upload replay
          }
          s_dkgl_interleave = false;
        } else {
          dkocean::capture_chain(g_dk->chain_data, g_dk->chain_offset, dk_direct_lookup,
                                 s_dk_direct);
        }
      }
      if (s_dk_merc && s_dk_direct && !dkgl::eyes_active()) {
        // T82: after sprite too (iris/pupil/lid textures come from this frame's uploads).
        dkeye::capture_chain(g_dk->chain_data, g_dk->chain_offset,
                             [](uint32_t tbp) { return dk_direct_lookup(tbp, false); });
      }
    }
    if (dk_gen2_dkgl()) {
      // §7.2 phase 1 (AI-assisted) / J4 (jak3): the natives read their buckets before the
      // ack (same rule as the jak1 path above; tfrag first -- it captures the fog from the
      // vis bucket that tie/shrub copy). THEN the gen-2 game replays EVERY
      // TextureUploadHandler bucket in GL order (the SpriteDk walker, sprite capture off)
      // and runs the dkgl buckets inside that replay at their GL position, exactly like
      // the jak1 interleave (#16/#19). jak3 also reads the HFRAG bucket (J4: native
      // mountains, T113/FIX 117's renderer) and walks its own upload list (J3 tables).
      const bool jak3 = g_game_version == GameVersion::Jak3;
      if (dk_gen2_world()) {
        CapLap lap(kCapWorld);
        dktf::capture_chain(g_dk->chain_data, g_dk->chain_offset);
        if (jak3) {
          dkhfrag::capture_chain(g_dk->chain_data, g_dk->chain_offset);  // J4: bucket 8
        }
        dktie::capture_chain(g_dk->chain_data, g_dk->chain_offset);
        dkshrub::capture_chain(g_dk->chain_data, g_dk->chain_offset);
      }
      s_dk_upload_base = (const u8*)g_dk->chain_data;
      s_sky_hook_data = nullptr;  // no jak1 sky blend on this path
      CapLap lap_walk(kCapWalk);
      s_dkgl_interleave = dkgl::capture_begin(g_dk->chain_data, g_dk->chain_offset);
      dksprite::capture_chain(g_dk->chain_data, g_dk->chain_offset, dk_sprite_upload,
                              dk_sprite_lookup, dk_upload_bucket_hook,
                              jak3 ? dksprite::kJak3TexBuckets : dksprite::kJak2TexBuckets,
                              jak3 ? (int)(sizeof(dksprite::kJak3TexBuckets) / sizeof(int))
                                   : (int)(sizeof(dksprite::kJak2TexBuckets) / sizeof(int)),
                              jak3 ? dksprite::kJak3SpriteBucket : dksprite::kJak2SpriteBucket,
                              dk_gen2_sprite(),  // read_sprites: particles bucket (313 / 570)
                              false,  // head_call: jak2/jak3 chains have no CALL head
                              dk_texture_anim,  // T125: imm-12 -> GL TextureAnimator (dkgl)
                              dk_glow,          // T126: glow section -> GL GlowRenderer (dkgl)
                              dk_distort);      // T127: distort section -> GL distorter (dkgl)
      if (s_dkgl_interleave) {
        CapLap lap(kCapEnd);  // the dkgl buckets the walk did not reach + frame end
        dkgl::capture_end();
        dkgl::note_interleaved_frame();
      }
    }
    g_dk->has_data_to_render = false;
    g_dk->dma_cv.notify_all();
  }
  const u64 t_cap1 = dk_tick();

  // swap interval from the target framerate (FIX 10 semantics: 30 fps = interval 2)
  unsigned want = Gfx::g_global_settings.vsync ? 1 : 0;
  if (Gfx::g_global_settings.vsync && Gfx::g_global_settings.target_fps > 0 &&
      Gfx::g_global_settings.target_fps <= 35) {
    want = 2;
  }
  if (want != m_swap_interval) {
    m_swap_interval = want;
    dkdev::set_swap_interval(want);
    switch_run_logf("[dk] swap interval %u (vsync=%d target_fps=%.0f)", want,
                    (int)Gfx::g_global_settings.vsync, Gfx::g_global_settings.target_fps);
  }

  // T10 / P1.2b: stop presenting while out of focus (HOME menu / overlay / sleep). The
  // present_frame() call is the only GPU-submitting step in this function; guarding it is
  // enough to pause GPU work. The engine handshake below (frame_idx advance + sync_cv
  // notify + the chain-acknowledge above) is NOT gated on focus: per FIX 19 the engine
  // relies on frame_idx advancing every iteration and on sync_cv being notified, so the
  // render loop must never deadlock on an unfocused frame.
  const bool focused = dkdev::in_focus();
  if (focused != m_focused) {
    switch_run_logf("[dk] focus %s", focused ? "regained" : "lost: pausing present");
    if (!focused) {
      // Drain once so nothing is in flight while suspended (plan P1.2b focus/suspend).
      dkdev::wait_idle();
    }
    m_focused = focused;
  }
  // FIX 124 (AI-assisted): advance the pool's draw-stamp clock once per frame,
  // before this frame's lookups/binds can write any stamps. s_dk_tex_pool is
  // created in dk_loader_start() before the render loop begins (it is
  // derefered by the loader update right below).
  s_dk_tex_pool->tick_frame();
  if (s_dk_loader) {
    // T49: render thread, before present. Same as OpenGLRenderer's loader step: a bigger
    // budget while the screen is black, and when the game fades back in, finish every
    // pending load first so the first visible frame has the whole level (fast blackout
    // loads). Without it the fade-in showed the level half loaded.
    const float alp = s_dk_pmode_alp.load(std::memory_order_relaxed);
    s_dk_loader->set_blackout(alp == 0.f);
    if (m_last_pmode_alp == 0.f && alp != 0.f) {
      const u64 t0 = dk_tick();
      s_dk_loader->update_blocking(*s_dk_tex_pool);
      switch_run_logf("[dk] fade-in: loader finished pending loads in %.1f ms",
                      dk_ms(t0, dk_tick()));
    } else {
      s_dk_loader->update(*s_dk_tex_pool);
    }
    m_last_pmode_alp = alp;
  }
  const u64 t_build0 = dk_tick();
  if (s_dk_tfrag && got_chain && focused) {
    // T60: before present_frame -- palette uploads reuse the frame cmd buffer. Only when
    // a present follows (the index ring relies on one present per build).
    u64 tp = dk_tick();
    auto lap = [&](int pass) {  // T84: CPU ms of the build just finished
      const u64 now = dk_tick();
      m_st.build_pass[pass] += dk_ms(tp, now);
      tp = now;
    };
    dktexgpu::begin_deferred_frame();  // T86: new staging slot for the tod palette copies
    if (s_dk_sky) {
      dksky::flush_blend(dk_sky_store);  // T88: this frame's sky/cloud blends into the pool
    }
    dktf::build_frame(s_dk_loader.get());
    if (s_dk_hfrag) {
      dkhfrag::build_frame(s_dk_loader.get());  // T113/FIX 117: jak3 only (empty on jak1)
    }
    lap(kPTfrag);
    if (s_dk_shrub) {
      dkshrub::build_frame(s_dk_loader.get());  // T72: own index ring + tod palettes
      lap(kPShrub);
    }
    if (s_dk_tie) {
      dktie::build_frame(s_dk_loader.get());  // T76: own index ring + tod palettes
      lap(kPTie);
    }
    if (s_dk_merc && !dkgl::merc_active()) {
      dkmerc::build_frame(s_dk_loader.get(), dktf::frame_draws().fog_rgba);  // T78
      lap(kPMerc);
    }
    if (s_dk_sprite) {
      dksprite::build_frame();  // T79: own vertex/index ring
      lap(kPSprite);
    }
    if (s_dk_direct) {
      dkdirect::build_frame(dktf::frame_draws().fog_rgba);  // T80: own vertex ring
      lap(kPDirect);
    }
    if (s_dk_merc && s_dk_direct && !dkgl::eyes_active()) {
      dkeye::build_frame();  // T82
      lap(kPEye);
    }
    if (s_dk_ocean && !dkgl::active()) {
      dkocean::build_frame(dktf::frame_draws().fog_rgba);  // T95
    }
  }
  if (dk_gen2_world() && got_chain && focused) {
    // §7.2 phase 1 (AI-assisted) / J4 (jak3): the native world builds for the gen-2 game,
    // same order as jak1 (tfrag first -- the fog). merc/sprite/direct/ocean stay dkgl /
    // inert on this path. jak3 builds hfrag after the fog is in (T113/FIX 117 -- the GL
    // Hfrag renderer needs dktf::frame_draws() fog, like the jak1 path above).
    u64 tp = dk_tick();
    auto lap2 = [&](int pass) {
      const u64 now = dk_tick();
      m_st.build_pass[pass] += dk_ms(tp, now);
      tp = now;
    };
    // begin_deferred_frame() runs at capture start (see above)
    dktf::build_frame(s_dk_loader.get());
    lap2(kPTfrag);
    dktie::build_frame(s_dk_loader.get());
    lap2(kPTie);
    dkshrub::build_frame(s_dk_loader.get());
    lap2(kPShrub);
    if (g_game_version == GameVersion::Jak3) {
      dkhfrag::build_frame(s_dk_loader.get());  // J4: bucket 8 mountains (own tod palettes)
    }
  }
  if (dk_gen2_sprite() && got_chain && focused) {
    dksprite::build_frame();  // §7.2 particles: own vertex/index ring
  }
  const u64 t_build1 = dk_tick();
  dkdev::FrameTiming ft{};
  if (focused) {
    dkdev::set_pcrtc(Gfx::g_global_settings.brightness_contrast_color,
                     Gfx::g_global_settings.brightness_contrast_alpha);
    ft = dkdev::present_frame(show_splash);
    dktexgpu::end_frame();  // T87: free textures released 3+ presents ago
  } else {
    // Out of focus: do not submit, but keep the ~60 Hz pacing so the loop doesn't spin.
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  // FIX 19: frame_idx must advance every iteration -- it is the engine's liveness signal.
  {
    std::unique_lock<std::mutex> lock(g_dk->sync_mutex);
    g_dk->frame_idx++;
    g_dk->sync_cv.notify_all();
  }

  // [dk] telemetry (plan P1.5): every 2 s with R3+Minus diag on, else every 10 s, since
  // each SD line costs ~6 ms (FIX 40). A non-presenting (out-of-focus) frame contributes
  // no timing samples, so skip accumulation when unfocused; the loop still paces at
  // ~16 ms (see the sleep above).
  const double total = m_period.getMs();
  m_period.start();
  if (focused) {
    m_st.frames++;
    m_st.chains += got_chain ? 1 : 0;
    m_st.wait += ft.wait_ms;
    m_st.record += ft.record_ms;
    m_st.submit += ft.submit_ms;
    m_st.total += total;
    m_st.max_total = std::max(m_st.max_total, total);
    const double c_wait = dk_ms(t_chain0, t_cap0), c_cap = dk_ms(t_cap0, t_cap1),
                 c_load = dk_ms(t_cap1, t_build0), c_build = dk_ms(t_build0, t_build1);
    m_st.chain_wait += c_wait;
    m_st.capture += c_cap;
    m_st.loader += c_load;
    m_st.build += c_build;
    m_st.fence += ft.fence_ms;
    {
      // T84: GPU ms per pass from the timestamp marks (frame N-2, same slice). Mark 0 =
      // world start; pass p ends at mark 1+p, starts at the previous written mark.
      const double end = dkdev::gpu_mark_ms(dkdev::kGpuMarks - 1);
      if (end >= 0) {
        double prev = 0;
        for (int p = 0; p < kPassCount; p++) {
          const double m = dkdev::gpu_mark_ms(1 + p);
          if (m >= 0) {
            m_st.gpu_pass[p] += m - prev;
            prev = m;
          }
        }
        m_st.gpu_total += end;
        m_st.gpu_max = std::max(m_st.gpu_max, end);
        m_st.gpu_frames++;
      }
      prof_accumulate();
    }
    m_st.acquire += ft.acquire_ms;
    if (total > 18.0) {
      m_st.slow++;
      const double cpu = c_cap + c_build + ft.record_ms + ft.submit_ms;
      const double parts[5] = {c_wait, ft.fence_ms, ft.acquire_ms, c_load, cpu};
      int big = 0;
      for (int i = 1; i < 5; i++) {
        if (parts[i] > parts[big]) {
          big = i;
        }
      }
      int* slot[5] = {&m_st.slow_ee, &m_st.slow_gpu, &m_st.slow_vsync, &m_st.slow_loader,
                      &m_st.slow_cpu};
      (*slot[big])++;
    }
  }
  const double window = switch_diag_enabled() ? 2.0 : 10.0;
  if (m_report.getSeconds() >= window && m_st.frames > 0) {
    const double n = (double)m_st.frames;
    const dkdev::HeapInfo h = dkdev::heap_info();
    switch_run_logf("[dk] %.1f fps | frame %.2f (max %.1f) ms | wait %.2f record %.3f submit %.3f "
                    "| chains %d/%d | pools %llu KB | heap free %llu MB",
                    n / m_report.getSeconds(), m_st.total / n, m_st.max_total, m_st.wait / n,
                    m_st.record / n, m_st.submit / n, m_st.chains, m_st.frames,
                    dkdev::pool_bytes() / 1024, h.total_free() >> 20);
    {
      // T83: where the frame time goes. chainwait/ee = waiting for the game (GOAL) thread's
      // chain, gpu = slice fence,
      // vsync = image acquire; slow = frames over 18 ms by their largest part.
      switch_run_logf("[dk] split ms | chainwait %.2f capture %.2f loader %.2f build %.2f "
                      "gpufence %.2f vsync %.2f | slow %d: ee %d gpu %d vsync %d loader %d cpu %d",
                      m_st.chain_wait / n, m_st.capture / n, m_st.loader / n, m_st.build / n,
                      m_st.fence / n, m_st.acquire / n, m_st.slow,
                      m_st.slow_ee, m_st.slow_gpu, m_st.slow_vsync, m_st.slow_loader,
                      m_st.slow_cpu);
      // T84: per pass "cpu build / gpu" ms averages; gpu world = whole world pass on the GPU.
      char pl[384];
      int pn = 0;
      const double gf = m_st.gpu_frames ? (double)m_st.gpu_frames : 1.0;
      for (int p = 0; p < kPassCount && pn < (int)sizeof(pl) - 40; p++) {
        pn += snprintf(pl + pn, sizeof(pl) - pn, " %s %.2f/%.2f", kPassName[p],
                       m_st.build_pass[p] / n, m_st.gpu_pass[p] / gf);
      }
      // T86: time-of-day palette copies per frame (deferred) + waiting fallbacks since boot
      static uint64_t s_tod_last = 0;
      const uint64_t tod_now = dktexgpu::deferred_total();
      switch_run_logf("[dk] pass ms cpu/gpu |%s | gpu world %.2f (max %.1f, %d frames) | tod "
                      "%.1f/frame sync %llu",
                      pl, m_st.gpu_total / gf, m_st.gpu_max, m_st.gpu_frames,
                      (double)(tod_now - s_tod_last) / n,
                      (unsigned long long)dktexgpu::deferred_sync_fallbacks());
      s_tod_last = tod_now;
      prof_report();
      if (dk_gen2_dkgl()) {
        // §7.0 I (AI-assisted): the capture split. drain = from the dma lock to the world
        // reads (tex ops), walk = the tex-bucket walk itself (nested parts subtracted).
        double c[kCapCount];
        for (int i = 0; i < kCapCount; i++) {
          c[i] = s_cap_ms[i] / n;
          s_cap_ms[i] = 0;
        }
        c[kCapWalk] = std::max(0.0, c[kCapWalk] - c[kCapUpload] - c[kCapDkgl] -
                                        c[kCapTexAnim] - c[kCapGlow] - c[kCapDistort] -
                                        c[kCapEnd]);
        c[kCapDrain] = std::max(0.0, c[kCapDrain] - c[kCapWorld] - c[kCapWalk] -
                                         c[kCapUpload] - c[kCapDkgl] - c[kCapTexAnim] -
                                         c[kCapGlow] - c[kCapDistort] - c[kCapEnd]);
        char cl[512];
        int cn = snprintf(cl, sizeof(cl), "[dk] capture ms |");
        for (int i = 0; i < kCapCount; i++) {
          cn += snprintf(cl + cn, sizeof(cl) - cn, " %s %.2f", kCapName[i], c[i]);
        }
        int tb[10];
        double tm[10];
        const int nt = dkgl::capture_cpu_top(tb, tm, 10);
        cn += snprintf(cl + cn, sizeof(cl) - cn, " | dkgl top");
        for (int i = 0; i < nt && cn < (int)sizeof(cl) - 16; i++) {
          cn += snprintf(cl + cn, sizeof(cl) - cn, " b%d %.2f", tb[i], tm[i] / n);
        }
        switch_run_logf("%s", cl);
      }
      if (s_dk_ocean) {
        // T95: ocean pass counters for this window.
        const dkocean::FlushStats& fs = dkocean::flush_stats();
        const dkocean::DriverStats& ds = dkocean::driver_stats();
        static dkocean::FlushStats s_f_last;
        static dkocean::DriverStats s_d_last;
        const double fwin = (double)(fs.frames - s_f_last.frames);
        switch_run_logf("[dk] ocean | texgen %.1f mip %.1f near %.1f mid %.1f per frame | nb %u,%u,%u "
                        "mb %u,%u/frame | no_tex %u "
                        "drop %u miss %u ubo %u | walks mf %u n %u mid_runs %u far %u "
                        "abort %u",
                        fwin > 0 ? (double)(fs.tex_gen_draws - s_f_last.tex_gen_draws) / fwin : 0,
                        fwin > 0 ? (double)(fs.tex_mip_draws - s_f_last.tex_mip_draws) / fwin : 0,
                        fwin > 0 ? (double)(fs.near_draws - s_f_last.near_draws) / fwin : 0,
                        fwin > 0 ? (double)(fs.mid_draws - s_f_last.mid_draws) / fwin : 0,
                        fwin > 0 ? (unsigned)((fs.near_bucket_draws[0] - s_f_last.near_bucket_draws[0]) / fwin) : 0,
                        fwin > 0 ? (unsigned)((fs.near_bucket_draws[1] - s_f_last.near_bucket_draws[1]) / fwin) : 0,
                        fwin > 0 ? (unsigned)((fs.near_bucket_draws[2] - s_f_last.near_bucket_draws[2]) / fwin) : 0,
                        fwin > 0 ? (unsigned)((fs.mid_bucket_draws[0] - s_f_last.mid_bucket_draws[0]) / fwin) : 0,
                        fwin > 0 ? (unsigned)((fs.mid_bucket_draws[1] - s_f_last.mid_bucket_draws[1]) / fwin) : 0,
                        fs.no_tex - s_f_last.no_tex, fs.dropped - s_f_last.dropped,
                        fs.misses - s_f_last.misses, fs.ubo_fail - s_f_last.ubo_fail,
                        ds.mid_far_walks - s_d_last.mid_far_walks,
                        ds.near_walks - s_d_last.near_walks,
                        ds.mid_runs - s_d_last.mid_runs, ds.far_packets - s_d_last.far_packets,
                        ds.mid_aborts - s_d_last.mid_aborts);
        s_f_last = fs;
        s_d_last = ds;
      }
      if (s_dk_sky) {
        const dksky::BlendStats& sb = dksky::blend_stats();
        switch_run_logf(
            "[dk] sky | pairs %u buckets %u draw %u/%u blend %u/%u | no-src %u (abs %u ph "
            "%u; %ux%u %ux%u) bad %u/%u/%u/%u/%u | store-fail %u | frames %llu",
            sb.pairs, sb.buckets, sb.sky_draws, sb.cloud_draws, sb.sky_blends, sb.cloud_blends,
            sb.no_src, sb.no_src_absent, sb.no_src_placeholder, sb.no_src_tbp[0],
            sb.no_src_tbp_count[0], sb.no_src_tbp[1], sb.no_src_tbp_count[1], sb.bad_adgif,
            sb.bad_alpha, sb.bad_draw, sb.bad_coord, sb.bad_size, s_sky_store_fails,
            (unsigned long long)sb.frames);
        if (sb.bad_size > 0 && s_dk_tex_pool) {
          GpuTexture* bt = s_dk_tex_pool->lookup_gpu_texture(sb.last_bad_tbp);
          switch_run_logf("[dk] sky bad-size | buf %d tbp %u bytes %u tex %ux%u '%s'",
                          sb.last_bad_idx, sb.last_bad_tbp, sb.last_bad_bytes,
                          bt ? bt->w : 0, bt ? bt->h : 0,
                          s_dk_tex_pool->get_debug_texture_name_from_tbp(sb.last_bad_tbp).c_str());
        }
      }
    }
    // T24 (P2.3 soak telemetry): one more [dk] line in the SAME report window as the
    // fps line just above (piggyback its exact timing/guard). Null-check each
    // subsystem so an inactive group prints "<name> -" instead of numbers; never
    // crash, never spam per frame (shares the report-window guard, so at most
    // every 2 s diag-on / 10 s diag-off, like the fps line).
    {
      const dkring::FrameRing* ring = dkdev::ubo_ring();
      const dkpool::BufferPool* pool = dkdev::buffer_pool();
      char tel[256];
      // tex: dktexgpu (T22) telemetry; active() = the 48 MB image memblock exists.
      if (dktexgpu::active()) {
        snprintf(tel, sizeof(tel),
                 "[dk] tex live=%u high=%u fail=%d freed=%llu mem=%lluKB holes=%lluKB | ",
                 dktexgpu::live_count(), dktexgpu::high_water(), dktexgpu::failed_allocs(),
                 (unsigned long long)dktexgpu::released_total(),
                 (unsigned long long)(dktexgpu::image_bytes_used() / 1024),
                 (unsigned long long)(dktexgpu::free_hole_bytes() / 1024));
      } else {
        snprintf(tel, sizeof(tel), "[dk] tex - | ");
      }
      // pool: P2.3 BufferPool (T21); null when dk_device never carved its region.
      size_t pos = strlen(tel);
      if (pool != nullptr) {
        snprintf(tel + pos, sizeof(tel) - pos, "pool out=%u pooled=%u fail=%d | ",
                 pool->outstanding(), pool->pooled_chunks(), pool->failed_allocations());
      } else {
        snprintf(tel + pos, sizeof(tel) - pos, "pool - | ");
      }
      // ring: UBO ring (T20): used / segment_size / high_water; null when not wired.
      pos = strlen(tel);
      if (ring != nullptr) {
        snprintf(tel + pos, sizeof(tel) - pos, "ring used=%u/%u hw=%u", ring->used(),
                 ring->segment_size(), ring->high_water());
      } else {
        snprintf(tel + pos, sizeof(tel) - pos, "ring -");
      }
      switch_run_logf("%s", tel);
    }
    m_st = Stats();
    m_report.start();
  }

  if (m_should_quit) {
    switch_run_logf("[dk] quit latched -> MasterExit=EXIT");
    {
      std::unique_lock<std::mutex> lock(g_dk->sync_mutex);
      MasterExit = RuntimeExitStatus::EXIT;
      g_dk->sync_cv.notify_all();
    }
    std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
    g_dk->dma_cv.notify_all();
  }
}

std::shared_ptr<GfxDisplay> dk_make_display(int /*width*/,
                                            int /*height*/,
                                            const char* /*title*/,
                                            GfxGlobalSettings& /*settings*/,
                                            GameVersion /*version*/,
                                            bool is_main) {
  // Same sizing rule as the GL path (FIX 73c): decided once, from the operation mode.
  const auto size = switch_platform::get_display_size_for_operation_mode();
  if (!dkdev::init(size.w, size.h)) {
    return nullptr;  // Gfx::Init falls back to OpenGL
  }
  switch_platform::set_created_window_size(size.w, size.h);
  auto display = std::make_shared<DkDisplay>(is_main);
  display->get_display_manager()->set_native_window(size.w, size.h);
  display->set_imgui_visible(Gfx::g_debug_settings.show_imgui);
  return display;
}

u32 dk_vsync() {
  if (!g_dk) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(g_dk->sync_mutex);
  const auto init_frame = g_dk->frame_idx_of_input_data;
  g_dk->sync_cv.wait(lock, [=] {
    return (MasterExit != RuntimeExitStatus::RUNNING) || g_dk->frame_idx > init_frame;
  });
  return g_dk->frame_idx & 1;
}

u32 dk_sync_path() {
  if (!g_dk) {
    return 0;
  }
  // FIX 11: wait on dma_cv under dma_mutex (the predicate's own mutex).
  std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
  if (!g_dk->has_data_to_render) {
    return 0;
  }
  g_dk->dma_cv.wait(lock, [] {
    return (MasterExit != RuntimeExitStatus::RUNNING) || !g_dk->has_data_to_render;
  });
  return 0;
}

void dk_send_chain(const void* data, u32 offset) {
  if (!g_dk) {
    return;
  }
  std::unique_lock<std::mutex> lock(g_dk->dma_mutex);
  if (g_dk->has_data_to_render) {
    lg::error("Gfx::send_chain called when the deko3d renderer has pending data");
    return;
  }
  // Phase 1: chains are acknowledged, not rendered. T60: the pointer is kept for the
  // opt-in tfrag capture, which reads it on the render thread before the ack.
  g_dk->chain_data = data;
  g_dk->chain_offset = offset;
  g_dk->has_data_to_render = true;
  g_dk->chains_received++;
  g_dk->dma_cv.notify_all();
}

// T80 (AI-assisted): GOAL upload-now! and texture-relocate (the jak1 font: upload, then a copy
// to the PSMT4HH slots that menu/HUD text samples). GOAL calls these on the EE thread, but
// dktexgpu is render-thread only, so they are queued in order and replayed by
// dk_drain_tex_ops() while the EE waits for the chain ack (its memory is stable then).
struct DkTexOp {
  const u8* tpage;  // nullptr = relocate
  int mode;
  u32 a, b, c;  // upload: s7 | relocate: dst, src, format
};
std::mutex s_dk_tex_ops_mutex;
std::vector<DkTexOp> s_dk_tex_ops;
constexpr size_t kMaxTexOps = 512;  // only grows while no pool drains it

void dk_texture_upload_now(const u8* tpage, int mode, u32 s7_ptr) {
  std::lock_guard<std::mutex> lk(s_dk_tex_ops_mutex);
  if (s_dk_tex_ops.size() < kMaxTexOps) {
    s_dk_tex_ops.push_back({tpage, mode, s7_ptr, 0, 0});
  } else {
    // FIX 123 (AI-assisted): a silent drop here would unlink a whole level's tbps
    // (upload-now! during a blackout load with no chains flowing to drain the
    // queue). The old code dropped ops without a trace.
    static u32 s_dropped_ops = 0;
    if ((++s_dropped_ops % 64) == 1) {
      switch_run_logf("[dk] tex op queue FULL: dropped %u upload-now ops (cap %u)",
                      s_dropped_ops, (u32)kMaxTexOps);
    }
  }
}
void dk_texture_relocate(u32 dst, u32 src, u32 format) {
  std::lock_guard<std::mutex> lk(s_dk_tex_ops_mutex);
  if (s_dk_tex_ops.size() < kMaxTexOps) {
    s_dk_tex_ops.push_back({nullptr, 0, dst, src, format});
  }
}
void dk_drain_tex_ops() {
  std::vector<DkTexOp> ops;
  {
    std::lock_guard<std::mutex> lk(s_dk_tex_ops_mutex);
    ops.swap(s_dk_tex_ops);
  }
  if (!s_dk_tex_pool) {
    return;
  }
  for (const DkTexOp& op : ops) {
    if (op.tpage) {
      s_dk_tex_pool->handle_upload_now(op.tpage, op.mode, g_ee_main_mem, op.a, false);
    } else {
      // TexturePool::relocate ASSERTs on a missing source and reads gpu_textures[0].
      GpuTexture* src = s_dk_tex_pool->lookup_gpu_texture(op.b);
      if (src && !src->is_placeholder && !src->gpu_textures.empty()) {
        s_dk_tex_pool->relocate(op.a, op.b, op.c);
        switch_run_logf("[dk] tex relocate %u -> %u fmt %u ok", op.b, op.a, op.c);
      } else {
        switch_run_logf("[dk] tex relocate %u -> %u fmt %u: source missing", op.b, op.a, op.c);
      }
    }
  }
}
void dk_set_levels(const std::vector<std::string>& levels) {
  if (Loader* l = s_dk_loader_ptr.load()) {
    l->set_want_levels(levels);  // T49: same call as gl_set_levels
  }
}
void dk_set_active_levels(const std::vector<std::string>& levels) {
  if (Loader* l = s_dk_loader_ptr.load()) {
    l->set_active_levels(levels);  // T49: same call as gl_set_active_levels
  }
}
void dk_force_reload_all() {}
void dk_force_reload_level(const std::string&) {}
void dk_force_reload_common() {}
// GL gl_set_pmode_alp: GOAL's blackout (0 = black screen while a level loads, 1 = visible).
void dk_set_pmode_alp(float val) {
  s_dk_pmode_alp.store(val, std::memory_order_relaxed);
  dkgl::set_pmode_alp(val);
}

}  // namespace

namespace dk_renderer {
bool kill_switch_present(GameVersion version) {
  // jak1: deko3d only, the gk_opengl.txt opt-out is gone (2026-10-30, user decision).
  // jak2: deko3d too (2026-11-13, rows A-I HW-passed) -- no gk_dk3d.txt/gk_dkload.txt needed.
  return dk_pass_on(version, "gk_dk3d.txt");
}
}  // namespace dk_renderer

const GfxRendererModule gRendererDeko3D = {
    dk_init,                 // init
    dk_make_display,         // make_display
    dk_exit,                 // exit
    dk_vsync,                // vsync
    dk_sync_path,            // sync_path
    dk_send_chain,           // send_chain
    dk_texture_upload_now,   // texture_upload_now
    dk_texture_relocate,     // texture_relocate
    dk_set_levels,           // set_levels
    dk_set_active_levels,    // set_active_levels
    dk_force_reload_all,     // force_reload_all
    dk_force_reload_level,   // force_reload_level
    dk_force_reload_common,  // force_reload_common
    dk_set_pmode_alp,        // set_pmode_alp
    GfxPipeline::Deko3D,     // pipeline
    "deko3d (P1 skeleton)"   // name
};

#endif  // __SWITCH__
