// T106 (AI-assisted): dkgl draw emission.
// draw_arrays/draw_elements (dkgl_state.cpp) funnel into flush_pending, which turns
// the lazy shadow state into a minimal DIFFED command stream through the Emitter:
// only what changed since the last draw is re-emitted (begin_bucket resets the cache),
// but UNIFORMS are snapshotted into a FRESH UBO slice on EVERY draw (brief lesson:
// the GL side may rewrite uniforms between draws that share all other state).
// Row I pass 2 (AI-assisted): the derivation itself is now diffed too -- a draw whose
// VAO serial + buffer epoch + texture-param epoch all match the last draw skips the
// vtx-format rebuild (step 7) and the texture bind emit (step 9) outright.
//
// Blend enable: DkBlendState has no enable bit (deko3d 0.5.0) -- a disabled GL blend
// is encoded as factors ONE/ZERO with op ADD (result = src), the canonical mapping.
#include <cstdio>
#include <cstring>

#include "game/graphics/deko3d_renderer/dkgl/dkgl_gl_enums.h"
#include "game/graphics/deko3d_renderer/dkgl/dkgl_state.h"
#include "game/switch/run_log.h"  // #J12: notex probe logs (host-safe no-op off Switch)

namespace dkgl {
namespace {

// ---- last-emitted cache (diffing) ----------------------------------------------
struct EmitCache {
  bool valid = false;  // false until the first draw / after begin_bucket
  u32 program = 0;
  u32 program_variant = 0;  // 1 = direct_basic_textured_fast (per-draw decision)
  DkDepthStencilState depth{};
  DkBlendState blend{};
  float blend_color[4] = {-1.f, -1.f, -1.f, -1.f};  // T111
  DkViewport vp{};
  const void* vao = nullptr;  // bound_vao() identity
  u64 vtx_gpu[kMaxAttribs] = {};
  u32 vtx_bytes[kMaxAttribs] = {};
  u32 n_vtx_bufs = 0;
  DkIdxFormat idx_fmt = DkIdxFormat_Uint32;
  u64 idx_gpu = 0;
  bool restart_on = false;
  u32 restart_index = 0xFFFF'FFFFu;
  u32 stencil_dyn = 0;   // T113: write_mask << 16 | ref << 8 | func_mask
  u32 color_mask = 0xF;  // T113
  u64 ubo_gpu[kMaxUboBindings] = {};  // TODO #5: glBindBufferRange addresses last bound
  u32 ubo_size[kMaxUboBindings] = {};
  u64 uni7_gpu = 0;  // Row I: binding-7 uniform slice last emitted (0 = none yet)
  u32 uni7_vs = 0;   // Row I: its vert / frag block sizes
  u32 uni7_fs = 0;
  // Row I pass 2 (AI-assisted): fast-path state. vao_serial/buf_epoch gate the step-7
  // vtx-format derivation skip; tex_* gate the step-9 texture bind emit skip.
  u32 vao_serial = 0;              // serial of the VAO the cached format came from
  u64 buf_epoch = 0;               // buffer re-spec epoch at cache time
  u32 tex_gl[kMaxTexUnits] = {};   // per unit: last-bound GL texture id (0 = none)
  u32 tex_dk[kMaxTexUnits] = {};   // per unit: the dk texture id it resolved to
  u32 tex_epoch = 0;               // tex-param epoch the cached binds were made at
};
EmitCache g_cache;
u64 g_inert_draw_count = 0;
u64 g_bad_blend_factor_count = 0;
u64 g_bad_depth_func_count = 0;
u64 g_bad_prim_count = 0;
u64 g_bad_stencil_count = 0;  // T113
u64 g_no_texture_draws = 0;   // §7.2 phase 2: draws whose every sampler was unbound
u64 g_bad_attrib_count = 0;
bool g_int_attrib_draws = false;
u64 g_vtx_stride_mismatch_count = 0;
u64 g_missing_element_buffer_count = 0;
u64 g_bad_ubo_count = 0;  // TODO #5: draw whose bound uniform range has no live data
bool g_logged_bad_attrib = false;
// (AI-assisted): the skip counters below existed but were never printed, and the GL_INT
// allow-list policy skip was not even counted. One-shot stderr logs + per-reason counters
// so gk_run_log.txt can say WHY a draw vanished.
u64 g_int_attrib_skip_count = 0;  // GL_INT attribs outside the allow-list (policy skip)
bool g_logged_int_attrib_skip = false;
// Row I pass 2 (AI-assisted): fast-path hit counters for the 10 s perf line.
u64 g_vao_fast_hits = 0;    // draws that skipped the step-7 vtx-format derivation
u64 g_tex_bind_skips = 0;   // texture binds suppressed in step 9 (same id + same params)
bool g_logged_dead_attrib_buffer = false;
bool g_logged_no_attribs = false;
bool g_logged_missing_element = false;
bool g_logged_bad_ubo = false;

// ---- maps (spellings verbatim from deko3d.h 0.5.0; check-dk.py validates) ------
bool blend_factor_map(u32 gl, DkBlendFactor* out) {
  switch (gl) {
    case gl::ZERO:
      *out = DkBlendFactor_Zero;
      return true;
    case gl::ONE:
      *out = DkBlendFactor_One;
      return true;
    case gl::SRC_COLOR:
      *out = DkBlendFactor_SrcColor;
      return true;
    case gl::ONE_MINUS_SRC_COLOR:
      *out = DkBlendFactor_InvSrcColor;
      return true;
    case gl::SRC_ALPHA:
      *out = DkBlendFactor_SrcAlpha;
      return true;
    case gl::ONE_MINUS_SRC_ALPHA:
      *out = DkBlendFactor_InvSrcAlpha;
      return true;
    case gl::DST_ALPHA:
      *out = DkBlendFactor_DstAlpha;
      return true;
    case gl::ONE_MINUS_DST_ALPHA:
      *out = DkBlendFactor_InvDstAlpha;
      return true;
    case gl::DST_COLOR:
      *out = DkBlendFactor_DstColor;
      return true;
    case gl::ONE_MINUS_DST_COLOR:
      *out = DkBlendFactor_InvDstColor;
      return true;
    case gl::CONST_COLOR:
      *out = DkBlendFactor_ConstColor;
      return true;
    default:
      return false;
  }
}
DkBlendOp blend_op_map(u32 eq, bool* ok) {
  *ok = true;
  switch (eq) {
    case gl::FUNC_ADD:
      return DkBlendOp_Add;
    case gl::FUNC_SUBTRACT:
      return DkBlendOp_Sub;
    case gl::FUNC_REVERSE_SUBTRACT:
      return DkBlendOp_RevSub;
    default:
      *ok = false;  // counter + treat as Add
      return DkBlendOp_Add;
  }
}
DkCompareOp depth_func_map(u32 func, bool* ok) {
  *ok = true;
  switch (func) {
    case gl::NEVER:
      return DkCompareOp_Never;
    case gl::LESS:
      return DkCompareOp_Less;
    case gl::EQUAL:
      return DkCompareOp_Equal;
    case gl::LEQUAL:
      return DkCompareOp_Lequal;
    case gl::GREATER:
      return DkCompareOp_Greater;
    case gl::NOTEQUAL:
      return DkCompareOp_NotEqual;
    case gl::GEQUAL:
      return DkCompareOp_Gequal;
    case gl::ALWAYS:
      return DkCompareOp_Always;
    default:
      *ok = false;  // counter + Always (test disabled anyway)
      return DkCompareOp_Always;
  }
}
DkStencilOp stencil_op_map(u32 op, bool* ok) {
  switch (op) {
    case gl::KEEP:
      return DkStencilOp_Keep;
    case gl::ZERO:
      return DkStencilOp_Zero;
    case gl::REPLACE:
      return DkStencilOp_Replace;
    case gl::INCR:
      return DkStencilOp_Incr;
    case gl::DECR:
      return DkStencilOp_Decr;
    case gl::INVERT:
      return DkStencilOp_Invert;
    case gl::INCR_WRAP:
      return DkStencilOp_IncrWrap;
    case gl::DECR_WRAP:
      return DkStencilOp_DecrWrap;
    default:
      *ok = false;  // counter + Keep (leave the buffer alone)
      return DkStencilOp_Keep;
  }
}
bool prim_map(u32 mode, DkPrimitive* out) {
  switch (mode) {
    case gl::TRIANGLES:
      *out = DkPrimitive_Triangles;
      return true;
    case gl::TRIANGLE_STRIP:
      *out = DkPrimitive_TriangleStrip;
      return true;
    case gl::LINES:
      *out = DkPrimitive_Lines;
      return true;
    case gl::TRIANGLE_FAN:
      *out = DkPrimitive_TriangleFan;
      return true;
    default:
      return false;
  }
}
bool idx_fmt_map(u32 type, DkIdxFormat* out) {
  switch (type) {
    case gl::UNSIGNED_INT:
      *out = DkIdxFormat_Uint32;
      return true;
    case gl::UNSIGNED_SHORT:
      *out = DkIdxFormat_Uint16;
      return true;
    default:
      return false;
  }
}
// GL (size,type,normalized) -> DkVtxAttribSize + DkVtxAttribType. Field order of
// DkVtxAttribState is {buffer, isFixed, offset, size, type, isBgra} (recipe #2).
// NOTE: the Nx32 sizes are NOT contiguous values (1x32=0x12, 2x32=0x04, 3x32=0x02,
// 4x32=0x01) -- explicit switch, never arithmetic.
bool attrib_map(int size, u32 type, bool normalized, bool integer, DkVtxAttribSize* sz,
                DkVtxAttribType* ty) {
  if (type == gl::FLOAT) {
    switch (size) {
      case 1:
        *sz = DkVtxAttribSize_1x32;
        break;
      case 2:
        *sz = DkVtxAttribSize_2x32;
        break;
      case 3:
        *sz = DkVtxAttribSize_3x32;
        break;
      case 4:
        *sz = DkVtxAttribSize_4x32;
        break;
      default:
        return false;
    }
    *ty = DkVtxAttribType_Float;
    return true;
  }
  // dkgl review: integer attribs (glVertexAttribIPointer) read as Uint, normalized ones as
  // Unorm, plain ones as Uscaled (GL converts them to float without normalizing).
  // CommonOceanRenderer's fog is an IPointer of 4 u8 (all its draws were skipped before).
  const DkVtxAttribType int_ty =
      integer ? DkVtxAttribType_Uint : (normalized ? DkVtxAttribType_Unorm : DkVtxAttribType_Uscaled);
  if (type == gl::UNSIGNED_INT || type == gl::INT) {
    // GL_INT: TextureAnimator's vertex_index (glVertexAttribIPointer, 1 x GL_INT)
    switch (size) {
      case 1:
        *sz = DkVtxAttribSize_1x32;
        break;
      case 2:
        *sz = DkVtxAttribSize_2x32;
        break;
      case 3:
        *sz = DkVtxAttribSize_3x32;
        break;
      case 4:
        *sz = DkVtxAttribSize_4x32;
        break;
      default:
        return false;
    }
    if (type == gl::INT) {
      *ty = integer ? DkVtxAttribType_Sint
                    : (normalized ? DkVtxAttribType_Snorm : DkVtxAttribType_Sscaled);
    } else {
      *ty = integer ? DkVtxAttribType_Uint : DkVtxAttribType_Uscaled;
    }
    return true;
  }
  if (type == gl::UNSIGNED_BYTE) {
    switch (size) {
      case 1:
        *sz = DkVtxAttribSize_1x8;
        break;
      case 2:
        *sz = DkVtxAttribSize_2x8;
        break;
      case 3:
        *sz = DkVtxAttribSize_3x8;
        break;
      case 4:
        *sz = DkVtxAttribSize_4x8;
        break;
      default:
        return false;
    }
    *ty = int_ty;
    return true;
  }
  if (type == gl::UNSIGNED_SHORT) {
    // §7.0 D: tfrag3 verts pack the time-of-day color index as 1 x u16 (an
    // IPointer), which the map above would silently drop.
    switch (size) {
      case 1:
        *sz = DkVtxAttribSize_1x16;
        break;
      case 2:
        *sz = DkVtxAttribSize_2x16;
        break;
      case 3:
        *sz = DkVtxAttribSize_3x16;
        break;
      case 4:
        *sz = DkVtxAttribSize_4x16;
        break;
      default:
        return false;
    }
    *ty = int_ty;
    return true;
  }
  if (type == gl::INT_2_10_10_10_REV) {
    *sz = DkVtxAttribSize_10_10_10_2;
    *ty = DkVtxAttribType_Snorm;  // brief pitfall: the packed format is SNORM in dk
    return true;
  }
  return false;
}

}  // namespace

bool g_no_ubo_reuse = false;  // J18 A/B: gk_dkgl_nouboreuse.txt (set in dkgl_frame.cpp)

void reset_emit_cache() {
  g_cache = EmitCache{};
}
void restore_bucket_end_state() {
  Emitter* e = emitter();
  if (!e || !g_cache.valid) {
    return;  // nothing drawn in this bucket: the GPU still has the segment defaults
  }
  if (g_cache.color_mask != 0xFu) {
    e->set_color_mask(0xFu);
    g_cache.color_mask = 0xFu;
  }
  if (g_cache.depth.stencilTestEnable) {
    DkDepthStencilState d = g_cache.depth;
    d.stencilTestEnable = 0;
    e->set_depth(d);
    g_cache.depth = d;
  }
}
// jak2 (AI-assisted): GL_INT attribs = the TextureAnimator composite quad. Off by default
// (the pre-8fe05566c behaviour): only allow-listed animations draw (skull gem).
void set_int_attrib_draws(bool on) {
  g_int_attrib_draws = on;
  g_cache.valid = false;  // Row I pass 2: the GL_INT allow-list decision changed
}
u64 inert_draw_count() {
  return g_inert_draw_count;
}
u64 skipped_draw_count() {
  return g_bad_attrib_count + g_missing_element_buffer_count + g_bad_prim_count +
         g_bad_ubo_count;
}
u64 no_texture_draw_count() {
  return g_no_texture_draws;
}
// per-reason breakdown for the 10 s stats line
u64 bad_attrib_draw_count() {
  return g_bad_attrib_count;
}
u64 missing_element_draw_count() {
  return g_missing_element_buffer_count;
}
u64 bad_ubo_draw_count() {
  return g_bad_ubo_count;
}
u64 int_attrib_skip_count() {
  return g_int_attrib_skip_count;
}
u64 vao_fast_hit_count() {
  return g_vao_fast_hits;
}
u64 tex_bind_skip_count() {
  return g_tex_bind_skips;
}

// Switch perf: read direct_basic_textured's frag alpha-test window + scissor flag
// out of the 512-byte program shadow (write_uniform lays it out as vert block then
// frag block; uniform1i stored the int/bool as 4 raw bytes). Returns false if the
// generated table is missing a slot (drift) -- the caller then keeps the base shader.
bool read_direct_textured_window(const ProgramUniformTable* t,
                                 const u8* shadow,
                                 float* amin,
                                 float* amax,
                                 int* scissor,
                                 int* greater) {
  static constexpr const char* kNames[4] = {"alpha_min", "alpha_max", "scissor_enable",
                                            "greater"};
  const UniformSlot* slots[4] = {nullptr, nullptr, nullptr, nullptr};
  for (u32 i = 0; i < t->frag.count; ++i) {
    const char* n = t->frag.slots[i].name;
    for (int k = 0; k < 4; ++k) {
      if (n && !slots[k] && std::strcmp(n, kNames[k]) == 0) {
        slots[k] = &t->frag.slots[i];
      }
    }
  }
  for (int k = 0; k < 4; ++k) {
    if (!slots[k]) {
      return false;
    }
  }
  const u8* fb = shadow + t->vert.block_size;
  std::memcpy(amin, fb + slots[0]->offset, 4);
  std::memcpy(amax, fb + slots[1]->offset, 4);
  std::memcpy(scissor, fb + slots[2]->offset, 4);
  std::memcpy(greater, fb + slots[3]->offset, 4);
  return true;
}

void flush_pending(u32 mode, int first, int count, u32 idx_type, u64 idx_off, bool indexed) {
  Emitter* e = emitter();
  if (!e) {
    return;  // shadow-only mode (host tests without a mock): nothing to emit
  }
  const ShadowState& sh = shadow();

  // 1. pending target switch (queued by bind_framebuffer): emits FIRST, and the new
  // target's size differs -> the viewport + vtx format must be re-derived.
  if (pending_target_switch()) {
    if (pending_target_offscreen()) {
      e->bind_offscreen_target(pending_target_dk_tex(), pending_target_level(),
                               pending_target_depth_dk_tex());
    } else {
      e->bind_frame_target();
    }
    pending_target_consumed();
    g_cache.vp.width = 0;  // force viewport re-emit (target size changed)
    g_cache.vao = nullptr;
    g_cache.n_vtx_bufs = 0;  // force vtx-format re-emit
  }

  // 2. pending clear, BEFORE any draw state (the ocean clears right after binding)
  if (pending_clear_mask()) {
    const u32 mask = pending_clear_mask();
    // §7.2 phase 2: GL_STENCIL_BUFFER_BIT (Shadow2 resets the stencil buffer between its two
    // instances). GL masks the clear by the stencil write mask; reading it at flush time
    // matches every real sequence (the mask is 0xFF whenever Shadow2 clears).
    const bool sten = (mask & gl::STENCIL_BUFFER_BIT) != 0;
    e->clear((mask & gl::COLOR_BUFFER_BIT) != 0, (mask & gl::DEPTH_BUFFER_BIT) != 0, sten,
             sten ? (u8)(sh.stencil_write_mask & 0xFFu) : (u8)0,
             (u8)(sh.clear_stencil & 0xFFu), sh.clear_color);
    consume_pending_clear();
  }

  // 3. program: inert (name not in the T103 table) -> skip the whole draw, counted
  const u32 prog = bound_program();
  const ProgramUniformTable* table = program_table(prog);
  if (!table) {
    ++g_inert_draw_count;
    return;
  }
  // Switch perf (AI-assisted): when direct_basic_textured's per-draw alpha window
  // rejects nothing AND the GS scissor is off, bind the discard-free
  // direct_basic_textured_fast fragment shader instead (same UBO/vertex interface,
  // same color) so the GPU keeps early-Z enabled. This is what makes the folded
  // jak2 sky draw cheap (see DirectRenderer.cpp flush_pending). The decision is
  // per-DRAW data (alpha_min/alpha_max change via glUniform between draws of the
  // same GL program), so it must take part in the bind cache below.
  u32 variant = 0;  // 0 = table shader, 1 = _fast
  if (std::strcmp(table->program_name, "direct_basic_textured") == 0) {
    const u8* shadow = program_uniform_shadow(prog);
    float amin = 0.f, amax = 0.f;
    int scissor = 1, greater = 0;
    if (shadow && read_direct_textured_window(table, shadow, &amin, &amax, &scissor,
                                              &greater)) {
      // keep [min,max) for greater=0 / (min,max] for greater=1; fragment alpha is
      // color.a*2, so [0,2] is every possible value.
      const bool rejects_nothing = (greater == 0 && amin <= 0.f && amax >= 2.f) ||
                                   (amin <= -1.f && amax >= 3.f);
      if (rejects_nothing && scissor == 0) {
        variant = 1;
      }
    }
  }
  if (!g_cache.valid || g_cache.program != prog || g_cache.program_variant != variant) {
    e->bind_shaders(prog, variant ? "direct_basic_textured_fast" : table->program_name);
    g_cache.program = prog;
    g_cache.program_variant = variant;
  }

  // 4. depth (GL-style depth: device runs DepthMinusOneToOne, clears 0.0 -- P5)
  bool ok = true;
  const DkCompareOp cmp = depth_func_map(sh.depth_func, &ok);
  if (!ok) {
    ++g_bad_depth_func_count;
  }
  DkDepthStencilState depth{};
  depth.depthTestEnable = sh.depth_test ? 1u : 0u;
  // GL never writes depth while GL_DEPTH_TEST is off (dkgl review)
  depth.depthWriteEnable = (sh.depth_test && sh.depth_write) ? 1u : 0u;
  depth.depthCompareOp = cmp;
  // T113: GL stencil (glStencilFunc/glStencilOp apply to both faces). Disabled -> valid
  // Keep/Always enums anyway, so the cached struct never holds a 0 (= invalid) op.
  bool ok_st = true;
  const bool st = sh.stencil_test;
  const DkStencilOp sfail = st ? stencil_op_map(sh.stencil_sfail, &ok_st) : DkStencilOp_Keep;
  const DkStencilOp dpfail = st ? stencil_op_map(sh.stencil_dpfail, &ok_st) : DkStencilOp_Keep;
  const DkStencilOp dppass = st ? stencil_op_map(sh.stencil_dppass, &ok_st) : DkStencilOp_Keep;
  bool ok_sf = true;
  const DkCompareOp sfunc = st ? depth_func_map(sh.stencil_func, &ok_sf) : DkCompareOp_Always;
  if (!ok_st || !ok_sf) {
    ++g_bad_stencil_count;
  }
  depth.stencilTestEnable = st ? 1u : 0u;
  depth.stencilFrontFailOp = sfail;
  depth.stencilFrontPassOp = dppass;
  depth.stencilFrontDepthFailOp = dpfail;
  depth.stencilFrontCompareOp = sfunc;
  depth.stencilBackFailOp = sfail;
  depth.stencilBackPassOp = dppass;
  depth.stencilBackDepthFailOp = dpfail;
  depth.stencilBackCompareOp = sfunc;
  if (!g_cache.valid || std::memcmp(&g_cache.depth, &depth, sizeof(depth)) != 0) {
    e->set_depth(depth);
    g_cache.depth = depth;
  }
  if (st) {  // ref/masks are dynamic state; only stencil users pay for them
    const u32 dyn = (sh.stencil_write_mask & 0xFFu) << 16 | (sh.stencil_ref & 0xFFu) << 8 |
                    (sh.stencil_func_mask & 0xFFu);
    if (!g_cache.valid || g_cache.stencil_dyn != dyn) {
      e->set_stencil((u8)(dyn >> 16), (u8)(dyn >> 8), (u8)dyn);
      g_cache.stencil_dyn = dyn;
    }
  }
  // T113: colour mask (shadow volumes write stencil only, the shadow quad skips alpha)
  if (!g_cache.valid || g_cache.color_mask != sh.color_mask) {
    e->set_color_mask(sh.color_mask);
    g_cache.color_mask = sh.color_mask;
  }

  // 5. blend (enable encoded as ONE/ZERO factors; needs T101's patched deko3d for the
  // separate alpha factor to actually reach the GPU)
  bool ok_s = true, ok_d = true, ok_sa = true, ok_da = true, ok_op = true, ok_opa = true;
  DkBlendFactor src, dst, srca, dsta;
  if (sh.blend) {
    ok_s = blend_factor_map(sh.blend_src_rgb, &src);
    ok_d = blend_factor_map(sh.blend_dst_rgb, &dst);
    ok_sa = blend_factor_map(sh.blend_src_a, &srca);
    ok_da = blend_factor_map(sh.blend_dst_a, &dsta);
    if (!ok_s || !ok_d || !ok_sa || !ok_da) {
      ++g_bad_blend_factor_count;
      src = dst = srca = dsta = DkBlendFactor_One;  // safe fallback: as-if disabled
    }
  } else {
    src = DkBlendFactor_One;   // disabled GL blend == src replaces dst
    dst = DkBlendFactor_Zero;
    srca = DkBlendFactor_One;
    dsta = DkBlendFactor_Zero;
  }
  const DkBlendOp op_rgb = blend_op_map(sh.blend_eq_rgb, &ok_op);
  const DkBlendOp op_a = blend_op_map(sh.blend_eq_a, &ok_opa);
  if (!ok_op || !ok_opa) {
    ++g_bad_blend_factor_count;
  }
  DkBlendState blend{};
  blend.colorBlendOp = op_rgb;
  blend.srcColorBlendFactor = src;
  blend.dstColorBlendFactor = dst;
  blend.alphaBlendOp = op_a;
  blend.srcAlphaBlendFactor = srca;
  blend.dstAlphaBlendFactor = dsta;
  if (!g_cache.valid || std::memcmp(&g_cache.blend, &blend, sizeof(blend)) != 0) {
    e->set_blend(blend);
    g_cache.blend = blend;
  }
  if (!g_cache.valid || std::memcmp(g_cache.blend_color, sh.blend_color, 16) != 0) {
    e->set_blend_color(sh.blend_color);  // T111
    std::memcpy(g_cache.blend_color, sh.blend_color, 16);
  }

  // 6. viewport
  const DkViewport vp{(float)sh.vp_x, (float)sh.vp_y, (float)sh.vp_w, (float)sh.vp_h, 0.f,
                      1.f};
  if (!g_cache.valid || std::memcmp(&g_cache.vp, &vp, sizeof(vp)) != 0) {
    e->set_viewport(vp);
    g_cache.vp = vp;
  }

  // 7. vtx format, from the CURRENT VAO; attribs captured their buffer at
  // glVertexAttribPointer time (T104b). dkgl review: one DkVtxBufferState per DISTINCT
  // buffer (OceanTexture_PC: static positions + dynamic data in one VAO); the attribs of
  // one buffer must share its stride.
  // Row I pass 2 (AI-assisted): fast path -- same VAO, its serial unchanged (no
  // attrib/element-buffer mutation) and no buffer re-spec (global epoch) means the GPU
  // already holds this vertex format + bindings and every early-return check below
  // passed for the previous draw with identical state. Merc2 binds a per-model VAO and
  // draws without re-pointing attribs, so this hits between consecutive draws.
  const VaoObj* vao = bound_vao();
  if (g_cache.valid && g_cache.vao == (const void*)vao && g_cache.vao_serial == vao->serial &&
      g_cache.buf_epoch == buffer_epoch()) {
    ++g_vao_fast_hits;
  } else {
    DkVtxAttribState attribs[kMaxAttribs];
    DkVtxBufferState vbufs[kMaxAttribs];
    u32 buf_names[kMaxAttribs];
    u64 buf_gpu[kMaxAttribs];
    u32 buf_bytes[kMaxAttribs];
    u32 n_bufs = 0;
    u32 n_attribs = 0;
    for (u32 i = 0; i < kMaxAttribs; ++i) {
      const auto& a = vao->attribs[i];
      if (!a.enabled) {
        continue;
      }
      if (a.type == gl::INT && !g_int_attrib_draws) {
        ++g_int_attrib_skip_count;  // policy skip: counted separately (was invisible before)
        if (!g_logged_int_attrib_skip) {
          g_logged_int_attrib_skip = true;
          std::fprintf(stderr, "[dkgl] draw skipped: GL_INT attrib %u outside allow-list\n", i);
        }
        return;  // TextureAnimator composite outside the allow-list (see set_int_attrib_draws)
      }
      DkVtxAttribSize sz;
      DkVtxAttribType ty;
      if (!attrib_map(a.size, a.type, a.normalized, a.integer, &sz, &ty)) {
        ++g_bad_attrib_count;
        if (!g_logged_bad_attrib) {
          g_logged_bad_attrib = true;
          std::fprintf(stderr, "[dkgl] draw skipped: attrib %u size %d type 0x%x norm %d int %d\n",
                       i, a.size, a.type, (int)a.normalized, (int)a.integer);
        }
        return;  // unknown attrib format: skip the draw (never guess a layout)
      }
      const BufferObj* bo = buffer(a.buffer);
      if (!bo || !bo->valid || bo->cpu == nullptr) {
        ++g_bad_attrib_count;
        if (!g_logged_dead_attrib_buffer) {
          g_logged_dead_attrib_buffer = true;
          // FIX 139a (AI-assisted): the 2026-11-20 HW run skipped ~1 draw EVERY active
          // frame this way (attrib counter 17774 over 50460 frames) and the old one-shot
          // line named no culprit. Print the program + buffer state so the next run pins
          // the draw family (buffer id alone was not enough to identify it).
          std::fprintf(stderr,
                       "[dkgl] draw skipped: attrib %u buffer %u dead/empty (program %s, "
                       "stride %u, size %d type 0x%x, bo %d valid %d bytes %u)\n",
                       i, a.buffer, table ? table->program_name : "?", a.stride, a.size,
                       a.type, bo != nullptr, bo ? (int)bo->valid : -1, bo ? bo->bytes : 0);
        }
        return;  // attrib points at a dead/failed buffer: skip the draw
      }
      u32 b = 0;
      while (b < n_bufs && buf_names[b] != a.buffer) {
        ++b;
      }
      if (b == n_bufs) {
        buf_names[b] = a.buffer;  // first attrib of this buffer fixes its stride
        buf_gpu[b] = bo->gpu;
        buf_bytes[b] = bo->bytes;
        vbufs[b] = DkVtxBufferState{a.stride, 0};
        ++n_bufs;
      } else if (a.stride != vbufs[b].stride) {
        ++g_vtx_stride_mismatch_count;  // counter + FIRST stride (spec rule)
      }
      attribs[n_attribs++] = DkVtxAttribState{b, 0, (u32)a.offset, sz, ty, 0};
    }
    if (n_attribs == 0 || n_bufs == 0) {
      ++g_bad_attrib_count;
      if (!g_logged_no_attribs) {
        g_logged_no_attribs = true;
        std::fprintf(stderr, "[dkgl] draw skipped: no enabled vertex attribs\n");
      }
      return;  // nothing to draw without a vertex layout
    }
    e->set_vtx_format(attribs, n_attribs, vbufs, buf_gpu, buf_bytes, n_bufs);
    g_cache.vao = vao;
    g_cache.vao_serial = vao->serial;
    g_cache.buf_epoch = buffer_epoch();
    g_cache.n_vtx_bufs = n_bufs;
    for (u32 b = 0; b < n_bufs; ++b) {
      g_cache.vtx_gpu[b] = buf_gpu[b];
      g_cache.vtx_bytes[b] = buf_bytes[b];
    }
  }

  // 8. indexed draws bind the CURRENT VAO's element buffer
  DkIdxFormat idx_fmt = DkIdxFormat_Uint32;
  if (indexed) {
    const BufferObj* ib = buffer(vao->element_buffer);
    if (!ib || !ib->valid || ib->cpu == nullptr) {
      ++g_missing_element_buffer_count;
      if (!g_logged_missing_element) {
        g_logged_missing_element = true;
        std::fprintf(stderr, "[dkgl] draw skipped: element buffer %u dead/empty\n",
                     vao->element_buffer);
      }
      return;
    }
    if (!idx_fmt_map(idx_type, &idx_fmt)) {
      ++g_bad_attrib_count;
      return;
    }
    if (!g_cache.valid || g_cache.idx_gpu != ib->gpu || g_cache.idx_fmt != idx_fmt) {
      e->bind_idx_buffer(ib->gpu, idx_fmt);
      g_cache.idx_gpu = ib->gpu;
      g_cache.idx_fmt = idx_fmt;
    }
  }

  // 9. samplers: one per unit with a bound texture; an RT that was rendered to gets
  // its render-to-texture barrier exactly once (before the first sampling draw).
  // Row I pass 2 (AI-assisted): a unit whose gl texture, resolved dk image AND the
  // tex-param epoch all match the last emit keeps its binding -- the handle is already
  // bound on both stages (bind_tex emits two commands), so skip the combo walk and the
  // emit. The barrier check stays unconditional: an RT re-rendered between two draws
  // with the same binding re-arms it.
  if (g_cache.tex_epoch != texture_param_epoch()) {
    for (u32 u2 = 0; u2 < kMaxTexUnits; ++u2) {
      g_cache.tex_gl[u2] = 0;  // params changed: every cached combo is stale
    }
    g_cache.tex_epoch = texture_param_epoch();
  }
  u32 n_bound_units = 0;  // §7.2 phase 2: draws with ZERO textures run on stale
                          // bindings (a pool miss GL would render with its placeholder)
  for (u32 unit = 0; unit < kMaxTexUnits; ++unit) {
    const u32 gl_id = bound_texture(unit);
    if (gl_id == 0) {
      continue;
    }
    ++n_bound_units;
    u32 dk_id;
    if (gl_id >= kTexIdBase) {
      dk_id = materialized_dk_tex(gl_id);
      if (dk_id == 0) {
        dk_id = resolve_texture_id(gl_id);  // materialize on demand
      }
    } else {
      dk_id = gl_id;  // pool path: the binding IS a raw dktexgpu id
      // FIX 124 (AI-assisted): tell the pool this id is being drawn. These
      // binds never go through lookup(tbp), so without the stamp the pool
      // could unload a page an active level still draws -- the black-wall /
      // white-box-Jak symptom after area transitions.
      note_pool_draw(gl_id);
    }
    if (texture_needs_barrier(gl_id)) {
      e->barrier_before_sample();
      texture_barrier_sent(gl_id);
    }
    if (g_cache.valid && g_cache.tex_gl[unit] == gl_id && g_cache.tex_dk[unit] == dk_id) {
      ++g_tex_bind_skips;
      continue;  // same texture + same params: nothing to re-bind
    }
    e->bind_tex(unit, dk_id, texture_sampler_combo(unit, gl_id));
    g_cache.tex_gl[unit] = gl_id;
    g_cache.tex_dk[unit] = dk_id;
  }
  if (n_bound_units == 0) {
    ++g_no_texture_draws;
#if defined(__SWITCH__)
    // #J12 probe (AI-assisted): which bucket issues the zero-sampler draws (~46/frame
    // in jak3, 0 in jak2). Budgeted by gk_dkgl_notexprobe.txt on the SD.
    if (notex_probe_take()) {
      switch_run_logf("[dkgl] notex probe: zero-sampler draw in bucket %d", cur_bucket_id());
    }
#endif
  }

  // J18 A/B (AI-assisted): gk_dkgl_nouboreuse.txt -> fresh uniform snapshot + UBO re-bind on
  // every draw (no Row I slice reuse, no emit-cache skip for bindings 0-7).
  const bool force_ubo = g_no_ubo_reuse;

  // 9b. TODO #5 (AI-assisted): glBindBufferRange(GL_UNIFORM_BUFFER) ranges (Merc2 bones,
  // ub_bones = binding 1 in the generated shaders), bound to both stages. A range whose
  // buffer has no live data skips the draw: skinning with stale bones would be garbage.
  u64 ubo_gpu[kMaxUboBindings];
  u32 ubo_size[kMaxUboBindings];
  for (u32 i = 0; i < kMaxUboBindings; ++i) {
    const UboBinding& ub = ubo_binding(i);
    ubo_gpu[i] = 0;
    ubo_size[i] = 0;
    if (ub.name == 0) {
      continue;
    }
    const BufferObj* bo = buffer(ub.name);
    if (!bo || !bo->valid || bo->gpu == 0 || ((bo->gpu + ub.offset) & (kUboAlign - 1)) != 0) {
      ++g_bad_ubo_count;
      if (!g_logged_bad_ubo) {
        g_logged_bad_ubo = true;
        std::fprintf(stderr, "[dkgl] draw skipped: ubo %u (binding %u) dead/unaligned\n", ub.name,
                     i);
      }
      return;
    }
    ubo_gpu[i] = bo->gpu + ub.offset;
    ubo_size[i] = ub.size < DK_UNIFORM_BUF_MAX_SIZE ? ub.size : DK_UNIFORM_BUF_MAX_SIZE;
  }
  for (u32 i = 0; i < kMaxUboBindings; ++i) {
    if (ubo_size[i] == 0) {
      continue;
    }
    if (force_ubo || !g_cache.valid || g_cache.ubo_gpu[i] != ubo_gpu[i] ||
        g_cache.ubo_size[i] != ubo_size[i]) {
      e->bind_ubo(DkStage_Vertex, i, ubo_gpu[i], ubo_size[i]);
      e->bind_ubo(DkStage_Fragment, i, ubo_gpu[i], ubo_size[i]);
      g_cache.ubo_gpu[i] = ubo_gpu[i];
      g_cache.ubo_size[i] = ubo_size[i];
    }
  }

  // 10. uniforms. Row I (AI-assisted): write_uniform now suppresses no-op writes, so a
  // draw whose program shadow is clean re-BINDS the slice that program last uploaded
  // in THIS frame slot instead of snapshotting a fresh one (Merc2 re-sends the same
  // light/decal values for every draw of a model). ring_take never wraps within a
  // frame, so the old slice stays intact until the slot is recycled; a slot change
  // (next frame) or a dirty shadow forces a fresh snapshot. dkgl review: a UBO
  // address must be 0x100-aligned (DK_UNIFORM_BUF_ALIGNMENT), so the frag block
  // starts at the vert block size rounded up to 0x100, not right after it (the
  // shadow keeps the packed layout).
  const u32 vs_bytes = table->vert.block_size;
  const u32 fs_bytes = table->frag.block_size;
  const u32 fs_off = (vs_bytes + 0xFFu) & ~0xFFu;
  if (vs_bytes + fs_bytes > 0) {
    u64 slice_gpu = 0;
    u32 slice_slot = 0;
    if (force_ubo || program_uniform_dirty(prog) ||
        (slice_gpu = program_uniform_slice(prog, &slice_slot)) == 0 ||
        slice_slot != e->ubo_ring_slot()) {
      slice_gpu = 0;
      void* cpu = nullptr;
      u64 gpu = 0;
      if (e->ubo_alloc(fs_off + fs_bytes, &cpu, &gpu)) {
        const u8* shadow_bytes = program_uniform_shadow(prog);
        u8* shadow_dst = (u8*)cpu;  // named shadow_dst: `dst` is the blend local above
        if (shadow_bytes) {
          std::memcpy(shadow_dst, shadow_bytes, vs_bytes);
          std::memcpy(shadow_dst + fs_off, shadow_bytes + vs_bytes, fs_bytes);
        } else {
          std::memset(shadow_dst, 0, fs_off + fs_bytes);
        }
        program_uniform_set_slice(prog, gpu, e->ubo_ring_slot());
        slice_gpu = gpu;
      } else {
        // ring full: keep the old slice if it came from this slot, else nothing to bind
        u32 old_slot = 0;
        const u64 old_gpu = program_uniform_slice(prog, &old_slot);
        if (old_gpu != 0 && old_slot == e->ubo_ring_slot()) {
          slice_gpu = old_gpu;
        }
      }
      program_uniform_clear_dirty(prog);
    }
    if (slice_gpu != 0 && (force_ubo || !g_cache.valid || g_cache.uni7_gpu != slice_gpu ||
                           g_cache.uni7_vs != vs_bytes || g_cache.uni7_fs != fs_bytes)) {
      if (vs_bytes) {
        e->bind_ubo(DkStage_Vertex, 7, slice_gpu, vs_bytes);
      }
      if (fs_bytes) {
        e->bind_ubo(DkStage_Fragment, 7, slice_gpu + fs_off, fs_bytes);
      }
      g_cache.uni7_gpu = slice_gpu;
      g_cache.uni7_vs = vs_bytes;
      g_cache.uni7_fs = fs_bytes;
    }
  }

  // 11. primitive restart
  if (!g_cache.valid || g_cache.restart_on != sh.primitive_restart ||
      g_cache.restart_index != sh.restart_index) {
    e->set_primitive_restart(sh.primitive_restart, sh.restart_index);
    g_cache.restart_on = sh.primitive_restart;
    g_cache.restart_index = sh.restart_index;
  }

  // 12. the draw
  DkPrimitive prim;
  if (!prim_map(mode, &prim)) {
    ++g_bad_prim_count;
    return;
  }
  if (indexed) {
    e->draw_elements(prim, (u32)count, (u32)idx_off);
  } else {
    e->draw_arrays(prim, (u32)first, (u32)count);
  }
  g_cache.valid = true;
}

}  // namespace dkgl
