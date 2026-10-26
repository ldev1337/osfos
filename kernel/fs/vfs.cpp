#include <kernel/fs/vfs.h>
#include <kernel/drivers/ata.h>
#include <stdint.h>
#include <stddef.h>

namespace Kernel::FS {
namespace {
constexpr uint32_t FS_VERSION = 1;
constexpr uint32_t FAT_FREE = 0;
constexpr uint32_t FAT_EOC = 0xFFFFFFFFu;
constexpr uint32_t ENTRY_FREE = 0;
constexpr uint32_t ENTRY_FILE = 1;
constexpr uint32_t ENTRY_DIR = 2;
constexpr uint32_t FAT_ENTRIES_PER_SECTOR = VFS_SECTOR_SIZE / sizeof(uint32_t);
constexpr uint32_t FAT_DIRTY_COUNT = (VFS_MAX_SECTORS + FAT_ENTRIES_PER_SECTOR - 1) / FAT_ENTRIES_PER_SECTOR;
constexpr uint32_t DIR_ENTRY_SIZE = 128;
constexpr uint32_t DIR_ENTRIES_PER_SECTOR = VFS_SECTOR_SIZE / DIR_ENTRY_SIZE;
constexpr uint32_t DIR_SECTORS = (VFS_MAX_ENTRIES * DIR_ENTRY_SIZE) / VFS_SECTOR_SIZE;
constexpr char FS_MAGIC[8] = {'O','S','F','O','S','F','S','1'};

#pragma pack(push, 1)
struct Superblock {
    char magic[8];
    uint32_t version;
    uint32_t sector_size;
    uint32_t total_sectors;
    uint32_t fat_start;
    uint32_t fat_sectors;
    uint32_t dir_start;
    uint32_t dir_sectors;
    uint32_t data_start;
    uint32_t entry_count;
    uint32_t checksum;
    uint8_t reserved[VFS_SECTOR_SIZE - 8 - (10 * sizeof(uint32_t))];
};
struct DirectoryEntry {
    char path[VFS_MAX_PATH];
    uint32_t size;
    uint32_t first_cluster;
    uint32_t flags;
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(Superblock) == VFS_SECTOR_SIZE, "Superblock must occupy one sector");
static_assert(sizeof(DirectoryEntry) == DIR_ENTRY_SIZE, "Directory entries must be 128 bytes");

struct FileStream {
    bool used;
    bool readable;
    bool writable;
    bool append;
    uint32_t entry_index;
    uint32_t offset;
};

Superblock g_sb{};
uint32_t g_fat[VFS_MAX_SECTORS]{};
DirectoryEntry g_dir[VFS_MAX_ENTRIES]{};
bool g_fat_dirty[FAT_DIRTY_COUNT]{};
FileStream g_streams[VFS_MAX_OPEN_FILES]{};
bool g_mounted = false;
uint32_t g_next_free_cluster = 0;
uint32_t g_next_free_entry = 0;

inline void bytes_zero(void* ptr, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(ptr);
    for (size_t i = 0; i < n; ++i) p[i] = 0;
}
inline void bytes_copy(void* dst, const void* src, size_t n) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
}
inline bool str_equal(const char* a, const char* b) {
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}
inline bool bytes_equal(const void* a, const void* b, size_t n) {
    const uint8_t* x = static_cast<const uint8_t*>(a);
    const uint8_t* y = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return false;
    return true;
}
inline size_t bounded_length(const char* s, size_t max) { size_t n = 0; while (n < max && s[n]) ++n; return n; }
inline size_t str_length(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
inline char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
inline uint32_t checksum_fnv1a(const uint8_t* data, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= data[i]; h *= 16777619u; }
    return h;
}

bool read_sector(uint32_t relative_lba, uint8_t* out) {
    if (!out || relative_lba >= g_sb.total_sectors || relative_lba > UINT32_MAX - VFS_START_LBA) return false;
    return Kernel::Drivers::ATADriver::read_sectors(VFS_START_LBA + relative_lba, 1, out);
}
bool write_sector(uint32_t relative_lba, const uint8_t* data) {
    if (!data || relative_lba >= g_sb.total_sectors || relative_lba > UINT32_MAX - VFS_START_LBA) return false;
    return Kernel::Drivers::ATADriver::write_sectors(VFS_START_LBA + relative_lba, 1, const_cast<uint8_t*>(data));
}
bool flush_fat_dirty() {
    if (!g_sb.total_sectors) return false;
    for (uint32_t s = 0; s < g_sb.fat_sectors; ++s) {
        if (!g_fat_dirty[s]) continue;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(g_fat + s * FAT_ENTRIES_PER_SECTOR);
        if (!write_sector(g_sb.fat_start + s, data)) return false;
        g_fat_dirty[s] = false;
    }
    return true;
}
inline void mark_fat_dirty(uint32_t cluster) { g_fat_dirty[cluster / FAT_ENTRIES_PER_SECTOR] = true; }
bool write_dir_sector(uint32_t sector_index) {
    if (sector_index >= g_sb.dir_sectors) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(g_dir + sector_index * DIR_ENTRIES_PER_SECTOR);
    return write_sector(g_sb.dir_start + sector_index, p);
}
bool flush_dir_entry(uint32_t index) {
    if (index >= VFS_MAX_ENTRIES) return false;
    return write_dir_sector(index / DIR_ENTRIES_PER_SECTOR);
}

bool normalize_path(const char* input, char out[VFS_MAX_PATH]) {
    if (!input || !out) return false;
    size_t out_len = 0;
    out[out_len++] = '/';
    while (*input == '/' || *input == '\\') ++input;
    while (*input) {
        char component[VFS_MAX_PATH];
        size_t n = 0;
        while (*input && *input != '/' && *input != '\\') {
            if (n + 1 >= sizeof(component)) return false;
            component[n++] = ascii_lower(*input++);
        }
        while (*input == '/' || *input == '\\') ++input;
        component[n] = '\0';
        if (n == 0 || (n == 1 && component[0] == '.')) continue;
        if (n == 2 && component[0] == '.' && component[1] == '.') {
            if (out_len == 1) return false;
            while (out_len > 1 && out[out_len - 1] != '/') --out_len;
            if (out_len > 1) --out_len;
            out[out_len] = '\0';
            continue;
        }
        if (out_len > 1) {
            if (out_len + 1 + n >= VFS_MAX_PATH) return false;
            out[out_len++] = '/';
        } else if (out_len + n >= VFS_MAX_PATH) return false;
        for (size_t i = 0; i < n; ++i) out[out_len++] = component[i];
        out[out_len] = '\0';
    }
    out[out_len] = '\0';
    return true;
}

int find_entry_index(const char* normalized) {
    for (uint32_t i = 0; i < VFS_MAX_ENTRIES; ++i)
        if (g_dir[i].flags != ENTRY_FREE && str_equal(g_dir[i].path, normalized)) return static_cast<int>(i);
    return -1;
}

int free_entry_index() {
    for (uint32_t count = 0, i = g_next_free_entry; count < VFS_MAX_ENTRIES; ++count, i = (i + 1) % VFS_MAX_ENTRIES) {
        if (g_dir[i].flags == ENTRY_FREE) {
            g_next_free_entry = i;
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool parent_path(const char* path, char out[VFS_MAX_PATH]) {
    size_t n = str_length(path);
    if (n == 0 || str_equal(path, "/")) return false;
    while (n > 1 && path[n - 1] != '/') --n;
    if (n <= 1) { out[0] = '/'; out[1] = '\0'; return true; }
    size_t parent_len = n - 1;
    if (parent_len >= VFS_MAX_PATH) return false;
    bytes_copy(out, path, parent_len); out[parent_len] = '\0'; return true;
}
bool parent_exists(const char* normalized) {
    char parent[VFS_MAX_PATH];
    if (!parent_path(normalized, parent)) return false;
    if (str_equal(parent, "/")) return true;
    int p = find_entry_index(parent);
    return p >= 0 && g_dir[p].flags == ENTRY_DIR;
}
bool has_children(const char* dir_path) {
    size_t n = str_length(dir_path);
    if (n + 1 >= VFS_MAX_PATH) return false;
    char prefix[VFS_MAX_PATH]; bytes_copy(prefix, dir_path, n);
    if (n > 1) prefix[n++] = '/';
    prefix[n] = '\0';
    for (uint32_t i = 0; i < VFS_MAX_ENTRIES; ++i) {
        if (g_dir[i].flags == ENTRY_FREE) continue;
        const char* p = g_dir[i].path;
        size_t j = 0;
        while (prefix[j] && p[j] == prefix[j]) ++j;
        if (prefix[j] == '\0' && p[j] != '\0') return true;
    }
    return false;
}

bool fill_public_file(uint32_t index, VFSFile* out) {
    if (!out || index >= VFS_MAX_ENTRIES || g_dir[index].flags == ENTRY_FREE) return false;
    bytes_zero(out, sizeof(*out));
    bytes_copy(out->name, g_dir[index].path, VFS_MAX_PATH);
    out->size = g_dir[index].size;
    out->start_lba = g_dir[index].first_cluster;
    out->exists = true;
    out->entry_index = index;
    out->is_directory = g_dir[index].flags == ENTRY_DIR;
    return true;
}

inline bool is_valid_cluster(uint32_t c) { return c >= g_sb.data_start && c < g_sb.total_sectors; }
inline uint32_t next_cluster(uint32_t cluster) {
    if (!is_valid_cluster(cluster)) return FAT_FREE;
    return g_fat[cluster];
}

bool validate_directory_and_chains() {
    static uint8_t owned[VFS_MAX_SECTORS];
    bytes_zero(owned, sizeof(owned));
    for (uint32_t i = 0; i < VFS_MAX_ENTRIES; ++i) {
        DirectoryEntry& e = g_dir[i];
        if (e.flags == ENTRY_FREE) continue;
        if (e.flags != ENTRY_FILE && e.flags != ENTRY_DIR) return false;
        size_t n = bounded_length(e.path, VFS_MAX_PATH);
        if (n == 0 || n >= VFS_MAX_PATH || e.path[0] != '/' || (n == 1 && e.path[0] == '/')) return false;
    }
    for (uint32_t i = 0; i < VFS_MAX_ENTRIES; ++i) {
        DirectoryEntry& e = g_dir[i];
        if (e.flags == ENTRY_FREE) continue;
        char canonical[VFS_MAX_PATH];
        if (!normalize_path(e.path, canonical) || !str_equal(canonical, e.path)) return false;
        for (uint32_t j = 0; j < i; ++j)
            if (g_dir[j].flags != ENTRY_FREE && str_equal(g_dir[j].path, e.path)) return false;
        if (!parent_exists(e.path)) return false;
        if (e.flags == ENTRY_DIR && (e.size != 0 || e.first_cluster != 0)) return false;
        if (e.flags == ENTRY_FILE) {
            if (e.size == 0 && e.first_cluster == 0) continue;
            if (!is_valid_cluster(e.first_cluster)) return false;
            uint32_t needed = e.size / VFS_SECTOR_SIZE + ((e.size % VFS_SECTOR_SIZE) != 0);
            uint32_t c = e.first_cluster, length = 0;
            while (true) {
                if (!is_valid_cluster(c) || g_fat[c] == FAT_FREE || owned[c]) return false;
                owned[c] = 1;
                ++length;
                if (length > g_sb.total_sectors - g_sb.data_start) return false;
                uint32_t next = g_fat[c];
                if (next == FAT_EOC) break;
                if (!is_valid_cluster(next)) return false;
                c = next;
            }
            if (length < needed) return false;
        }
    }
    return true;
}

bool mount_internal() {
    uint8_t sector[VFS_SECTOR_SIZE];
    bytes_zero(&g_sb, sizeof(g_sb));
    if (!Kernel::Drivers::ATADriver::read_sectors(VFS_START_LBA, 1, sector)) return false;
    bytes_copy(&g_sb, sector, sizeof(g_sb));
    if (checksum_fnv1a(reinterpret_cast<const uint8_t*>(&g_sb), 44) != g_sb.checksum) return false;
    if (!bytes_equal(g_sb.magic, FS_MAGIC, sizeof(FS_MAGIC)) || g_sb.version != FS_VERSION || g_sb.sector_size != VFS_SECTOR_SIZE) return false;
    if (g_sb.total_sectors < 2048 || g_sb.total_sectors > VFS_MAX_SECTORS) return false;
    const uint32_t expected_fat = (g_sb.total_sectors + FAT_ENTRIES_PER_SECTOR - 1) / FAT_ENTRIES_PER_SECTOR;
    const uint32_t expected_dir = (VFS_MAX_ENTRIES * DIR_ENTRY_SIZE) / VFS_SECTOR_SIZE;
    if (g_sb.fat_start != 1 || g_sb.fat_sectors != expected_fat || g_sb.dir_start != 1 + expected_fat ||
        g_sb.dir_sectors != expected_dir || g_sb.entry_count != VFS_MAX_ENTRIES ||
        g_sb.data_start != g_sb.dir_start + g_sb.dir_sectors || g_sb.data_start >= g_sb.total_sectors) return false;
    if (VFS_START_LBA > UINT32_MAX - g_sb.total_sectors) return false;

    bytes_zero(g_fat, sizeof(g_fat));
    bytes_zero(g_fat_dirty, sizeof(g_fat_dirty));
    for (uint32_t s = 0; s < g_sb.fat_sectors; ++s) {
        if (!Kernel::Drivers::ATADriver::read_sectors(VFS_START_LBA + g_sb.fat_start + s, 1,
                reinterpret_cast<uint8_t*>(g_fat + s * FAT_ENTRIES_PER_SECTOR))) return false;
    }
    for (uint32_t s = 0; s < g_sb.dir_sectors; ++s) {
        if (!Kernel::Drivers::ATADriver::read_sectors(VFS_START_LBA + g_sb.dir_start + s, 1,
                reinterpret_cast<uint8_t*>(g_dir + s * DIR_ENTRIES_PER_SECTOR))) return false;
    }
    for (uint32_t i = 0; i < g_sb.data_start; ++i) if (g_fat[i] == FAT_FREE) return false;
    if (!validate_directory_and_chains()) return false;
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; ++i) g_streams[i].used = false;
    g_next_free_cluster = g_sb.data_start;
    g_next_free_entry = 0;
    return true;
}

bool create_entry(const char* normalized, uint32_t flags, uint32_t* out_index) {
    if (!g_mounted || !normalized || str_equal(normalized, "/") || !parent_exists(normalized)) return false;
    if (find_entry_index(normalized) >= 0) return false;
    int free_idx = free_entry_index();
    if (free_idx < 0) return false;
    DirectoryEntry& e = g_dir[free_idx];
    bytes_zero(&e, sizeof(e));
    size_t n = str_length(normalized);
    if (n >= sizeof(e.path)) return false;
    bytes_copy(e.path, normalized, n + 1);
    e.flags = flags;
    if (!flush_dir_entry(static_cast<uint32_t>(free_idx))) { bytes_zero(&e, sizeof(e)); return false; }
    if (out_index) *out_index = static_cast<uint32_t>(free_idx);
    return true;
}

bool ensure_parent_directories(const char* normalized) {
    char parent[VFS_MAX_PATH];
    if (!parent_path(normalized, parent))
        return false;
    if (str_equal(parent, "/"))
        return true;
    if (find_entry_index(parent) >= 0) {
        int index = find_entry_index(parent);
        return g_dir[index].flags == ENTRY_DIR;
    }
    if (!ensure_parent_directories(parent))
        return false;
    return create_entry(parent, ENTRY_DIR, nullptr);
}

bool free_chain(uint32_t first) {
    uint32_t c = first;
    for (uint32_t steps = 0; c != FAT_EOC && c != FAT_FREE; ++steps) {
        if (steps >= g_sb.total_sectors - g_sb.data_start || !is_valid_cluster(c)) return false;
        uint32_t next = g_fat[c];
        g_fat[c] = FAT_FREE; mark_fat_dirty(c);
        if (c < g_next_free_cluster) g_next_free_cluster = c;
        if (next == FAT_EOC) break;
        if (!is_valid_cluster(next)) return false;
        c = next;
    }
    return flush_fat_dirty();
}

bool truncate_entry(uint32_t index) {
    if (index >= VFS_MAX_ENTRIES || g_dir[index].flags != ENTRY_FILE) return false;
    uint32_t old_first = g_dir[index].first_cluster;
    DirectoryEntry old = g_dir[index];
    g_dir[index].size = 0;
    g_dir[index].first_cluster = 0;
    if (!flush_dir_entry(index)) { g_dir[index] = old; return false; }
    if (old_first && !free_chain(old_first)) return false; 
    return true;
}

int allocate_one_cluster() {
    for (uint32_t count = 0, c = g_next_free_cluster; count < (g_sb.total_sectors - g_sb.data_start); ++count, c = (c + 1 >= g_sb.total_sectors) ? g_sb.data_start : c + 1) {
        if (g_fat[c] != FAT_FREE) continue;
        g_fat[c] = FAT_EOC; mark_fat_dirty(c);
        g_next_free_cluster = c + 1 >= g_sb.total_sectors ? g_sb.data_start : c + 1;
        return static_cast<int>(c);
    }
    return -1;
}

bool ensure_capacity(uint32_t index, uint32_t end_offset) {
    if (index >= VFS_MAX_ENTRIES || g_dir[index].flags != ENTRY_FILE) return false;
    uint32_t needed = end_offset / VFS_SECTOR_SIZE + ((end_offset % VFS_SECTOR_SIZE) != 0);
    if (needed == 0) return true;
    if (needed > g_sb.total_sectors - g_sb.data_start) return false;
    DirectoryEntry& e = g_dir[index];
    uint32_t existing = 0, last = 0;
    if (e.first_cluster) {
        uint32_t c = e.first_cluster;
        for (;;) {
            if (!is_valid_cluster(c) || g_fat[c] == FAT_FREE || ++existing > g_sb.total_sectors - g_sb.data_start) return false;
            last = c;
            uint32_t next = g_fat[c];
            if (next == FAT_EOC) break;
            if (!is_valid_cluster(next)) return false;
            c = next;
        }
    }
    if (existing >= needed) return true;
    const uint32_t old_last = last;
    uint32_t new_first = 0, new_last = 0;
    while (existing < needed) {
        int allocated = allocate_one_cluster();
        if (allocated < 0) {
            if (old_last) { g_fat[old_last] = FAT_EOC; mark_fat_dirty(old_last); }
            else e.first_cluster = 0;
            uint32_t c = new_first;
            while (c) {
                uint32_t next = g_fat[c];
                g_fat[c] = FAT_FREE; mark_fat_dirty(c);
                if (c < g_next_free_cluster) g_next_free_cluster = c;
                if (next == FAT_EOC || !is_valid_cluster(next)) break;
                c = next;
            }
            (void)flush_fat_dirty();
            return false;
        }
        uint32_t c = static_cast<uint32_t>(allocated);
        if (!new_first) new_first = c;
        if (new_last) { g_fat[new_last] = c; mark_fat_dirty(new_last); }
        else if (old_last) { g_fat[old_last] = c; mark_fat_dirty(old_last); }
        new_last = c;
        ++existing;
    }
    if (!old_last) e.first_cluster = new_first;
    if (!flush_fat_dirty()) return false;
    if (!old_last && !flush_dir_entry(index)) return false;
    return true;
}

int raw_read(uint32_t index, void* buffer, uint32_t length, uint32_t offset) {
    if (!buffer || length == 0 || index >= VFS_MAX_ENTRIES || g_dir[index].flags != ENTRY_FILE) return 0;
    DirectoryEntry& e = g_dir[index];
    if (offset >= e.size || !e.first_cluster) return 0;
    if (length > e.size - offset) length = e.size - offset;
    uint32_t c = e.first_cluster;
    uint32_t skip = offset / VFS_SECTOR_SIZE;
    for (uint32_t i = 0; i < skip; ++i) {
        uint32_t n = next_cluster(c);
        if (!is_valid_cluster(n)) return 0;
        c = n;
    }
    uint32_t byte_in_sector = offset % VFS_SECTOR_SIZE;
    uint32_t done = 0;
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    while (done < length && is_valid_cluster(c)) {
        uint32_t amount = VFS_SECTOR_SIZE - byte_in_sector;
        if (amount > length - done) amount = length - done;
        if (byte_in_sector == 0 && amount == VFS_SECTOR_SIZE) {
            if (!read_sector(c, dst + done)) break;
        } else {
            uint8_t sector[VFS_SECTOR_SIZE];
            if (!read_sector(c, sector)) break;
            bytes_copy(dst + done, sector + byte_in_sector, amount);
        }
        done += amount;
        byte_in_sector = 0;
        if (done < length) {
            uint32_t n = next_cluster(c);
            if (!is_valid_cluster(n)) break;
            c = n;
        }
    }
    return static_cast<int>(done);
}

int raw_write(uint32_t index, const void* buffer, uint32_t length, uint32_t offset) {
    if (!buffer || length == 0 || index >= VFS_MAX_ENTRIES || g_dir[index].flags != ENTRY_FILE) return 0;
    DirectoryEntry& e = g_dir[index];
    if (!e.first_cluster) return 0;
    uint32_t c = e.first_cluster;
    uint32_t skip = offset / VFS_SECTOR_SIZE;
    for (uint32_t i = 0; i < skip; ++i) {
        uint32_t n = next_cluster(c);
        if (!is_valid_cluster(n)) return 0;
        c = n;
    }
    uint32_t byte_in_sector = offset % VFS_SECTOR_SIZE;
    uint32_t done = 0;
    const uint8_t* src = static_cast<const uint8_t*>(buffer);
    while (done < length && is_valid_cluster(c)) {
        uint32_t amount = VFS_SECTOR_SIZE - byte_in_sector;
        if (amount > length - done) amount = length - done;
        if (byte_in_sector == 0 && amount == VFS_SECTOR_SIZE) {
            if (!write_sector(c, src + done)) break;
        } else {
            uint8_t sector[VFS_SECTOR_SIZE];
            if (!read_sector(c, sector)) break;
            bytes_copy(sector + byte_in_sector, src + done, amount);
            if (!write_sector(c, sector)) break;
        }
        done += amount;
        byte_in_sector = 0;
        if (done < length) {
            uint32_t n = next_cluster(c);
            if (!is_valid_cluster(n)) break;
            c = n;
        }
    }
    return static_cast<int>(done);
}

int write_entry(uint32_t index, const void* buffer, uint32_t length, uint32_t offset) {
    if (index >= VFS_MAX_ENTRIES || g_dir[index].flags != ENTRY_FILE || !buffer || length == 0) return 0;
    if (length > UINT32_MAX - offset) return 0;
    uint32_t end = offset + length;
    uint32_t old_size = g_dir[index].size;
    if (!ensure_capacity(index, end)) return 0;
    if (offset > old_size) {
        uint8_t zeros[VFS_SECTOR_SIZE]{};
        uint32_t gap = offset - old_size, pos = old_size;
        while (gap) {
            uint32_t chunk = gap > VFS_SECTOR_SIZE ? VFS_SECTOR_SIZE : gap;
            int z = raw_write(index, zeros, chunk, pos);
            if (z != static_cast<int>(chunk)) return 0;
            pos += chunk; gap -= chunk;
        }
    }
    int written = raw_write(index, buffer, length, offset);
    if (written <= 0) return 0;
    uint32_t new_end = offset + static_cast<uint32_t>(written);
    if (new_end > g_dir[index].size) {
        uint32_t previous = g_dir[index].size;
        g_dir[index].size = new_end;
        if (!flush_dir_entry(index)) { g_dir[index].size = previous; return 0; }
    }
    return written;
}

FileStream* acquire_stream() {
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; ++i)
        if (!g_streams[i].used) { bytes_zero(&g_streams[i], sizeof(g_streams[i])); g_streams[i].used = true; return &g_streams[i]; }
    return nullptr;
}
FileStream* validate_stream(void* handle) {
    if (!handle) return nullptr;
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; ++i)
        if (handle == &g_streams[i] && g_streams[i].used) return &g_streams[i];
    return nullptr;
}
bool parse_mode(const char* mode, bool& readable, bool& writable, bool& append, bool& truncate) {
    readable = writable = append = truncate = false;
    if (!mode || !mode[0]) return false;
    char first = mode[0];
    if (first == 'r') { readable = true; }
    else if (first == 'w') { writable = true; truncate = true; }
    else if (first == 'a') { writable = true; append = true; }
    else return false;
    bool plus_seen = false, binary_seen = false;
    for (size_t i = 1; mode[i]; ++i) {
        if (mode[i] == '+' && !plus_seen) { plus_seen = true; readable = writable = true; }
        else if (mode[i] == 'b' && !binary_seen) binary_seen = true;
        else return false;
    }
    return true;
}
} 

void VFS::init() {
    Kernel::Drivers::ATADriver::init();
    g_mounted = mount_internal();
}
bool VFS::mount() { g_mounted = mount_internal(); return g_mounted; }
bool VFS::is_mounted() { return g_mounted; }

bool VFS::format(uint32_t total_sectors) {
    if (total_sectors < 2048 || total_sectors > VFS_MAX_SECTORS || VFS_START_LBA > UINT32_MAX - total_sectors) return false;
    bytes_zero(&g_sb, sizeof(g_sb));
    bytes_copy(g_sb.magic, FS_MAGIC, sizeof(FS_MAGIC));
    g_sb.version = FS_VERSION;
    g_sb.sector_size = VFS_SECTOR_SIZE;
    g_sb.total_sectors = total_sectors;
    g_sb.fat_start = 1;
    g_sb.fat_sectors = (total_sectors + FAT_ENTRIES_PER_SECTOR - 1) / FAT_ENTRIES_PER_SECTOR;
    g_sb.dir_start = g_sb.fat_start + g_sb.fat_sectors;
    g_sb.dir_sectors = DIR_SECTORS;
    g_sb.data_start = g_sb.dir_start + g_sb.dir_sectors;
    g_sb.entry_count = VFS_MAX_ENTRIES;
    if (g_sb.data_start + 1 >= total_sectors) return false;
    g_sb.checksum = checksum_fnv1a(reinterpret_cast<const uint8_t*>(&g_sb), 44);
    bytes_zero(g_fat, sizeof(g_fat));
    bytes_zero(g_dir, sizeof(g_dir));
    bytes_zero(g_fat_dirty, sizeof(g_fat_dirty));
    for (uint32_t i = 0; i < g_sb.data_start; ++i) g_fat[i] = FAT_EOC;
    for (uint32_t i = 0; i < g_sb.fat_sectors; ++i) g_fat_dirty[i] = true;
    if (!flush_fat_dirty()) { g_mounted = false; return false; }
    for (uint32_t s = 0; s < g_sb.dir_sectors; ++s) {
        if (!write_dir_sector(s)) { g_mounted = false; return false; }
    }
    uint8_t sector[VFS_SECTOR_SIZE];
    bytes_copy(sector, &g_sb, sizeof(g_sb));
    if (!Kernel::Drivers::ATADriver::write_sectors(VFS_START_LBA, 1, sector)) { g_mounted = false; return false; }
    g_mounted = true;
    for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; ++i) g_streams[i].used = false;
    g_next_free_cluster = g_sb.data_start;
    g_next_free_entry = 0;
    return true;
}

bool VFS::find_file(const char* filename, VFSFile* out_file) {
    if (!out_file) return false;
    bytes_zero(out_file, sizeof(*out_file));
    if (!g_mounted) return false;
    char normalized[VFS_MAX_PATH];
    if (!normalize_path(filename, normalized)) return false;
    int idx = find_entry_index(normalized);
    if (idx < 0) return false;
    return fill_public_file(static_cast<uint32_t>(idx), out_file);
}
int VFS::read_file(const VFSFile* file, void* buffer, uint32_t length, uint32_t offset) {
    if (!g_mounted || !file || !file->exists || file->is_directory || file->entry_index >= VFS_MAX_ENTRIES ||
        g_dir[file->entry_index].flags != ENTRY_FILE) return 0;
    return raw_read(file->entry_index, buffer, length, offset);
}

void* VFS::vfs_open(const char* path, const char* mode) {
    if (!g_mounted) return nullptr;
    bool readable, writable, append, truncate;
    if (!parse_mode(mode, readable, writable, append, truncate)) return nullptr;
    char normalized[VFS_MAX_PATH];
    if (!normalize_path(path, normalized) || str_equal(normalized, "/")) return nullptr;
    FileStream* stream = acquire_stream();
    if (!stream) return nullptr;
    int idx = find_entry_index(normalized);
    if (idx < 0) {
        uint32_t created_index = 0;
        if (!writable ||
            !ensure_parent_directories(normalized) ||
            !create_entry(normalized, ENTRY_FILE, &created_index)) {
            stream->used = false;
            return nullptr;
        }
        idx = static_cast<int>(created_index);
    } else if (g_dir[idx].flags != ENTRY_FILE) {
        stream->used = false; return nullptr;
    }
    uint32_t index = static_cast<uint32_t>(idx);
    if (truncate && !truncate_entry(index)) { stream->used = false; return nullptr; }
    stream->entry_index = index;
    stream->readable = readable;
    stream->writable = writable;
    stream->append = append;
    stream->offset = append ? g_dir[index].size : 0;
    return stream;
}
void* VFS::vfs_open(const char* path) { return vfs_open(path, "r+"); }
size_t VFS::vfs_read(void* buffer, size_t size, size_t count, void* handle) {
    FileStream* s = validate_stream(handle);
    if (!s || !s->readable || !buffer || size == 0 || count == 0 || count > UINT32_MAX / size) return 0;
    uint32_t bytes = static_cast<uint32_t>(size * count);
    int got = raw_read(s->entry_index, buffer, bytes, s->offset);
    if (got <= 0) return 0;
    s->offset += static_cast<uint32_t>(got);
    return static_cast<size_t>(got) / size;
}
size_t VFS::vfs_write(const void* buffer, size_t size, size_t count, void* handle) {
    FileStream* s = validate_stream(handle);
    if (!s || !s->writable || !buffer || size == 0 || count == 0 || count > UINT32_MAX / size) return 0;
    uint32_t bytes = static_cast<uint32_t>(size * count);
    if (s->append) s->offset = g_dir[s->entry_index].size;
    int put = write_entry(s->entry_index, buffer, bytes, s->offset);
    if (put <= 0) return 0;
    s->offset += static_cast<uint32_t>(put);
    return static_cast<size_t>(put) / size;
}
int VFS::vfs_close(void* handle) {
    FileStream* s = validate_stream(handle);
    if (!s) return -1;
    bool ok = flush_fat_dirty();
    s->used = false;
    return ok ? 0 : -1;
}
long VFS::vfs_tell(void* handle) {
    FileStream* s = validate_stream(handle);
    return s ? static_cast<long>(s->offset) : -1;
}
int VFS::vfs_seek(void* handle, long offset, int whence) {
    FileStream* s = validate_stream(handle);
    if (!s) return -1;
    int64_t base;
    if (whence == 0) base = 0;
    else if (whence == 1) base = s->offset;
    else if (whence == 2) base = g_dir[s->entry_index].size;
    else return -1;
    int64_t delta = static_cast<int64_t>(offset);
    if (delta > static_cast<int64_t>(UINT32_MAX) || delta < -static_cast<int64_t>(UINT32_MAX)) return -1;
    int64_t target = base + delta;
    if (target < 0 || target > static_cast<int64_t>(UINT32_MAX)) return -1;
    s->offset = static_cast<uint32_t>(target);
    return 0;
}
size_t VFS::vfs_size(void* handle) {
    FileStream* s = validate_stream(handle);
    return s ? g_dir[s->entry_index].size : 0;
}
int VFS::vfs_mkdir(const char* pathname, int mode) {
    (void)mode;
    if (!g_mounted) return -1;
    char normalized[VFS_MAX_PATH];
    if (!normalize_path(pathname, normalized) || str_equal(normalized, "/")) return -1;
    int existing = find_entry_index(normalized);
    if (existing >= 0) return g_dir[existing].flags == ENTRY_DIR ? 0 : -1;
    return create_entry(normalized, ENTRY_DIR, nullptr) ? 0 : -1;
}
int VFS::vfs_remove(const char* pathname) {
    if (!g_mounted) return -1;
    char normalized[VFS_MAX_PATH];
    if (!normalize_path(pathname, normalized) || str_equal(normalized, "/")) return -1;
    int idx = find_entry_index(normalized);
    if (idx < 0) return -1;
    DirectoryEntry old = g_dir[idx];
    if (old.flags == ENTRY_DIR && has_children(old.path)) return -1;
    bytes_zero(&g_dir[idx], sizeof(g_dir[idx]));
    if (!flush_dir_entry(static_cast<uint32_t>(idx))) { g_dir[idx] = old; return -1; }
    if (old.flags == ENTRY_FILE && old.first_cluster && !free_chain(old.first_cluster)) return -1;
    return 0;
}
int VFS::vfs_rename(const char* oldpath, const char* newpath) {
    if (!g_mounted) return -1;
    char oldname[VFS_MAX_PATH], newname[VFS_MAX_PATH];
    if (!normalize_path(oldpath, oldname) || !normalize_path(newpath, newname) ||
        str_equal(oldname, "/") || str_equal(newname, "/")) return -1;
    int src = find_entry_index(oldname);
    if (src < 0 || find_entry_index(newname) >= 0 || !parent_exists(newname)) return -1;
    if (g_dir[src].flags == ENTRY_DIR && has_children(oldname)) return -1;
    DirectoryEntry old = g_dir[src];
    bytes_zero(g_dir[src].path, sizeof(g_dir[src].path));
    size_t n = str_length(newname);
    if (n >= sizeof(g_dir[src].path)) { g_dir[src] = old; return -1; }
    bytes_copy(g_dir[src].path, newname, n + 1);
    if (!flush_dir_entry(static_cast<uint32_t>(src))) { g_dir[src] = old; return -1; }
    return 0;
}
bool VFS::vfs_exists(const char* path) {
    if (!g_mounted) return false;
    char normalized[VFS_MAX_PATH];
    return normalize_path(path, normalized) && find_entry_index(normalized) >= 0;
}
bool VFS::get_file_at(uint32_t index, VFSFile* out_file) {
    if (index >= VFS_MAX_ENTRIES || !g_mounted) return false;
    return fill_public_file(index, out_file);
}
} 

extern "C" {
void* kernel_vfs_open(const char* path, const char* mode) { return Kernel::FS::VFS::vfs_open(path, mode); }
size_t kernel_vfs_read(void* ptr, size_t size, size_t nmemb, void* stream) { return Kernel::FS::VFS::vfs_read(ptr, size, nmemb, stream); }
size_t kernel_vfs_write(const void* ptr, size_t size, size_t nmemb, void* stream) { return Kernel::FS::VFS::vfs_write(ptr, size, nmemb, stream); }
int kernel_vfs_close(void* stream) { return Kernel::FS::VFS::vfs_close(stream); }
long kernel_vfs_tell(void* stream) { return Kernel::FS::VFS::vfs_tell(stream); }
int kernel_vfs_seek(void* stream, long offset, int whence) { return Kernel::FS::VFS::vfs_seek(stream, offset, whence); }
size_t kernel_vfs_size(void* stream) { return Kernel::FS::VFS::vfs_size(stream); }
int kernel_vfs_mkdir(const char* path, int mode) { return Kernel::FS::VFS::vfs_mkdir(path, mode); }
int kernel_vfs_remove(const char* path) { return Kernel::FS::VFS::vfs_remove(path); }
int kernel_vfs_rename(const char* oldpath, const char* newpath) { return Kernel::FS::VFS::vfs_rename(oldpath, newpath); }
bool kernel_vfs_exists(const char* path) { return Kernel::FS::VFS::vfs_exists(path); }
}