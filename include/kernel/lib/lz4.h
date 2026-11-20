
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace lib::lz4 {
constexpr size_t HASH_ENTRIES = 65536;
constexpr size_t WORKSPACE_SIZE = HASH_ENTRIES * sizeof(uint32_t);
enum Error : int {
  InvalidArgument = -1,
  OutputTooSmall = -2,
  InvalidBlock = -3,
  UnsupportedSize = -4
};
// Returns maximum required output capacity, or 0 on overflow.
size_t compress_bound(size_t input_size);
int compress(const void *src, size_t src_size, void *dst, size_t dst_capacity,
             uint32_t *hash_table, size_t hash_entries);

int decompress(const void *src, size_t src_size, void *dst,
               size_t dst_capacity);
} // namespace lib::lz4
