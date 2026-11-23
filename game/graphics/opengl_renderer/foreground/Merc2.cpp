#include "Merc2.h"
#include "game/graphics/opengl_renderer/GfxDrawStats.h"

#include "common/global_profiler/GlobalProfiler.h"
#include "common/util/fnv.h"
#include "common/util/simd_util.h"

#include "game/graphics/opengl_renderer/EyeRenderer.h"
#include "game/graphics/opengl_renderer/background/background_common.h"

#if defined(__SWITCH__)
#include <unordered_map>
#include <unordered_set>

#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"

#include "game/switch/imgui_stub.h"
#include "game/switch/run_log.h"
#else
#include "third-party/imgui/imgui.h"
#endif

/* Merc 2 renderer:
 The merc2 renderer is the main "foreground" renderer, which draws characters, collectables,
 and even some water.

 The PC format renderer does the usual tricks of buffering stuff head of time as much as possible.
 The main trick here is to buffer up draws and upload "bones" (skinning matrix) for many draws all
 at once.

 The other tricky part is "mod vertices", which may be modified by the game.
 We know ahead of time which vertices could be modified, and have a way to upload only those
 vertices.

 Each "merc model" corresponds to a merc-ctrl in game. There's one merc-ctrl per LOD of an
 art-group. So generally, this will be something like "jak" or "orb" or "some enemy".

 Each model is made up of "effect"s. There are a number of per-effect settings, like environment
 mapping. Generally, the purpose of an "effect" is to divide up a model into parts that should be
 rendered with a different configuration.

 Within each model, there are fragments. These correspond to how much data can be uploaded to VU1
 memory. For the most part, fragments are not considered by the PC renderer. The only exception is
 updating vertices - we must read the data from the game, which is stored in fragments.

 Per level, there is an FR3 file loaded by the loader. Each merc renderer can access multiple
 levels.
*/

/*!
 * Remaining ideas for optimization:
 * - port blerc to C++, do it in the rendering thread and avoid the lock.
 * - combine envmap draws per effect (might require some funky indexing stuff, or multidraw)
 * - smaller vertex formats for mod-vertex
 * - AVX version of vertex conversion math
 * - eliminate the "copy" step of vertex modification
 * - batch uploading the vertex modification data
 */

std::mutex g_merc_data_mutex;
u32 (*g_merc2_eye_tex_lookup)(u32 eye_id) = nullptr;  // Switch dkgl (AI-assisted)

Merc2::Merc2(ShaderLibrary& shaders, const std::vector<GLuint>* anim_slot_array)
    : m_anim_slot_array(anim_slot_array) {
  ASSERT(fnv64("the quick brown fox jumps over the lazy dog") == 0x7404cea13ff89bb0);

  // Set up main vertex array. This will point to the data stored in the .FR3 level file, and will
  // be uploaded to the GPU by the Loader.
  glGenVertexArrays(1, &m_vao);
  glBindVertexArray(m_vao);

  // Bone buffer to store skinning matrices for multiple draws
  glGenBuffers(1, &m_bones_buffer);
  glBindBuffer(GL_UNIFORM_BUFFER, m_bones_buffer);

  // zero initialize the bone buffer.
  std::vector<u8> temp(MAX_SHADER_BONE_VECTORS * sizeof(math::Vector4f));
  glBufferData(GL_UNIFORM_BUFFER, MAX_SHADER_BONE_VECTORS * sizeof(math::Vector4f), temp.data(),
               GL_DYNAMIC_DRAW);
  glBindBuffer(GL_UNIFORM_BUFFER, 0);

  // annoyingly, glBindBufferRange can have alignment restrictions that vary per platform.
  // the GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT gives us the minimum alignment for views into the bone
  // buffer. The bone buffer stores things per-16-byte "quadword".
  GLint val;
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &val);
  if (val <= 16) {
    // somehow doubt this can happen, but just in case
    m_opengl_buffer_alignment = 1;
  } else {
    m_opengl_buffer_alignment = val / 16;  // number of bone vectors
    if (m_opengl_buffer_alignment * 16 != (u32)val) {
      ASSERT_MSG(false,
                 fmt::format("opengl uniform buffer alignment is {}, which is strange\n", val));
    }
  }

  // initialize draw buffers, these will store lists of draws to flush.
  for (int i = 0; i < MAX_LEVELS; i++) {
    auto& draws = m_level_draw_buckets.emplace_back();
    draws.draws.resize(MAX_DRAWS_PER_LEVEL);
    draws.envmap_draws.resize(MAX_ENVMAP_DRAWS_PER_LEVEL);
  }

  m_mod_vtx_temp.resize(MAX_MOD_VTX);
  m_mod_vtx_unpack_temp.resize(MAX_MOD_VTX * 2);

  for (auto& x : m_effect_debug_mask) {
    x = true;
  }

  init_shader_common(shaders[ShaderId::MERC2], &m_merc_uniforms, true);
  init_shader_common(shaders[ShaderId::EMERC], &m_emerc_uniforms, false);
  m_emerc_uniforms.fade = gl_uniform_loc(shaders[ShaderId::EMERC].id(), "fade");
}

Merc2::~Merc2() {
  for (auto& x : m_mod_vtx_buffers) {
    glDeleteBuffers(1, &x.vertex);
    glDeleteVertexArrays(1, &x.vao);
  }

  glDeleteBuffers(1, &m_bones_buffer);
  glDeleteVertexArrays(1, &m_vao);
}

/*!
 * Modify vertices for blerc.
 */
void blerc_avx(const u32* i_data,
               const u32* i_data_end,
               const tfrag3::BlercFloatData* floats,
               const float* weights,
               tfrag3::MercVertex* out,
               float multiplier) {
  // store a table of weights. It's faster to load the 16-bytes of weights than load and broadcast
  // the float.
  __m128 weights_table[Merc2::kMaxBlerc];
  for (int i = 0; i < Merc2::kMaxBlerc; i++) {
    weights_table[i] = _mm_set1_ps(weights[i] * multiplier);
  }

  // loop over vertices
  while (i_data != i_data_end) {
    // load the base position
    __m128 pos = _mm_load_ps(floats->v);
    __m128 nrm = _mm_load_ps(floats->v + 4);
    floats++;

    // loop over targets
    while (*i_data != tfrag3::Blerc::kTargetIdxTerminator) {
      // get the weights for this target, from the game data.
      __m128 weight_multiplier = weights_table[*i_data];
      // get the pos/normal offset for this target.
      __m128 posm = _mm_load_ps(floats->v);
      __m128 nrmm = _mm_load_ps(floats->v + 4);
      floats++;

      // apply weights and add
      posm = _mm_mul_ps(posm, weight_multiplier);
      nrmm = _mm_mul_ps(nrmm, weight_multiplier);
      pos = _mm_add_ps(pos, posm);
      nrm = _mm_add_ps(nrm, nrmm);

      i_data++;
    }
    i_data++;

    // store final position/normal.
    _mm_store_ps(out[*i_data].pos, pos);
    _mm_store_ps(out[*i_data].normal, nrm);
    i_data++;
  }
}
namespace {
float blerc_multiplier = 1.f;
}

void Merc2::model_mod_blerc_draws(int num_effects,
                                  const tfrag3::MercModel* model,
                                  const LevelData* lev,
                                  ModBuffers* mod_opengl_buffers,
                                  const float* blerc_weights,
                                  MercDebugStats* stats) {
  // loop over effects.
  for (int ei = 0; ei < num_effects; ei++) {
    const auto& effect = model->effects[ei];
    // some effects might have no mod draw info, and no modifiable vertices
    if (effect.mod.mod_draw.empty()) {
      continue;
    }

    // grab opengl buffer
    auto opengl_buffers = alloc_mod_vtx_buffer(lev);
    mod_opengl_buffers[ei] = opengl_buffers;

    // check that we have enough room for the finished thing.
    if (effect.mod.vertices.size() > MAX_MOD_VTX) {
      fmt::print("More mod vertices than MAX_MOD_VTX. {} > {}\n", effect.mod.vertices.size(),
                 MAX_MOD_VTX);
      ASSERT_NOT_REACHED();
    }

    // start with the correct vertices from the model data:
    memcpy(m_mod_vtx_temp.data(), effect.mod.vertices.data(),
           sizeof(tfrag3::MercVertex) * effect.mod.vertices.size());

    // do blerc math
    const auto* f_data = effect.mod.blerc.float_data.data();
    const u32* i_data = effect.mod.blerc.int_data.data();
    const u32* i_data_end = i_data + effect.mod.blerc.int_data.size();
    blerc_avx(i_data, i_data_end, f_data, blerc_weights, m_mod_vtx_temp.data(), blerc_multiplier);

    // and upload to GPU
    stats->num_uploads++;
    stats->num_upload_bytes += effect.mod.vertices.size() * sizeof(tfrag3::MercVertex);
    {
      glBindBuffer(GL_ARRAY_BUFFER, opengl_buffers.vertex);
      glBufferData(GL_ARRAY_BUFFER, effect.mod.vertices.size() * sizeof(tfrag3::MercVertex),
                   m_mod_vtx_temp.data(), GL_DYNAMIC_DRAW);
    }
  }
}

// We can run into a problem where adding a PC model would overflow the
// preallocated draw/bone buffers.
// So we break this part into two functions:
// - init_pc_model, which doesn't allocate bones/draws

void Merc2::model_mod_draws(int num_effects,
                            const tfrag3::MercModel* model,
                            const LevelData* lev,
                            const u8* input_data,
                            const DmaTransfer& setup,
                            ModBuffers* mod_opengl_buffers,
                            MercDebugStats* stats) {
  auto p = scoped_prof("update-verts");

  // loop over effects. Mod vertices are done per effect (possibly a bad idea?)
  for (int ei = 0; ei < num_effects; ei++) {
    const auto& effect = model->effects[ei];
    // some effects might have no mod draw info, and no modifiable vertices
    if (effect.mod.mod_draw.empty()) {
      continue;
    }

    prof().begin_event("start1");
    // grab opengl buffer
    auto opengl_buffers = alloc_mod_vtx_buffer(lev);
    mod_opengl_buffers[ei] = opengl_buffers;

    // check that we have enough room for the finished thing.
    if (effect.mod.vertices.size() > MAX_MOD_VTX) {
      fmt::print("More mod vertices than MAX_MOD_VTX. {} > {}\n", effect.mod.vertices.size(),
                 MAX_MOD_VTX);
      ASSERT_NOT_REACHED();
    }

    // check that we have enough room for unpack
    if (effect.mod.expect_vidx_end > MAX_MOD_VTX) {
      fmt::print("More mod vertices (temp) than MAX_MOD_VTX. {} > {}\n", effect.mod.expect_vidx_end,
                 MAX_MOD_VTX);
      ASSERT_NOT_REACHED();
    }

    // start with the "correct" vertices from the model data:
    memcpy(m_mod_vtx_temp.data(), effect.mod.vertices.data(),
           sizeof(tfrag3::MercVertex) * effect.mod.vertices.size());

    // get pointers to the fragment and fragment control data
    u32 goal_addr;
    memcpy(&goal_addr, input_data + 4 * ei, 4);
    const u8* ee0 = setup.data - setup.data_offset;
    const u8* merc_effect = ee0 + goal_addr;
    u16 frag_cnt;
    memcpy(&frag_cnt, merc_effect + 18, 2);
    ASSERT(frag_cnt >= effect.mod.fragment_mask.size());
    u32 frag_goal;
    memcpy(&frag_goal, merc_effect, 4);
    u32 frag_ctrl_goal;
    memcpy(&frag_ctrl_goal, merc_effect + 4, 4);
    const u8* frag = ee0 + frag_goal;
    const u8* frag_ctrl = ee0 + frag_ctrl_goal;

    // loop over frags
    u32 vidx = 0;
    // u32 st_vif_add = model->st_vif_add;
    float xyz_scale = model->xyz_scale;
    prof().end_event();
    {
      // we're going to look at data that the game may be modifying.
      // in the original game, they didn't have any lock, but I think that the
      // scratchpad access from the EE would effectively block the VIF1 DMA, so you'd
      // hopefully never get a partially updated model (which causes obvious holes).
      // this lock is not ideal, and can block the rendering thread while blerc_execute runs,
      // which can take up to 2ms on really blerc-heavy scenes
      std::unique_lock<std::mutex> lk(g_merc_data_mutex);
      [[maybe_unused]] int frags_done = 0;
      auto p = scoped_prof("vert-math");

      // loop over fragments
      for (u32 fi = 0; fi < effect.mod.fragment_mask.size(); fi++) {
        frags_done++;
        u8 mat_xfer_count = frag_ctrl[3];

        // we create a mask of fragments to skip because they have no vertices.
        // the indexing data assumes that we skip the other fragments.
        if (effect.mod.fragment_mask[fi]) {
          // read fragment metadata
          u8 unsigned_four_count = frag_ctrl[0];
          u8 lump_four_count = frag_ctrl[1];
          u32 mm_qwc_off = frag[10];
          float float_offsets[3];
          memcpy(float_offsets, &frag[mm_qwc_off * 16], 12);
          u32 my_u4_count = ((unsigned_four_count + 3) / 4) * 16;
          u32 my_l4_count = my_u4_count + ((lump_four_count + 3) / 4) * 16;

          // loop over vertices in the fragment and unpack
          for (u32 w = my_u4_count / 4; w < (my_l4_count / 4) - 2; w += 3) {
            // positions
            u32 q0w = 0x4b010000 + frag[w * 4 + (0 * 4) + 3];
            u32 q1w = 0x4b010000 + frag[w * 4 + (1 * 4) + 3];
            u32 q2w = 0x4b010000 + frag[w * 4 + (2 * 4) + 3];

            // normals
            u32 q0z = 0x47800000 + frag[w * 4 + (0 * 4) + 2];
            u32 q1z = 0x47800000 + frag[w * 4 + (1 * 4) + 2];
            u32 q2z = 0x47800000 + frag[w * 4 + (2 * 4) + 2];

            // uvs
            u32 q2x = model->st_vif_add + frag[w * 4 + (2 * 4) + 0];
            u32 q2y = model->st_vif_add + frag[w * 4 + (2 * 4) + 1];

            auto* pos_array = m_mod_vtx_unpack_temp[vidx].pos;
            memcpy(&pos_array[0], &q0w, 4);
            memcpy(&pos_array[1], &q1w, 4);
            memcpy(&pos_array[2], &q2w, 4);
            pos_array[0] += float_offsets[0];
            pos_array[1] += float_offsets[1];
            pos_array[2] += float_offsets[2];
            pos_array[0] *= xyz_scale;
            pos_array[1] *= xyz_scale;
            pos_array[2] *= xyz_scale;

            auto* nrm_array = m_mod_vtx_unpack_temp[vidx].nrm;
            memcpy(&nrm_array[0], &q0z, 4);
            memcpy(&nrm_array[1], &q1z, 4);
            memcpy(&nrm_array[2], &q2z, 4);
            nrm_array[0] += -65537;
            nrm_array[1] += -65537;
            nrm_array[2] += -65537;

            auto* uv_array = m_mod_vtx_unpack_temp[vidx].uv;
            memcpy(&uv_array[0], &q2x, 4);
            memcpy(&uv_array[1], &q2y, 4);
            uv_array[0] += model->st_magic;
            uv_array[1] += model->st_magic;

            vidx++;
          }
        }

        // next control
        frag_ctrl += 4 + 2 * mat_xfer_count;

        // next frag
        u32 mm_qwc_count = frag[11];
        frag += mm_qwc_count * 16;
      }

      // sanity check
      if (effect.mod.expect_vidx_end != vidx) {
        fmt::print("---------- BAD {}/{}\n", effect.mod.expect_vidx_end, vidx);
        ASSERT(false);
      }
    }

    {
      auto pp = scoped_prof("copy");
      // now copy the data in merc original vertex order to the output.
      for (u32 vi = 0; vi < effect.mod.vertices.size(); vi++) {
        u32 addr = effect.mod.vertex_lump4_addr[vi];
        if (addr < vidx) {
          memcpy(&m_mod_vtx_temp[vi], &m_mod_vtx_unpack_temp[addr], 32);
          m_mod_vtx_temp[vi].st[0] = m_mod_vtx_unpack_temp[addr].uv[0];
          m_mod_vtx_temp[vi].st[1] = m_mod_vtx_unpack_temp[addr].uv[1];
        }
      }
    }

    // and upload to GPU
    stats->num_uploads++;
    stats->num_upload_bytes += effect.mod.vertices.size() * sizeof(tfrag3::MercVertex);
    {
      auto pp = scoped_prof("update-verts-upload");
      glBindBuffer(GL_ARRAY_BUFFER, opengl_buffers.vertex);
      glBufferData(GL_ARRAY_BUFFER, effect.mod.vertices.size() * sizeof(tfrag3::MercVertex),
                   m_mod_vtx_temp.data(), GL_DYNAMIC_DRAW);
    }
  }
}

/*!
 * Setup draws for a model, given the DMA data generated by the GOAL code.
 */
void Merc2::handle_pc_model(const DmaTransfer& setup,
                            SharedRenderState* render_state,
                            ScopedProfilerNode& proff,
                            MercDebugStats* stats) {
  auto p = scoped_prof("init-pc");

  // the format of the data is:
  //  ;; name   (128 char, 8 qw)
  //  ;; lights (7 qw x 1)
  //  ;; matrix slot string (128 char, 8 qw)
  //  ;; matrices (7 qw x N)
  //  ;; flags    (num-effects, effect-alpha-ignore, effect-disable)
  //  ;; fades    (u32 x N), padding to qw aligned
  //  ;; pointers (u32 x N), padding

  // Get the name
  const u8* input_data = setup.data;
  ASSERT(strlen((const char*)input_data) < 127);
  char name[128];
  strcpy(name, (const char*)setup.data);
  input_data += 128;

  // Look up the model by name in the loader.
  // This will return a reference to this model's data, plus a reference to the level's data
  // for stuff shared between models of the same level
  auto model_ref = render_state->loader->get_merc_model(name);
  if (!model_ref) {
    // it can fail, if the game is faster than the loader. In this case, we just don't draw.
    stats->num_missing_models++;
    return;
  }

  // next, we need to check if we have enough room to draw this effect.
  const LevelData* lev = model_ref->level;
  const tfrag3::MercModel* model = model_ref->model;

  // each model uses only 1 light.
  if (m_next_free_light >= MAX_LIGHTS) {
    fmt::print("MERC2 out of lights, consider increasing MAX_LIGHTS\n");
    flush_draw_buckets(render_state, proff, stats);
  }

  // models use many bones. First check if we need to flush:
  int bone_count = model->max_bones + 1;
  if (m_next_free_bone_vector + m_opengl_buffer_alignment + bone_count * 8 >
      MAX_SHADER_BONE_VECTORS) {
    fmt::print("MERC2 out of bones, consider increasing MAX_SHADER_BONE_VECTORS\n");
    flush_draw_buckets(render_state, proff, stats);
  }

  // also sanity check that we have enough to draw the model
  if (m_opengl_buffer_alignment + bone_count * 8 > MAX_SHADER_BONE_VECTORS) {
    fmt::print(
        "MERC2 doesn't have enough bones to draw a model, increase MAX_SHADER_BONE_VECTORS\n");
    ASSERT_NOT_REACHED();
  }

  // next, we need to find a bucket that holds draws for this level (will have the right buffers
  // bound for drawing)
  LevelDrawBucket* lev_bucket = nullptr;
  for (u32 i = 0; i < m_next_free_level_bucket; i++) {
    if (m_level_draw_buckets[i].level == lev) {
      lev_bucket = &m_level_draw_buckets[i];
      break;
    }
  }

  if (!lev_bucket) {
    // no existing bucket, allocate a new one.
    if (m_next_free_level_bucket >= m_level_draw_buckets.size()) {
      // out of room, flush
      // fmt::print("MERC2 out of levels, consider increasing MAX_LEVELS\n");
      flush_draw_buckets(render_state, proff, stats);
    }
    // alloc a new one
    lev_bucket = &m_level_draw_buckets[m_next_free_level_bucket++];
    lev_bucket->reset();
    lev_bucket->level = lev;
  }

  // next check draws:
  if (lev_bucket->next_free_draw + model->max_draws >= lev_bucket->draws.size()) {
    // out of room, flush
    fmt::print("MERC2 out of draws, consider increasing MAX_DRAWS_PER_LEVEL\n");
    flush_draw_buckets(render_state, proff, stats);
    if (model->max_draws >= lev_bucket->draws.size()) {
      ASSERT_NOT_REACHED_MSG("MERC2 draw buffer not big enough");
    }
  }

  // same for envmap draws
  if (lev_bucket->next_free_envmap_draw + model->max_draws >= lev_bucket->envmap_draws.size()) {
    // out of room, flush
    fmt::print("MERC2 out of envmap draws, consider increasing MAX_ENVMAP_DRAWS_PER_LEVEL\n");
    flush_draw_buckets(render_state, proff, stats);
    if (model->max_draws >= lev_bucket->envmap_draws.size()) {
      ASSERT_NOT_REACHED_MSG("MERC2 envmap draw buffer not big enough");
    }
  }

  // Next part of input data is the lights
  VuLights current_lights;
  memcpy(&current_lights, input_data, sizeof(VuLights));
  input_data += sizeof(VuLights);

  u64 uses_water = 0;
  if (render_state->version == GameVersion::Jak1) {
    // jak 1 figures out water at runtime sadly
    memcpy(&uses_water, input_data, 8);
    input_data += 16;
  }

  // Next part is the matrix slot string. The game sends us a bunch of bone matrices,
  // but they may not be in order, or include all bones. The matrix slot string tells
  // us which bones go where. (the game doesn't go in order because it follows the merc format)
  ShaderMercMat skel_matrix_buffer[MAX_SKEL_BONES];
  auto* matrix_array = (const u32*)(input_data + 128);
  int i;
  for (i = 0; i < 128; i++) {
    if (input_data[i] == 0xff) {  // indicates end of string.
      break;
    }
    // read goal addr of matrix (matrix data isn't known at merc dma time, bones runs after)
    u32 addr;
    memcpy(&addr, &matrix_array[i * 4], 4);
    const u8* real_addr = setup.data - setup.data_offset + addr;
    ASSERT(input_data[i] < MAX_SKEL_BONES);
    // get the matrix data
    memcpy(&skel_matrix_buffer[input_data[i]], real_addr, sizeof(MercMat));
  }
  input_data += 128 + 16 * i;

  // Next part is some flags
  struct PcMercFlags {
    u64 enable_mask;
    u64 ignore_alpha_mask;
    u8 effect_count;
    u8 bitflags;
  };
  auto* flags = (const PcMercFlags*)input_data;
  int num_effects = flags->effect_count;  // mostly just a sanity check
  ASSERT(num_effects < kMaxEffect);
  u64 current_ignore_alpha_bits = flags->ignore_alpha_mask;  // shader settings
  u64 current_effect_enable_bits = flags->enable_mask;       // mask for game to disable an effect
  bool model_uses_mod = flags->bitflags & 1;  // if we should update vertices from game.
  bool model_disables_fog = flags->bitflags & 2;
  bool model_uses_pc_blerc = flags->bitflags & 4;
  bool model_disables_envmap = flags->bitflags & 8;
  bool model_no_texture = flags->bitflags & 16;
  input_data += 32;

  float blerc_weights[kMaxBlerc];
  if (model_uses_pc_blerc) {
    memcpy(blerc_weights, input_data, kMaxBlerc * sizeof(float));
    input_data += kMaxBlerc * sizeof(float);
  }

  // Next is "fade data", indicating the color/intensity of envmap effect
  u8 fade_buffer[4 * kMaxEffect];
  for (int ei = 0; ei < num_effects; ei++) {
    for (int j = 0; j < 4; j++) {
      fade_buffer[ei * 4 + j] = input_data[ei * 4 + j];
    }
  }
  input_data += (((num_effects * 4) + 15) / 16) * 16;

  // Next is pointers to merc data, needed so we can update vertices

  // custom models are likely to have a different number of effects than what GOAL reports, update
  // the count here (after reading DMA) so we don't potentially go out of bounds when we do
  // blerc/mod draws
  if (model->effects.at(0).all_draws.at(0).no_strip) {
    num_effects = model->effects.size();
  }

  // will hold opengl buffers for the updated vertices
  ModBuffers mod_opengl_buffers[kMaxEffect];
  if (model_uses_pc_blerc) {
    model_mod_blerc_draws(num_effects, model, lev, mod_opengl_buffers, blerc_weights, stats);
  } else if (model_uses_mod) {  // only if we've enabled, this path is slow.
    model_mod_draws(num_effects, model, lev, input_data, setup, mod_opengl_buffers, stats);
  }

  // stats
  stats->num_models++;
  for (const auto& effect : model_ref->model->effects) {
    bool envmap = effect.has_envmap && !model_disables_envmap;
    stats->num_effects++;
    stats->num_predicted_draws += effect.all_draws.size();
    if (envmap) {
      stats->num_envmap_effects++;
      stats->num_predicted_draws += effect.all_draws.size();
    }
    for (const auto& draw : effect.all_draws) {
      stats->num_predicted_tris += draw.num_triangles;
      if (envmap) {
        stats->num_predicted_tris += draw.num_triangles;
      }
    }
  }

  if (stats->collect_debug_model_list) {
    auto& d = stats->model_list.emplace_back();
    d.name = model->name;
    d.level = model_ref->level->level->level_name;
    for (auto& e : model->effects) {
      auto& de = d.effects.emplace_back();
      de.envmap = e.has_envmap;
      de.envmap_mode = e.envmap_mode;
      for (auto& draw : e.all_draws) {
        auto& dd = de.draws.emplace_back();
        dd.mode = draw.mode;
        dd.num_tris = draw.num_triangles;
      }
    }
  }

  // allocate bones in shared bone buffer to be sent to GPU at flush-time
  u32 first_bone = alloc_bones(bone_count, skel_matrix_buffer);

  // allocate lights
  if (current_lights.w1) {
    if (render_state->version != GameVersion::Jak3) {
      current_lights.w1 = 0;  // force off merc fade in jak2/1 - a bunch of stuff uses this
    }
  }
  u32 lights = alloc_lights(current_lights);
  stats->num_lights++;

  u64 hash = fnv64(model->name);

#if defined(__SWITCH__)
  // J18 probe (AI-assisted): black merc props (jak3 wasteland cars/posts, jak2 breakable
  // walls). One line per model name: lights + the first textures it binds, so one SD trip
  // tells "black texture" from "black lighting". gk_dkgl_mercnotex.txt draws merc with
  // vertex lighting only (no texture) for the A/B.
  static const bool s_merc_notex =
      std::fopen("sdmc:/switch/jak3/gk_dkgl_mercnotex.txt", "r") != nullptr ||
      std::fopen("sdmc:/switch/jak2/gk_dkgl_mercnotex.txt", "r") != nullptr;
  {
    static std::unordered_set<u64> s_probed;
    if (s_probed.size() < 600 && s_probed.insert(hash).second) {
      const auto& a = current_lights.ambient;
      const auto& c0 = current_lights.color0;
      std::string texs;
      int shown = 0;
      for (const auto& e : model->effects) {
        for (const auto& d : e.all_draws) {
          if (shown >= 3 || d.eye_id != 0xff) {
            continue;
          }
          shown++;
          const int ti = d.tree_tex_id;
          char buf[200];
          if (ti >= 0 && ti < (int)lev->textures.size() && lev->level &&
              ti < (int)lev->level->textures.size()) {
            const auto& t = lev->level->textures[ti];
            u32 mean[3] = {0, 0, 0};
            if (t.format == tfrag3::TEXTURE_FMT_RGBA && (size_t)t.w * t.h * 4 <= t.data.size() &&
                t.w * t.h > 0) {
              const u8* px = (const u8*)t.data.data();
              for (size_t p = 0; p < (size_t)t.w * t.h; p++) {
                for (int c = 0; c < 3; c++) {
                  mean[c] += px[p * 4 + c];
                }
              }
              for (int c = 0; c < 3; c++) {
                mean[c] /= (u32)(t.w * t.h);
              }
            }
            snprintf(buf, sizeof(buf), " [t%d id=%u %s/%s %ux%u f%u data=%zu rgb=%u,%u,%u]", ti,
                     (unsigned)lev->textures[ti], t.debug_tpage_name.c_str(),
                     t.debug_name.c_str(), t.w, t.h, (unsigned)t.format, t.data.size(), mean[0],
                     mean[1], mean[2]);
          } else {
            snprintf(buf, sizeof(buf), " [t%d OUT-OF-RANGE n=%zu]", ti, lev->textures.size());
          }
          texs += buf;
        }
      }
      switch_run_logf("[mercprobe] %s lev=%s bits=%u w1=%u amb=%.2f,%.2f,%.2f,%.2f "
                      "c0=%.2f,%.2f,%.2f%s",
                      model->name.c_str(), lev->level ? lev->level->level_name.c_str() : "?",
                      (unsigned)flags->bitflags, (unsigned)current_lights.w1, a.x(), a.y(), a.z(),
                      a.w(), c0.x(), c0.y(), c0.z(), texs.c_str());
    }
  }
  // J18 periodic light log (AI-assisted): the probe above logs once per boot, so a car that
  // turns black later is never caught. gk_dkgl_merclog.txt = full lights every ~120 draws.
  static const bool s_merc_log =
      std::fopen("sdmc:/switch/jak3/gk_dkgl_merclog.txt", "r") != nullptr ||
      std::fopen("sdmc:/switch/jak2/gk_dkgl_merclog.txt", "r") != nullptr;
  if (s_merc_log) {
    static std::unordered_map<u64, u32> s_calls;
    static u32 s_lines = 0;
    if (s_lines < 6000 && (s_calls[hash]++ % 120) == 0) {
      s_lines++;
      const auto& L = current_lights;
      // J18 (AI-assisted): w1/w2 added -- the sink/despawn fade state was never captured
      // ([mercprobe] only fires at spawn, where w1=0 for cars).
      switch_run_logf("[merclog] %s w1=%u w2=%u amb=%.2f,%.2f,%.2f c0=%.2f,%.2f,%.2f "
                      "c1=%.2f,%.2f,%.2f c2=%.2f,%.2f,%.2f d0=%.2f,%.2f,%.2f",
                      model->name.c_str(), (unsigned)L.w1, (unsigned)L.w2, L.ambient.x(),
                      L.ambient.y(), L.ambient.z(), L.color0.x(), L.color0.y(), L.color0.z(),
                      L.color1.x(), L.color1.y(), L.color1.z(), L.color2.x(), L.color2.y(),
                      L.color2.z(), L.direction0.x(), L.direction0.y(), L.direction0.z());
    }
  }
#endif

  DrawArgs args;
  args.lev_bucket = lev_bucket;
  args.jak1_water_mode = uses_water;
  args.disable_fog = model_disables_fog;
  args.hash = hash;
  args.lights = lights;
  args.first_bone = first_bone;
  args.no_texture = render_state->version == GameVersion::Jak3 && model_no_texture;
#if defined(__SWITCH__)
  args.no_texture = args.no_texture || s_merc_notex;
#endif

  // loop over effects, creating draws for each
  for (size_t ei = 0; ei < model->effects.size(); ei++) {
    args.fade = fade_buffer + 4 * ei;

    // game has disabled it?
    if (!(current_effect_enable_bits & (1ull << ei))) {
      continue;
    }

    // imgui menu disabled it?
    if (!m_effect_debug_mask[ei]) {
      continue;
    }

    bool ignore_alpha = !!(current_ignore_alpha_bits & (1ull << ei));
    args.ignore_alpha = ignore_alpha;
    auto& effect = model->effects[ei];

    bool should_envmap = effect.has_envmap && !model_disables_envmap;
    bool should_mod = (model_uses_pc_blerc || model_uses_mod) && effect.has_mod_draw;

    if (should_mod) {
      // draw as two parts, fixed and mod

      // do fixed draws:
      for (auto& fdraw : effect.mod.fix_draw) {
        alloc_normal_draw(fdraw, args);
        if (should_envmap) {
          try_alloc_envmap_draw(fdraw, effect.envmap_mode, effect.envmap_texture, args);
        }
      }

      // do mod draws
      for (auto& mdraw : effect.mod.mod_draw) {
        auto n = alloc_normal_draw(mdraw, args);
        // modify the draw, set the mod flag and point it to the opengl buffer
        n->flags |= MOD_VTX;
        n->mod_vtx_buffer = mod_opengl_buffers[ei];
        if (should_envmap) {
          auto e = try_alloc_envmap_draw(mdraw, effect.envmap_mode, effect.envmap_texture, args);
          if (e) {
            e->flags |= MOD_VTX;
            e->mod_vtx_buffer = mod_opengl_buffers[ei];
          }
        }
      }
    } else {
      // no mod, just do all_draws
      for (auto& draw : effect.all_draws) {
        if (should_envmap) {
          try_alloc_envmap_draw(draw, effect.envmap_mode, effect.envmap_texture, args);
        }
        alloc_normal_draw(draw, args);
      }
    }
  }
}

void Merc2::draw_debug_window(MercDebugStats* stats) {
  ImGui::Text("Models   : %d", stats->num_models);
  ImGui::Text("Effects  : %d", stats->num_effects);
  ImGui::Text("Draws (p): %d", stats->num_predicted_draws);
  ImGui::Text("Tris  (p): %d", stats->num_predicted_tris);
  ImGui::Text("Bones    : %d", stats->num_bones_uploaded);
  ImGui::Text("Lights   : %d", stats->num_lights);
  ImGui::Text("Dflush   : %d", stats->num_draw_flush);

  ImGui::Text("EEffects : %d", stats->num_envmap_effects);
  ImGui::Text("ETris    : %d", stats->num_envmap_tris);

  ImGui::Text("Uploads  : %d", stats->num_uploads);
  ImGui::Text("Upload kB: %d", stats->num_upload_bytes / 1024);

  ImGui::Checkbox("Debug", &stats->collect_debug_model_list);

  ImGui::SliderFloat("blerc-nightmare", &blerc_multiplier, -3, 3);

  if (stats->collect_debug_model_list) {
    for (int i = 0; i < kMaxEffect; i++) {
      ImGui::Checkbox(fmt::format("e{:02d}", i).c_str(), &m_effect_debug_mask[i]);
    }

    for (const auto& model : stats->model_list) {
      if (ImGui::TreeNode(model.name.c_str())) {
        ImGui::Text("Level: %s\n", model.level.c_str());
        for (const auto& e : model.effects) {
          for (const auto& d : e.draws) {
            ImGui::Text("%s", d.mode.to_string().c_str());
          }
          ImGui::Separator();
        }
        ImGui::TreePop();
      }
    }
  }
}

void Merc2::init_shader_common(Shader& shader, Uniforms* uniforms, bool include_lights) {
  auto id = shader.id();
  shader.activate();
  if (include_lights) {
    uniforms->light_direction[0] = gl_uniform_loc(id, "light_dir0_fade");
    uniforms->light_direction[1] = gl_uniform_loc(id, "light_dir1_fade_en");
    uniforms->light_direction[2] = gl_uniform_loc(id, "light_dir2");
    uniforms->light_color[0] = gl_uniform_loc(id, "light_col0");
    uniforms->light_color[1] = gl_uniform_loc(id, "light_col1");
    uniforms->light_color[2] = gl_uniform_loc(id, "light_col2");
    uniforms->light_ambient = gl_uniform_loc(id, "light_ambient");
  }

  uniforms->hvdf_offset = gl_uniform_loc(id, "hvdf_offset");

  uniforms->fog = gl_uniform_loc(id, "fog_constants");
  uniforms->decal = gl_uniform_loc(id, "decal_enable");

  uniforms->fog_color = gl_uniform_loc(id, "fog_color");
  uniforms->perspective_matrix = gl_uniform_loc(id, "perspective_matrix");
  uniforms->ignore_alpha = gl_uniform_loc(id, "ignore_alpha");

  uniforms->gfx_hack_no_tex = gl_uniform_loc(id, "gfx_hack_no_tex");
}

void Merc2::switch_to_merc2(SharedRenderState* render_state) {
  render_state->shaders[ShaderId::MERC2].activate();

  // set uniforms that we know from render_state
  glUniform4f(m_merc_uniforms.fog_color, render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
              render_state->fog_intensity / 255);
  glUniform1i(m_merc_uniforms.gfx_hack_no_tex, Gfx::g_global_settings.hack_no_tex);
}

void Merc2::switch_to_emerc(SharedRenderState* render_state) {
  render_state->shaders[ShaderId::EMERC].activate();
  // set uniforms that we know from render_state
  glUniform4f(m_emerc_uniforms.fog_color, render_state->fog_color[0] / 255.f,
              render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
              render_state->fog_intensity / 255);
  glUniform1i(m_emerc_uniforms.gfx_hack_no_tex, Gfx::g_global_settings.hack_no_tex);
}

/*!
 * Main merc2 rendering.
 */
void Merc2::render(DmaFollower& dma,
                   SharedRenderState* render_state,
                   ScopedProfilerNode& prof,
                   MercDebugStats* stats) {
  bool hack = stats->collect_debug_model_list;
  *stats = {};
  stats->collect_debug_model_list = hack;
  if (stats->collect_debug_model_list) {
    stats->model_list.clear();
  }

  switch_to_merc2(render_state);

  {
    auto pp = scoped_prof("handle-all-dma");
    // iterate through the dma chain, filling buckets
    handle_all_dma(dma, render_state, prof, stats);
  }

  {
    auto pp = scoped_prof("flush-buckets");
    // flush buckets to draws
    flush_draw_buckets(render_state, prof, stats);
  }
}

u32 Merc2::alloc_lights(const VuLights& lights) {
  ASSERT(m_next_free_light < MAX_LIGHTS);
  u32 light_idx = m_next_free_light;
  m_lights_buffer[m_next_free_light++] = lights;
  static_assert(sizeof(VuLights) == 7 * 16);
  return light_idx;
}

std::string Merc2::ShaderMercMat::to_string() const {
  return fmt::format("tmat:\n{}\n{}\n{}\n{}\n", tmat[0].to_string_aligned(),
                     tmat[1].to_string_aligned(), tmat[2].to_string_aligned(),
                     tmat[3].to_string_aligned());
}

/*!
 * Main MERC2 function to handle DMA
 */
void Merc2::handle_all_dma(DmaFollower& dma,
                           SharedRenderState* render_state,
                           ScopedProfilerNode& prof,
                           MercDebugStats* stats) {
  // process the first tag. this is just jumping to the merc-specific dma.
  auto data0 = dma.read_and_advance();
  ASSERT(data0.vif1() == 0 || data0.vifcode1().kind == VifCode::Kind::NOP);
  ASSERT(data0.vif0() == 0 || data0.vifcode0().kind == VifCode::Kind::NOP ||
         data0.vifcode0().kind == VifCode::Kind::MARK);
  ASSERT(data0.size_bytes == 0);
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    // renderer didn't run, let's just get out of here.
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    ASSERT(dma.current_tag_offset() == render_state->next_bucket);
    return;
  }

  if (dma.current_tag_offset() == render_state->next_bucket) {
    return;
  }
  // if we reach here, there's stuff to draw
  // this handles merc-specific setup DMA
  handle_setup_dma(dma, render_state);

  // handle each merc transfer
  while (dma.current_tag_offset() != render_state->next_bucket) {
    handle_merc_chain(dma, render_state, prof, stats);
  }
  ASSERT(dma.current_tag_offset() == render_state->next_bucket);
}

namespace {
void set_uniform(GLuint uniform, const math::Vector3f& val) {
  glUniform3f(uniform, val.x(), val.y(), val.z());
}
void set_uniform(GLuint uniform, const math::Vector4f& val) {
  glUniform4f(uniform, val.x(), val.y(), val.z(), val.w());
}
}  // namespace

void Merc2::handle_setup_dma(DmaFollower& dma, SharedRenderState* render_state) {
  auto first = dma.read_and_advance();

  // 10 quadword setup packet
  ASSERT(first.size_bytes == 10 * 16);

  // transferred vifcodes
  {
    auto vif0 = first.vifcode0();
    auto vif1 = first.vifcode1();
    // STCYCL 4, 4
    ASSERT(vif0.kind == VifCode::Kind::STCYCL);
    auto vif0_st = VifCodeStcycl(vif0);
    ASSERT(vif0_st.cl == 4 && vif0_st.wl == 4);
    // STMOD
    ASSERT(vif1.kind == VifCode::Kind::STMOD);
    ASSERT(vif1.immediate == 0);
  }

  // 1 qw with 4 vifcodes.
  u32 vifcode_data[4];
  memcpy(vifcode_data, first.data, 16);
  {
    auto vif0 = VifCode(vifcode_data[0]);
    ASSERT(vif0.kind == VifCode::Kind::BASE);
    ASSERT(vif0.immediate == MercDataMemory::BUFFER_BASE);
    auto vif1 = VifCode(vifcode_data[1]);
    ASSERT(vif1.kind == VifCode::Kind::OFFSET);
    ASSERT((s16)vif1.immediate == MercDataMemory::BUFFER_OFFSET);
    auto vif2 = VifCode(vifcode_data[2]);
    ASSERT(vif2.kind == VifCode::Kind::NOP);
    auto vif3 = VifCode(vifcode_data[3]);
    ASSERT(vif3.kind == VifCode::Kind::UNPACK_V4_32);
    VifCodeUnpack up(vif3);
    ASSERT(up.addr_qw == MercDataMemory::LOW_MEMORY);
    ASSERT(!up.use_tops_flag);
    ASSERT(vif3.num == 8);
  }

  // 8 qw's of low memory data
  memcpy(&m_low_memory, first.data + 16, sizeof(LowMemory));

  switch_to_merc2(render_state);
  set_uniform(m_merc_uniforms.hvdf_offset, m_low_memory.hvdf_offset);
  set_uniform(m_merc_uniforms.fog, m_low_memory.fog);
  glUniformMatrix4fv(m_merc_uniforms.perspective_matrix, 1, GL_FALSE,
                     &m_low_memory.perspective[0].x());
  switch_to_emerc(render_state);
  set_uniform(m_emerc_uniforms.hvdf_offset, m_low_memory.hvdf_offset);
  set_uniform(m_emerc_uniforms.fog, m_low_memory.fog);
  glUniformMatrix4fv(m_emerc_uniforms.perspective_matrix, 1, GL_FALSE,
                     &m_low_memory.perspective[0].x());

  // 1 qw with another 4 vifcodes.
  u32 vifcode_final_data[4];
  memcpy(vifcode_final_data, first.data + 16 + sizeof(LowMemory), 16);
  {
    ASSERT(VifCode(vifcode_final_data[0]).kind == VifCode::Kind::FLUSHE);
    ASSERT(vifcode_final_data[1] == 0);
    ASSERT(vifcode_final_data[2] == 0);
    VifCode mscal(vifcode_final_data[3]);
    ASSERT(mscal.kind == VifCode::Kind::MSCAL);
    ASSERT(mscal.immediate == 0);
  }

  // TODO: process low memory initialization

  if (render_state->version == GameVersion::Jak1) {
    auto second = dma.read_and_advance();
    ASSERT(second.size_bytes == 32);  // setting up test register.
    auto nothing = dma.read_and_advance();
    ASSERT(nothing.size_bytes == 0);
    ASSERT(nothing.vif0() == 0);
    ASSERT(nothing.vif1() == 0);
  } else {
    auto second = dma.read_and_advance();
    ASSERT(second.size_bytes == 48);  // setting up test/zbuf register.
    // todo z write mask stuff.
    auto nothing = dma.read_and_advance();
    ASSERT(nothing.size_bytes == 0);
    ASSERT(nothing.vif0() == 0);
    ASSERT(nothing.vif1() == 0);
  }
}

namespace {
bool tag_is_nothing_next(const DmaFollower& dma) {
  return dma.current_tag().kind == DmaTag::Kind::NEXT && dma.current_tag().qwc == 0 &&
         dma.current_tag_vif0() == 0 && dma.current_tag_vif1() == 0;
}
}  // namespace

void Merc2::handle_merc_chain(DmaFollower& dma,
                              SharedRenderState* render_state,
                              ScopedProfilerNode& prof,
                              MercDebugStats* stats) {
  while (tag_is_nothing_next(dma)) {
    auto nothing = dma.read_and_advance();
    ASSERT(nothing.size_bytes == 0);
  }
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    return;
  }

  auto init = dma.read_and_advance();
  int skip_count = 2;
  if (render_state->version >= GameVersion::Jak2) {
    skip_count = 1;
  }

  while (init.vifcode1().kind == VifCode::Kind::PC_PORT) {
    // flush_pending_model(render_state, prof);
    handle_pc_model(init, render_state, prof, stats);
    for (int i = 0; i < skip_count; i++) {
      auto link = dma.read_and_advance();
      ASSERT(link.vifcode0().kind == VifCode::Kind::NOP);
      ASSERT(link.vifcode1().kind == VifCode::Kind::NOP);
      ASSERT(link.size_bytes == 0);
    }
    init = dma.read_and_advance();
  }

  if (init.vifcode0().kind == VifCode::Kind::FLUSHA) {
    int num_skipped = 0;
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
      num_skipped++;
    }
    ASSERT(num_skipped < 4);
    return;
  }
}

/*!
 * Queue up some bones to be included in the bone buffer.
 * Returns the index of the first bone vector.
 */
u32 Merc2::alloc_bones(int count, ShaderMercMat* data) {
  u32 first_bone_vector = m_next_free_bone_vector;
  ASSERT(count * 8 + first_bone_vector <= MAX_SHADER_BONE_VECTORS);

  // model should have under 128 bones.
  ASSERT(count <= MAX_SKEL_BONES);

  // iterate over each bone we need
  for (int i = 0; i < count; i++) {
    auto& skel_mat = data[i];
    auto* shader_mat = &m_shader_bone_vector_buffer[m_next_free_bone_vector];
    int bv = 0;

    // and copy to the large bone buffer.
    for (int j = 0; j < 4; j++) {
      shader_mat[bv++] = skel_mat.tmat[j];
    }

    for (int j = 0; j < 3; j++) {
      shader_mat[bv++] = skel_mat.nmat[j];
    }

    m_next_free_bone_vector += 8;
  }

  auto b0 = m_next_free_bone_vector;
  m_next_free_bone_vector += m_opengl_buffer_alignment - 1;
  m_next_free_bone_vector /= m_opengl_buffer_alignment;
  m_next_free_bone_vector *= m_opengl_buffer_alignment;
  ASSERT(b0 <= m_next_free_bone_vector);
  ASSERT(first_bone_vector + count * 8 <= m_next_free_bone_vector);
  return first_bone_vector;
}

Merc2::ModBuffers Merc2::alloc_mod_vtx_buffer(const LevelData* lev) {
  if (m_next_mod_vtx_buffer >= m_mod_vtx_buffers.size()) {
    GLuint b;
    glGenBuffers(1, &b);
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
    setup_merc_vao();
    m_mod_vtx_buffers.push_back({vao, b});
  }
  return m_mod_vtx_buffers[m_next_mod_vtx_buffer++];
}

Merc2::Draw* Merc2::try_alloc_envmap_draw(const tfrag3::MercDraw& mdraw,
                                          const DrawMode& envmap_mode,
                                          u32 envmap_texture,
                                          const DrawArgs& args) {
  bool nonzero_fade = false;
  for (int i = 0; i < 4; i++) {
    if (args.fade[i]) {
      nonzero_fade = true;
      break;
    }
  }
  if (!nonzero_fade) {
    return nullptr;
  }

  Draw* draw = &args.lev_bucket->envmap_draws[args.lev_bucket->next_free_envmap_draw++];
  draw->flags = 0;
  draw->first_index = mdraw.first_index;
  draw->index_count = mdraw.index_count;
  draw->mode = envmap_mode;
  draw->hash = 0;
  if (args.jak1_water_mode) {
    draw->mode.enable_ab();
    draw->mode.disable_depth_write();
  }
  draw->texture = envmap_texture;
  draw->first_bone = args.first_bone;
  draw->light_idx = args.lights;
  draw->num_triangles = mdraw.num_triangles;
  draw->no_strip = mdraw.no_strip;
  for (int i = 0; i < 4; i++) {
    draw->fade[i] = args.fade[i];
  }
  return draw;
}

Merc2::Draw* Merc2::alloc_normal_draw(const tfrag3::MercDraw& mdraw, const DrawArgs& args) {
  Draw* draw = &args.lev_bucket->draws[args.lev_bucket->next_free_draw++];
  draw->flags = 0;
  draw->first_index = mdraw.first_index;
  draw->index_count = mdraw.index_count;
  draw->mode = mdraw.mode;
  draw->hash = args.hash;
  if (args.jak1_water_mode) {
    draw->mode.set_ab(true);
    draw->mode.disable_depth_write();
  }

  if (args.disable_fog) {
    draw->mode.set_fog(false);
    // but don't toggle it the other way?
  }

  draw->texture = mdraw.eye_id == 0xff ? mdraw.tree_tex_id : (0xefffff00 | mdraw.eye_id);
  draw->first_bone = args.first_bone;
  draw->light_idx = args.lights;
  draw->num_triangles = mdraw.num_triangles;
  draw->no_strip = mdraw.no_strip;
  if (args.ignore_alpha) {
    draw->flags |= IGNORE_ALPHA;
  }
  if (args.no_texture) {
    draw->flags |= NO_TEXTURE;
  }
  for (int i = 0; i < 4; i++) {
    draw->fade[i] = 0;
  }
  return draw;
}

void Merc2::setup_merc_vao() {
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);
  glEnableVertexAttribArray(4);
  glEnableVertexAttribArray(5);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_GEQUAL);

  glVertexAttribPointer(0,                                        // location 0 in the shader
                        3,                                        // 3 values per vert
                        GL_FLOAT,                                 // floats
                        GL_FALSE,                                 // normalized
                        sizeof(tfrag3::MercVertex),               // stride
                        (void*)offsetof(tfrag3::MercVertex, pos)  // offset (0)
  );

  glVertexAttribPointer(1,                                              // location 1 in the
                        3,                                              // 3 values per vert
                        GL_FLOAT,                                       // floats
                        GL_FALSE,                                       // normalized
                        sizeof(tfrag3::MercVertex),                     // stride
                        (void*)offsetof(tfrag3::MercVertex, normal[0])  // offset (0)
  );

  glVertexAttribPointer(2,                                               // location 1 in the
                        3,                                               // 3 values per vert
                        GL_FLOAT,                                        // floats
                        GL_FALSE,                                        // normalized
                        sizeof(tfrag3::MercVertex),                      // stride
                        (void*)offsetof(tfrag3::MercVertex, weights[0])  // offset (0)
  );

  glVertexAttribPointer(3,                                          // location 1 in the shader
                        2,                                          // 3 values per vert
                        GL_FLOAT,                                   // floats
                        GL_FALSE,                                   // normalized
                        sizeof(tfrag3::MercVertex),                 // stride
                        (void*)offsetof(tfrag3::MercVertex, st[0])  // offset (0)
  );

  glVertexAttribPointer(4,                                            // location 1 in the shader
                        4,                                            // 3 values per vert
                        GL_UNSIGNED_BYTE,                             // floats
                        GL_TRUE,                                      // normalized
                        sizeof(tfrag3::MercVertex),                   // stride
                        (void*)offsetof(tfrag3::MercVertex, rgba[0])  // offset (0)
  );

  glVertexAttribIPointer(5,                                            // location 0 in the
                         4,                                            // 3 floats per vert
                         GL_UNSIGNED_BYTE,                             // u8's
                         sizeof(tfrag3::MercVertex),                   //
                         (void*)offsetof(tfrag3::MercVertex, mats[0])  // offset in array
  );
}

void Merc2::flush_draw_buckets(SharedRenderState* render_state,
                               ScopedProfilerNode& prof,
                               MercDebugStats* stats) {
  stats->num_draw_flush++;
  for (u32 li = 0; li < m_next_free_level_bucket; li++) {
    const auto& lev_bucket = m_level_draw_buckets[li];
    const auto* lev = lev_bucket.level;
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, lev->merc_vertices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
    setup_merc_vao();
    stats->num_bones_uploaded += m_next_free_bone_vector;

    glBindBuffer(GL_UNIFORM_BUFFER, m_bones_buffer);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, m_next_free_bone_vector * sizeof(math::Vector4f),
                    m_shader_bone_vector_buffer);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    switch_to_merc2(render_state);
    do_draws(lev_bucket.draws.data(), lev, lev_bucket.next_free_draw, m_merc_uniforms, prof, false,
             render_state);
    if (lev_bucket.next_free_envmap_draw) {
      switch_to_emerc(render_state);
      do_draws(lev_bucket.envmap_draws.data(), lev, lev_bucket.next_free_envmap_draw,
               m_emerc_uniforms, prof, true, render_state);
    }
  }

  m_next_free_light = 0;
  m_next_free_bone_vector = 0;
  m_next_free_level_bucket = 0;
  m_next_mod_vtx_buffer = 0;
}

#if defined(__SWITCH__)
// FIX 163 (AI-assisted, J18): defined in loader/LoaderStages.cpp -- the same BCn decoder
// the FIX 162 h64 path used to upload these very textures as RGBA8. Global scope on
// purpose: a block-scope declaration inside the anonymous namespace below would denote a
// different, never-defined (anonymous)::fix89_decode_bcn_mip and fail to link.
void fix89_decode_bcn_mip(const u8* src, u32 w, u32 h, bool is_bc1, u32* dst);

namespace {
// J18 bind probe (AI-assisted): black merc (jak3 cars, jak2 breakable walls) samples black RGB
// with the model's own texture id. Log what the bound dk id really is at DRAW time: alive or
// freed, and the image's size/format/mips next to the fr3 texture it should be. One line per
// id, plus a line whenever its alive state flips; dead binds are counted.
void merc_bind_probe(const LevelData* lev, int ti) {
  static std::unordered_map<u32, int> s_seen;  // id -> last alive state
  static u32 s_lines = 0;
  static u64 s_dead = 0;
  static u64 s_next_dead_report = 1;
  const u32 id = lev->textures[ti];
  u32 w = 0, h = 0, mips = 0, fmt = 0;
  const bool alive = dktexgpu::describe(id, &w, &h, &mips, &fmt);
  if (!alive && ++s_dead >= s_next_dead_report) {
    s_next_dead_report *= 2;
    switch_run_logf("[mercbind] dead binds total %llu", (unsigned long long)s_dead);
  }
  auto it = s_seen.find(id);
  if (it != s_seen.end() && it->second == (int)alive) {
    return;
  }
  s_seen[id] = (int)alive;
  if (s_lines >= 1500) {
    return;
  }
  s_lines++;
  const tfrag3::Texture* t = (lev->level && ti < (int)lev->level->textures.size())
                                 ? &lev->level->textures[ti]
                                 : nullptr;
  const bool mismatch = t && alive && (t->w != w || t->h != h);
  switch_run_logf("[mercbind] lev=%s t%d id=%u alive=%d dk=%ux%u m%u f%u fr3=%s/%s %ux%u f%u%s",
                  lev->level ? lev->level->level_name.c_str() : "?", ti, (unsigned)id,
                  (int)alive, w, h, mips, fmt, t ? t->debug_tpage_name.c_str() : "?",
                  t ? t->debug_name.c_str() : "?", t ? t->w : 0, t ? t->h : 0,
                  t ? (unsigned)t->format : 0, mismatch ? " MISMATCH" : "");

  // FIX 163 (AI-assisted, J18 content probe): FIX 162 put these textures on the GPU as
  // RGBA8 (f0 m7/m8 in every [mercbind] line of the 2026-11-23 boot) and the cars STILL
  // sample black, so log what the level-0 content actually is, once per id: overall mean
  // RGB ("the decode is black"), border mean ("UV overrun samples a black tpage edge")
  // and texel 0. Runs off the fr3 payload, needs nothing from the GPU.
  static std::unordered_map<u32, u32> s_rgb_done;
  if (t && s_rgb_done.insert({id, 1}).second) {
    const u32 pw = t->w, ph = t->h;
    const u32* px = nullptr;
    std::vector<u32> rgba;
    const u64 blocks = u64((pw + 3) / 4) * ((ph + 3) / 4);
    if (t->format == tfrag3::TEXTURE_FMT_RGBA && t->data.size() >= (size_t)pw * ph) {
      px = t->data.data();
    } else if ((t->format == tfrag3::TEXTURE_FMT_BC1 ||
                t->format == tfrag3::TEXTURE_FMT_BC3) &&
               !t->mip_offsets.empty() &&
               u64(t->mip_offsets[0]) + blocks * (t->format == tfrag3::TEXTURE_FMT_BC1 ? 8 : 16) <=
                   t->bcn_data.size()) {
      rgba.resize((size_t)pw * ph);
      fix89_decode_bcn_mip(t->bcn_data.data() + t->mip_offsets[0], pw, ph,
                           t->format == tfrag3::TEXTURE_FMT_BC1, rgba.data());
      px = rgba.data();
    }
    if (px && pw && ph) {
      double mr = 0, mg = 0, mb = 0;  // all-pixel sums (data words are 0xAABBGGRR)
      double br = 0, bg = 0, bb = 0;  // border-pixel sums
      u64 n = 0, bn = 0;
      for (u32 y = 0; y < ph; y++) {
        for (u32 x = 0; x < pw; x++) {
          const u32 p = px[(size_t)y * pw + x];
          const u32 r = p & 0xff, g = (p >> 8) & 0xff, b = (p >> 16) & 0xff;
          mr += r;
          mg += g;
          mb += b;
          n++;
          if (x == 0 || y == 0 || x + 1 == pw || y + 1 == ph) {
            br += r;
            bg += g;
            bb += b;
            bn++;
          }
        }
      }
      switch_run_logf("[mercrgb] id=%u %s/%s f%u mean=%u,%u,%u border=%u,%u,%u tex0=%08x",
                      (unsigned)id, t->debug_tpage_name.c_str(), t->debug_name.c_str(),
                      (unsigned)t->format, (unsigned)(mr / n), (unsigned)(mg / n),
                      (unsigned)(mb / n), (unsigned)(br / bn), (unsigned)(bg / bn),
                      (unsigned)(bb / bn), px[0]);
    }
  }
}
}  // namespace
#endif

void Merc2::do_draws(const Draw* draw_array,
                     const LevelData* lev,
                     u32 num_draws,
                     const Uniforms& uniforms,
                     ScopedProfilerNode& prof,
                     bool set_fade,
                     SharedRenderState* render_state) {
  glBindVertexArray(m_vao);
  s32 last_tex = INT32_MIN;
  int last_light = -1;
  bool normal_vtx_buffer_bound = true;

  bool fog_on = true;
#if defined(__SWITCH__)
  {
    // J18 fog probe (AI-assisted): distant jak3 merc fogs to black. Which fog colour does
    // merc get? (FIX 156 fills render_state->fog_color from the bucket-2 vis packets.)
    static u32 s_fog_calls = 0;
    if ((s_fog_calls++ % 3000) == 0 && s_fog_calls < 60000) {
      switch_run_logf("[mercfog] rgb=%u,%u,%u intensity=%.1f",
                      (unsigned)render_state->fog_color[0], (unsigned)render_state->fog_color[1],
                      (unsigned)render_state->fog_color[2], (double)render_state->fog_intensity);
    }
  }
#endif

  for (u32 di = 0; di < num_draws; di++) {
    auto& draw = draw_array[di];
    if (draw.flags & MOD_VTX) {
      glBindVertexArray(draw.mod_vtx_buffer.vao);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
      glBindBuffer(GL_ARRAY_BUFFER, lev->merc_vertices);
      normal_vtx_buffer_bound = false;
    } else {
      if (!normal_vtx_buffer_bound) {
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
        glBindBuffer(GL_ARRAY_BUFFER, lev->merc_vertices);
        normal_vtx_buffer_bound = true;
      }
    }
    glUniform1i(uniforms.ignore_alpha, draw.flags & DrawFlags::IGNORE_ALPHA);

    if (fog_on && !draw.mode.get_fog_enable()) {
      // on -> off
      glUniform4f(uniforms.fog_color, render_state->fog_color[0] / 255.f,
                  render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f, 0);
      fog_on = false;
    } else if (!fog_on && draw.mode.get_fog_enable()) {
      glUniform4f(uniforms.fog_color, render_state->fog_color[0] / 255.f,
                  render_state->fog_color[1] / 255.f, render_state->fog_color[2] / 255.f,
                  render_state->fog_intensity / 255);
      fog_on = true;
    }
#if defined(__SWITCH__)
    // DKGL_TODO Q (AI-assisted): since dkgl honours sampler objects (c155bb1b7) merc really
    // samples the BCn mip chain (jak2 only), and some small mips are wrong on deko3d (breakable
    // ruins props turned flat orange/beige). Back to level 0 only, like before c155bb1b7;
    // gk_dkgl_mercmips.txt turns merc mips back on for A/B.
    static const bool s_merc_mips =
        std::fopen("sdmc:/switch/jak2/gk_dkgl_mercmips.txt", "r") != nullptr;
    bool use_mipmaps_for_filtering = s_merc_mips || render_state->version != GameVersion::Jak2;
#else
    bool use_mipmaps_for_filtering = true;
#endif
    if (draw.texture != last_tex) {
      if (draw.texture < (int)lev->textures.size() && draw.texture >= 0) {
#if defined(__SWITCH__)
        merc_bind_probe(lev, draw.texture);
#endif
        glBindTexture(GL_TEXTURE_2D, lev->textures.at(draw.texture));
      } else if ((draw.texture & 0xffffff00) == 0xefffff00) {
        // Switch dkgl (AI-assisted): the dkgl render state has no EyeRenderer; the native
        // deko3d eye pass renders the eyes and dkgl hands out their texture ids instead.
        if (!render_state->eye_renderer) {
          const u32 eye_tex =
              g_merc2_eye_tex_lookup ? g_merc2_eye_tex_lookup(draw.texture & 0xff) : 0;
          if (eye_tex) {
            glBindTexture(GL_TEXTURE_2D, eye_tex);
          }
        } else if (render_state->version == GameVersion::Jak3 ||
            render_state->version == GameVersion::JakX) {
          auto maybe_eye =
              render_state->eye_renderer->lookup_eye_texture_hash(draw.hash, (draw.texture & 1));
          if (maybe_eye) {
            glBindTexture(GL_TEXTURE_2D, *maybe_eye);
          }
        } else {
          auto maybe_eye = render_state->eye_renderer->lookup_eye_texture(draw.texture & 0xff);
          if (maybe_eye) {
            glBindTexture(GL_TEXTURE_2D, *maybe_eye);
          }
        }

        use_mipmaps_for_filtering = false;
      } else if (draw.texture < 0) {
        int slot = -(draw.texture + 1);
        glBindTexture(GL_TEXTURE_2D, m_anim_slot_array->at(slot));
      } else {
        fmt::print("Invalid draw.texture is {}, would have crashed.\n", draw.texture);
      }
      last_tex = draw.texture;
    }

    if ((int)draw.light_idx != last_light && !set_fade) {
      const auto& l0_dir = m_lights_buffer[draw.light_idx].direction0;
      const auto& l1_dir = m_lights_buffer[draw.light_idx].direction1;
      float fade = 1.f;
      float fade_enable = 0.f;
      if (m_lights_buffer[draw.light_idx].w1) {
        fade = m_lights_buffer[draw.light_idx].w2 / 128.f;
        fade_enable = 1.f;
      }
      math::Vector4f l0_dir_f(l0_dir.x(), l0_dir.y(), l0_dir.z(), fade);
      set_uniform(uniforms.light_direction[0], l0_dir_f);
      math::Vector4f l1_dir_f(l1_dir.x(), l1_dir.y(), l1_dir.z(), fade_enable);
      set_uniform(uniforms.light_direction[1], l1_dir_f);
      set_uniform(uniforms.light_direction[2], m_lights_buffer[draw.light_idx].direction2);
      set_uniform(uniforms.light_color[0], m_lights_buffer[draw.light_idx].color0);
      set_uniform(uniforms.light_color[1], m_lights_buffer[draw.light_idx].color1);
      set_uniform(uniforms.light_color[2], m_lights_buffer[draw.light_idx].color2);
      set_uniform(uniforms.light_ambient, m_lights_buffer[draw.light_idx].ambient);
#if defined(__SWITCH__)
      // J18 A/B (AI-assisted): neutral lights on all merc. Cars coloured with this flag ->
      // the game's light values are the bug; still black -> texture/GPU side.
      static const bool s_light_fix =
          std::fopen("sdmc:/switch/jak3/gk_dkgl_merclightfix.txt", "r") != nullptr ||
          std::fopen("sdmc:/switch/jak2/gk_dkgl_merclightfix.txt", "r") != nullptr;
      if (s_light_fix) {
        set_uniform(uniforms.light_color[0], math::Vector4f(1.f, 1.f, 1.f, 1.f));
        set_uniform(uniforms.light_color[1], math::Vector4f(0.f, 0.f, 0.f, 1.f));
        set_uniform(uniforms.light_color[2], math::Vector4f(0.f, 0.f, 0.f, 1.f));
        set_uniform(uniforms.light_ambient, math::Vector4f(0.5f, 0.5f, 0.5f, 1.f));
      }
#endif
      last_light = draw.light_idx;
    }

    glUniform1i(uniforms.decal, draw.mode.get_decal());
    glUniform1i(uniforms.gfx_hack_no_tex, (draw.flags & NO_TEXTURE) != 0);

    if (set_fade) {
      math::Vector4f fade =
          math::Vector4f(draw.fade[0], draw.fade[1], draw.fade[2], draw.fade[3]) / 255.f;
      set_uniform(uniforms.fade, fade);
      ASSERT(draw.mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_DST_DST);
    }

    if (m_lights_buffer[draw.light_idx].w1 && !set_fade) {
#if defined(__SWITCH__)
      // J18 fade probe (AI-assisted): every captured [mercprobe] line is spawn-time w1=0, so
      // the two-pass fade path has NEVER been observed on hardware. Log when a model actually
      // enters it (event + fade-value change detection, keyed on the model hash) so the next
      // SD trip shows whether the black car is mid-fade (w1!=0) or in a plain dead-state draw.
      {
        static const bool s_fade_log =
            std::fopen("sdmc:/switch/jak3/gk_dkgl_merclog.txt", "r") != nullptr ||
            std::fopen("sdmc:/switch/jak2/gk_dkgl_merclog.txt", "r") != nullptr;
        static std::unordered_map<u64, float> s_fade_seen;
        static u32 s_fade_lines = 0;
        if (s_fade_log && s_fade_lines < 1500) {
          const float fade = m_lights_buffer[draw.light_idx].w2 / 128.f;
          auto it = s_fade_seen.find(draw.hash);
          if (it == s_fade_seen.end() || it->second - fade > 0.05f ||
              fade - it->second > 0.05f) {
            s_fade_seen[draw.hash] = fade;
            s_fade_lines++;
            switch_run_logf("[mercfade] hash=%llx lev=%s w1=%u w2=%u fade=%.3f",
                            (unsigned long long)draw.hash,
                            lev->level ? lev->level->level_name.c_str() : "?",
                            (unsigned)m_lights_buffer[draw.light_idx].w1,
                            (unsigned)m_lights_buffer[draw.light_idx].w2, (double)fade);
          }
        }
      }
#endif
      DrawMode mode = draw.mode;
      mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
      mode.set_ab(true);
      setup_opengl_from_draw_mode(mode, GL_TEXTURE0, use_mipmaps_for_filtering);

      prof.add_draw_call(2);
      prof.add_tri(draw.num_triangles * 2);
      glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_bones_buffer,
                        sizeof(math::Vector4f) * draw.first_bone, 128 * sizeof(ShaderMercMat));
      // draw rgb
      const auto& l1_dir = m_lights_buffer[draw.light_idx].direction1;
      math::Vector4f l1_dir_f(l1_dir.x(), l1_dir.y(), l1_dir.z(), 1);
      set_uniform(uniforms.light_direction[1], l1_dir_f);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
      gfx::count_draw(draw.index_count);
      glDrawElements(draw.no_strip ? GL_TRIANGLES : GL_TRIANGLE_STRIP, draw.index_count,
                     GL_UNSIGNED_INT, (void*)(sizeof(u32) * draw.first_index));
      // draw a
      setup_opengl_from_draw_mode(draw.mode, GL_TEXTURE0, use_mipmaps_for_filtering);
      math::Vector4f l1_dir_f_off(l1_dir.x(), l1_dir.y(), l1_dir.z(), -1);
      set_uniform(uniforms.light_direction[1], l1_dir_f_off);
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
      gfx::count_draw(draw.index_count);
      glDrawElements(draw.no_strip ? GL_TRIANGLES : GL_TRIANGLE_STRIP, draw.index_count,
                     GL_UNSIGNED_INT, (void*)(sizeof(u32) * draw.first_index));
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    } else {
      setup_opengl_from_draw_mode(draw.mode, GL_TEXTURE0, use_mipmaps_for_filtering);
      prof.add_draw_call();
      prof.add_tri(draw.num_triangles);
      glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_bones_buffer,
                        sizeof(math::Vector4f) * draw.first_bone, 128 * sizeof(ShaderMercMat));
      gfx::count_draw(draw.index_count);
      glDrawElements(draw.no_strip ? GL_TRIANGLES : GL_TRIANGLE_STRIP, draw.index_count,
                     GL_UNSIGNED_INT, (void*)(sizeof(u32) * draw.first_index));
    }
  }

  if (!normal_vtx_buffer_bound) {
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lev->merc_indices);
    glBindBuffer(GL_ARRAY_BUFFER, lev->merc_vertices);
  }
}
