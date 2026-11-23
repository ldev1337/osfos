#include <kernel/media/jpeg.h>

#include <climits>
#include <cstddef>

extern "C" {
void *kmalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);
}

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(condition) ((void)0)
#define STBI_MALLOC(size) kmalloc(size)
#define STBI_REALLOC(pointer, size) krealloc(pointer, size)
#define STBI_REALLOC_SIZED(pointer, old_size, new_size) krealloc(pointer, new_size)
#define STBI_FREE(pointer) kfree(pointer)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace Kernel::Media::JPEG {
namespace {

bool output_dimensions(uint32_t width, uint32_t height, uint32_t max_width,
                      uint32_t max_height, uint32_t &out_width,
                      uint32_t &out_height) {
  if (!width || !height || !max_width || !max_height)
    return false;
  out_width = width;
  out_height = height;
  if (out_width > max_width || out_height > max_height) {
    if (static_cast<uint64_t>(max_width) * height <=
        static_cast<uint64_t>(max_height) * width) {
      out_width = max_width;
      out_height = static_cast<uint32_t>(
          static_cast<uint64_t>(height) * max_width / width);
    } else {
      out_height = max_height;
      out_width = static_cast<uint32_t>(
          static_cast<uint64_t>(width) * max_height / height);
    }
    if (!out_width)
      out_width = 1;
    if (!out_height)
      out_height = 1;
  }
  return true;
}

} // namespace

int decode(const uint8_t *input, size_t input_size, uint32_t *output,
           size_t output_pixels, uint32_t max_width, uint32_t max_height,
           uint32_t *output_width, uint32_t *output_height) {
  if (!input || !output || input_size < 4 || input_size > INT_MAX ||
      !output_width || !output_height)
    return -1;

  int source_width = 0;
  int source_height = 0;
  int source_components = 0;
  if (!stbi_info_from_memory(input, static_cast<int>(input_size),
                             &source_width, &source_height,
                             &source_components))
    return -2;
  if (source_width <= 0 || source_height <= 0 || source_width > 512 ||
      source_height > 512 || source_components < 1 || source_components > 4)
    return -3;

  uint32_t out_width = 0;
  uint32_t out_height = 0;
  if (!output_dimensions(static_cast<uint32_t>(source_width),
                         static_cast<uint32_t>(source_height), max_width,
                         max_height, out_width, out_height))
    return -4;
  const uint64_t needed_pixels =
      static_cast<uint64_t>(out_width) * out_height;
  if (needed_pixels > output_pixels)
    return -5;

  int decoded_width = 0;
  int decoded_height = 0;
  int decoded_components = 0;
  stbi_uc *rgb = stbi_load_from_memory(
      input, static_cast<int>(input_size), &decoded_width, &decoded_height,
      &decoded_components, 3);
  if (!rgb)
    return -6;
  if (decoded_width != source_width || decoded_height != source_height) {
    stbi_image_free(rgb);
    return -7;
  }

  for (uint32_t y = 0; y < out_height; ++y) {
    const uint32_t source_y = static_cast<uint32_t>(
        static_cast<uint64_t>(y) * decoded_height / out_height);
    for (uint32_t x = 0; x < out_width; ++x) {
      const uint32_t source_x = static_cast<uint32_t>(
          static_cast<uint64_t>(x) * decoded_width / out_width);
      const size_t source_offset =
          (static_cast<size_t>(source_y) * decoded_width + source_x) * 3;
      output[static_cast<size_t>(y) * out_width + x] =
          0xFF000000u | (static_cast<uint32_t>(rgb[source_offset]) << 16) |
          (static_cast<uint32_t>(rgb[source_offset + 1]) << 8) |
          static_cast<uint32_t>(rgb[source_offset + 2]);
    }
  }
  stbi_image_free(rgb);
  *output_width = out_width;
  *output_height = out_height;
  return 0;
}

} // namespace Kernel::Media::JPEG
