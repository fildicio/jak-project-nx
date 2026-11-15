// T105 (AI-assisted): dkgl textures + FBOs.
// glGenTextures objects are dktexgpu render targets created LAZILY -- the pseudo GL id
// only becomes a real dktexgpu id at framebuffer_texture_2d / resolve_texture_id time
// (the pool seam, TexturePool.cpp, and T106 sampling both go through those). FBO 0 is
// the dk frame target; a bound offscreen FBO is an emitter target switch, no GL work.
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "game/graphics/deko3d_renderer/dkgl/dkgl_gl_enums.h"
#include "game/graphics/deko3d_renderer/dkgl/dkgl_state.h"

#if defined(__SWITCH__)
// same seam pair TexturePool.cpp includes for the T23 check (plain headers)
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"  // dktexgpu::create_render_target_mipped
#include "game/graphics/pipelines/dk_device.h"             // dkdev::is_up
#endif

namespace dkgl {
namespace {

struct TexObj {
  bool live = false;
  int w = 0, h = 0;
  u32 levels = 0;
  u32 min_filter = gl::LINEAR, mag_filter = gl::LINEAR;  // TexturePool.cpp upload defaults
  u32 wrap_s = gl::CLAMP_TO_EDGE, wrap_t = gl::CLAMP_TO_EDGE;
  u32 dk_tex_id = 0;  // dktexgpu id once materialized (0 = not yet)
  bool was_rendered_to = false;  // an offscreen FBO with this texture was unbound
  bool barrier_done = false;     // T106 sent the render-to-texture barrier
  bool rgb = false;  // T112: GL_RGB -- sampled with alpha forced to 1, like GL
  // T126: depth texture -> the frame depth alias; depth + renderbuffer -> own Z24S8 target
  bool depth = false;
  bool renderbuffer = false;
};
struct FboObj {
  bool live = false;
  u32 attached_tex = 0;  // GL texture id attached via framebuffer_texture_2d
  u32 level = 0;         // dkgl review: mip level attached (the ocean renders its mips one by one)
  u32 depth_attached = 0;  // T126: GL texture/renderbuffer id on the depth attachment
};
// dkgl review: sampler parameters set on POOL textures (raw dktexgpu ids). The ocean binds
// its pool textures and sets REPEAT + LINEAR_MIPMAP_LINEAR on them (CommonOceanRenderer);
// without this they would be sampled clamped, without mips.
struct PoolTexParams {
  u32 min_filter = gl::LINEAR, mag_filter = gl::LINEAR;
  u32 wrap_s = gl::CLAMP_TO_EDGE, wrap_t = gl::CLAMP_TO_EDGE;
};
std::unordered_map<u32, PoolTexParams> g_pool_params;
// GL sampler objects: id = slot + 1 (own GL namespace, 0 = none). The jak2 background
// renderers (background_common.cpp) put wrap/filter here instead of on the texture; before
// these existed every such draw sampled with the pool defaults (clamp, no mips).
constexpr u32 kMaxSamplers = 64;
struct SamplerObj {
  bool live = false;
  PoolTexParams p;  // set to the GL sampler defaults by gen_samplers
};
SamplerObj g_samplers[kMaxSamplers];
u32 g_unit_sampler[kMaxTexUnits] = {};
SamplerObj* sampler_by_id(u32 id) {
  return (id >= 1 && id <= kMaxSamplers && g_samplers[id - 1].live) ? &g_samplers[id - 1]
                                                                     : nullptr;
}

TexObj g_textures[kMaxTextures];
FboObj g_fbos[kMaxFbos];
u32 g_next_tex = 0;  // slot round-robin cursor, like g_next_vao
u32 g_next_fbo = 0;
u32 g_bound_fbo = 0;  // 0 = the dk frame target
u32 g_read_fbo = 0;   // T112: glBlitFramebuffer source (GL_READ_FRAMEBUFFER)
u32 g_bound_rb = 0;   // T126: GL_RENDERBUFFER binding (an id in the texture table)
u32 g_frame_depth_tex = 0;  // T126: dktexgpu id of the frame depth image (0 = unknown)
struct PendingTarget {  // T106 flush step 1
  bool active = false;
  bool offscreen = false;
  u32 dk_tex_id = 0;
};
PendingTarget g_pending_target;

u64 g_tex_data_count = 0;       // tex_image_2d with pixels (ocean never does)
u64 g_bad_tex_param_count = 0;  // tex_parameteri on a pname we don't store
u64 g_bad_tex_format_count = 0;
u64 g_rt_no_dims_count = 0;  // materialize before any tex_image_2d
u64 g_bad_draw_buffers_count = 0;
#if defined(__SWITCH__)
bool g_logged_tex_data = false;
// log the first failed level-0 pixel upload (bad format / ring + sync both failed)
void dk_failed_upload() {
  if (!g_logged_tex_data) {
    std::fprintf(stderr, "[dkgl] tex_image_2d pixel upload failed (format/size)\n");
    g_logged_tex_data = true;
  }
}
#endif
u64 g_unsupported_blit_count = 0;  // T112
u64 g_respecify_count = 0;         // T112: level-0 tex_image_2d with new dims/format
u64 g_tex_sub_count = 0;           // §7.0 D: tex_sub_image_2d calls (tfrag tod palettes)
u64 g_bad_sub_rect_count = 0;      // §7.0 D: tex_sub_image_2d with a non-top-left rect
// T128: dktexgpu id -> dkgl texture id, for every id resolve_texture_id handed out
// (old ids stay mapped after a respecify: they are not freed or reused, P8).
std::unordered_map<u32, u32> g_exported;

TexObj* tex_by_gl(u32 gl_id) {
  if (gl_id < kTexIdBase + 1 || gl_id > kTexIdBase + kMaxTextures) {
    return nullptr;
  }
  TexObj* t = &g_textures[gl_id - kTexIdBase - 1];
  return t->live ? t : nullptr;
}

FboObj* fbo_by_id(u32 id) {
  if (id == 0 || id > kMaxFbos) {
    return nullptr;
  }
  FboObj* f = &g_fbos[id - 1];
  return f->live ? f : nullptr;
}

// Create the dktexgpu render target on first need. Returns 0 ONLY if the Switch-side
// create fails; on host (no dk device) materializes to a deterministic fake id so
// tests can assert "second resolve -> same id, one creation".
u32 materialize(TexObj& t, u32 gl_id) {
  if (t.dk_tex_id) {
    return t.dk_tex_id;
  }
  if (t.depth && !t.renderbuffer) {
    // T126: depth textures are the frame depth (see set_frame_depth_tex); not cached, so
    // a later install is picked up
    return g_frame_depth_tex;
  }
  if (t.levels == 0 || t.w == 0 || t.h == 0) {
    ++g_rt_no_dims_count;  // no tex_image_2d seen: 1x1x1 placeholder target
  }
  const u32 w = t.w > 0 ? (u32)t.w : 1;
  const u32 h = t.h > 0 ? (u32)t.h : 1;
  const u32 levels = t.levels > 0 ? t.levels : 1;
#if defined(__SWITCH__)
  // T23 activity-check pattern (TexturePool.cpp): only touch dktexgpu when it is real.
  if (dkdev::is_up() && dktexgpu::active()) {
    t.dk_tex_id = t.depth ? dktexgpu::create_depth_target(w, h)
                          : dktexgpu::create_render_target_mipped(w, h, levels, t.rgb);
    return t.dk_tex_id;  // 0 = create failed (caller reports false)
  }
#endif
  t.dk_tex_id = kTexIdBase + 0x1000 + (gl_id - kTexIdBase);  // host fake
  return t.dk_tex_id;
}

}  // namespace

// ---- textures -----------------------------------------------------------------
void gen_textures(int n, u32* out) {
  for (int i = 0; i < n; ++i) {
    out[i] = 0;
    for (u32 attempt = 0; attempt < kMaxTextures && out[i] == 0; ++attempt) {
      const u32 slot = g_next_tex;
      g_next_tex = (g_next_tex + 1) % kMaxTextures;
      if (!g_textures[slot].live) {
        g_textures[slot] = TexObj{};
        g_textures[slot].live = true;
        out[i] = kTexIdBase + slot + 1;
      }
    }
    if (out[i] == 0) {
      note_table_full("textures");
    }
  }
}
void delete_textures(int n, const u32* in) {
  for (int i = 0; i < n; ++i) {
    TexObj* t = tex_by_gl(in[i]);
    if (t) {
      *t = TexObj{};  // drops dk_tex_id: the image memory is NOT freed (P8, by design)
    }
    for (auto it = g_exported.begin(); it != g_exported.end();) {
      it = it->second == in[i] ? g_exported.erase(it) : std::next(it);
    }
    forget_texture_binding(in[i]);
  }
}
void bind_texture(u32 name) {
  // >= kTexIdBase: one of OUR ids; any other nonzero name is a real dktexgpu id that
  // came through the pool path -- record it as-is. 0 unbinds. No GL work.
  // T128: a dktexgpu id that resolve_texture_id handed out (the TexturePool seam keeps it in
  // a tbp slot, e.g. the jak2 Warp frame copy) binds as its dkgl texture again -- like a GL
  // name it then follows a resize (respecify), gets its T106 barrier and its tex params.
  if (name != 0 && name < kTexIdBase) {
    const auto it = g_exported.find(name);
    if (it != g_exported.end()) {
      name = it->second;
    }
  }
  set_bound_texture(name);
}
void tex_parameteri(u32 pname, u32 param) {
  const u32 bound = bound_texture(active_unit());
  TexObj* t = tex_by_gl(bound);
  if (!t) {
    if (bound != 0 && bound < kTexIdBase) {
      PoolTexParams& p = g_pool_params[bound];
      if (pname == gl::TEXTURE_MIN_FILTER) {
        p.min_filter = param;
      } else if (pname == gl::TEXTURE_MAG_FILTER) {
        p.mag_filter = param;
      } else if (pname == gl::TEXTURE_WRAP_S) {
        p.wrap_s = param;
      } else if (pname == gl::TEXTURE_WRAP_T) {
        p.wrap_t = param;
      } else {
        ++g_bad_tex_param_count;
      }
    }
    return;
  }
  if (pname == gl::TEXTURE_MIN_FILTER) {
    t->min_filter = param;
  } else if (pname == gl::TEXTURE_MAG_FILTER) {
    t->mag_filter = param;
  } else if (pname == gl::TEXTURE_WRAP_S) {
    t->wrap_s = param;
  } else if (pname == gl::TEXTURE_WRAP_T) {
    t->wrap_t = param;
  } else {
    ++g_bad_tex_param_count;  // stored nowhere, but the call is not an error
  }
}
void gen_samplers(int n, u32* out) {
  for (int i = 0; i < n; ++i) {
    out[i] = 0;
    for (u32 s = 0; s < kMaxSamplers; ++s) {
      if (!g_samplers[s].live) {
        g_samplers[s].live = true;
        g_samplers[s].p = {gl::NEAREST_MIPMAP_LINEAR, gl::LINEAR, gl::REPEAT, gl::REPEAT};
        out[i] = s + 1;
        break;
      }
    }
  }
}
void delete_samplers(int n, const u32* in) {
  for (int i = 0; i < n; ++i) {
    if (SamplerObj* s = sampler_by_id(in[i])) {
      s->live = false;
      for (u32& u : g_unit_sampler) {
        u = u == in[i] ? 0 : u;
      }
    }
  }
}
void bind_sampler(u32 unit, u32 sampler) {
  if (unit < kMaxTexUnits) {
    g_unit_sampler[unit] = sampler;
  }
}
void sampler_parameteri(u32 sampler, u32 pname, u32 param) {
  SamplerObj* s = sampler_by_id(sampler);
  if (!s) {
    return;
  }
  if (pname == gl::TEXTURE_MIN_FILTER) {
    s->p.min_filter = param;
  } else if (pname == gl::TEXTURE_MAG_FILTER) {
    s->p.mag_filter = param;
  } else if (pname == gl::TEXTURE_WRAP_S) {
    s->p.wrap_s = param;
  } else if (pname == gl::TEXTURE_WRAP_T) {
    s->p.wrap_t = param;
  }
}
bool tex_image_2d(int level, u32 internal_format, int w, int h, u32 format, u32 type,
                  const void* data) {
  (void)format;
  TexObj* t = tex_by_gl(bound_texture(active_unit()));
  if (!t || level < 0 || level >= 16) {
    return false;
  }
  const bool rgb = internal_format == gl::RGB;
  const bool depth =
      internal_format == gl::DEPTH_COMPONENT || internal_format == gl::DEPTH24_STENCIL8;
  if (depth) {
    // T126: GlowRenderer's probe depth texture. Never materialized as its own image: it
    // aliases the frame depth (blit_framebuffer DEPTH_BUFFER_BIT), so only record the dims.
    t->depth = true;
    t->renderbuffer = false;
    t->dk_tex_id = 0;
    t->w = w;
    t->h = h;
    t->levels = 1;
    return true;
  }
  if (internal_format != gl::RGBA && internal_format != gl::RGBA8 && !rgb) {
    ++g_bad_tex_format_count;
  }
  if (type != gl::UNSIGNED_INT_8_8_8_8_REV && type != gl::UNSIGNED_BYTE) {
    ++g_bad_tex_format_count;
  }
  if (level == 0) {
    // level 0 fixes the dimensions (mip chain: 128,64,...,1). T112: a later level-0 call
    // with other dims/format RESPECIFIES the texture -- the old dk image is dropped (not
    // freed, P8) and a new one is materialized lazily; FBOs resolve their attachment lazily
    // too (pending_target_dk_tex), so they follow automatically.
    const bool changed = t->levels == 0 || t->w != w || t->h != h || t->rgb != rgb;
    if (changed) {
      if (t->dk_tex_id != 0) {
        ++g_respecify_count;
      }
      t->dk_tex_id = 0;
      t->was_rendered_to = false;
      t->barrier_done = false;
      t->w = w;
      t->h = h;
      t->rgb = rgb;
      t->levels = 1;
    }
  } else if ((u32)level + 1 > t->levels) {
    t->levels = (u32)level + 1;
  }
  if (data && level == 0) {
    // TextureAnimator uploads (clut-blender outputs, fixed-anim sources, vram entries,
    // GL_RED sky noise): level-0 texels into the lazily created image through the
    // deferred staging ring (copies land before the next frame's draws). Other levels
    // come from glGenerateMipmap.
    ++g_tex_data_count;
#if defined(__SWITCH__)
    if (dkdev::is_up() && dktexgpu::active()) {
      const u32 dk = materialize(*t, bound_texture(active_unit()));
      const void* px = data;
      std::vector<u32> expanded;
      if (format == gl::RED && type == gl::UNSIGNED_BYTE) {
        // GL samples a RED texture as (r, 0, 0, 1)
        const u8* src = static_cast<const u8*>(data);
        expanded.resize((size_t)w * h);
        for (size_t i = 0; i < expanded.size(); i++) {
          expanded[i] = (u32)src[i] | 0xff000000u;
        }
        px = expanded.data();
      } else if (format != gl::RGBA) {
        dk_failed_upload();
        return true;
      }
      if (dk == 0 || !dktexgpu::update_rgba8_deferred(dk, (u32)w, (u32)h, px)) {
        dk_failed_upload();
      }
    }
#endif
  }
  return true;
}

// §7.0 D (AI-assisted): tfrag time-of-day palettes (GL TFragment). The tod texture is
// created with a null glTexImage2D (dims only, level 0 fixes 8192x1), then every palette
// change rewrites the top-left color_count x 1 texels -- exactly update_rgba8's top-left
// sub-rect semantics. The dk image is materialized lazily on the first sub-rect at the
// exact palette size (the null tex_image_2d reserved nothing; tod textures are never
// rendered to), and later updates go through the deferred staging ring (the same path as
// the native tod palettes; dk_renderer records those copies before any draw).
bool tex_sub_image_2d(int level, int xoff, int yoff, int w, int h, u32 format, u32 type,
                      const void* pixels) {
  TexObj* t = tex_by_gl(bound_texture(active_unit()));
  if (!t || level != 0 || !pixels) {
    return false;
  }
  if (format != gl::RGBA || (type != gl::UNSIGNED_BYTE && type != gl::UNSIGNED_INT_8_8_8_8_REV)) {
    ++g_bad_tex_format_count;
    return false;
  }
  if (xoff != 0 || yoff != 0 || w <= 0 || h <= 0) {
    ++g_bad_sub_rect_count;
    return false;
  }
  ++g_tex_sub_count;
#if defined(__SWITCH__)
  if (t->dk_tex_id == 0 || w > t->w || h > t->h) {
    // first palette of this texture (or a bigger one): (re)materialize at the exact
    // sub-rect size. The old image, if any, is dropped, not freed (P8), like the
    // respecify path above.
    if (t->dk_tex_id != 0) {
      ++g_respecify_count;
    }
    const u32 id = dktexgpu::upload_rgba8((u32)w, (u32)h, pixels);
    if (id == 0) {
      return false;
    }
    t->dk_tex_id = id;
    t->w = w;
    t->h = h;
    t->levels = 1;
    t->was_rendered_to = false;
    t->barrier_done = false;
    return true;
  }
  return dktexgpu::update_rgba8_deferred(t->dk_tex_id, (u32)w, (u32)h, pixels);
#else
  // Host build (goalc-test): same state bookkeeping as the Switch path, no GPU work --
  // mirrors how tex_image_2d behaves off-console.
  if (t->dk_tex_id == 0 || w > t->w || h > t->h) {
    t->w = w;
    t->h = h;
    t->levels = 1;
  }
  return true;
#endif
}

// ---- FBOs ----------------------------------------------------------------------
void gen_framebuffers(int n, u32* out) {
  for (int i = 0; i < n; ++i) {
    out[i] = 0;
    for (u32 attempt = 0; attempt < kMaxFbos && out[i] == 0; ++attempt) {
      const u32 slot = g_next_fbo;
      g_next_fbo = (g_next_fbo + 1) % kMaxFbos;
      if (!g_fbos[slot].live) {
        g_fbos[slot] = FboObj{};
        g_fbos[slot].live = true;
        out[i] = slot + 1;  // ids 1..kMaxFbos, 0 reserved for the frame target
      }
    }
    if (out[i] == 0) {
      note_table_full("framebuffers");
    }
  }
}
void delete_framebuffers(int n, const u32* in) {
  for (int i = 0; i < n; ++i) {
    FboObj* f = fbo_by_id(in[i]);
    if (f) {
      *f = FboObj{};
    }
    if (g_bound_fbo == in[i]) {
      g_bound_fbo = 0;  // deleting the bound FBO falls back to the frame target
    }
  }
}
void bind_framebuffer_target(u32 target, u32 name) {
  if (target == gl::READ_FRAMEBUFFER) {
    g_read_fbo = fbo_by_id(name) ? name : 0;  // blit source only: no target switch
    return;
  }
  if (target != gl::DRAW_FRAMEBUFFER) {
    g_read_fbo = fbo_by_id(name) ? name : 0;  // GL_FRAMEBUFFER binds both
  }
  bind_framebuffer(name);
}
bool blit_framebuffer(int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
                      u32 mask, u32 filter) {
  FboObj* f = fbo_by_id(g_bound_fbo);
  if (mask == gl::DEPTH_BUFFER_BIT) {
    // T126: GlowRenderer::blit_depth (frame depth -> probe FBO depth texture, same size).
    // No GPU copy: the depth texture IS the frame depth when sampled. Mark it written so
    // the first draw sampling it gets the T106 barrier (the world passes wrote it).
    TexObj* d = f ? tex_by_gl(f->depth_attached) : nullptr;
    if (g_read_fbo != 0 || !d || !d->depth || d->renderbuffer || g_frame_depth_tex == 0) {
      ++g_unsupported_blit_count;
      return false;
    }
    d->was_rendered_to = true;
    d->barrier_done = false;
    return true;
  }
  // DepthCue: frame (read FBO 0) -> its sample texture, colour, no flip.
  TexObj* t = f ? tex_by_gl(f->attached_tex) : nullptr;
  if (g_read_fbo != 0 || !t || mask != gl::COLOR_BUFFER_BIT || sx1 <= sx0 || sy1 <= sy0 ||
      dx1 <= dx0 || dy1 <= dy0 || sx0 < 0 || sy0 < 0 || dx0 < 0 || dy0 < 0) {
    ++g_unsupported_blit_count;
    return false;
  }
  const u32 dk = materialize(*t, f->attached_tex);
  if (dk == 0) {
    ++g_unsupported_blit_count;
    return false;
  }
  const DkImageRect src{(u32)sx0, (u32)sy0, 0, (u32)(sx1 - sx0), (u32)(sy1 - sy0), 1};
  const DkImageRect dst{(u32)dx0, (u32)dy0, 0, (u32)(dx1 - dx0), (u32)(dy1 - dy0), 1};
  t->was_rendered_to = true;  // the first draw sampling it gets the T106 barrier
  t->barrier_done = false;
  if (Emitter* e = emitter()) {
    e->blit_frame_to_tex(dk, src, dst, filter == gl::LINEAR);
  }
  return true;
}
u64 unsupported_blit_count() {
  return g_unsupported_blit_count;
}
// T129a (AI-assisted): glGenerateMipmap. The TextureAnimator composites each frame into
// the FBO target's level 0 and then regenerates the chain; dkgl allocated the whole mip
// chain (create_render_target_mipped) but only level 0 is ever written, so the small mips
// sample garbage (HW: skull gems were flat orange discs). There is no readback on the
// Switch: queue GPU downsample blits level L-1 -> L. The emitter records them into the
// CURRENT bucket (the tex-anim list), right after the composite pass.
// Guard: only a dkgl-created mipped COLOUR target can do this -- pool ids never resolve
// here, and tex_sub_image_2d materializes single-level images and resets `levels`, so
// `levels > 1` implies the image was created with that many mip levels.
void generate_mips_flush(u32 gl_id) {
  TexObj* t = tex_by_gl(gl_id);
  if (!t || t->depth || t->renderbuffer || t->levels <= 1 || t->w <= 0 || t->h <= 0) {
    return;
  }
  const u32 dk = materialize(*t, gl_id);
  if (dk == 0) {
    return;  // Switch-side create failed (the host fake-id scheme never fails)
  }
  t->was_rendered_to = true;  // mips 1+ written: the next sample needs the T106 barrier
  t->barrier_done = false;
  if (Emitter* e = emitter()) {
    e->generate_mips(dk, t->levels, (u32)t->w, (u32)t->h);
  }
}
void bind_framebuffer(u32 name) {
  // Leaving an offscreen FBO means its texture was rendered to: T106 flushes the
  // render-to-texture barrier before the first draw that samples it. Re-binding the
  // SAME fbo (one render pass, many state groups) is not a leave.
  FboObj* prev = fbo_by_id(g_bound_fbo);
  if (prev && g_bound_fbo != name) {
    if (TexObj* t = tex_by_gl(prev->attached_tex)) {
      t->was_rendered_to = true;
      t->barrier_done = false;  // written again: the next sample needs a new barrier
    }
  }
  g_bound_fbo = fbo_by_id(name) ? name : 0;
  // T106: the switch is QUEUED, not emitted -- it flushes at the next draw, before
  // everything else (the ocean can bind an FBO and set up state before drawing). The
  // attachment is resolved when the switch is consumed (pending_target_dk_tex/level):
  // FramebufferTexturePairContext binds the FBO first and attaches the level after.
  g_pending_target.active = true;
  g_pending_target.offscreen = (g_bound_fbo != 0);
  g_pending_target.dk_tex_id = 0;
}
bool framebuffer_texture_2d(u32 target, u32 attachment, u32 tex_name, int level) {
  (void)target;      // GL_FRAMEBUFFER only, in the ocean
  FboObj* f = fbo_by_id(g_bound_fbo);
  if (!f) {
    return false;
  }
  if (attachment == gl::DEPTH_ATTACHMENT || attachment == gl::DEPTH_STENCIL_ATTACHMENT) {
    // T126: GlowRenderer's probe FBO depth texture. Only a blit target (frame depth alias):
    // nothing is materialized here.
    if (f->depth_attached != tex_name) {
      f->depth_attached = tex_name;
      g_pending_target.active = true;
      g_pending_target.offscreen = true;
    }
    return tex_by_gl(tex_name) != nullptr;
  }
  if (f->attached_tex != tex_name || f->level != (u32)level) {
    f->attached_tex = tex_name;
    f->level = level > 0 ? (u32)level : 0;
    g_pending_target.active = true;  // same FBO, new attachment: the target changed
    g_pending_target.offscreen = true;
  }
  TexObj* t = tex_by_gl(tex_name);
  if (!t) {
    return false;
  }
  if (t->dk_tex_id == 0) {
    return materialize(*t, tex_name) != 0;  // false only if the dk create failed
  }
  return true;
}
// ---- T126 renderbuffers (depth only) ----------------------------------------------
void gen_renderbuffers(int n, u32* out) {
  gen_textures(n, out);
  for (int i = 0; i < n; ++i) {
    if (TexObj* t = tex_by_gl(out[i])) {
      t->depth = true;
      t->renderbuffer = true;
    }
  }
}
void delete_renderbuffers(int n, const u32* in) {
  delete_textures(n, in);
  for (int i = 0; i < n; ++i) {
    if (g_bound_rb == in[i]) {
      g_bound_rb = 0;
    }
  }
}
void bind_renderbuffer(u32 name) {
  g_bound_rb = name;
}
void renderbuffer_storage(u32 internal_format, int w, int h) {
  TexObj* t = tex_by_gl(g_bound_rb);
  if (!t || !t->renderbuffer) {
    return;
  }
  if (internal_format != gl::DEPTH24_STENCIL8 && internal_format != gl::DEPTH_COMPONENT) {
    ++g_bad_tex_format_count;  // colour renderbuffers: no user
  }
  if (t->w != w || t->h != h) {
    t->dk_tex_id = 0;  // respecified: a new image is materialized lazily (old one not freed, P8)
  }
  t->w = w;
  t->h = h;
  t->levels = 1;
}
bool framebuffer_renderbuffer(u32 attachment, u32 rb_name) {
  FboObj* f = fbo_by_id(g_bound_fbo);
  TexObj* t = tex_by_gl(rb_name);
  if (!f || (rb_name != 0 && !t)) {
    return false;
  }
  if (attachment != gl::DEPTH_ATTACHMENT && attachment != gl::DEPTH_STENCIL_ATTACHMENT) {
    ++g_bad_draw_buffers_count;  // colour renderbuffers: no user
    return false;
  }
  if (f->depth_attached != rb_name) {
    f->depth_attached = rb_name;
    g_pending_target.active = true;
    g_pending_target.offscreen = true;
  }
  return true;
}
void set_frame_depth_tex(u32 dk_tex_id) {
  g_frame_depth_tex = dk_tex_id;
}

void draw_buffers(int n, const u32* bufs) {
  if (n != 1 || !bufs || bufs[0] != gl::COLOR_ATTACHMENT0) {
    ++g_bad_draw_buffers_count;
  }
}
u32 check_framebuffer_status() {
  return gl::FRAMEBUFFER_COMPLETE;
}

// ---- pool seam + T106 accessors -------------------------------------------------
u32 resolve_texture_id(u32 gl_id) {
  if (gl_id < kTexIdBase) {
    return gl_id;  // not a dkgl pseudo id (real dktexgpu id from the pool path)
  }
  TexObj* t = tex_by_gl(gl_id);
  if (!t) {
    return gl_id;
  }
  const u32 dk = materialize(*t, gl_id);  // T126: depth textures return the frame alias
  if (dk != 0 && !t->depth) {
    g_exported[dk] = gl_id;  // T128
  }
  return dk != 0 ? dk : gl_id;
}
u32 materialized_dk_tex(u32 gl_id) {
  TexObj* t = tex_by_gl(gl_id);
  return t ? t->dk_tex_id : 0;
}

// §7.0 F (AI-assisted): the jak2 anim slot table (dkgl_frame owns it, GL ids).
namespace {
const u32* g_anim_slots = nullptr;
size_t g_anim_slot_count = 0;
}  // namespace
void set_anim_slots(const u32* slots, size_t count) {
  g_anim_slots = slots;
  g_anim_slot_count = count;
}
u32 anim_slot_dk_tex(int slot) {
  if (!g_anim_slots || slot < 0 || (size_t)slot >= g_anim_slot_count) {
    return 0;
  }
  const u32 gl = g_anim_slots[slot];
  return gl < kTexIdBase ? gl : materialized_dk_tex(gl);
}
bool texture_needs_barrier(u32 gl_id) {
  TexObj* t = tex_by_gl(gl_id);
  return t && t->was_rendered_to && !t->barrier_done;
}
void texture_barrier_sent(u32 gl_id) {
  if (TexObj* t = tex_by_gl(gl_id)) {
    t->barrier_done = true;
  }
}

// ---- T106 seams: queued target switch + sampler combo ---------------------------
bool pending_target_switch() {
  return g_pending_target.active;
}
bool pending_target_offscreen() {
  return g_pending_target.offscreen;
}
u32 pending_target_dk_tex() {
  FboObj* f = fbo_by_id(g_bound_fbo);
  if (!g_pending_target.offscreen || !f) {
    return 0;
  }
  TexObj* t = tex_by_gl(f->attached_tex);
  return t ? materialize(*t, f->attached_tex) : 0;  // lazily, at flush time
}
u32 pending_target_depth_dk_tex() {
  FboObj* f = fbo_by_id(g_bound_fbo);
  if (!g_pending_target.offscreen || !f) {
    return 0;
  }
  TexObj* t = tex_by_gl(f->depth_attached);
  // only renderbuffers are depth TARGETS; a depth texture attachment is a blit alias
  return (t && t->renderbuffer) ? materialize(*t, f->depth_attached) : 0;
}
u32 pending_target_level() {
  FboObj* f = fbo_by_id(g_bound_fbo);
  return (g_pending_target.offscreen && f) ? f->level : 0;
}
u32 bound_framebuffer() {
  return g_bound_fbo;
}
void pending_target_consumed() {
  g_pending_target.active = false;
}
u32 texture_sampler_combo(u32 unit, u32 gl_id) {
  TexObj* t = tex_by_gl(gl_id);
  PoolTexParams pool;
  u32 min_filter, mag_filter, wrap_s, wrap_t;
  const SamplerObj* so = unit < kMaxTexUnits ? sampler_by_id(g_unit_sampler[unit]) : nullptr;
  if (so) {
    min_filter = so->p.min_filter;
    mag_filter = so->p.mag_filter;
    wrap_s = so->p.wrap_s;
    wrap_t = so->p.wrap_t;
  } else if (t) {
    min_filter = t->min_filter;
    mag_filter = t->mag_filter;
    wrap_s = t->wrap_s;
    wrap_t = t->wrap_t;
  } else {
    // pool textures: what glTexParameteri set on them, else the TexturePool upload
    // defaults (LINEAR/LINEAR + CLAMP)
    auto it = g_pool_params.find(gl_id);
    if (it != g_pool_params.end()) {
      pool = it->second;
    }
    min_filter = pool.min_filter;
    mag_filter = pool.mag_filter;
    wrap_s = pool.wrap_s;
    wrap_t = pool.wrap_t;
  }
  u32 combo = 0;
  switch (min_filter) {
    case gl::LINEAR:
    case gl::LINEAR_MIPMAP_NEAREST:
    case gl::LINEAR_MIPMAP_LINEAR:
      combo |= 1;  // min_linear
      break;
    default:
      break;
  }
  if (mag_filter == gl::LINEAR) {
    combo |= 2;
  }
  // Horizon A/B (AI-assisted): gk_dkgl_nomip.txt samples level 0 only (DkMipFilter_None),
  // to test whether the ocean's small mips are black on hardware.
  static const bool s_no_mip = std::fopen("sdmc:/switch/jak1/gk_dkgl_nomip.txt", "r") != nullptr;
  if (!s_no_mip &&
      (min_filter == gl::LINEAR_MIPMAP_LINEAR || min_filter == gl::NEAREST_MIPMAP_LINEAR)) {
    combo |= 4;  // mip_linear
  }
  if (wrap_s == gl::REPEAT) {
    combo |= 8;
  }
  if (wrap_t == gl::REPEAT) {
    combo |= 16;
  }
  return combo;
}

}  // namespace dkgl
