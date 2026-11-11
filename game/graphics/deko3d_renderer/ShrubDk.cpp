#include "game/graphics/deko3d_renderer/ShrubDk.h"

// T70/T71 (AI-assisted): deko3d shrub CPU half, cloned from TFragmentDk.cpp. Port of GL
// Shrub::render + render_tree (no culling, strips only; jak2 proto-vis mask like GL).
// DK_HOST_REPLAY: test/test_dk_shrub_replay.cpp compiles this file on the Mac.
#if defined(__SWITCH__) || defined(DK_HOST_REPLAY)
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#if defined(__SWITCH__)
#include <unistd.h>
#endif
#include <unordered_map>
#include <vector>

#include "common/dma/dma_chain_read.h"
#include "common/versions/versions.h"  // §7.2 phase 1: set_game pick

#include "game/graphics/deko3d_renderer/dk_buffer_pool.h"
#include "game/graphics/deko3d_renderer/dk_proto_vis.h"
#include "game/graphics/deko3d_renderer/dk_texture_gpu.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/loader/Loader.h"
#include "game/graphics/pipelines/dk_device.h"
#include "game/switch/run_log.h"

#if defined(DK_HOST_REPLAY)
const dkpool::PoolBuffer* dk_loader_buffer_find(GLuint id);  // test-defined
const LevelData* dktf_host_level(const char* name);          // test-defined
#define DKSH_LEVEL(loader, name) dktf_host_level(name)
#define DKSH_NEED_LOADER 0
#else
#define DKSH_LEVEL(loader, name) (loader)->get_tfrag3_level(name)
#define DKSH_NEED_LOADER 1
#endif

// Defined in background_common.cpp, not declared in its header (same as TFragmentDk.cpp).
std::array<math::Vector4f, 4> make_new_cam_mat(const math::Vector4f cam_T_w[4],
                                               const math::Vector4f persp[4],
                                               float fog_constant,
                                               float hvdf_z);

namespace {

constexpr uint32_t kMaxDraws = 500;  // village1 = 47; small: shares the frame cmd slice with tfrag
constexpr int kTodColors = 8192;      // Shrub TIME_OF_DAY_COLOR_COUNT (same as tfrag)
constexpr int kIdxSlots = 3;          // index ring slots, same reason as TFragmentDk.cpp

struct Captured {
  TfragPcPortData pc;
  std::vector<u8> proto_mask;  // jak2: hidden proto names (dk_proto_vis.h)
  bool valid = false;
};
// §7.2 phase 1: per-game bucket pick (defaults to jak1, like dkgl_frame.cpp).
const int* s_buckets = dkshrub::kJak1Buckets;
int s_pass_count = dkshrub::kNumPasses;
bool s_jak2 = false;
std::unordered_map<const tfrag3::ShrubTree*, dkprotovis::Cache> s_pv;
Captured s_cap[dkshrub::kMaxPasses];
dktf::FrameDraws s_frame;

// Same reads as GL Shrub::render for jak1. true = the bucket had shrub data.
bool walk_bucket(const void* data, uint32_t slot, uint32_t next, Captured* out) {
  DmaFollower d(data, slot);
  d.read_and_advance();  // the bucket's own NEXT (size 0)
  if (d.current_tag().kind == DmaTag::Kind::CALL) {
    return false;  // renderer didn't run this frame
  }
  if (d.current_tag_offset() == next) {
    return false;
  }
  auto pc = d.read_and_advance();
  if (pc.size_bytes != sizeof(TfragPcPortData)) {
    return false;
  }
  memcpy(&out->pc, pc.data, sizeof(TfragPcPortData));
  out->pc.level_name[11] = '\0';
  out->proto_mask.clear();
  if (s_jak2 && d.current_tag_offset() != next) {
    // GL Shrub::render: jak2 sends the proto-vis mask right after the pc-port data.
    auto mask = d.read_and_advance();
    dkprotovis::copy_mask(mask.data, mask.size_bytes, &out->proto_mask);
  }
  return true;
}

// ---- index ring (copy of TFragmentDk.cpp's, own slots) ----
struct IdxSlot {
  dkpool::PoolBuffer pb;
  uint32_t used = 0;
  uint32_t want = 0;
};
// AI-assisted: shrubs have no visibility structure in the fr3 ("todo some visibility structure"),
// so the GL path draws every shrub of the level every frame. Here each draw's strips are sorted
// spatially (Morton order of the strip centres) at first use and grouped into chunks of ~kChunkIdx
// indices with a bounding sphere; per frame only chunks whose sphere is inside the frustum are
// copied into the index ring. Flag sdmc:/gk_noshrubcull.txt draws every chunk (old behaviour).
struct ShrubChunk {
  uint32_t begin = 0;  // into TreeIdxCache::sorted
  uint32_t count = 0;
  math::Vector4f sphere;
};
struct TreeIdxCache {
  uint64_t load_id = ~0ull;
  std::vector<u32> sorted;  // strips regrouped by chunk, each strip ends with a restart index
  std::vector<std::vector<ShrubChunk>> chunks;  // per draw
};
constexpr uint32_t kChunkIdx = 600;
constexpr u32 kRestart = 0xFFFFFFFFu;

bool shrub_cull_enabled() {
#if defined(__SWITCH__)
  static const bool s_on = access("sdmc:/gk_noshrubcull.txt", F_OK) != 0;
  return s_on;
#else
  return true;
#endif
}

uint32_t morton3(uint32_t x, uint32_t y, uint32_t z) {
  auto spread = [](uint32_t v) {
    v &= 0x3ff;
    v = (v | (v << 16)) & 0x030000ff;
    v = (v | (v << 8)) & 0x0300f00f;
    v = (v | (v << 4)) & 0x030c30c3;
    v = (v | (v << 2)) & 0x09249249;
    return v;
  };
  return spread(x) | (spread(y) << 1) | (spread(z) << 2);
}

void build_shrub_chunks(const tfrag3::ShrubTree& tree, TreeIdxCache* out) {
  struct Strip {
    uint32_t start, len;
    float mn[3], mx[3];
    uint32_t key;
  };
  const auto& verts = tree.unpacked.vertices;
  out->sorted.clear();
  out->chunks.assign(tree.static_draws.size(), {});
  out->sorted.reserve(tree.indices.size() + tree.indices.size() / 8);
  std::vector<Strip> strips;
  for (size_t di = 0; di < tree.static_draws.size(); di++) {
    const auto& d = tree.static_draws[di];
    const u32* in = tree.indices.data() + d.first_index_index;
    const uint32_t n = d.num_indices;
    strips.clear();
    bool bad = false;
    float gmn[3] = {1e30f, 1e30f, 1e30f}, gmx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < n && !bad;) {
      if (in[i] == kRestart) {
        i++;
        continue;
      }
      Strip st{i, 0, {1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, 0};
      while (i < n && in[i] != kRestart) {
        if (in[i] >= verts.size()) {
          bad = true;
          break;
        }
        const auto& v = verts[in[i]];
        const float p[3] = {v.x, v.y, v.z};
        for (int c = 0; c < 3; c++) {
          st.mn[c] = std::min(st.mn[c], p[c]);
          st.mx[c] = std::max(st.mx[c], p[c]);
        }
        i++;
      }
      st.len = i - st.start;
      for (int c = 0; c < 3; c++) {
        gmn[c] = std::min(gmn[c], 0.5f * (st.mn[c] + st.mx[c]));
        gmx[c] = std::max(gmx[c], 0.5f * (st.mn[c] + st.mx[c]));
      }
      strips.push_back(st);
    }
    auto& dc = out->chunks[di];
    if (bad || strips.empty()) {
      // unknown geometry: copy the draw as it is, one chunk that is always visible
      ShrubChunk c;
      c.begin = (uint32_t)out->sorted.size();
      out->sorted.insert(out->sorted.end(), in, in + n);
      c.count = n;
      c.sphere = math::Vector4f(0.f, 0.f, 0.f, 1e30f);
      dc.push_back(c);
      continue;
    }
    for (auto& st : strips) {
      uint32_t q[3];
      for (int c = 0; c < 3; c++) {
        const float span = std::max(gmx[c] - gmn[c], 1.f);
        const float t = (0.5f * (st.mn[c] + st.mx[c]) - gmn[c]) / span;
        q[c] = (uint32_t)std::min(1023.f, std::max(0.f, t * 1023.f));
      }
      st.key = morton3(q[0], q[1], q[2]);
    }
    std::sort(strips.begin(), strips.end(),
              [](const Strip& a, const Strip& b) { return a.key < b.key; });
    size_t k = 0;
    while (k < strips.size()) {
      ShrubChunk c;
      c.begin = (uint32_t)out->sorted.size();
      float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
      while (k < strips.size() && (c.count == 0 || c.count < kChunkIdx)) {
        const Strip& st = strips[k++];
        out->sorted.insert(out->sorted.end(), in + st.start, in + st.start + st.len);
        out->sorted.push_back(kRestart);
        c.count += st.len + 1;
        for (int a = 0; a < 3; a++) {
          mn[a] = std::min(mn[a], st.mn[a]);
          mx[a] = std::max(mx[a], st.mx[a]);
        }
      }
      const float cx = 0.5f * (mn[0] + mx[0]), cy = 0.5f * (mn[1] + mx[1]),
                  cz = 0.5f * (mn[2] + mx[2]);
      const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
      // small padding: the vertex shader may displace nothing, but be safe against rounding
      c.sphere = math::Vector4f(cx, cy, cz, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz) * 1.02f + 16.f);
      dc.push_back(c);
    }
  }
}
std::unordered_map<const void*, TreeIdxCache> s_tree_idx;
IdxSlot s_idx[kIdxSlots];
uint32_t s_build = 0;

IdxSlot* ring_begin() {
  IdxSlot* s = &s_idx[s_build % kIdxSlots];
  s->used = 0;
  dkpool::BufferPool* pool = dkdev::buffer_pool();
  const uint32_t need = std::max<uint32_t>(1u << 20, s->want + s->want / 2);
  if (pool && (!s->pb.ok() || s->pb.capacity < s->want)) {
    if (s->pb.ok()) {
      pool->release(s->pb.id);
    }
    s->pb = pool->acquire(need);
    switch_run_logf("[dk] shrub index ring slot %u: %u KB %s", s_build % kIdxSlots, need >> 10,
                    s->pb.ok() ? "ok" : "FAILED");
  }
  s->want = 0;
  return s;
}

// ---- time-of-day palettes (copy of TFragmentDk.cpp's, keyed by ShrubTree) ----
struct TodEntry {
  uint32_t id = 0;
  uint64_t load_id = 0;
  math::Vector<s32, 4> itimes[4];
  bool valid = false;
  uint32_t last_build = 0;
};
std::unordered_map<const tfrag3::ShrubTree*, TodEntry> s_tod;
std::vector<uint32_t> s_tod_free;
std::vector<math::Vector<u8, 4>> s_tod_colors;

uint32_t tod_for(const LevelData* ld,
                 const tfrag3::ShrubTree& tree,
                 const math::Vector<s32, 4>* it) {
  const auto& colors = tree.time_of_day_colors;
  if (colors.color_count == 0 || colors.color_count > (u32)kTodColors) {
    return 0;
  }
  TodEntry& e = s_tod[&tree];
  e.last_build = s_build;
  if (e.id == 0) {
    if (!s_tod_free.empty()) {
      e.id = s_tod_free.back();
      s_tod_free.pop_back();
    } else {
      static uint8_t s_zeros[kTodColors * 4] = {};
      e.id = dktexgpu::upload_rgba8(kTodColors, 1, s_zeros);
      if (e.id == 0) {
        s_tod.erase(&tree);
        return 0;
      }
    }
    e.valid = false;
  }
  if (e.load_id != ld->load_id) {
    e.load_id = ld->load_id;
    e.valid = false;
  }
  if (!e.valid || tod_itimes_changed(e.itimes, it)) {
    s_tod_colors.resize(colors.color_count);
    interp_time_of_day(it, colors, s_tod_colors.data());
    if (dktexgpu::update_rgba8_deferred(e.id, colors.color_count, 1, s_tod_colors.data())) {
      tod_itimes_store(e.itimes, it);
      e.valid = true;
    }
  }
  return e.id;
}

void tod_evict_unused() {
  for (auto it = s_tod.begin(); it != s_tod.end();) {
    if (s_build - it->second.last_build > 300) {
      s_tod_free.push_back(it->second.id);
      it = s_tod.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

namespace dkshrub {

void capture_chain(const void* data, uint32_t start_offset) {
  for (auto& c : s_cap) {
    c.valid = false;
  }
  if (data == nullptr) {
    return;
  }
  DmaFollower d(data, start_offset);
  uint32_t buckets_base;
  if (s_jak2) {
    // §7.2 phase 1: jak2 chains have no CALL head -- the bucket table starts at the
    // offset itself (GL dispatch_buckets_jak2).
    buckets_base = d.current_tag_offset();
  } else {
    if (d.current_tag().kind != DmaTag::Kind::CALL) {
      return;  // not a game frame chain
    }
    buckets_base = d.current_tag_offset() + 16;
  }
  for (int i = 0; i < s_pass_count; i++) {
    const uint32_t slot = buckets_base + 16 * s_buckets[i];
    s_cap[i].valid = walk_bucket(data, slot, slot + 16, &s_cap[i]);
  }
}

const TfragPcPortData* captured_pc(int pass) {
  if (pass < 0 || pass >= s_pass_count || !s_cap[pass].valid) {
    return nullptr;
  }
  return &s_cap[pass].pc;
}

const dktf::FrameDraws& frame_draws() {
  return s_frame;
}

void build_frame(Loader* loader) {
  // same as TieDk: record_list fogs with f.fog_rgba, taken from tfrag (built first) (AI-assisted)
  memcpy(s_frame.fog_rgba, dktf::frame_draws().fog_rgba, 4);
  s_frame.cams.clear();
  s_frame.draws.clear();
  s_frame.tris = 0;
  s_frame.dropped = 0;
  s_frame.alpha_draws = 0;
  bool any = false;
  for (const auto& c : s_cap) {
    any = any || c.valid;
  }
  if ((DKSH_NEED_LOADER && loader == nullptr) || !any) {
    return;
  }
  s_build++;
  IdxSlot* ring = ring_begin();
  static std::vector<std::pair<int, int>> groups;
  static std::vector<u32> idx;

  for (int b = 0; b < s_pass_count; b++) {
    if (!s_cap[b].valid) {
      continue;
    }
    const auto& cam = s_cap[b].pc.camera;
    const LevelData* ld = DKSH_LEVEL(loader, s_cap[b].pc.level_name);
    if (ld == nullptr || !ld->level) {
      continue;
    }
    const uint8_t cam_idx = (uint8_t)s_frame.cams.size();
    dktf::CamParams cp;
    memcpy(cp.camera, cam.camera, sizeof(cp.camera));
    const auto pcm = make_new_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
    memcpy(cp.pc_camera, pcm.data(), sizeof(cp.pc_camera));
    cp.hvdf_off = cam.hvdf_off;
    cp.trans = cam.trans;
    cp.fog = cam.fog;
    s_frame.cams.push_back(cp);

    const auto& trees = ld->level->shrub_trees;
    for (size_t t = 0; t < trees.size() && t < ld->shrub_vertex_data.size(); t++) {
      const auto& tree = trees[t];
      const dkpool::PoolBuffer* vb = dk_loader_buffer_find(ld->shrub_vertex_data[t]);
      if (vb == nullptr || tree.indices.empty()) {
        continue;
      }
      const uint32_t tod = tod_for(ld, tree, cam.itimes);
      if (tod == 0) {
        continue;
      }
      TreeIdxCache& ic = s_tree_idx[&tree];
      if (ic.load_id != ld->load_id || ic.chunks.size() != tree.static_draws.size()) {
        const auto t0 = std::chrono::steady_clock::now();
        build_shrub_chunks(tree, &ic);
        ic.load_id = ld->load_id;
        size_t nch = 0;
        for (const auto& dc : ic.chunks) {
          nch += dc.size();
        }
        switch_run_logf("[dk] shrub chunks: %zu draws, %zu chunks, %zu idx, built in %.1f ms",
                        tree.static_draws.size(), nch, ic.sorted.size(),
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count());
      }
      const bool cull = s_jak2 && shrub_cull_enabled();  // jak1 keeps drawing every chunk
      groups.resize(tree.static_draws.size());
      if (idx.size() < ic.sorted.size()) {
        idx.resize(ic.sorted.size());
      }
      u32 n = 0;
      for (size_t di = 0; di < tree.static_draws.size(); di++) {
        const uint32_t first = n;
        for (const ShrubChunk& c : ic.chunks[di]) {
          if (!cull || sphere_in_view_ref(c.sphere, cam.planes)) {
            memcpy(&idx[n], &ic.sorted[c.begin], c.count * sizeof(u32));
            n += c.count;
          }
        }
        groups[di] = {(int)first, (int)(n - first)};
      }
      const auto& gr = groups;
      const uint32_t bytes = n * 4;
      ring->want += bytes;
      if (!ring->pb.ok() || ring->used + bytes > ring->pb.capacity) {
        s_frame.dropped += (uint32_t)tree.static_draws.size();
        continue;  // ring_begin grows the slot next time this slot comes round
      }
      const uint32_t base = ring->used;
      memcpy(ring->pb.cpu + base, idx.data(), bytes);
      ring->used += (bytes + 255) & ~255u;
      const std::vector<u8>* pv = nullptr;
      if (s_jak2) {
        pv = &dkprotovis::update(&s_pv[&tree], ld->load_id, tree.proto_names,
                                 s_cap[b].proto_mask);
      }

      for (size_t i = 0; i < tree.static_draws.size(); i++) {
        if (gr[i].second == 0) {
          continue;
        }
        const u16 proto = tree.static_draws[i].proto_idx;
        if (pv && proto < pv->size() && !(*pv)[proto]) {
          continue;  // GL Shrub: proto hidden by the game this frame
        }
        if (s_frame.draws.size() >= kMaxDraws) {
          s_frame.dropped++;
          continue;
        }
        const auto& draw = tree.static_draws[i];
        dktf::DrawCmd c;
        c.vert_gpu = vb->gpu;
        c.vert_bytes = vb->capacity;
        c.idx_gpu = ring->pb.gpu + base + (uint64_t)gr[i].first * 4;
        c.idx_count = (uint32_t)gr[i].second;
        if (draw.tree_tex_id < ld->textures.size()) {
          c.tex_id = ld->textures[draw.tree_tex_id];
        }
        c.tod_id = tod;
        c.strips = true;  // GL Shrub always draws GL_TRIANGLE_STRIP
        c.cam = cam_idx;
        c.range = (uint8_t)b;  // §7.2 phase 1: record split tag
        dktf::fill_draw_state(draw.mode, &c);
        s_frame.draws.push_back(c);
        s_frame.tris += draw.num_triangles;
      }
    }
  }
  ring->want = std::max(ring->want, ring->used);
  tod_evict_unused();
}

// ---- §7.2 phase 1 (AI-assisted): per-game bucket tables ----

void set_game(GameVersion v) {
  s_jak2 = v == GameVersion::Jak2;
  s_buckets = s_jak2 ? kJak2Buckets : kJak1Buckets;
  s_pass_count = s_jak2 ? kJak2NumPasses : kNumPasses;
}

int pass_count() {
  return s_pass_count;
}

}  // namespace dkshrub
#endif  // __SWITCH__ || DK_HOST_REPLAY
