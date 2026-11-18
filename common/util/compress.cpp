#include "compress.h"

#include <cstdio>
#include <cstring>

#include "common/util/Assert.h"

#include "common/log/log.h"
#include "fmt/format.h"
#include "third-party/zstd/lib/zstd.h"

namespace compression {

/*!
 * Compress data with zstd.  There is an 8-byte header containing the decompressed data's size.
 */
std::vector<u8> compress_zstd(const void* data, size_t size) {
  auto max_compressed = ZSTD_compressBound(size);
  std::vector<u8> result(sizeof(size_t) + max_compressed);
  memcpy(result.data(), &size, sizeof(size_t));
  auto compressed_size =
      ZSTD_compress(result.data() + sizeof(size_t), max_compressed, data, size, 1);
  if (ZSTD_isError(compressed_size)) {
    ASSERT_MSG(false, fmt::format("ZSTD error: {}", ZSTD_getErrorName(compressed_size)));
  }
  result.resize(sizeof(size_t) + compressed_size);
  return result;
}

/*!
 * Decompress data with zstd.  The first 8-bytes of the data should be a header containing the
 * decompressed data's size.
 */
std::vector<u8> decompress_zstd(const void* data, size_t size) {
  ASSERT(size >= sizeof(size_t));
  size_t decompressed_size;
  memcpy(&decompressed_size, data, sizeof(size_t));
  size_t compressed_size = size - sizeof(size_t);

  // FIX 120 (AI-assisted): tolerate trailing bytes after a complete frame.
  // The f119 hardware log died with "ZSTD error: Src size is incorrect" every
  // time the player took the ship to the Metal Head nest, on every build,
  // even though all fsdev users were locked and the card's bytes were
  // md5-identical to the extraction. The cause was the file itself:
  // out/jak2/fr3/nest.fr3 carried a complete, valid frame followed by 2282
  // stale bytes (the tail of an older, longer copy left by a non-truncating
  // replace). One-shot ZSTD_decompress requires the frame to consume the whole
  // source, so the game asserted. Trim the source to the frame so a padded or
  // partially-replaced file logs a warning and loads instead of killing the
  // process. If there is no complete frame at all we keep the original size
  // and the error/assert path below fires exactly as before.
  size_t frame_size =
      ZSTD_findFrameCompressedSize((const u8*)data + sizeof(size_t), compressed_size);
  if (!ZSTD_isError(frame_size)) {
    if (frame_size != compressed_size) {
      lg::warn(
          "decompress_zstd: ignoring {} trailing bytes after the zstd frame "
          "(file was padded or partially replaced)",
          compressed_size - frame_size);
    }
    compressed_size = frame_size;
  }

  std::vector<u8> result(decompressed_size);
  auto decomp_size = ZSTD_decompress(result.data(), decompressed_size,
                                     (const u8*)data + sizeof(size_t), compressed_size);
  if (ZSTD_isError(decomp_size)) {
    ASSERT_MSG(false, fmt::format("ZSTD error: {}", ZSTD_getErrorName(decomp_size)));
  }

  ASSERT(decomp_size == decompressed_size);
  return result;
}

std::vector<u8> compress_zstd_no_header(const void* data, size_t size) {
  size_t max_compressed = ZSTD_compressBound(size);
  std::vector<u8> result(max_compressed);

  size_t compressed_size = ZSTD_compress(result.data(), max_compressed, data, size, 1);
  if (ZSTD_isError(compressed_size)) {
    ASSERT_MSG(false, fmt::format("ZSTD error: {}", ZSTD_getErrorName(compressed_size)));
  }

  result.resize(compressed_size);
  return result;
}
}  // namespace compression
