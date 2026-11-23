// merc_dump (AI-assisted): offline census of the merc models in an fr3 level file.
//
// Prints, per merc model, every draw with the things a renderer can get wrong per
// draw: the texture it binds (name, size, payload format, mip count, pool flag),
// the DrawMode, the eye id, the UV range and the mean vertex colour over the
// draw's index range, and the highest bone index it references.
//
// Written for the Switch deko3d port's "black merc props" investigation
// (JAK3_BRINGUP_TODO.md J18): it answers "which merc features does this model use
// that Jak/Daxter don't" straight from the data, with no console round trip.
//
// Usage:
//   merc_dump <file.fr3>                     list every model (one line each)
//   merc_dump <file.fr3> <substr> [substr..] full dump of the models whose name contains a substr
//   merc_dump <file.fr3> --tex               texture census of the level (format x size x mips)
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"
#include "common/util/compress.h"
#include "fmt/format.h"

namespace {

const char* fmt_name(u8 f) {
  switch (f) {
    case tfrag3::TEXTURE_FMT_RGBA:
      return "RGBA";
    case tfrag3::TEXTURE_FMT_BC1:
      return "BC1";
    case tfrag3::TEXTURE_FMT_BC3:
      return "BC3";
    default:
      return "FMT?";
  }
}

// count the mip levels the extractor stored for a BCn payload
u32 mip_count(const tfrag3::Texture& t) {
  return (u32)t.mip_offsets.size();
}

void describe_texture(const tfrag3::Level& lev, s32 tex_id, std::string* out) {
  if (tex_id < 0) {
    fmt::format_to(std::back_inserter(*out), "anim-slot{}", -(tex_id + 1));
    return;
  }
  if (tex_id >= (s32)lev.textures.size()) {
    fmt::format_to(std::back_inserter(*out), "OUT-OF-RANGE({})", tex_id);
    return;
  }
  const auto& t = lev.textures[tex_id];
  fmt::format_to(std::back_inserter(*out), "t{} {}/{} {}x{} {} m{} pool={}", tex_id,
                 t.debug_tpage_name, t.debug_name, t.w, t.h, fmt_name(t.format),
                 mip_count(t), (int)t.load_to_pool);
}

struct DrawStats {
  float smin = 1e30f, smax = -1e30f, tmin = 1e30f, tmax = -1e30f;
  float rgba[4] = {0, 0, 0, 0};
  u32 mats_max = 0;
  u32 n = 0;
};

DrawStats scan_draw(const tfrag3::MercModelGroup& g, const tfrag3::MercDraw& d) {
  DrawStats st;
  for (u32 i = d.first_index;
       i < d.first_index + d.index_count && i < g.indices.size() && st.n < 100000; i++, st.n++) {
    const u32 vi = g.indices[i];
    if (vi >= g.vertices.size()) {
      continue;
    }
    const auto& v = g.vertices[vi];
    st.smin = std::min(st.smin, v.st[0]);
    st.smax = std::max(st.smax, v.st[0]);
    st.tmin = std::min(st.tmin, v.st[1]);
    st.tmax = std::max(st.tmax, v.st[1]);
    for (int c = 0; c < 4; c++) {
      st.rgba[c] += v.rgba[c];
    }
    for (int m = 0; m < 3; m++) {
      st.mats_max = std::max(st.mats_max, (u32)v.mats[m]);
    }
  }
  return st;
}

void dump_draw(const std::string& tag,
               const tfrag3::Level& lev,
               const tfrag3::MercModelGroup& g,
               const tfrag3::MercDraw& d,
               bool envmap) {
  std::string tex;
  describe_texture(lev, d.tree_tex_id, &tex);
  const DrawStats st = scan_draw(g, d);
  const float n = (float)std::max(1u, st.n);
  fmt::print("    {:<9} {:<48} eye={:02x} idx=[{},{}) tris={}{}\n", tag, tex, d.eye_id,
             d.first_index, d.first_index + d.index_count, d.num_triangles,
             envmap ? " (envmap pass)" : "");
  fmt::print("              mode: {}\n", d.mode.to_string());
  if (st.n > 0) {
    fmt::print(
        "              uv=[{:.3f}..{:.3f}]x[{:.3f}..{:.3f}] rgba=({:.0f},{:.0f},{:.0f},{:.0f}) "
        "mats_max={}\n",
        st.smin, st.smax, st.tmin, st.tmax, st.rgba[0] / n, st.rgba[1] / n, st.rgba[2] / n,
        st.rgba[3] / n, st.mats_max);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fmt::print("usage: merc_dump <file.fr3> [model-substring ...] [--tex]\n");
    return 1;
  }
  const std::string path = argv[1];
  auto data = file_util::read_binary_file(path);
  auto decomp = compression::decompress_zstd(data.data(), data.size());
  Serializer ser(decomp.data(), decomp.size());
  tfrag3::Level level;
  level.serialize(ser);

  fmt::print("level={} textures={} merc_vertices={} merc_indices={} models={}\n",
             level.level_name, level.textures.size(), level.merc_data.vertices.size(),
             level.merc_data.indices.size(), level.merc_data.models.size());

  if (argc >= 3 && std::strcmp(argv[2], "--tex") == 0) {
    std::map<std::string, u32> census;
    for (const auto& t : level.textures) {
      std::string key = fmt::format("{} {}x{} m{} pool={}", fmt_name(t.format), t.w, t.h,
                                    mip_count(t), (int)t.load_to_pool);
      census[key]++;
    }
    for (const auto& kv : census) {
      fmt::print("  {:4} {}\n", kv.second, kv.first);
    }
    return 0;
  }

  std::vector<std::string> filters;
  for (int i = 2; i < argc; i++) {
    filters.emplace_back(argv[i]);
  }

  for (const auto& m : level.merc_data.models) {
    bool match = filters.empty();
    for (const auto& f : filters) {
      if (m.name.find(f) != std::string::npos) {
        match = true;
      }
    }
    if (!match) {
      continue;
    }
    if (filters.empty()) {
      u32 draws = 0, envmap = 0, mod = 0;
      std::map<std::string, u32> fmts;
      for (const auto& e : m.effects) {
        draws += (u32)e.all_draws.size();
        envmap += e.has_envmap ? 1u : 0u;
        mod += e.has_mod_draw ? 1u : 0u;
        for (const auto& d : e.all_draws) {
          if (d.tree_tex_id >= 0 && d.tree_tex_id < (s32)level.textures.size()) {
            fmts[fmt_name(level.textures[d.tree_tex_id].format)]++;
          }
        }
      }
      std::string fs;
      for (const auto& kv : fmts) {
        fs += fmt::format("{}:{} ", kv.first, kv.second);
      }
      fmt::print("  {:<40} effects={:2} draws={:3} envmap_fx={} mod_fx={} bones={} {}\n", m.name,
                 m.effects.size(), draws, envmap, mod, m.max_bones, fs);
      continue;
    }
    fmt::print("[model] {} effects={} max_draws={} max_bones={} xyz_scale={} st_magic={}\n",
               m.name, m.effects.size(), m.max_draws, m.max_bones, m.xyz_scale, m.st_magic);
    for (size_t ei = 0; ei < m.effects.size(); ei++) {
      const auto& e = m.effects[ei];
      fmt::print("  [effect {}] draws={} envmap={} mod_draw={}\n", ei, e.all_draws.size(),
                 (int)e.has_envmap, (int)e.has_mod_draw);
      if (e.has_envmap) {
        std::string tex;
        describe_texture(level, (s32)e.envmap_texture, &tex);
        fmt::print("    envmap texture: {} envmap mode: {}\n", tex, e.envmap_mode.to_string());
      }
      for (size_t di = 0; di < e.all_draws.size(); di++) {
        dump_draw(fmt::format("draw{}", di), level, level.merc_data, e.all_draws[di], false);
      }
    }
  }
  return 0;
}
