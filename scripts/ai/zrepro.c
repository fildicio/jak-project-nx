/* Reproduce the game's decompress_zstd behavior on an fr3 file.
 * Usage: zrepro <file.fr3>
 */
#include <zstd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int main(int argc, char** argv) {
  if (argc < 2) return 1;
  FILE* f = fopen(argv[1], "rb");
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  rewind(f);
  uint8_t* data = malloc(len);
  if (fread(data, 1, len, f) != (size_t)len) { printf("short read\n"); return 1; }
  fclose(f);

  if (len < 8) { printf("too small\n"); return 1; }
  uint64_t declared;
  memcpy(&declared, data, 8);

  size_t frame_csize = ZSTD_findFrameCompressedSize(data + 8, len - 8);
  printf("file=%s file_size=%ld declared_output=%llu (0x%llx)\n",
         argv[1], len, (unsigned long long)declared, (unsigned long long)declared);
  if (ZSTD_isError(frame_csize)) {
    printf("findFrameCompressedSize: %s\n", ZSTD_getErrorName(frame_csize));
  } else {
    printf("first frame compressed size = %zu (0x%zx), trailing bytes after frame = %zu\n",
           frame_csize, frame_csize, (size_t)(len - 8) - frame_csize);
  }

  uint8_t* dst = malloc(declared ? declared : 1);
  size_t r = ZSTD_decompress(dst, declared, data + 8, len - 8);
  if (ZSTD_isError(r)) {
    printf("ZSTD_decompress -> ERROR: %s  <-- what the game's assert prints\n",
           ZSTD_getErrorName(r));
  } else {
    printf("ZSTD_decompress -> ok, %zu bytes (declared %llu)\n", r,
           (unsigned long long)declared);
  }
  return 0;
}
