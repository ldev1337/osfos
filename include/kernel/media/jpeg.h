#pragma once

#include <cstddef>
#include <cstdint>

namespace Kernel::Media::JPEG {

// Decode an 8-bit baseline sequential JPEG into opaque 0xAARRGGBB pixels.
// Output is nearest-neighbour downscaled to fit max_width/max_height; it is
// never enlarged. Progressive, arithmetic-coded, CMYK, and >2x sampling files
// are rejected. Returns 0 on success.
int decode(const uint8_t *input, size_t input_size, uint32_t *output,
           size_t output_pixels, uint32_t max_width, uint32_t max_height,
           uint32_t *output_width, uint32_t *output_height);

} // namespace Kernel::Media::JPEG
