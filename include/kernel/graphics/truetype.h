#pragma once

#include <cstdint>
#include <cstddef>

namespace Kernel::Graphics {

class TrueTypeFont {
public:
    TrueTypeFont();
    ~TrueTypeFont();

    void reset();

    // Load font from a memory buffer loaded via LazioFS.
    bool load(const uint8_t *buffer, size_t size, int font_index = 0);
    bool is_valid() const { return initialized_ && font_info_storage_ != nullptr; }

    // Render text into a target pixel buffer (e.g. VirtualFramebuffer buffer) with anti-aliasing.
    int render_text(uint32_t *dest_buffer, uint32_t dest_width, uint32_t dest_height, uint32_t dest_pitch,
                    int x, int y, float font_size_pixels, const char *text, uint32_t color);

    // Measuring utilities for callers that need bounds before rendering.
    float scale_for_pixel_height(float font_size_pixels) const;
    int get_text_width(float font_size_pixels, const char *text) const;

    // Get vertical font metrics scaled to pixel height.
    void get_v_metrics(float font_size_pixels, float *ascent, float *descent, float *lineGap);

private:
    uint8_t *font_buffer_;
    size_t buffer_size_;
    void *font_info_storage_; // Opaque storage for stbtt_fontinfo
    bool initialized_;
};

} // namespace Kernel::Graphics