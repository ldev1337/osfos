#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC

// Kernel memory and string overrides
#include <arch/x86_64/spinlock.h>
#include <kernel/memory/heap.h>
#include <arch/x86_64/string.h>

#define STBTT_malloc(x,u)  ((void)(u), kmalloc(x))
#define STBTT_free(x,u)    ((void)(u), kfree(x))
#define STBTT_assert(x)    ((void)(0))
#define STBTT_memcpy       memcpy
#define STBTT_memset       memset
#define STBTT_strlen       strlen

// Math overrides using GCC/Clang builtins for freestanding environments
#define STBTT_ifloor(x)    ((int)__builtin_floorf(x))
#define STBTT_iceil(x)     ((int)__builtin_ceilf(x))
#define STBTT_sqrt(x)      __builtin_sqrtf(x)
#define STBTT_pow(x,y)     __builtin_powf(x,y)
#define STBTT_fmod(x,y)    __builtin_fmodf(x,y)
#define STBTT_cos(x)       __builtin_cosf(x)
#define STBTT_acos(x)      __builtin_acosf(x)
#define STBTT_fabs(x)      __builtin_fabsf(x)

#include <stb/stb_truetype.h>
#include <kernel/graphics/truetype.h>

namespace {

constexpr size_t GLYPH_CACHE_SLOTS = 96;
constexpr size_t GLYPH_CACHE_BITMAP_BYTES = 2048;

struct GlyphCacheEntry {
    const void *font;
    uint32_t codepoint;
    uint32_t size_key;
    uint32_t age;
    uint16_t width;
    uint16_t height;
    int16_t x0;
    int16_t y0;
    int16_t advance;
    uint16_t pins;
    bool valid;
    uint8_t bitmap[GLYPH_CACHE_BITMAP_BYTES];
};

Spinlock glyph_cache_lock;
GlyphCacheEntry glyph_cache[GLYPH_CACHE_SLOTS]{};
uint32_t glyph_cache_age = 0;

bool decode_utf8_codepoint(const char *text, const char *&next, uint32_t &codepoint) {
    if (!text)
        return false;

    const unsigned char c0 = static_cast<unsigned char>(text[0]);
    if ((c0 & 0x80u) == 0u) {
        codepoint = c0;
        next = text + 1;
        return true;
    }

    if ((c0 & 0xE0u) == 0xC0u && text[1] != '\0') {
        const unsigned char c1 = static_cast<unsigned char>(text[1]);
        if ((c1 & 0xC0u) == 0x80u) {
            codepoint = (static_cast<uint32_t>(c0 & 0x1Fu) << 6) |
                        static_cast<uint32_t>(c1 & 0x3Fu);
            next = text + 2;
            return true;
        }
    }

    if ((c0 & 0xF0u) == 0xE0u && text[1] != '\0' && text[2] != '\0') {
        const unsigned char c1 = static_cast<unsigned char>(text[1]);
        const unsigned char c2 = static_cast<unsigned char>(text[2]);
        if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u) {
            codepoint = (static_cast<uint32_t>(c0 & 0x0Fu) << 12) |
                        (static_cast<uint32_t>(c1 & 0x3Fu) << 6) |
                        static_cast<uint32_t>(c2 & 0x3Fu);
            next = text + 3;
            return true;
        }
    }

    if ((c0 & 0xF8u) == 0xF0u && text[1] != '\0' && text[2] != '\0' && text[3] != '\0') {
        const unsigned char c1 = static_cast<unsigned char>(text[1]);
        const unsigned char c2 = static_cast<unsigned char>(text[2]);
        const unsigned char c3 = static_cast<unsigned char>(text[3]);
        if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u && (c3 & 0xC0u) == 0x80u) {
            codepoint = (static_cast<uint32_t>(c0 & 0x07u) << 18) |
                        (static_cast<uint32_t>(c1 & 0x3Fu) << 12) |
                        (static_cast<uint32_t>(c2 & 0x3Fu) << 6) |
                        static_cast<uint32_t>(c3 & 0x3Fu);
            next = text + 4;
            return true;
        }
    }

    codepoint = 0;
    next = text + 1;
    return false;
}

} // namespace

namespace Kernel::Graphics {

TrueTypeFont::TrueTypeFont()
    : font_buffer_(nullptr), buffer_size_(0), font_info_storage_(nullptr), initialized_(false) {
    font_info_storage_ = kmalloc(sizeof(stbtt_fontinfo));
    if (font_info_storage_)
        memset(font_info_storage_, 0, sizeof(stbtt_fontinfo));
}

TrueTypeFont::~TrueTypeFont() {
    reset();
    if (font_info_storage_)
        kfree(font_info_storage_);
    font_info_storage_ = nullptr;
}

void TrueTypeFont::reset() {
    {
        SpinlockGuard guard(glyph_cache_lock);
        for (auto &entry : glyph_cache) {
            if (entry.font == this)
                entry.valid = false;
        }
    }
    if (font_buffer_) {
        kfree(font_buffer_);
        font_buffer_ = nullptr;
    }
    buffer_size_ = 0;
    if (font_info_storage_)
        memset(font_info_storage_, 0, sizeof(stbtt_fontinfo));
    initialized_ = false;
}

bool TrueTypeFont::load(const uint8_t *buffer, size_t size, int font_index) {
    if (!buffer || size == 0 || font_index < 0 || !font_info_storage_)
        return false;

    reset();

    font_buffer_ = static_cast<uint8_t *>(kmalloc(size));
    if (!font_buffer_)
        return false;

    memcpy(font_buffer_, buffer, size);
    buffer_size_ = size;

    auto *info = static_cast<stbtt_fontinfo *>(font_info_storage_);
    const int offset = stbtt_GetFontOffsetForIndex(font_buffer_, font_index);
    if (offset < 0) {
        reset();
        return false;
    }

    if (!stbtt_InitFont(info, font_buffer_, offset)) {
        reset();
        return false;
    }

    initialized_ = true;
    return true;
}

float TrueTypeFont::scale_for_pixel_height(float font_size_pixels) const {
    if (!initialized_ || font_size_pixels <= 0.0f)
        return 0.0f;
    auto *info = static_cast<const stbtt_fontinfo *>(font_info_storage_);
    return stbtt_ScaleForPixelHeight(info, font_size_pixels);
}

int TrueTypeFont::get_text_width(float font_size_pixels, const char *text) const {
    if (!initialized_ || !text || font_size_pixels <= 0.0f)
        return 0;

    auto *info = static_cast<const stbtt_fontinfo *>(font_info_storage_);
    const float scale = stbtt_ScaleForPixelHeight(info, font_size_pixels);
    int width = 0;
    const char *p = text;

    while (*p) {
        const char *next = p;
        uint32_t codepoint = 0;
        if (!decode_utf8_codepoint(p, next, codepoint)) {
            ++p;
            continue;
        }

        int advance = 0;
        int lsb = 0;
        stbtt_GetCodepointHMetrics(info, codepoint, &advance, &lsb);
        width += static_cast<int>(advance * scale);
        p = next;
    }

    return width;
}

void TrueTypeFont::get_v_metrics(float font_size_pixels, float *ascent, float *descent, float *lineGap) {
    if (!initialized_) {
        if (ascent) *ascent = 0.0f;
        if (descent) *descent = 0.0f;
        if (lineGap) *lineGap = 0.0f;
        return;
    }

    auto *info = static_cast<stbtt_fontinfo *>(font_info_storage_);
    int ia, id, il;
    stbtt_GetFontVMetrics(info, &ia, &id, &il);
    const float scale = stbtt_ScaleForPixelHeight(info, font_size_pixels);
    if (ascent)  *ascent  = static_cast<float>(ia) * scale;
    if (descent) *descent = static_cast<float>(id) * scale;
    if (lineGap) *lineGap = static_cast<float>(il) * scale;
}

int TrueTypeFont::render_text(uint32_t *dest_buffer, uint32_t dest_width, uint32_t dest_height, uint32_t dest_pitch,
                            int x, int y, float font_size_pixels, const char *text, uint32_t color) {
    if (!initialized_ || !text || !dest_buffer || dest_pitch == 0 || dest_width == 0 || dest_height == 0 || font_size_pixels <= 0.0f)
        return x;

    auto *info = static_cast<stbtt_fontinfo *>(font_info_storage_);
    const float scale = stbtt_ScaleForPixelHeight(info, font_size_pixels);

    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
    const int baseline = y + static_cast<int>(ascent * scale);
    const uint32_t foreground_r = (color >> 16) & 0xFFu;
    const uint32_t foreground_g = (color >> 8) & 0xFFu;
    const uint32_t foreground_b = color & 0xFFu;

    int cursor_x = x;
    const char *p = text;

    while (*p) {
        const char *next = p;
        uint32_t codepoint = 0;
        if (!decode_utf8_codepoint(p, next, codepoint)) {
            ++p;
            continue;
        }

        int advance = 0;
        int lsb = 0;
        stbtt_GetCodepointHMetrics(info, codepoint, &advance, &lsb);
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetCodepointBitmapBox(info, codepoint, scale, scale, &x0, &y0, &x1, &y1);

        const int gw = x1 - x0;
        const int gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            const size_t bitmap_size = static_cast<size_t>(gw) * static_cast<size_t>(gh);
            union {
                float value;
                uint32_t bits;
            } size_key{font_size_pixels};
            GlyphCacheEntry *cached_entry = nullptr;
            const unsigned char *glyph_bitmap = nullptr;
            unsigned char *temporary_bitmap = nullptr;

            if (bitmap_size <= GLYPH_CACHE_BITMAP_BYTES) {
                SpinlockGuard guard(glyph_cache_lock);
                for (auto &entry : glyph_cache) {
                    if (entry.valid && entry.font == this &&
                        entry.codepoint == codepoint && entry.size_key == size_key.bits) {
                        ++entry.pins;
                        entry.age = ++glyph_cache_age;
                        cached_entry = &entry;
                        glyph_bitmap = entry.bitmap;
                        break;
                    }
                }
            }

            if (!glyph_bitmap) {
                temporary_bitmap = static_cast<unsigned char *>(kmalloc(bitmap_size));
                if (temporary_bitmap) {
                    stbtt_MakeCodepointBitmap(info, temporary_bitmap, gw, gh, gw, scale, scale, codepoint);
                    if (bitmap_size <= GLYPH_CACHE_BITMAP_BYTES) {
                        SpinlockGuard guard(glyph_cache_lock);
                        GlyphCacheEntry *slot = nullptr;
                        for (auto &entry : glyph_cache) {
                            if (entry.pins == 0 && !entry.valid) {
                                slot = &entry;
                                break;
                            }
                            if (entry.pins == 0 &&
                                (!slot || entry.age < slot->age))
                                slot = &entry;
                        }
                        if (slot) {
                            slot->valid = false;
                            slot->font = this;
                            slot->codepoint = codepoint;
                            slot->size_key = size_key.bits;
                            slot->width = static_cast<uint16_t>(gw);
                            slot->height = static_cast<uint16_t>(gh);
                            slot->x0 = static_cast<int16_t>(x0);
                            slot->y0 = static_cast<int16_t>(y0);
                            slot->advance = static_cast<int16_t>(advance);
                            memcpy(slot->bitmap, temporary_bitmap, bitmap_size);
                            slot->age = ++glyph_cache_age;
                            slot->pins = 1;
                            slot->valid = true;
                            cached_entry = slot;
                            glyph_bitmap = slot->bitmap;
                        }
                    }
                    if (!glyph_bitmap)
                        glyph_bitmap = temporary_bitmap;
                }
            }

            if (glyph_bitmap) {

                for (int gy = 0; gy < gh; ++gy) {
                    const int dest_y = baseline + y0 + gy;
                    if (dest_y < 0 || dest_y >= static_cast<int>(dest_height))
                        continue;

                    for (int gx = 0; gx < gw; ++gx) {
                        const int dest_x = cursor_x + x0 + gx;
                        if (dest_x < 0 || dest_x >= static_cast<int>(dest_width))
                            continue;

                        const uint8_t alpha = glyph_bitmap[static_cast<size_t>(gy) * static_cast<size_t>(gw) + static_cast<size_t>(gx)];
                        if (alpha == 0u)
                            continue;

                        uint32_t *dest_pixel = dest_buffer + static_cast<size_t>(dest_y) * (dest_pitch / sizeof(uint32_t)) + dest_x;
                        if (alpha == 255u) {
                            *dest_pixel = color;
                            continue;
                        }

                        const uint32_t bg = *dest_pixel;
                        const uint32_t rb = static_cast<uint32_t>((bg >> 16) & 0xFFu);
                        const uint32_t gb = static_cast<uint32_t>((bg >> 8) & 0xFFu);
                        const uint32_t bb = static_cast<uint32_t>(bg & 0xFFu);

                        const uint32_t r = (foreground_r * alpha + rb * (255u - alpha)) / 255u;
                        const uint32_t g = (foreground_g * alpha + gb * (255u - alpha)) / 255u;
                        const uint32_t b = (foreground_b * alpha + bb * (255u - alpha)) / 255u;
                        *dest_pixel = (r << 16) | (g << 8) | b;
                    }
                }
                if (cached_entry) {
                    SpinlockGuard guard(glyph_cache_lock);
                    --cached_entry->pins;
                }
            }
            if (temporary_bitmap)
                kfree(temporary_bitmap);
        }

        cursor_x += static_cast<int>(advance * scale);
        p = next;
    }

    return cursor_x;
}

} // namespace Kernel::Graphics