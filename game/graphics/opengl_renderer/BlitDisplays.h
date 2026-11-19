#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/SlowTimeEffect.h"
#include "game/graphics/opengl_renderer/opengl_utils.h"
#include "game/graphics/texture/TexturePool.h"

struct PcZoomBlur {
  math::Vector4f pos;
  math::Vector4<int32_t> color;
  uint32_t is_2d;
  int32_t texels;
  float alpha_current;
};

// FIX 138 (AI-assisted): read-only view of the end-of-frame effect pendings for the dkgl
// layer (usage probe + the gk_dkgl_copyback.txt A/B). Pure accessor -- no GL behavior
// changes; GL consumes the pendings in do_copy_back / apply_color_filter exactly as before.
struct BlitPendingInfo {
  bool copy_back = false;
  bool zoom_blur = false;
  bool color_filter = false;
  bool slow_time = false;
  float slow_time_amount = 0;
  math::Vector4f color_filter_rgba;
  PcZoomBlur zoom_blur_params;
};

// FIX 138 (AI-assisted): armed by dkgl's first capture_begin (Switch only). Lives in
// namespace dkgl; declared here so dkgl_frame.cpp (anonymous-namespace region) sees it.
namespace dkgl {
extern bool g_dkgl_copyprobe;
}

/*!
 * The BlitDisplays renderer does various blitting and effects on the previous frame
 */
class BlitDisplays : public BucketRenderer {
 public:
  BlitDisplays(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void init_textures(TexturePool& texture_pool, GameVersion) override;
  void draw_debug_window() override;
  void do_copy_back(SharedRenderState* render_state, ScopedProfilerNode& prof);
  void do_zoom_blur(SharedRenderState* render_state, ScopedProfilerNode& prof);
  void do_slow_time(SharedRenderState* render_state, ScopedProfilerNode& prof);
  void apply_color_filter(SharedRenderState* render_state, ScopedProfilerNode& prof);
  // FIX 138 (AI-assisted): read-only pending state for the dkgl copyback wiring/probe.
  BlitPendingInfo pending_effects() const;

 private:
  std::unique_ptr<FramebufferCopier> m_copier;
  FramebufferCopier m_blur_old_copier;
  FramebufferCopier m_blur_new_copier;
  GpuTexture* m_gpu_tex;
  u32 m_tbp;
  bool m_copy_back_pending = false;
  bool m_zoom_blur_pending = false;
  bool m_color_filter_pending = false;
  bool m_slow_time_pending = false;
  float m_slow_time_amount = 0;
  math::Vector4f m_color_filter;
  PcZoomBlur m_zoom_blur;
  FullScreenTexDraw m_fullscreen_tex_draw;
  FullScreenDraw m_color_draw;
  SlowTimeEffect m_slow_time_effect;
};
