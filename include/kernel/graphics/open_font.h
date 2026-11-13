#pragma once

#include <cstdint>
#include <cstddef>
#include <kernel/graphics/truetype.h>
#include <kernel/fs/laziofs.h>
#include <kernel/memory/heap.h>
#include <arch/x86_64/string.h>

// Link external symbols defined by incbin in embedded font assembly
extern "C" {
    extern const uint8_t geist_ttf_start[];
    extern const uint8_t geist_ttf_end[];
    extern const uint8_t geistmono_ttf_start[];
    extern const uint8_t geistmono_ttf_end[];
}

namespace Kernel::Graphics {

class OpenFont {
private:
    static bool equals_ci(const char *a, const char *b) {
        if (!a || !b)
            return a == b;
        for (; *a && *b; ++a, ++b) {
            unsigned char ca = static_cast<unsigned char>(*a);
            unsigned char cb = static_cast<unsigned char>(*b);
            if (ca >= 'A' && ca <= 'Z')
                ca += 'a' - 'A';
            if (cb >= 'A' && cb <= 'Z')
                cb += 'a' - 'A';
            if (ca != cb)
                return false;
        }
        return *a == *b;
    }

    static const char *basename_of(const char *path) {
        if (!path)
            return nullptr;
        const char *slash = strrchr(path, '/');
        return slash ? slash + 1 : path;
    }

    static bool is_named_font(const char *path, const char *expected_name) {
        const char *base = basename_of(path);
        return base && equals_ci(base, expected_name);
    }

    static bool ends_with_ttf(const char *name) {
        size_t len = strlen(name);
        if (len < 4) return false;
        const char *ext = name + (len - 4);
        return (ext[0] == '.') &&
               (ext[1] == 't' || ext[1] == 'T') &&
               (ext[2] == 't' || ext[2] == 'T') &&
               (ext[3] == 'f' || ext[3] == 'F');
    }

    static bool is_in_fonts_dir(const char *path) {
        if (!path) return false;
        if (path[0] == '/') path++;
        return strncmp(path, "fonts/", 6) == 0 && path[6] != '\0';
    }

public:
    static bool load(TrueTypeFont &out_font, const char *font_name = nullptr) {
        if (!font_name || font_name[0] == '\0')
            return load_embedded(out_font);
        return load_specific(out_font, font_name);
    }

    static bool load_preferred(TrueTypeFont &out_font, const char *primary_name,
                               const char *secondary_name = nullptr,
                               const char *tertiary_name = nullptr) {
        if (primary_name && primary_name[0] != '\0' && load_specific(out_font, primary_name))
            return true;
        if (secondary_name && secondary_name[0] != '\0' && load_specific(out_font, secondary_name))
            return true;
        if (tertiary_name && tertiary_name[0] != '\0' && load_specific(out_font, tertiary_name))
            return true;
        return load_first_available(out_font, nullptr, 0) || load_embedded(out_font);
    }

    static size_t enumerate_available(char *out_names, size_t max_entries, size_t entry_len) {
        if (!out_names || max_entries == 0 || entry_len == 0)
            return 0;
        if (!Kernel::FS::VFS::is_mounted())
            return 0;

        size_t count = 0;
        for (uint32_t i = 0; i < Kernel::FS::VFS_MAX_INODES && count < max_entries; ++i) {
            Kernel::FS::VFSFile entry{};
            if (!Kernel::FS::VFS::get_file_at(i, &entry) || !entry.exists || entry.is_directory)
                continue;
            if (!ends_with_ttf(entry.name))
                continue;

            char *slot = out_names + count * entry_len;
            const size_t copy_len = entry_len - 1;
            strncpy(slot, entry.name, copy_len);
            slot[copy_len] = '\0';
            ++count;
        }
        return count;
    }

    static bool load_first_available(TrueTypeFont &out_font, char *out_name = nullptr, size_t out_name_len = 0) {
        static constexpr const char *preferred_names[] = {
            "GeistMono.ttf",
            "Geist.ttf",
        };

        for (const char *preferred_name : preferred_names) {
            if (load_specific(out_font, preferred_name)) {
                if (out_name && out_name_len > 0) {
                    const size_t copy_len = out_name_len - 1;
                    strncpy(out_name, preferred_name, copy_len);
                    out_name[copy_len] = '\0';
                }
                return true;
            }
        }

        if (Kernel::FS::VFS::is_mounted()) {
            for (uint32_t i = 0; i < Kernel::FS::VFS_MAX_INODES; ++i) {
                Kernel::FS::VFSFile entry{};
                if (!Kernel::FS::VFS::get_file_at(i, &entry) || !entry.exists || entry.is_directory)
                    continue;
                if (!ends_with_ttf(entry.name))
                    continue;

                if (out_name && out_name_len > 0) {
                    const size_t copy_len = out_name_len - 1;
                    strncpy(out_name, entry.name, copy_len);
                    out_name[copy_len] = '\0';
                }
                out_font.reset();
                if (load(out_font, entry.name))
                    return true;
            }
        }

        if (out_name && out_name_len > 0)
            out_name[0] = '\0';
        return load_embedded(out_font);
    }

    static bool load_embedded_named(TrueTypeFont &out_font, const char *font_name) {
        if (!font_name || font_name[0] == '\0')
            return false;

        const char *base = basename_of(font_name);
        if (equals_ci(base, "Geist.ttf")) {
            const size_t size = static_cast<size_t>(geist_ttf_end - geist_ttf_start);
            if (size == 0)
                return false;
            out_font.reset();
            return out_font.load(geist_ttf_start, size);
        }
        if (equals_ci(base, "GeistMono.ttf") || equals_ci(base, "GeistMono.ttc")) {
            const size_t size = static_cast<size_t>(geistmono_ttf_end - geistmono_ttf_start);
            if (size == 0)
                return false;
            out_font.reset();
            return out_font.load(geistmono_ttf_start, size);
        }
        return false;
    }

    /// Load the built-in embedded fallback font directly from memory
    static bool load_embedded(TrueTypeFont &out_font) {
        if (load_embedded_named(out_font, "GeistMono.ttf"))
            return true;
        if (load_embedded_named(out_font, "Geist.ttf"))
            return true;
        return false;
    }

    /// Load a specific font by name or path, falling back to embedded font if unavailable
    static bool load_specific(TrueTypeFont &out_font, const char *font_name) {
        if (!font_name || font_name[0] == '\0')
            return load_embedded(out_font);

        if (Kernel::FS::VFS::is_mounted()) {
            char path[Kernel::FS::VFS_MAX_PATH];
            const char *candidate = font_name;
            if (candidate[0] == '/')
                candidate++;

            const char *basename = basename_of(font_name);
            if (is_in_fonts_dir(font_name)) {
                __builtin_snprintf(path, sizeof(path), "%s", font_name);
            } else {
                __builtin_snprintf(path, sizeof(path), "fonts/%s", basename);
            }

            Kernel::FS::VFSFile file;
            if (Kernel::FS::VFS::find_file(path, &file) && file.exists && file.size > 0) {
                uint8_t *buffer = static_cast<uint8_t *>(kmalloc(file.size));
                if (buffer) {
                    const int read_bytes = Kernel::FS::VFS::read_file(&file, buffer, file.size, 0);
                    if (read_bytes == static_cast<int>(file.size)) {
                        out_font.reset();
                        const bool success = out_font.load(buffer, file.size);
                        kfree(buffer);
                        if (success)
                            return true;
                    }
                    kfree(buffer);
                }
            }

            // Also try the direct name if the VFS path did not resolve, which is common for mounted assets.
            if (Kernel::FS::VFS::find_file(font_name, &file) && file.exists && file.size > 0) {
                uint8_t *buffer = static_cast<uint8_t *>(kmalloc(file.size));
                if (buffer) {
                    const int read_bytes = Kernel::FS::VFS::read_file(&file, buffer, file.size, 0);
                    if (read_bytes == static_cast<int>(file.size)) {
                        out_font.reset();
                        const bool success = out_font.load(buffer, file.size);
                        kfree(buffer);
                        if (success)
                            return true;
                    }
                    kfree(buffer);
                }
            }
        }

        if (load_embedded_named(out_font, basename_of(font_name)))
            return true;
        return load_embedded(out_font);
    }

    /// Pick and load a random .ttf font found in the fonts directory
    static bool load_random(TrueTypeFont &out_font) {
        if (!Kernel::FS::VFS::is_mounted())
            return load_embedded(out_font);

        constexpr uint32_t MAX_FONTS = 32;
        Kernel::FS::VFSFile font_files[MAX_FONTS];
        uint32_t count = 0;

        for (uint32_t i = 0; i < Kernel::FS::VFS_MAX_INODES && count < MAX_FONTS; ++i) {
            Kernel::FS::VFSFile entry;
            if (!Kernel::FS::VFS::get_file_at(i, &entry))
                continue;
            if (!entry.exists || entry.is_directory || entry.size == 0)
                continue;

            if (is_in_fonts_dir(entry.name) && ends_with_ttf(entry.name))
                font_files[count++] = entry;
        }

        if (count == 0)
            return load_embedded(out_font);

        static uint32_t seed = 0x5EEEDu;
        seed = seed * 1103515245u + 12345u;
        const uint32_t index = (seed >> 16) % count;

        const Kernel::FS::VFSFile &selected = font_files[index];
        uint8_t *buffer = static_cast<uint8_t *>(kmalloc(selected.size));
        if (!buffer)
            return load_embedded(out_font);

        const int read_bytes = Kernel::FS::VFS::read_file(&selected, buffer, selected.size, 0);
        if (read_bytes != static_cast<int>(selected.size)) {
            kfree(buffer);
            return load_embedded(out_font);
        }

        out_font.reset();
        const bool success = out_font.load(buffer, selected.size);
        kfree(buffer);
        return success ? true : load_embedded(out_font);
    }
};

} // namespace Kernel::Graphics