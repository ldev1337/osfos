#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <kernel/drivers/ata.h>
#include <kernel/fs/laziofs.h>
#include <utility>

// ═══════════════════════════════════════════════════════════════════════════════
// Ext-like LazioFS v3 – Extent & Inode Based Lazy-IO Optimized Filesystem
// ═══════════════════════════════════════════════════════════════════════════════

namespace Kernel::FS {
namespace {

constexpr uint32_t FS_VERSION = 2;
constexpr uint32_t INODE_FREE = 0;
constexpr uint32_t INODE_FILE = 1;
constexpr uint32_t INODE_DIR = 2;
constexpr char FS_MAGIC[8] = {'L', 'A', 'Z', 'I', 'O', 'E', 'X', 'T'};

#pragma pack(push, 1)
struct Superblock {
  char magic[8];
  uint32_t version;
  uint32_t sector_size;
  uint32_t total_sectors;
  uint32_t inode_bitmap_start;
  uint32_t inode_bitmap_sectors;
  uint32_t block_bitmap_start;
  uint32_t block_bitmap_sectors;
  uint32_t inode_table_start;
  uint32_t inode_table_sectors;
  uint32_t data_start;
  uint32_t total_inodes;
  uint32_t checksum;
  uint8_t reserved[VFS_SECTOR_SIZE - 8 - (12 * sizeof(uint32_t))];
};

struct Extent {
  uint32_t start_block;
  uint32_t count;
};

constexpr uint32_t MAX_EXTENTS_PER_INODE = 14;

struct Inode {
  uint32_t flags;
  uint32_t size;
  uint32_t links_count;
  uint32_t extents_count;
  Extent extents[MAX_EXTENTS_PER_INODE];
  uint8_t padding[128 - (4 * sizeof(uint32_t)) -
                  (MAX_EXTENTS_PER_INODE * sizeof(Extent))];
};

struct DiskDirEntry {
  uint32_t inode_index;
  uint16_t rec_len;
  uint8_t name_len;
  uint8_t file_type;
  char name[VFS_MAX_PATH];
};
#pragma pack(pop)

static_assert(sizeof(Superblock) == VFS_SECTOR_SIZE,
              "Superblock must be 1 sector");
static_assert(sizeof(Inode) == 128, "Inode must be 128 bytes");

struct FileStream {
  bool used;
  bool readable;
  bool writable;
  bool append;
  uint32_t inode_index;
  uint32_t offset;
  uint32_t cursor_extent_idx;
  uint32_t cursor_block;
  uint32_t cursor_byte_base;
};

// ── Sector Cache (Write-back, LRU) ──────────────────────────────────────────
constexpr uint32_t CACHE_SLOTS = VFS_CACHE_SLOTS;
constexpr uint32_t CACHE_BUCKETS = CACHE_SLOTS * 2;
constexpr uint32_t CACHE_INVALID_LBA = 0xFFFFFFFFU;

struct CacheSlot {
  uint32_t abs_lba;
  uint32_t age;
  int32_t hash_prev;
  int32_t hash_next;
  bool dirty;
  uint8_t data[VFS_SECTOR_SIZE];
};

CacheSlot g_cache[CACHE_SLOTS]{};
int32_t g_cache_buckets[CACHE_BUCKETS]{};
uint32_t g_cache_tick = 0;

// ── Global State ────────────────────────────────────────────────────────────
Superblock g_sb{};
Inode g_inodes[VFS_MAX_INODES]{};
constexpr uint32_t INODE_BITMAP_SECTORS = (VFS_MAX_INODES + 4095) / 4096;
uint8_t g_inode_bitmap[INODE_BITMAP_SECTORS * VFS_SECTOR_SIZE]{};
bool g_inode_bitmap_dirty = false;
bool g_inode_dirty[VFS_MAX_INODES]{};
bool g_mounted = false;
uint32_t g_next_free_inode = 0;
uint32_t g_next_free_block = 0;
FileStream g_streams[VFS_MAX_OPEN_FILES]{};

// Dynamic block bitmap representation in RAM
constexpr uint32_t MAX_BLOCKS = VFS_MAX_SECTORS;
constexpr uint32_t BLOCK_BITMAP_SECTORS_MAX = (MAX_BLOCKS + 4095) / 4096;
constexpr uint32_t BLOCK_BITMAP_BYTES = BLOCK_BITMAP_SECTORS_MAX * VFS_SECTOR_SIZE;
uint8_t g_block_bitmap[BLOCK_BITMAP_BYTES]{};
bool g_block_bitmap_dirty = false;

// ── Utility Helpers ─────────────────────────────────────────────────────────
inline void bytes_zero(void *ptr, size_t n) {
  uint8_t *p = static_cast<uint8_t *>(ptr);
  for (size_t i = 0; i < n; ++i)
    p[i] = 0;
}
inline void bytes_copy(void *dst, const void *src, size_t n) {
  uint8_t *d = static_cast<uint8_t *>(dst);
  const uint8_t *s = static_cast<const uint8_t *>(src);
  for (size_t i = 0; i < n; ++i)
    d[i] = s[i];
}
inline bool str_equal(const char *a, const char *b) {
  while (*a && *b) {
    if (*a++ != *b++)
      return false;
  }
  return *a == *b;
}
inline size_t str_length(const char *s) {
  size_t n = 0;
  while (s[n])
    ++n;
  return n;
}
inline char ascii_lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}
inline uint32_t hash_fnv1a(const uint8_t *data, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

// ── Cache Implementation ────────────────────────────────────────────────────
void cache_init() {
  for (uint32_t i = 0; i < CACHE_SLOTS; ++i) {
    g_cache[i].abs_lba = CACHE_INVALID_LBA;
    g_cache[i].age = 0;
    g_cache[i].hash_prev = -1;
    g_cache[i].hash_next = -1;
    g_cache[i].dirty = false;
  }
  for (uint32_t i = 0; i < CACHE_BUCKETS; ++i)
    g_cache_buckets[i] = -1;
  g_cache_tick = 0;
}

bool cache_flush_slot(CacheSlot &slot) {
  if (!slot.dirty || slot.abs_lba == CACHE_INVALID_LBA)
    return true;
  if (!Kernel::Drivers::ATADriver::write_sectors(slot.abs_lba, 1, slot.data))
    return false;
  slot.dirty = false;
  return true;
}

bool cache_flush_all() {
  bool ok = true;
  for (uint32_t i = 0; i < CACHE_SLOTS; ++i) {
    if (!cache_flush_slot(g_cache[i]))
      ok = false;
  }
  return ok;
}

uint32_t cache_bucket(uint32_t abs_lba) {
  return (abs_lba * 2654435761U) % CACHE_BUCKETS;
}

void cache_remove(CacheSlot &slot) {
  if (slot.abs_lba == CACHE_INVALID_LBA)
    return;
  uint32_t bucket = cache_bucket(slot.abs_lba);
  if (slot.hash_prev >= 0)
    g_cache[slot.hash_prev].hash_next = slot.hash_next;
  else
    g_cache_buckets[bucket] = slot.hash_next;
  if (slot.hash_next >= 0)
    g_cache[slot.hash_next].hash_prev = slot.hash_prev;
  slot.hash_prev = -1;
  slot.hash_next = -1;
}

void cache_insert(CacheSlot &slot) {
  uint32_t bucket = cache_bucket(slot.abs_lba);
  uint32_t slot_index = static_cast<uint32_t>(&slot - g_cache);
  int32_t old_head = g_cache_buckets[bucket];
  slot.hash_prev = -1;
  slot.hash_next = old_head;
  if (old_head >= 0)
    g_cache[old_head].hash_prev = static_cast<int32_t>(slot_index);
  g_cache_buckets[bucket] = static_cast<int32_t>(slot_index);
}

CacheSlot *cache_find(uint32_t abs_lba) {
  uint32_t bucket = cache_bucket(abs_lba);
  for (int32_t i = g_cache_buckets[bucket]; i >= 0;
       i = g_cache[i].hash_next) {
    if (g_cache[i].abs_lba == abs_lba) {
      g_cache[i].age = ++g_cache_tick;
      return &g_cache[i];
    }
  }
  return nullptr;
}

CacheSlot *cache_evict() {
  uint32_t best = 0;
  for (uint32_t i = 1; i < CACHE_SLOTS; ++i) {
    if (g_cache[i].age < g_cache[best].age)
      best = i;
  }
  if (!cache_flush_slot(g_cache[best]))
    return nullptr;
  cache_remove(g_cache[best]);
  g_cache[best].abs_lba = CACHE_INVALID_LBA;
  return &g_cache[best];
}

bool cached_read_sector(uint32_t rel_lba, uint8_t *out) {
  if (!out || rel_lba >= g_sb.total_sectors)
    return false;
  uint32_t abs = VFS_START_LBA + rel_lba;
  CacheSlot *slot = cache_find(abs);
  if (slot) {
    bytes_copy(out, slot->data, VFS_SECTOR_SIZE);
    return true;
  }
  slot = cache_evict();
  if (!slot || !Kernel::Drivers::ATADriver::read_sectors(abs, 1, slot->data))
    return false;
  slot->abs_lba = abs;
  slot->age = ++g_cache_tick;
  slot->dirty = false;
  cache_insert(*slot);
  bytes_copy(out, slot->data, VFS_SECTOR_SIZE);
  return true;
}

bool cached_write_sector(uint32_t rel_lba, const uint8_t *data) {
  if (!data || rel_lba >= g_sb.total_sectors)
    return false;
  uint32_t abs = VFS_START_LBA + rel_lba;
  CacheSlot *slot = cache_find(abs);
  if (!slot) {
    slot = cache_evict();
    if (!slot)
      return false;
    slot->abs_lba = abs;
    cache_insert(*slot);
  }
  bytes_copy(slot->data, data, VFS_SECTOR_SIZE);
  slot->age = ++g_cache_tick;
  slot->dirty = true;
  return true;
}

uint8_t *cached_read_sector_ptr(uint32_t rel_lba) {
  if (rel_lba >= g_sb.total_sectors)
    return nullptr;
  uint32_t abs = VFS_START_LBA + rel_lba;
  CacheSlot *slot = cache_find(abs);
  if (slot)
    return slot->data;
  slot = cache_evict();
  if (!slot || !Kernel::Drivers::ATADriver::read_sectors(abs, 1, slot->data))
    return nullptr;
  slot->abs_lba = abs;
  slot->age = ++g_cache_tick;
  slot->dirty = false;
  cache_insert(*slot);
  return slot->data;
}

void cached_mark_dirty(uint32_t rel_lba) {
  uint32_t abs = VFS_START_LBA + rel_lba;
  CacheSlot *slot = cache_find(abs);
  if (slot)
    slot->dirty = true;
}

// ── Bitmaps & Inodes Persistence ────────────────────────────────────────────
bool flush_bitmaps_and_inodes() {
  if (g_inode_bitmap_dirty) {
    uint32_t sectors = (VFS_MAX_INODES + 4095) / 4096;
    for (uint32_t s = 0; s < sectors; ++s) {
      if (!cached_write_sector(g_sb.inode_bitmap_start + s,
                               g_inode_bitmap + s * VFS_SECTOR_SIZE))
        return false;
    }
    g_inode_bitmap_dirty = false;
  }
  if (g_block_bitmap_dirty) {
    uint32_t sectors = (g_sb.total_sectors + 4095) / 4096;
    for (uint32_t s = 0; s < sectors; ++s) {
      if (!cached_write_sector(g_sb.block_bitmap_start + s,
                               g_block_bitmap + s * VFS_SECTOR_SIZE))
        return false;
    }
    g_block_bitmap_dirty = false;
  }
  for (uint32_t i = 0; i < VFS_MAX_INODES; ++i) {
    if (!g_inode_dirty[i])
      continue;
    uint32_t table_sec =
        g_sb.inode_table_start + (i * sizeof(Inode)) / VFS_SECTOR_SIZE;
    uint8_t *sec_ptr = cached_read_sector_ptr(table_sec);
    if (!sec_ptr)
      return false;
    uint32_t offset = (i * sizeof(Inode)) % VFS_SECTOR_SIZE;
    bytes_copy(sec_ptr + offset, &g_inodes[i], sizeof(Inode));
    cached_mark_dirty(table_sec);
    g_inode_dirty[i] = false;
  }
  return true;
}

// ── Path Normalization ──────────────────────────────────────────────────────
bool normalize_path(const char *input, char out[VFS_MAX_PATH]) {
  if (!input || !out)
    return false;
  size_t out_len = 0;
  out[out_len++] = '/';
  while (*input == '/' || *input == '\\')
    ++input;
  while (*input) {
    char comp[VFS_MAX_PATH];
    size_t n = 0;
    while (*input && *input != '/' && *input != '\\') {
      if (n + 1 >= sizeof(comp))
        return false;
      comp[n++] = ascii_lower(*input++);
    }
    while (*input == '/' || *input == '\\')
      ++input;
    comp[n] = '\0';
    if (n == 0 || (n == 1 && comp[0] == '.'))
      continue;
    if (n == 2 && comp[0] == '.' && comp[1] == '.') {
      if (out_len == 1)
        return false;
      while (out_len > 1 && out[out_len - 1] != '/')
        --out_len;
      if (out_len > 1)
        --out_len;
      out[out_len] = '\0';
      continue;
    }
    if (out_len > 1) {
      if (out_len + 1 + n >= VFS_MAX_PATH)
        return false;
      out[out_len++] = '/';
    } else if (out_len + n >= VFS_MAX_PATH)
      return false;
    for (size_t i = 0; i < n; ++i)
      out[out_len++] = comp[i];
    out[out_len] = '\0';
  }
  out[out_len] = '\0';
  return true;
}

// ── Directory & Inode Lookup ────────────────────────────────────────────────
int lookup_inode_in_dir(uint32_t dir_inode_idx, const char *name) {
  Inode &dir = g_inodes[dir_inode_idx];
  if (dir.flags != INODE_DIR)
    return -1;

  uint8_t sector_buf[VFS_SECTOR_SIZE];

  for (uint32_t e = 0; e < dir.extents_count; ++e) {
    uint32_t block = dir.extents[e].start_block;
    for (uint32_t c = 0; c < dir.extents[e].count; ++c) {
      if (!cached_read_sector(block + c, sector_buf))
        continue;
      uint32_t offset = 0;
      while (offset < VFS_SECTOR_SIZE) {
        DiskDirEntry *de =
            reinterpret_cast<DiskDirEntry *>(sector_buf + offset);
        if (de->rec_len == 0 || offset + de->rec_len > VFS_SECTOR_SIZE)
          break;
        if (de->inode_index != 0 && str_equal(de->name, name)) {
          return static_cast<int>(de->inode_index);
        }
        offset += de->rec_len;
      }
    }
  }
  return -1;
}

int find_inode_by_path(const char *normalized_path) {
  if (str_equal(normalized_path, "/"))
    return 0; // Root inode is always 0

  uint32_t current_ino = 0;
  char component[VFS_MAX_PATH];
  const char *p = normalized_path + 1; // skip leading '/'

  while (*p) {
    size_t i = 0;
    while (*p && *p != '/') {
      if (i + 1 < VFS_MAX_PATH)
        component[i++] = *p;
      ++p;
    }
    component[i] = '\0';
    if (*p == '/')
      ++p;

    int next_ino = lookup_inode_in_dir(current_ino, component);
    if (next_ino < 0)
      return -1;
    current_ino = static_cast<uint32_t>(next_ino);
  }
  return static_cast<int>(current_ino);
}

bool build_inode_path(uint32_t inode_index, char out_path[VFS_MAX_PATH]) {
  if (inode_index == 0) {
    out_path[0] = '/';
    out_path[1] = '\0';
    return true;
  }

  char path[VFS_MAX_PATH]{};
  size_t position = VFS_MAX_PATH - 1;
  uint32_t current_inode = inode_index;

  for (uint32_t depth = 0; depth < VFS_MAX_INODES && current_inode != 0;
       ++depth) {
    bool found_parent = false;
    for (uint32_t parent_index = 0;
         parent_index < VFS_MAX_INODES && !found_parent; ++parent_index) {
      Inode &parent = g_inodes[parent_index];
      if (parent.flags != INODE_DIR)
        continue;

      for (uint32_t extent_index = 0;
           extent_index < parent.extents_count && !found_parent;
           ++extent_index) {
        for (uint32_t block_offset = 0;
             block_offset < parent.extents[extent_index].count &&
             !found_parent;
             ++block_offset) {
          uint8_t sector[VFS_SECTOR_SIZE];
          uint32_t block = parent.extents[extent_index].start_block +
                           block_offset;
          if (!cached_read_sector(block, sector))
            continue;

          uint32_t offset = 0;
          while (offset + 8 <= VFS_SECTOR_SIZE) {
            DiskDirEntry *entry =
                reinterpret_cast<DiskDirEntry *>(sector + offset);
            if (entry->rec_len < 8 ||
                offset + entry->rec_len > VFS_SECTOR_SIZE ||
                entry->name_len > entry->rec_len - 8)
              break;
            if (entry->inode_index == current_inode && entry->name_len > 0) {
              size_t name_length = entry->name_len;
              if (position < name_length + 1)
                return false;
              position -= name_length;
              bytes_copy(path + position, entry->name, name_length);
              path[--position] = '/';
              current_inode = parent_index;
              found_parent = true;
              break;
            }
            offset += entry->rec_len;
          }
        }
      }
    }
    if (!found_parent)
      return false;
  }

  if (current_inode != 0)
    return false;
  size_t path_length = VFS_MAX_PATH - 1 - position;
  bytes_copy(out_path, path + position, path_length);
  out_path[path_length] = '\0';
  return true;
}

// ── Block Allocation ────────────────────────────────────────────────────────
int allocate_block() {
  for (uint32_t b = g_next_free_block; b < g_sb.total_sectors; ++b) {
    if ((g_block_bitmap[b / 8] & (1 << (b % 8))) == 0) {
      g_block_bitmap[b / 8] |= (1 << (b % 8));
      g_block_bitmap_dirty = true;
      g_next_free_block = b + 1;
      return static_cast<int>(b);
    }
  }
  return -1;
}

void free_block_bitmap(uint32_t block) {
  if (block < g_sb.total_sectors) {
    g_block_bitmap[block / 8] &= ~(1 << (block % 8));
    g_block_bitmap_dirty = true;
    if (block < g_next_free_block)
      g_next_free_block = block;
  }
}

int allocate_inode() {
  for (uint32_t i = g_next_free_inode; i < VFS_MAX_INODES; ++i) {
    if ((g_inode_bitmap[i / 8] & (1 << (i % 8))) == 0) {
      g_inode_bitmap[i / 8] |= (1 << (i % 8));
      g_inode_bitmap_dirty = true;
      g_next_free_inode = i + 1;
      return static_cast<int>(i);
    }
  }
  return -1;
}

void free_inode_bitmap(uint32_t ino) {
  if (ino < VFS_MAX_INODES) {
    g_inode_bitmap[ino / 8] &= ~(1 << (ino % 8));
    g_inode_bitmap_dirty = true;
    if (ino < g_next_free_inode)
      g_next_free_inode = ino;
  }
}

// ── Mount Validation ────────────────────────────────────────────────────────
bool mount_internal() {
  g_mounted = false;
  g_inode_bitmap_dirty = false;
  g_block_bitmap_dirty = false;
  bytes_zero(g_inode_dirty, sizeof(g_inode_dirty));
  bytes_zero(g_inode_bitmap, sizeof(g_inode_bitmap));
  bytes_zero(g_block_bitmap, sizeof(g_block_bitmap));

  uint8_t sector[VFS_SECTOR_SIZE];
  if (!Kernel::Drivers::ATADriver::read_sectors(VFS_START_LBA, 1, sector))
    return false;
  bytes_copy(&g_sb, sector, sizeof(g_sb));

  constexpr uint32_t expected_inode_table_sectors =
      (VFS_MAX_INODES * sizeof(Inode) + VFS_SECTOR_SIZE - 1) / VFS_SECTOR_SIZE;
  const uint32_t expected_block_bitmap_sectors =
      (g_sb.total_sectors + 4095) / 4096;
  for (size_t i = 0; i < sizeof(FS_MAGIC); ++i) {
    if (g_sb.magic[i] != FS_MAGIC[i])
      return false;
  }
  if (g_sb.version != FS_VERSION || g_sb.sector_size != VFS_SECTOR_SIZE ||
      g_sb.total_sectors < 4096 || g_sb.total_sectors > VFS_MAX_SECTORS ||
      g_sb.total_inodes != VFS_MAX_INODES ||
      g_sb.inode_bitmap_sectors != INODE_BITMAP_SECTORS ||
      g_sb.block_bitmap_sectors != expected_block_bitmap_sectors ||
      g_sb.inode_table_sectors != expected_inode_table_sectors ||
      g_sb.inode_bitmap_start != 1 ||
      g_sb.block_bitmap_start != g_sb.inode_bitmap_start + g_sb.inode_bitmap_sectors ||
      g_sb.inode_table_start != g_sb.block_bitmap_start + g_sb.block_bitmap_sectors ||
      g_sb.data_start != g_sb.inode_table_start + g_sb.inode_table_sectors ||
      g_sb.data_start >= g_sb.total_sectors ||
      hash_fnv1a(reinterpret_cast<const uint8_t *>(&g_sb), 48) != g_sb.checksum)
    return false;

  for (uint32_t s = 0; s < g_sb.inode_bitmap_sectors; ++s) {
    if (!Kernel::Drivers::ATADriver::read_sectors(
            VFS_START_LBA + g_sb.inode_bitmap_start + s, 1,
            g_inode_bitmap + s * VFS_SECTOR_SIZE))
      return false;
  }
  for (uint32_t s = 0; s < g_sb.block_bitmap_sectors; ++s) {
    if (!Kernel::Drivers::ATADriver::read_sectors(
            VFS_START_LBA + g_sb.block_bitmap_start + s, 1,
            g_block_bitmap + s * VFS_SECTOR_SIZE))
      return false;
  }

  uint8_t sec_buf[VFS_SECTOR_SIZE];
  for (uint32_t i = 0; i < VFS_MAX_INODES; ++i) {
    uint32_t inode_offset = i * sizeof(Inode);
    uint32_t table_sec = g_sb.inode_table_start + inode_offset / VFS_SECTOR_SIZE;
    if (inode_offset % VFS_SECTOR_SIZE == 0 &&
        !Kernel::Drivers::ATADriver::read_sectors(VFS_START_LBA + table_sec, 1,
                                                  sec_buf))
      return false;
    bytes_copy(&g_inodes[i], sec_buf + inode_offset % VFS_SECTOR_SIZE,
               sizeof(Inode));
  }

  if ((g_inode_bitmap[0] & 1U) == 0 || g_inodes[0].flags != INODE_DIR ||
      g_inodes[0].extents_count == 0 ||
      g_inodes[0].extents_count > MAX_EXTENTS_PER_INODE)
    return false;
  for (uint32_t i = 0; i < VFS_MAX_INODES; ++i) {
    if (g_inodes[i].extents_count > MAX_EXTENTS_PER_INODE)
      return false;
  }

  cache_init();
  g_next_free_inode = 0;
  g_next_free_block = g_sb.data_start;
  for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; ++i)
    g_streams[i].used = false;
  g_mounted = true;
  return true;
}

// ── Stream & File Operations ────────────────────────────────────────────────
FileStream *acquire_stream() {
  for (auto &s : g_streams) {
    if (!s.used) {
      bytes_zero(&s, sizeof(s));
      s.used = true;
      return &s;
    }
  }
  return nullptr;
}

FileStream *validate_stream(void *handle) {
  if (!handle)
    return nullptr;
  for (auto &s : g_streams) {
    if (handle == &s && s.used)
      return &s;
  }
  return nullptr;
}

bool parse_mode(const char *mode, bool &r, bool &w, bool &a, bool &t) {
  r = w = a = t = false;
  if (!mode || !mode[0])
    return false;
  if (mode[0] == 'r')
    r = true;
  else if (mode[0] == 'w') {
    w = true;
    t = true;
  } else if (mode[0] == 'a') {
    w = true;
    a = true;
  } else
    return false;
  for (size_t i = 1; mode[i]; ++i) {
    if (mode[i] == '+')
      r = w = true;
  }
  return true;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Public API Implementation
// ═════════════════════════════════════════════════════════════════════════════

void VFS::init() {
  Kernel::Drivers::ATADriver::init();
  g_mounted = mount_internal();
}

bool VFS::mount() {
  g_mounted = mount_internal();
  return g_mounted;
}

bool VFS::is_mounted() { return g_mounted; }

bool VFS::format(uint32_t total_sectors) {
  if (total_sectors < 4096 || total_sectors > VFS_MAX_SECTORS)
    return false;
  if (g_mounted && !vfs_sync())
    return false;

  g_mounted = false;
  cache_init();
  bytes_zero(&g_sb, sizeof(g_sb));
  bytes_copy(g_sb.magic, FS_MAGIC, sizeof(FS_MAGIC));
  g_sb.version = FS_VERSION;
  g_sb.sector_size = VFS_SECTOR_SIZE;
  g_sb.total_sectors = total_sectors;
  g_sb.inode_bitmap_start = 1;
  g_sb.inode_bitmap_sectors = INODE_BITMAP_SECTORS;
  g_sb.block_bitmap_start = g_sb.inode_bitmap_start + g_sb.inode_bitmap_sectors;
  g_sb.block_bitmap_sectors = (total_sectors + 4095) / 4096;
  g_sb.inode_table_start = g_sb.block_bitmap_start + g_sb.block_bitmap_sectors;
  g_sb.inode_table_sectors =
      (VFS_MAX_INODES * sizeof(Inode) + VFS_SECTOR_SIZE - 1) / VFS_SECTOR_SIZE;
  g_sb.data_start = g_sb.inode_table_start + g_sb.inode_table_sectors;
  g_sb.total_inodes = VFS_MAX_INODES;
  if (g_sb.data_start >= total_sectors)
    return false;
  g_sb.checksum = hash_fnv1a(reinterpret_cast<const uint8_t *>(&g_sb), 48);

  bytes_zero(g_inode_bitmap, sizeof(g_inode_bitmap));
  bytes_zero(g_block_bitmap, sizeof(g_block_bitmap));
  bytes_zero(g_inodes, sizeof(g_inodes));
  bytes_zero(g_inode_dirty, sizeof(g_inode_dirty));
  g_inode_bitmap_dirty = false;
  g_block_bitmap_dirty = false;

  for (uint32_t b = 0; b <= g_sb.data_start; ++b)
    g_block_bitmap[b / 8] |= static_cast<uint8_t>(1U << (b % 8));

  g_inode_bitmap[0] = 1;
  g_inodes[0].flags = INODE_DIR;
  g_inodes[0].links_count = 1;
  g_inodes[0].extents_count = 1;
  g_inodes[0].extents[0].start_block = g_sb.data_start;
  g_inodes[0].extents[0].count = 1;

  // Persist the complete metadata before publishing the superblock.
  for (uint32_t sec = 0; sec < g_sb.inode_bitmap_sectors; ++sec) {
    if (!Kernel::Drivers::ATADriver::write_sectors(
            VFS_START_LBA + g_sb.inode_bitmap_start + sec, 1,
            g_inode_bitmap + sec * VFS_SECTOR_SIZE))
      return false;
  }
  for (uint32_t sec = 0; sec < g_sb.block_bitmap_sectors; ++sec) {
    if (!Kernel::Drivers::ATADriver::write_sectors(
            VFS_START_LBA + g_sb.block_bitmap_start + sec, 1,
            g_block_bitmap + sec * VFS_SECTOR_SIZE))
      return false;
  }

  const uint8_t *inode_bytes = reinterpret_cast<const uint8_t *>(g_inodes);
  uint8_t sec[VFS_SECTOR_SIZE];
  for (uint32_t sec_idx = 0; sec_idx < g_sb.inode_table_sectors; ++sec_idx) {
    bytes_zero(sec, sizeof(sec));
    size_t byte_offset = static_cast<size_t>(sec_idx) * VFS_SECTOR_SIZE;
    size_t inode_bytes_left = sizeof(g_inodes) > byte_offset
                                  ? sizeof(g_inodes) - byte_offset
                                  : 0;
    size_t copy_len = inode_bytes_left < VFS_SECTOR_SIZE
                          ? inode_bytes_left
                          : VFS_SECTOR_SIZE;
    if (copy_len)
      bytes_copy(sec, inode_bytes + byte_offset, copy_len);
    if (!Kernel::Drivers::ATADriver::write_sectors(
            VFS_START_LBA + g_sb.inode_table_start + sec_idx, 1, sec))
      return false;
  }

  bytes_zero(sec, sizeof(sec));
  if (!Kernel::Drivers::ATADriver::write_sectors(
          VFS_START_LBA + g_sb.data_start, 1, sec))
    return false;

  bytes_zero(sec, sizeof(sec));
  bytes_copy(sec, &g_sb, sizeof(g_sb));
  if (!Kernel::Drivers::ATADriver::write_sectors(VFS_START_LBA, 1, sec))
    return false;

  cache_init();
  g_next_free_inode = 1;
  g_next_free_block = g_sb.data_start + 1;
  for (uint32_t i = 0; i < VFS_MAX_OPEN_FILES; ++i)
    g_streams[i].used = false;
  g_mounted = true;
  return true;
}

bool VFS::find_file(const char *filename, VFSFile *out_file) {
  if (!out_file || !g_mounted)
    return false;
  bytes_zero(out_file, sizeof(*out_file));
  char normalized[VFS_MAX_PATH];
  if (!normalize_path(filename, normalized))
    return false;
  int ino = find_inode_by_path(normalized);
  if (ino < 0)
    return false;

  Inode &inode = g_inodes[ino];
  bytes_copy(out_file->name, normalized, VFS_MAX_PATH);
  out_file->size = inode.size;
  out_file->start_lba = inode.extents[0].start_block;
  out_file->exists = true;
  out_file->entry_index = static_cast<uint32_t>(ino);
  out_file->is_directory = (inode.flags == INODE_DIR);
  return true;
}

int VFS::read_file(const VFSFile *file, void *buffer, uint32_t length,
                   uint32_t offset) {
  if (!g_mounted || !file || !file->exists || file->is_directory)
    return 0;
  Inode &inode = g_inodes[file->entry_index];
  if (offset >= inode.size)
    return 0;
  if (length > inode.size - offset)
    length = inode.size - offset;

  uint32_t done = 0;
  uint8_t *dst = static_cast<uint8_t *>(buffer);

  for (uint32_t e = 0; e < inode.extents_count && done < length; ++e) {
    uint32_t ext_bytes = inode.extents[e].count * VFS_SECTOR_SIZE;
    // Skip extents before offset
    if (offset >= done + ext_bytes) {
      done += ext_bytes;
      continue;
    }
    uint32_t ext_offset = (offset > done) ? (offset - done) : 0;
    uint32_t block_idx =
        inode.extents[e].start_block + (ext_offset / VFS_SECTOR_SIZE);
    uint32_t byte_in_sec = ext_offset % VFS_SECTOR_SIZE;

    while (block_idx < inode.extents[e].start_block + inode.extents[e].count &&
           done < length) {
      uint8_t *sptr = cached_read_sector_ptr(block_idx);
      if (!sptr)
        break;
      uint32_t chunk = VFS_SECTOR_SIZE - byte_in_sec;
      if (chunk > length - done)
        chunk = length - done;
      bytes_copy(dst + done, sptr + byte_in_sec, chunk);
      done += chunk;
      byte_in_sec = 0;
      block_idx++;
    }
  }
  return static_cast<int>(done);
}

// ── Directory Linking Helpers ───────────────────────────────────────────────
inline bool split_path(const char *normalized, char parent_out[VFS_MAX_PATH],
                       char name_out[VFS_MAX_PATH]) {
  size_t len = str_length(normalized);
  if (len == 0 || normalized[0] != '/')
    return false;
  if (str_equal(normalized, "/"))
    return false;

  size_t last_slash = 0;
  for (size_t i = 0; i < len; ++i) {
    if (normalized[i] == '/')
      last_slash = i;
  }

  if (last_slash == 0) {
    parent_out[0] = '/';
    parent_out[1] = '\0';
    size_t n = 0;
    for (size_t i = 1; normalized[i]; ++i) {
      name_out[n++] = normalized[i];
    }
    name_out[n] = '\0';
  } else {
    for (size_t i = 0; i < last_slash; ++i) {
      parent_out[i] = normalized[i];
    }
    parent_out[last_slash] = '\0';
    if (parent_out[0] == '\0') {
      parent_out[0] = '/';
      parent_out[1] = '\0';
    }
    size_t n = 0;
    for (size_t i = last_slash + 1; normalized[i]; ++i) {
      name_out[n++] = normalized[i];
    }
    name_out[n] = '\0';
  }
  return true;
}

bool add_inode_to_dir(uint32_t parent_ino, uint32_t new_ino,
                      const char *filename) {
  Inode &dir = g_inodes[parent_ino];
  if (dir.flags != INODE_DIR)
    return false;

  size_t name_len = str_length(filename);
  uint16_t needed_len = static_cast<uint16_t>(8 + name_len);
  needed_len = (needed_len + 3) & ~3; // 4-byte alignment

  uint8_t sector_buf[VFS_SECTOR_SIZE];

  // Scan existing directory blocks for free space
  for (uint32_t e = 0; e < dir.extents_count; ++e) {
    uint32_t block = dir.extents[e].start_block;
    for (uint32_t c = 0; c < dir.extents[e].count; ++c) {
      uint32_t rel_block = block + c;
      if (!cached_read_sector(rel_block, sector_buf))
        continue;
      uint32_t offset = 0;
      while (offset < VFS_SECTOR_SIZE) {
        DiskDirEntry *de =
            reinterpret_cast<DiskDirEntry *>(sector_buf + offset);
        if (de->rec_len == 0) {
          if (offset + needed_len <= VFS_SECTOR_SIZE) {
            de->inode_index = new_ino;
            de->rec_len = static_cast<uint16_t>(VFS_SECTOR_SIZE - offset);
            de->name_len = static_cast<uint8_t>(name_len);
            de->file_type = INODE_FILE;
            bytes_copy(de->name, filename, name_len + 1);
            de->name[name_len] = '\0';
            if (!cached_write_sector(rel_block, sector_buf))
              return false;
            dir.size += needed_len;
            g_inode_dirty[parent_ino] = true;
            return true;
          }
          break;
        }
        uint16_t actual_rec_len = 8 + de->name_len;
        actual_rec_len = (actual_rec_len + 3) & ~3;
        if (de->rec_len > actual_rec_len) {
          uint16_t slack = de->rec_len - actual_rec_len;
          if (slack >= needed_len) {
            uint16_t old_rec_len = de->rec_len;
            de->rec_len = actual_rec_len;
            DiskDirEntry *new_de = reinterpret_cast<DiskDirEntry *>(
                sector_buf + offset + actual_rec_len);
            new_de->inode_index = new_ino;
            new_de->rec_len = old_rec_len - actual_rec_len;
            new_de->name_len = static_cast<uint8_t>(name_len);
            new_de->file_type = INODE_FILE;
            bytes_copy(new_de->name, filename, name_len + 1);
            new_de->name[name_len] = '\0';
            if (!cached_write_sector(rel_block, sector_buf))
              return false;
            dir.size += needed_len;
            g_inode_dirty[parent_ino] = true;
            return true;
          }
        }
        offset += de->rec_len;
      }
    }
  }

  // Allocate a new block for the directory if existing extents are full
  int new_blk = allocate_block();
  if (new_blk < 0)
    return false;

  if (dir.extents_count > 0 &&
      dir.extents[dir.extents_count - 1].start_block +
              dir.extents[dir.extents_count - 1].count ==
          static_cast<uint32_t>(new_blk)) {
    dir.extents[dir.extents_count - 1].count++;
  } else if (dir.extents_count < MAX_EXTENTS_PER_INODE) {
    dir.extents[dir.extents_count].start_block = static_cast<uint32_t>(new_blk);
    dir.extents[dir.extents_count].count = 1;
    dir.extents_count++;
  } else {
    free_block_bitmap(new_blk);
    return false;
  }

  bytes_zero(sector_buf, VFS_SECTOR_SIZE);
  DiskDirEntry *de = reinterpret_cast<DiskDirEntry *>(sector_buf);
  de->inode_index = new_ino;
  de->rec_len = VFS_SECTOR_SIZE;
  de->name_len = static_cast<uint8_t>(name_len);
  de->file_type = INODE_FILE;
  bytes_copy(de->name, filename, name_len + 1);
  de->name[name_len] = '\0';

  if (!cached_write_sector(static_cast<uint32_t>(new_blk), sector_buf)) {
    return false;
  }
  dir.size += needed_len;
  g_inode_dirty[parent_ino] = true;
  return true;
}

void *VFS::vfs_open(const char *path, const char *mode) {
  if (!g_mounted)
    return nullptr;
  bool r, w, a, t;
  if (!parse_mode(mode, r, w, a, t))
    return nullptr;
  char normalized[VFS_MAX_PATH];
  if (!normalize_path(path, normalized) || str_equal(normalized, "/"))
    return nullptr;

  int ino = find_inode_by_path(normalized);
  if (ino >= 0 && g_inodes[ino].flags != INODE_FILE)
    return nullptr;
  if (ino < 0) {
    if (!w)
      return nullptr;
    char parent_path[VFS_MAX_PATH];
    char filename[VFS_MAX_PATH];
    if (!split_path(normalized, parent_path, filename))
      return nullptr;
    size_t filename_len = str_length(filename);
    if (filename_len == 0 || filename_len > UINT8_MAX ||
        filename_len + 8 > VFS_SECTOR_SIZE)
      return nullptr;
    int parent_ino = find_inode_by_path(parent_path);
    if (parent_ino < 0)
      return nullptr;
    int new_ino = allocate_inode();
    if (new_ino < 0)
      return nullptr;
    bytes_zero(&g_inodes[new_ino], sizeof(Inode));
    g_inodes[new_ino].flags = INODE_FILE;
    g_inodes[new_ino].links_count = 1;
    g_inode_dirty[new_ino] = true;
    if (!add_inode_to_dir(static_cast<uint32_t>(parent_ino),
                          static_cast<uint32_t>(new_ino), filename)) {
      free_inode_bitmap(static_cast<uint32_t>(new_ino));
      bytes_zero(&g_inodes[new_ino], sizeof(Inode));
      g_inode_dirty[new_ino] = true;
      return nullptr;
    }
    ino = new_ino;
  }

  FileStream *stream = acquire_stream();
  if (!stream)
    return nullptr;
  Inode &inode = g_inodes[ino];
  if (t) {
    for (uint32_t e = 0; e < inode.extents_count; ++e) {
      for (uint32_t b = 0; b < inode.extents[e].count; ++b)
        free_block_bitmap(inode.extents[e].start_block + b);
    }
    bytes_zero(inode.extents, sizeof(inode.extents));
    inode.extents_count = 0;
    inode.size = 0;
    g_inode_dirty[ino] = true;
  }
  stream->inode_index = static_cast<uint32_t>(ino);
  stream->readable = r;
  stream->writable = w;
  stream->append = a;
  stream->offset = a ? inode.size : 0;
  return stream;
}

void *VFS::vfs_open(const char *path) { return vfs_open(path, "r+"); }

size_t VFS::vfs_read(void *buffer, size_t size, size_t count, void *stream) {
  FileStream *s = validate_stream(stream);
  if (!s || !s->readable || !buffer || size == 0 || count == 0)
    return 0;
  uint32_t bytes = static_cast<uint32_t>(size * count);
  // Read implementation via read_file helper
  VFSFile vf;
  vf.exists = true;
  vf.is_directory = false;
  vf.entry_index = s->inode_index;
  int got = read_file(&vf, buffer, bytes, s->offset);
  if (got <= 0)
    return 0;
  s->offset += static_cast<uint32_t>(got);
  return static_cast<size_t>(got) / size;
}

size_t VFS::vfs_write(const void *buffer, size_t size, size_t count,
                      void *stream) {
  FileStream *s = validate_stream(stream);
  if (!s || !s->writable || !buffer || size == 0 || count == 0)
    return 0;
  if (count > UINT32_MAX / size)
    return 0;
  uint32_t bytes = static_cast<uint32_t>(size * count);
  if (bytes > UINT32_MAX - s->offset)
    return 0;
  Inode &inode = g_inodes[s->inode_index];

  uint32_t written = 0;
  const uint8_t *src = static_cast<const uint8_t *>(buffer);

  while (written < bytes) {
    uint32_t current_offset = s->offset;
    uint32_t sector_idx = current_offset / VFS_SECTOR_SIZE;
    uint32_t byte_in_sec = current_offset % VFS_SECTOR_SIZE;
    uint32_t chunk = VFS_SECTOR_SIZE - byte_in_sec;
    if (chunk > bytes - written)
      chunk = bytes - written;

    uint32_t target_block = 0;
    bool block_found = false;
    uint32_t sectors_covered = 0;

    for (uint32_t e = 0; e < inode.extents_count; ++e) {
      if (sector_idx < sectors_covered + inode.extents[e].count) {
        target_block =
            inode.extents[e].start_block + (sector_idx - sectors_covered);
        block_found = true;
        break;
      }
      sectors_covered += inode.extents[e].count;
    }

    if (!block_found) {
      int new_blk = allocate_block();
      if (new_blk < 0)
        break; // Disk full

      if (inode.extents_count > 0 &&
          inode.extents[inode.extents_count - 1].start_block +
                  inode.extents[inode.extents_count - 1].count ==
              static_cast<uint32_t>(new_blk)) {
        inode.extents[inode.extents_count - 1].count++;
      } else if (inode.extents_count < MAX_EXTENTS_PER_INODE) {
        inode.extents[inode.extents_count].start_block =
            static_cast<uint32_t>(new_blk);
        inode.extents[inode.extents_count].count = 1;
        inode.extents_count++;
      } else {
        free_block_bitmap(new_blk);
        break; // Max extents reached
      }
      target_block = static_cast<uint32_t>(new_blk);
    }

    uint8_t *sptr = cached_read_sector_ptr(target_block);
    if (!sptr)
      break;

    bytes_copy(sptr + byte_in_sec, src + written, chunk);
    cached_mark_dirty(target_block);

    written += chunk;
    s->offset += chunk;
  }

  if (s->offset > inode.size)
    inode.size = s->offset;
  g_inode_dirty[s->inode_index] = true;
  return written / size;
}

int VFS::vfs_close(void *stream) {
  FileStream *s = validate_stream(stream);
  if (!s)
    return -1;
  bool synced = vfs_sync();
  s->used = false;
  return synced ? 0 : -1;
}

long VFS::vfs_tell(void *stream) {
  FileStream *s = validate_stream(stream);
  return s ? static_cast<long>(s->offset) : -1;
}

int VFS::vfs_seek(void *stream, long offset, int whence) {
  FileStream *s = validate_stream(stream);
  if (!s)
    return -1;
  int64_t base =
      (whence == 0) ? 0
                    : (whence == 1 ? s->offset : g_inodes[s->inode_index].size);
  int64_t target = base + offset;
  if (target < 0 || target > static_cast<int64_t>(UINT32_MAX))
    return -1;
  s->offset = static_cast<uint32_t>(target);
  return 0;
}

size_t VFS::vfs_size(void *stream) {
  FileStream *s = validate_stream(stream);
  return s ? g_inodes[s->inode_index].size : 0;
}

int VFS::vfs_mkdir(const char *pathname, int mode) {
  (void)mode;
  if (!g_mounted)
    return -1;
  char normalized[VFS_MAX_PATH];
  if (!normalize_path(pathname, normalized) || str_equal(normalized, "/"))
    return -1;
  int ino = allocate_inode();
  if (ino < 0)
    return -1;
  g_inodes[ino].flags = INODE_DIR;
  g_inode_dirty[ino] = true;
  return 0;
}

int VFS::vfs_remove(const char *pathname) {
  if (!g_mounted)
    return -1;
  char normalized[VFS_MAX_PATH];
  if (!normalize_path(pathname, normalized) || str_equal(normalized, "/"))
    return -1;
  int ino = find_inode_by_path(normalized);
  if (ino < 0)
    return -1;

  free_inode_bitmap(static_cast<uint32_t>(ino));
  g_inodes[ino].flags = INODE_FREE;
  g_inode_dirty[ino] = true;
  return 0;
}

int VFS::vfs_rename(const char *oldpath, const char *newpath) {
  (void)oldpath;
  (void)newpath;
  return -1;
}

bool VFS::vfs_exists(const char *path) {
  if (!g_mounted)
    return false;
  char normalized[VFS_MAX_PATH];
  return normalize_path(path, normalized) &&
         find_inode_by_path(normalized) >= 0;
}

bool VFS::get_file_at(uint32_t index, VFSFile *out_file) {
  if (!out_file || index >= VFS_MAX_INODES || !g_mounted ||
      g_inodes[index].flags == INODE_FREE)
    return false;
  bytes_zero(out_file, sizeof(*out_file));
  if (!build_inode_path(index, out_file->name))
    return false;
  out_file->size = g_inodes[index].size;
  out_file->start_lba = g_inodes[index].extents[0].start_block;
  out_file->exists = true;
  out_file->entry_index = index;
  out_file->is_directory = (g_inodes[index].flags == INODE_DIR);
  return true;
}

bool VFS::vfs_sync() {
  if (!g_mounted)
    return false;
  bool ok = flush_bitmaps_and_inodes();
  if (!cache_flush_all())
    ok = false;
  return ok;
}

} // namespace Kernel::FS

extern "C" {
void *kernel_vfs_open(const char *path, const char *mode) {
  return Kernel::FS::VFS::vfs_open(path, mode);
}
size_t kernel_vfs_read(void *ptr, size_t size, size_t nmemb, void *stream) {
  return Kernel::FS::VFS::vfs_read(ptr, size, nmemb, stream);
}
size_t kernel_vfs_write(const void *ptr, size_t size, size_t nmemb,
                        void *stream) {
  return Kernel::FS::VFS::vfs_write(ptr, size, nmemb, stream);
}
int kernel_vfs_close(void *stream) {
  return Kernel::FS::VFS::vfs_close(stream);
}
long kernel_vfs_tell(void *stream) { return Kernel::FS::VFS::vfs_tell(stream); }
int kernel_vfs_seek(void *stream, long offset, int whence) {
  return Kernel::FS::VFS::vfs_seek(stream, offset, whence);
}
size_t kernel_vfs_size(void *stream) {
  return Kernel::FS::VFS::vfs_size(stream);
}
int kernel_vfs_mkdir(const char *path, int mode) {
  return Kernel::FS::VFS::vfs_mkdir(path, mode);
}
int kernel_vfs_remove(const char *path) {
  return Kernel::FS::VFS::vfs_remove(path);
}
int kernel_vfs_rename(const char *oldpath, const char *newpath) {
  return Kernel::FS::VFS::vfs_rename(oldpath, newpath);
}
bool kernel_vfs_exists(const char *path) {
  return Kernel::FS::VFS::vfs_exists(path);
}
int kernel_vfs_sync() { return Kernel::FS::VFS::vfs_sync() ? 0 : -1; }
}