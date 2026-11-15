// T79 (AI-assisted): host replay test for the deko3d Sprite3 CPU half (SpriteDk.cpp).
// Uses the same jak1 frame dump as the tfrag/tie/merc replay tests. Texture lookups are faked
// (id = tbp + 1), so this checks the DMA walk, quad layout, batching and ring layout.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/deko3d_renderer/SpriteDk.h"

#include "gtest/gtest.h"

bool dk_replay_load(std::vector<u8>* chain, u32* start_offset);  // test_dk_tfrag_replay.cpp

namespace {
std::vector<u8> g_sp_chain;
u32 g_sp_start = 0;
uint32_t g_uploads = 0;

void count_upload(uint32_t, int64_t) {
  g_uploads++;
}
uint32_t fake_lookup(uint32_t tbp) {
  return tbp + 1;
}

bool load_and_capture() {
  if (!dk_replay_load(&g_sp_chain, &g_sp_start)) {
    return false;
  }
  g_uploads = 0;
  dksprite::capture_chain(g_sp_chain.data(), g_sp_start, count_upload, fake_lookup);
  return true;
}
}  // namespace

TEST(SpriteReplay, CapturesSprites) {
  if (!load_and_capture()) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  const auto& st = dksprite::capture_stats();
  EXPECT_FALSE(st.bad) << "sprite DMA did not match the GL layout";
  EXPECT_GT(st.uploads, 0u) << "no texture-upload packets in the tex buckets";
  EXPECT_EQ(st.uploads, g_uploads);
  ASSERT_TRUE(st.ran) << "sprite renderer did not run in the dump frame";
  EXPECT_EQ(st.capped, 0u);

  const auto& v = dksprite::captured_vertices();
  const auto& idx = dksprite::captured_indices();
  const auto& batches = dksprite::captured_batches();
  EXPECT_EQ(v.size() % 4, 0u);
  EXPECT_EQ(v.size() / 4, (size_t)st.count_2d + st.count_hud);

  uint32_t quads = 0;
  for (const auto& b : batches) {
    EXPECT_GT(b.idx_count, 0u);
    EXPECT_EQ(b.idx_count % 5, 0u);
    EXPECT_LE((size_t)b.idx_first + b.idx_count, idx.size());
    EXPECT_NE(b.tex_id, 0u);
    quads += b.idx_count / 5;
    for (uint32_t i = 0; i < b.idx_count; i += 5) {
      const uint32_t v0 = idx[b.idx_first + i];
      ASSERT_LT((size_t)v0 + 3, v.size());
      EXPECT_EQ(idx[b.idx_first + i + 4], UINT32_MAX);
      EXPECT_EQ(v[v0].info[2], 0);
      EXPECT_EQ(v[v0 + 1].info[2], 1);
      EXPECT_EQ(v[v0 + 2].info[2], 3);
      EXPECT_EQ(v[v0 + 3].info[2], 2);
      const uint16_t mode = v[v0].info[3];
      EXPECT_TRUE(b.hud ? mode == 2 : (mode == 1 || mode == 3));
      for (int c = 0; c < 4; c++) {
        EXPECT_GE(v[v0].rgba[c], 0.f);
        EXPECT_LE(v[v0].rgba[c], 1.f);
      }
    }
  }
  EXPECT_EQ(quads * 4, v.size());

  const auto& p = dksprite::captured_params();
  if (!v.empty()) {
    EXPECT_NE(p.consts0[0], 0.f) << "pfog0";
    EXPECT_NE(p.consts1[1], 0.f) << "deg_to_rad";
  }
  std::printf("dk-sprite: uploads=%u direct=%u distort=%u 2d/3d=%u hud=%u batches=%zu\n",
              st.uploads, st.direct, st.distort, st.count_2d, st.count_hud, batches.size());
}

TEST(SpriteReplay, BuildRing) {
  if (!load_and_capture()) {
    GTEST_SKIP() << "no replay data (see docs/deko3d/REPLAY_TEST.md)";
  }
  dksprite::build_frame();
  const auto& f = dksprite::frame_draws();
  const auto& batches = dksprite::captured_batches();
  EXPECT_EQ(f.dropped, 0u);
  EXPECT_EQ(f.no_tex, 0u);
  EXPECT_GE(f.draws.size(), batches.size());
  if (!batches.empty()) {
    EXPECT_NE(f.vert_gpu, 0u);
    EXPECT_EQ(f.vert_bytes, dksprite::captured_vertices().size() * sizeof(dksprite::Vertex));
    EXPECT_EQ(f.params_gpu & 0xff, 0u);
  }
  for (const auto& c : f.draws) {
    EXPECT_NE(c.tex_id, 0u);
    EXPECT_GT(c.idx_count, 0u);
    EXPECT_EQ(c.idx_gpu % 4, 0u);
    EXPECT_NE(c.frag_gpu, 0u);
    EXPECT_EQ(c.frag_gpu & 0xff, 0u);
  }
  std::printf("dk-sprite: sprites=%u draws=%zu\n", f.sprites, f.draws.size());
}

// ---- bug M / green squares: the sprite bucket must be read at ITS bucket id ----
// jak2 has TextureUploadHandler buckets ABOVE the sprite bucket (TEX_ALL_WARP 316,
// DEBUG_NO_ZBUF1 318, TEX_ALL_MAP 319, SUBTITLE 322). GL's Sprite3 flushes at bucket 313,
// so its texture lookups see VRAM as the uploads BELOW it left it. Reading the sprites only
// after replaying every upload resolved effect pages against end-of-frame VRAM, and pages
// reused by a later upload drew that texture instead (green squares: DKGL_TODO bug M).

namespace {
std::vector<std::string> g_sched;

void sched_upload(uint32_t page, int64_t) {
  g_sched.push_back("U:" + std::to_string(page));
}
uint32_t sched_lookup(uint32_t tbp) {
  g_sched.push_back("L:" + std::to_string(tbp));
  return tbp + 1;
}
void sched_hook(int b) {
  g_sched.push_back("H:" + std::to_string(b));
}
std::string sched_join() {
  std::string s;
  for (const auto& e : g_sched) {
    if (!s.empty()) {
      s += ' ';
    }
    s += e;
  }
  return s;
}

// DMA tag helpers (common/dma/dma.h): NEXT (kind 2) jumps to addr, CNT (kind 1) carries
// qwc*16 bytes of data right after the tag, PC_PORT = VIF kind 8 (vif0 = 8 << 24).
void put_next(std::vector<u8>& buf, u32 at, u32 next_addr) {
  const u64 tag = ((u64)DmaTag::Kind::NEXT << 28) | ((u64)next_addr << 32);
  memcpy(buf.data() + at, &tag, 8);
}
void put_cnt_upload(std::vector<u8>& buf, u32 at, uint64_t page) {
  const u64 tag = ((u64)DmaTag::Kind::CNT << 28) | 1;  // qwc 1 -> 16 B payload
  memcpy(buf.data() + at, &tag, 8);
  const u32 vif0 = 0x08000000, vif1 = 3;  // PC_PORT / upload marker
  memcpy(buf.data() + at + 8, &vif0, 4);
  memcpy(buf.data() + at + 12, &vif1, 4);
  const int64_t mode = 0;
  memcpy(buf.data() + at + 16, &page, 8);
  memcpy(buf.data() + at + 24, &mode, 8);
}
// One TextureUploadHandler bucket: slot jumps to a chain of one upload packet, which
// ends at the next bucket-table slot (walk_tex_bucket's `next`).
void put_upload_bucket(std::vector<u8>& buf, u32 slot, u32 chain_at, uint64_t page) {
  put_next(buf, slot, chain_at);
  put_cnt_upload(buf, chain_at, page);
  put_next(buf, chain_at + 32, slot + 16);
}
}  // namespace

TEST(SpriteReplay, SpriteWalkRunsAtItsBucketJak2) {
  // synthetic jak2-shaped chain (no CALL head): upload buckets 312 (below) and 316+319
  // (above the sprite bucket 313). Bucket 313 "didn't run": its slot jumps straight to
  // the next slot, like a frame without particles.
  std::vector<u8> chain(0x2400, 0);
  const u32 s = 0x2000;
  put_upload_bucket(chain, 16 * 312, s + 0x00, 100);
  put_upload_bucket(chain, 16 * 316, s + 0x100, 200);
  put_upload_bucket(chain, 16 * 319, s + 0x200, 300);
  put_next(chain, 16 * 313, 16 * 314);
  const int tex[] = {312, 316, 319};
  g_sched.clear();
  dksprite::capture_chain(chain.data(), 0, sched_upload, sched_lookup, sched_hook, tex, 3,
                          313, true, false, nullptr, nullptr, nullptr);
  // bucket 312's upload replays, then the sprite bucket runs at 313 -- BEFORE the
  // buckets above it. The old order was "H:312 U:100 H:316 U:200 H:319 U:300 H:313".
  EXPECT_EQ(sched_join(), "H:312 U:100 H:313 H:316 U:200 H:319 U:300");
  EXPECT_FALSE(dksprite::capture_stats().ran);
  EXPECT_FALSE(dksprite::capture_stats().bad);
  EXPECT_EQ(dksprite::capture_stats().uploads, 3u);
}

TEST(SpriteReplay, SpriteWalkRunsAfterTheLoopJak1) {
  // jak1-shaped chain (CALL head, all upload buckets below the sprite bucket 66):
  // unchanged behaviour -- the sprite bucket is still read after the whole loop.
  std::vector<u8> chain(0x2400, 0);
  const u64 call = (u64)DmaTag::Kind::CALL << 28;
  memcpy(chain.data(), &call, 8);  // head: CALL -> bucket table follows at +16
  const u32 s = 0x2000;
  put_upload_bucket(chain, 16 + 16 * 5, s + 0x00, 500);
  put_upload_bucket(chain, 16 + 16 * 12, s + 0x100, 1200);
  // sprite bucket 66 "didn't run" (jak1): the first advance lands on a CALL tag.
  put_next(chain, 16 + 16 * 66, 16 + 16 * 67);
  const u64 call2 = (u64)DmaTag::Kind::CALL << 28;
  memcpy(chain.data() + 16 + 16 * 67, &call2, 8);
  const int tex[] = {5, 12};
  g_sched.clear();
  dksprite::capture_chain(chain.data(), 0, sched_upload, sched_lookup, sched_hook, tex, 2,
                          66, true, true, nullptr, nullptr, nullptr);
  EXPECT_EQ(sched_join(), "H:5 U:500 H:12 U:1200 H:66");
  EXPECT_FALSE(dksprite::capture_stats().ran);
  EXPECT_FALSE(dksprite::capture_stats().bad);
  EXPECT_EQ(dksprite::capture_stats().uploads, 2u);
}
