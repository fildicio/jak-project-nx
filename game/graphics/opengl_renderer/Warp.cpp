#include "Warp.h"

Warp::Warp(const std::string& name, int id, std::shared_ptr<Generic2> generic)
    : BucketRenderer(name, id), m_generic(generic) {}

void Warp::draw_debug_window() {
  m_generic->draw_debug_window();
}

void Warp::render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) {
  m_fb_copier.copy_now(render_state->render_fb_w, render_state->render_fb_h,
                       render_state->render_fb);
  render_state->texture_pool->move_existing_to_vram(m_warp_src_tex, m_tbp);
  // FIX 141 (AI-assisted): re-link the tbp slot after the capture. On GL a texture's
  // name is stable across the glTexImage2D resize inside copy_now, so the slot stayed
  // valid by itself. On dkgl a respecify drops the materialized dktexgpu id
  // (dkgl_texture.cpp tex_image_2d) and the blit materializes a NEW image, so the
  // slot kept pointing at the boot-time image and everything sampling tbp 1216
  // (gmerc-warp effect, jak2 save-menu backdrop quads) drew a stale, never-blitted
  // image = corrupted picture. update_gl_texture re-resolves the CURRENT id and
  // refreshes the record's w/h -- the same pool seam TextureAnimator already uses
  // for PC-ANIM (which is why PC-ANIM resolves correctly and PC-WARP did not).
  render_state->texture_pool->update_gl_texture(m_warp_src_tex, render_state->render_fb_w,
                                                render_state->render_fb_h,
                                                (GLuint)m_fb_copier.texture());
  m_generic->render_in_mode(dma, render_state, prof, Generic2::Mode::WARP);
}

void Warp::init_textures(TexturePool& tex_pool, GameVersion version) {
  TextureInput in;
  // point to fb copier's texture.
  in.gpu_texture = m_fb_copier.texture();
  in.w = 32;
  in.h = 32;
  in.debug_page_name = "PC-WARP";
  in.debug_name = "PC-WARP";
  in.id = tex_pool.allocate_pc_port_texture(version);
  m_warp_src_tex = tex_pool.give_texture_and_load_to_vram(in, m_tbp);
}