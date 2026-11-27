#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <kernel/drivers/ata.h>
#include <kernel/fs/laziofs.h>
#include <kernel/spinlock.h>

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
constexpr uint32_t CACHE_PREFETCH_SECTORS = CACHE_SLOTS < 8 ? CACHE_SLOTS : 8;
constexpr uint32_t CACHE_FLUSH_BATCH_SECTORS = 16;

struct CacheSlot {
  uint32_t abs_lba;
  uint32_t age;
  int32_t hash_prev;
  int32_t hash_next;
  bool dirty;
  bool reserved;
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
uint32_t g_vfs_start_lba = VFS_START_LBA;
uint32_t g_next_free_inode = 0;
uint32_t g_next_free_block = 0;
FileStream g_streams[VFS_MAX_OPEN_FILES]{};
Spinlock g_vfs_lock;
struct VfsLockGuard {
  VfsLockGuard() { g_vfs_lock.lock(); }
  ~VfsLockGuard() { g_vfs_lock.unlock(); }
};

// Dynamic block bitmap representation in RAM
constexpr uint32_t MAX_BLOCKS = VFS_MAX_SECTORS;
constexpr uint32_t BLOCK_BITMAP_SECTORS_MAX = (MAX_BLOCKS + 4095) / 4096;
constexpr uint32_t BLOCK_BITMAP_BYTES =
    BLOCK_BITMAP_SECTORS_MAX * VFS_SECTOR_SIZE;
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
inline bool str_equal_n(const char *a, const char *b, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (a[i] != b[i])
      return false;
  }
  return b[length] == '\0';
}
bool path_has_prefix(const char *path, const char *prefix, size_t length) {
  for (size_t i = 0; i < length; ++i)
    if (path[i] != prefix[i] || path[i] == '\0')
      return false;
  return path[length] == '/';
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
    g_cache[i].reserved = false;
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

CacheSlot *cache_find(uint32_t abs_lba);

bool cache_flush_all() {
  uint8_t batch[CACHE_FLUSH_BATCH_SECTORS * VFS_SECTOR_SIZE];
  for (;;) {
    CacheSlot *first = nullptr;
    for (uint32_t i = 0; i < CACHE_SLOTS; ++i)
      if (g_cache[i].dirty && (!first || g_cache[i].abs_lba < first->abs_lba))
        first = &g_cache[i];
    if (!first)
      return true;

    CacheSlot *slots[CACHE_FLUSH_BATCH_SECTORS];
    uint32_t count = 0;
    for (; count < CACHE_FLUSH_BATCH_SECTORS; ++count) {
      CacheSlot *slot = cache_find(first->abs_lba + count);
      if (!slot || !slot->dirty)
        break;
      slots[count] = slot;
      bytes_copy(batch + count * VFS_SECTOR_SIZE, slot->data, VFS_SECTOR_SIZE);
    }
    if (!count || !Kernel::Drivers::ATADriver::write_sectors(
                      first->abs_lba, static_cast<uint8_t>(count), batch))
      return false;
    for (uint32_t i = 0; i < count; ++i)
      slots[i]->dirty = false;
  }
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
  for (int32_t i = g_cache_buckets[bucket]; i >= 0; i = g_cache[i].hash_next) {
    if (g_cache[i].abs_lba == abs_lba) {
      g_cache[i].age = ++g_cache_tick;
      return &g_cache[i];
    }
  }
  return nullptr;
}

CacheSlot *cache_evict() {
  CacheSlot *best_clean = nullptr;
  for (uint32_t i = 0; i < CACHE_SLOTS; ++i) {
    CacheSlot &slot = g_cache[i];
    if (slot.abs_lba == CACHE_INVALID_LBA && !slot.reserved)
      return &slot;
    if (!slot.reserved && !slot.dirty &&
        (!best_clean || slot.age < best_clean->age))
      best_clean = &slot;
  }
  if (best_clean) {
    cache_remove(*best_clean);
    best_clean->abs_lba = CACHE_INVALID_LBA;
    return best_clean;
  }

  CacheSlot *best_dirty = nullptr;
  for (uint32_t i = 0; i < CACHE_SLOTS; ++i) {
    if (!g_cache[i].reserved &&
        (!best_dirty || g_cache[i].age < best_dirty->age))
      best_dirty = &g_cache[i];
  }
  if (!best_dirty)
    return nullptr;
  if (!cache_flush_slot(*best_dirty))
    return nullptr;
  cache_remove(*best_dirty);
  best_dirty->abs_lba = CACHE_INVALID_LBA;
  return best_dirty;
}

uint8_t *cached_read_sector_ptr(uint32_t rel_lba);

bool cached_read_sector(uint32_t rel_lba, uint8_t *out) {
  if (!out || rel_lba >= g_sb.total_sectors)
    return false;
  uint8_t *data = cached_read_sector_ptr(rel_lba);
  if (!data)
    return false;
  bytes_copy(out, data, VFS_SECTOR_SIZE);
  return true;
}

bool cached_write_sector(uint32_t rel_lba, const uint8_t *data) {
  if (!data || rel_lba >= g_sb.total_sectors)
    return false;
  uint32_t abs = g_vfs_start_lba + rel_lba;
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
  uint32_t abs = g_vfs_start_lba + rel_lba;
  CacheSlot *slot = cache_find(abs);
  if (slot)
    return slot->data;

  CacheSlot *reserved[CACHE_PREFETCH_SECTORS];
  uint8_t batch[CACHE_PREFETCH_SECTORS * VFS_SECTOR_SIZE];
  uint32_t count = 0;
  const uint32_t limit =
      std::min(CACHE_PREFETCH_SECTORS, g_sb.total_sectors - rel_lba);
  while (count < limit) {
    if (cache_find(abs + count))
      break;
    slot = cache_evict();
    if (!slot) {
      for (uint32_t i = 0; i < count; ++i) {
        reserved[i]->reserved = false;
        reserved[i]->abs_lba = CACHE_INVALID_LBA;
      }
      return nullptr;
    }
    slot->age = ++g_cache_tick; // Keep this reserved slot out of LRU eviction.
    slot->reserved = true;
    reserved[count++] = slot;
  }
  if (!count || !Kernel::Drivers::ATADriver::read_sectors(
                    abs, static_cast<uint8_t>(count), batch)) {
    for (uint32_t i = 0; i < count; ++i) {
      reserved[i]->reserved = false;
      reserved[i]->abs_lba = CACHE_INVALID_LBA;
    }
    return nullptr;
  }

  for (uint32_t i = 0; i < count; ++i) {
    slot = reserved[i];
    bytes_copy(slot->data, batch + i * VFS_SECTOR_SIZE, VFS_SECTOR_SIZE);
    slot->abs_lba = abs + i;
    slot->age = ++g_cache_tick;
    slot->dirty = false;
    slot->reserved = false;
    cache_insert(*slot);
  }
  return reserved[0]->data;
}

bool cached_write_full_sector(uint32_t rel_lba, const uint8_t *data) {
  if (!data || rel_lba >= g_sb.total_sectors)
    return false;
  const uint32_t abs = g_vfs_start_lba + rel_lba;
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

void cached_mark_dirty(uint32_t rel_lba) {
  uint32_t abs = g_vfs_start_lba + rel_lba;
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
        if (de->rec_len < 8 || offset + de->rec_len > VFS_SECTOR_SIZE ||
            de->name_len >= VFS_MAX_PATH || de->name_len > de->rec_len - 8)
          break;
        if (de->inode_index != 0 && de->inode_index < VFS_MAX_INODES &&
            g_inodes[de->inode_index].flags != INODE_FREE &&
            str_equal_n(de->name, name, de->name_len)) {
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
             block_offset < parent.extents[extent_index].count && !found_parent;
             ++block_offset) {
          uint8_t sector[VFS_SECTOR_SIZE];
          uint32_t block =
              parent.extents[extent_index].start_block + block_offset;
          if (!cached_read_sector(block, sector))
            continue;

          uint32_t offset = 0;
          while (offset + 8 <= VFS_SECTOR_SIZE) {
            DiskDirEntry *entry =
                reinterpret_cast<DiskDirEntry *>(sector + offset);
            if (entry->rec_len < 8 ||
                offset + entry->rec_len > VFS_SECTOR_SIZE ||
                entry->name_len >= VFS_MAX_PATH ||
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
  uint32_t selected_lba = VFS_START_LBA;
  auto has_fs_magic = [](const uint8_t *data) {
    for (size_t i = 0; i < sizeof(FS_MAGIC); ++i) {
      if (data[i] != static_cast<uint8_t>(FS_MAGIC[i]))
        return false;
    }
    return true;
  };

  bool found =
      Kernel::Drivers::ATADriver::read_sectors(selected_lba, 1, sector) &&
      has_fs_magic(sector);
  if (!found && Kernel::Drivers::ATADriver::read_sectors(0, 1, sector) &&
      sector[510] == 0x55 && sector[511] == 0xAA) {
    for (uint32_t i = 0; i < 4 && !found; ++i) {
      const uint8_t *entry = sector + 446 + i * 16;
      if (entry[4] != 0x83)
        continue;
      const uint32_t partition_lba = static_cast<uint32_t>(entry[8]) |
                                     (static_cast<uint32_t>(entry[9]) << 8) |
                                     (static_cast<uint32_t>(entry[10]) << 16) |
                                     (static_cast<uint32_t>(entry[11]) << 24);
      if (partition_lba &&
          Kernel::Drivers::ATADriver::read_sectors(partition_lba, 1, sector) &&
          has_fs_magic(sector)) {
        selected_lba = partition_lba;
        found = true;
      }
    }
  }
  if (!found)
    return false;
  g_vfs_start_lba = selected_lba;
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
      g_sb.block_bitmap_start !=
          g_sb.inode_bitmap_start + g_sb.inode_bitmap_sectors ||
      g_sb.inode_table_start !=
          g_sb.block_bitmap_start + g_sb.block_bitmap_sectors ||
      g_sb.data_start != g_sb.inode_table_start + g_sb.inode_table_sectors ||
      g_sb.data_start >= g_sb.total_sectors ||
      hash_fnv1a(reinterpret_cast<const uint8_t *>(&g_sb), 48) != g_sb.checksum)
    return false;

  for (uint32_t s = 0; s < g_sb.inode_bitmap_sectors; ++s) {
    if (!Kernel::Drivers::ATADriver::read_sectors(
            g_vfs_start_lba + g_sb.inode_bitmap_start + s, 1,
            g_inode_bitmap + s * VFS_SECTOR_SIZE))
      return false;
  }
  for (uint32_t s = 0; s < g_sb.block_bitmap_sectors; ++s) {
    if (!Kernel::Drivers::ATADriver::read_sectors(
            g_vfs_start_lba + g_sb.block_bitmap_start + s, 1,
            g_block_bitmap + s * VFS_SECTOR_SIZE))
      return false;
  }

  constexpr uint32_t MOUNT_BATCH_SECTORS = 16;
  uint8_t table_batch[MOUNT_BATCH_SECTORS * VFS_SECTOR_SIZE];
  uint8_t *inode_bytes = reinterpret_cast<uint8_t *>(g_inodes);
  for (uint32_t sector = 0; sector < g_sb.inode_table_sectors;) {
    uint32_t count =
        std::min(MOUNT_BATCH_SECTORS, g_sb.inode_table_sectors - sector);
    if (!Kernel::Drivers::ATADriver::read_sectors(
            g_vfs_start_lba + g_sb.inode_table_start + sector,
            static_cast<uint8_t>(count), table_batch))
      return false;
    bytes_copy(inode_bytes + sector * VFS_SECTOR_SIZE, table_batch,
               count * VFS_SECTOR_SIZE);
    sector += count;
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

bool sync_internal() {
  if (!g_mounted)
    return false;
  bool ok = flush_bitmaps_and_inodes();
  if (!cache_flush_all())
    ok = false;
  return ok;
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

bool parse_mode(const char *mode, bool &r, bool &w, bool &a, bool &t,
                bool &exclusive) {
  r = w = a = t = exclusive = false;
  if (!mode || !mode[0])
    return false;
  switch (mode[0]) {
  case 'r': r = true; break;
  case 'w': w = t = true; break;
  case 'a': w = a = true; break;
  default: return false;
  }
  bool plus_seen = false, binary_seen = false, exclusive_seen = false;
  for (size_t i = 1; mode[i]; ++i) {
    switch (mode[i]) {
    case '+':
      if (plus_seen) return false;
      plus_seen = true;
      r = w = true;
      break;
    case 'b':
      if (binary_seen) return false;
      binary_seen = true;
      break;
    case 'x':
      if (exclusive_seen || mode[0] != 'w') return false;
      exclusive_seen = exclusive = true;
      break;
    default: return false;
    }
  }
  return true;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Public API Implementation
// ═════════════════════════════════════════════════════════════════════════════

void VFS::init() {
  VfsLockGuard guard;
  Kernel::Drivers::ATADriver::init();
  g_mounted = mount_internal();
}

bool VFS::mount() {
  VfsLockGuard guard;
  g_mounted = mount_internal();
  return g_mounted;
}

bool VFS::is_mounted() {
  VfsLockGuard guard;
  return g_mounted;
}

bool VFS::format(uint32_t total_sectors) {
  VfsLockGuard guard;
  if (total_sectors < 4096 || total_sectors > VFS_MAX_SECTORS)
    return false;
  if (g_mounted && !sync_internal())
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
            g_vfs_start_lba + g_sb.inode_bitmap_start + sec, 1,
            g_inode_bitmap + sec * VFS_SECTOR_SIZE))
      return false;
  }
  for (uint32_t sec = 0; sec < g_sb.block_bitmap_sectors; ++sec) {
    if (!Kernel::Drivers::ATADriver::write_sectors(
            g_vfs_start_lba + g_sb.block_bitmap_start + sec, 1,
            g_block_bitmap + sec * VFS_SECTOR_SIZE))
      return false;
  }

  const uint8_t *inode_bytes = reinterpret_cast<const uint8_t *>(g_inodes);
  uint8_t sec[VFS_SECTOR_SIZE];
  for (uint32_t sec_idx = 0; sec_idx < g_sb.inode_table_sectors; ++sec_idx) {
    bytes_zero(sec, sizeof(sec));
    size_t byte_offset = static_cast<size_t>(sec_idx) * VFS_SECTOR_SIZE;
    size_t inode_bytes_left =
        sizeof(g_inodes) > byte_offset ? sizeof(g_inodes) - byte_offset : 0;
    size_t copy_len =
        inode_bytes_left < VFS_SECTOR_SIZE ? inode_bytes_left : VFS_SECTOR_SIZE;
    if (copy_len)
      bytes_copy(sec, inode_bytes + byte_offset, copy_len);
    if (!Kernel::Drivers::ATADriver::write_sectors(
            g_vfs_start_lba + g_sb.inode_table_start + sec_idx, 1, sec))
      return false;
  }

  bytes_zero(sec, sizeof(sec));
  if (!Kernel::Drivers::ATADriver::write_sectors(
          g_vfs_start_lba + g_sb.data_start, 1, sec))
    return false;

  bytes_zero(sec, sizeof(sec));
  bytes_copy(sec, &g_sb, sizeof(g_sb));
  if (!Kernel::Drivers::ATADriver::write_sectors(g_vfs_start_lba, 1, sec))
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
  VfsLockGuard guard;
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

static int read_file_internal(const VFSFile *file, void *buffer,
                              uint32_t length, uint32_t offset) {
  if (!g_mounted || !file || !file->exists || file->is_directory ||
      file->entry_index >= VFS_MAX_INODES)
    return -1;
  if (!buffer || !length)
    return 0;
  Inode &inode = g_inodes[file->entry_index];
  if (inode.flags != INODE_FILE)
    return -1;
  if (offset >= inode.size)
    return 0;
  const uint32_t wanted =
      length < inode.size - offset ? length : inode.size - offset;
  uint32_t logical_base = 0;
  uint32_t copied = 0;
  uint8_t *dst = static_cast<uint8_t *>(buffer);

  for (uint32_t e = 0; e < inode.extents_count && copied < wanted; ++e) {
    const uint32_t extent_bytes = inode.extents[e].count * VFS_SECTOR_SIZE;
    if (offset >= logical_base + extent_bytes) {
      logical_base += extent_bytes;
      continue;
    }
    uint32_t within_extent = offset > logical_base ? offset - logical_base : 0;
    while (within_extent < extent_bytes && copied < wanted) {
      const uint32_t block = inode.extents[e].start_block +
                             within_extent / VFS_SECTOR_SIZE;
      const uint32_t byte_in_sector = within_extent % VFS_SECTOR_SIZE;
      uint8_t *sector = cached_read_sector_ptr(block);
      if (!sector)
        return copied ? static_cast<int>(copied) : -1;
      uint32_t chunk = VFS_SECTOR_SIZE - byte_in_sector;
      if (chunk > extent_bytes - within_extent)
        chunk = extent_bytes - within_extent;
      if (chunk > wanted - copied)
        chunk = wanted - copied;
      bytes_copy(dst + copied, sector + byte_in_sector, chunk);
      copied += chunk;
      within_extent += chunk;
    }
    logical_base += extent_bytes;
  }
  return static_cast<int>(copied);
}

int VFS::read_file(const VFSFile *file, void *buffer, uint32_t length,
                   uint32_t offset) {
  VfsLockGuard guard;
  return read_file_internal(file, buffer, length, offset);
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
                      const char *filename, uint8_t file_type) {
  Inode &dir = g_inodes[parent_ino];
  if (dir.flags != INODE_DIR)
    return false;

  size_t name_len = str_length(filename);
  if (name_len == 0 || name_len >= VFS_MAX_PATH || name_len > UINT8_MAX)
    return false;
  uint16_t needed_len = static_cast<uint16_t>(8 + name_len);
  needed_len = (needed_len + 3) & ~3; // 4-byte alignment
  if (needed_len > VFS_SECTOR_SIZE)
    return false;

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
          if (offset + needed_len > VFS_SECTOR_SIZE)
            break;
          de->inode_index = new_ino;
          de->rec_len = static_cast<uint16_t>(VFS_SECTOR_SIZE - offset);
          de->name_len = static_cast<uint8_t>(name_len);
          de->file_type = file_type;
          bytes_copy(de->name, filename, name_len);
          de->name[name_len] = '\0';
          if (!cached_write_sector(rel_block, sector_buf))
            return false;
          dir.size += needed_len;
          g_inode_dirty[parent_ino] = true;
          return true;
        }
        if (de->rec_len < 8 || offset + de->rec_len > VFS_SECTOR_SIZE ||
            de->name_len >= VFS_MAX_PATH || de->name_len > de->rec_len - 8)
          return false;
        if (de->inode_index == 0) {
          if (de->rec_len >= needed_len) {
            de->inode_index = new_ino;
            de->name_len = static_cast<uint8_t>(name_len);
            de->file_type = file_type;
            bytes_zero(de->name, name_len + 1);
            bytes_copy(de->name, filename, name_len);
            de->name[name_len] = '\0';
            if (!cached_write_sector(rel_block, sector_buf))
              return false;
            dir.size += needed_len;
            g_inode_dirty[parent_ino] = true;
            return true;
          }
          offset += de->rec_len;
          continue;
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
            new_de->file_type = file_type;
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
  const bool extend_last = dir.extents_count > 0 &&
                           dir.extents[dir.extents_count - 1].start_block +
                                   dir.extents[dir.extents_count - 1].count ==
                               static_cast<uint32_t>(new_blk);
  if (!extend_last && dir.extents_count >= MAX_EXTENTS_PER_INODE) {
    free_block_bitmap(static_cast<uint32_t>(new_blk));
    return false;
  }

  bytes_zero(sector_buf, VFS_SECTOR_SIZE);
  DiskDirEntry *de = reinterpret_cast<DiskDirEntry *>(sector_buf);
  de->inode_index = new_ino;
  de->rec_len = VFS_SECTOR_SIZE;
  de->name_len = static_cast<uint8_t>(name_len);
  de->file_type = file_type;
  bytes_copy(de->name, filename, name_len + 1);
  de->name[name_len] = '\0';

  if (!cached_write_sector(static_cast<uint32_t>(new_blk), sector_buf)) {
    free_block_bitmap(static_cast<uint32_t>(new_blk));
    return false;
  }
  if (extend_last) {
    dir.extents[dir.extents_count - 1].count++;
  } else {
    dir.extents[dir.extents_count].start_block = static_cast<uint32_t>(new_blk);
    dir.extents[dir.extents_count].count = 1;
    dir.extents_count++;
  }
  dir.size += needed_len;
  g_inode_dirty[parent_ino] = true;
  return true;
}

bool remove_inode_from_dir(uint32_t parent_ino, uint32_t child_ino,
                           const char *expected_name = nullptr) {
  if (parent_ino >= VFS_MAX_INODES || g_inodes[parent_ino].flags != INODE_DIR)
    return false;
  Inode &dir = g_inodes[parent_ino];
  uint8_t sector[VFS_SECTOR_SIZE];
  for (uint32_t e = 0; e < dir.extents_count; ++e) {
    for (uint32_t b = 0; b < dir.extents[e].count; ++b) {
      const uint32_t block = dir.extents[e].start_block + b;
      if (!cached_read_sector(block, sector))
        return false;
      uint32_t offset = 0;
      while (offset + 8 <= VFS_SECTOR_SIZE) {
        auto *entry = reinterpret_cast<DiskDirEntry *>(sector + offset);
        if (entry->rec_len < 8 || offset + entry->rec_len > VFS_SECTOR_SIZE ||
            entry->name_len >= VFS_MAX_PATH ||
            entry->name_len > entry->rec_len - 8)
          return false;
        if (entry->inode_index == child_ino &&
            (!expected_name || str_equal_n(entry->name, expected_name,
                                            entry->name_len))) {
          const uint32_t old_name_len = entry->name_len;
          uint32_t used = 8u + old_name_len;
          used = (used + 3u) & ~3u;
          entry->inode_index = 0;
          entry->name_len = 0;
          entry->file_type = 0;
          // Directory entries are variable-length. Never clear the fixed-size
          // name array here: it may overlap the following entry in this sector.
          bytes_zero(entry->name, old_name_len + 1);
          dir.size = dir.size > used ? dir.size - used : 0;
          if (!cached_write_sector(block, sector))
            return false;
          g_inode_dirty[parent_ino] = true;
          return true;
        }
        offset += entry->rec_len;
      }
    }
  }
  return false;
}

bool directory_is_empty(uint32_t inode_index) {
  if (inode_index >= VFS_MAX_INODES || g_inodes[inode_index].flags != INODE_DIR)
    return false;
  Inode &dir = g_inodes[inode_index];
  uint8_t sector[VFS_SECTOR_SIZE];
  for (uint32_t e = 0; e < dir.extents_count; ++e) {
    for (uint32_t b = 0; b < dir.extents[e].count; ++b) {
      if (!cached_read_sector(dir.extents[e].start_block + b, sector))
        return false;
      uint32_t offset = 0;
      while (offset + 8 <= VFS_SECTOR_SIZE) {
        auto *entry = reinterpret_cast<DiskDirEntry *>(sector + offset);
        if (entry->rec_len < 8 || offset + entry->rec_len > VFS_SECTOR_SIZE ||
            entry->name_len >= VFS_MAX_PATH ||
            entry->name_len > entry->rec_len - 8)
          return false;
        if (entry->inode_index != 0)
          return false;
        offset += entry->rec_len;
      }
    }
  }
  return true;
}

bool inode_has_open_stream(uint32_t inode_index) {
  for (const auto &stream : g_streams)
    if (stream.used && stream.inode_index == inode_index)
      return true;
  return false;
}

void release_inode(uint32_t inode_index) {
  if (inode_index == 0 || inode_index >= VFS_MAX_INODES)
    return;
  Inode &inode = g_inodes[inode_index];
  for (uint32_t e = 0; e < inode.extents_count; ++e)
    for (uint32_t b = 0; b < inode.extents[e].count; ++b)
      free_block_bitmap(inode.extents[e].start_block + b);
  bytes_zero(&inode, sizeof(inode));
  g_inode_dirty[inode_index] = true;
  free_inode_bitmap(inode_index);
}

bool ensure_file_block(Inode &inode, uint32_t logical_sector,
                       uint32_t &physical_sector) {
  uint32_t sectors = 0;
  for (uint32_t e = 0; e < inode.extents_count; ++e) {
    const uint32_t count = inode.extents[e].count;
    if (logical_sector < sectors + count) {
      physical_sector = inode.extents[e].start_block + logical_sector - sectors;
      return true;
    }
    sectors += count;
  }
  while (sectors <= logical_sector) {
    const int allocated = allocate_block();
    if (allocated < 0)
      return false;
    uint8_t zero_sector[VFS_SECTOR_SIZE]{};
    if (!cached_write_full_sector(static_cast<uint32_t>(allocated), zero_sector)) {
      free_block_bitmap(static_cast<uint32_t>(allocated));
      return false;
    }
    const bool extend_last = inode.extents_count > 0 &&
        inode.extents[inode.extents_count - 1].start_block +
            inode.extents[inode.extents_count - 1].count ==
        static_cast<uint32_t>(allocated);
    if (extend_last) {
      ++inode.extents[inode.extents_count - 1].count;
    } else {
      if (inode.extents_count >= MAX_EXTENTS_PER_INODE) {
        free_block_bitmap(static_cast<uint32_t>(allocated));
        return false;
      }
      inode.extents[inode.extents_count].start_block =
          static_cast<uint32_t>(allocated);
      inode.extents[inode.extents_count].count = 1;
      ++inode.extents_count;
    }
    ++sectors;
    g_inode_dirty[static_cast<uint32_t>(&inode - g_inodes)] = true;
  }
  for (uint32_t e = 0, base = 0; e < inode.extents_count; base += inode.extents[e].count, ++e) {
    if (logical_sector < base + inode.extents[e].count) {
      physical_sector = inode.extents[e].start_block + logical_sector - base;
      return true;
    }
  }
  return false;
}

bool zero_file_range(Inode &inode, uint32_t begin, uint32_t end) {
  uint32_t position = begin;
  while (position < end) {
    uint32_t physical = 0;
    if (!ensure_file_block(inode, position / VFS_SECTOR_SIZE, physical))
      return false;
    uint8_t *sector = cached_read_sector_ptr(physical);
    if (!sector)
      return false;
    const uint32_t within = position % VFS_SECTOR_SIZE;
    uint32_t chunk = VFS_SECTOR_SIZE - within;
    if (chunk > end - position)
      chunk = end - position;
    bytes_zero(sector + within, chunk);
    cached_mark_dirty(physical);
    position += chunk;
  }
  return true;
}

void *VFS::vfs_open(const char *path, const char *mode) {
  VfsLockGuard guard;
  if (!g_mounted)
    return nullptr;
  bool readable, writable, append, truncate, exclusive;
  if (!parse_mode(mode, readable, writable, append, truncate, exclusive))
    return nullptr;
  char normalized[VFS_MAX_PATH];
  if (!normalize_path(path, normalized) || str_equal(normalized, "/"))
    return nullptr;

  FileStream *stream = acquire_stream();
  if (!stream)
    return nullptr;
  auto fail = [&]() -> void * {
    stream->used = false;
    return nullptr;
  };

  int inode_index = find_inode_by_path(normalized);
  if (inode_index >= 0 && exclusive)
    return fail();
  if (inode_index >= 0 && g_inodes[inode_index].flags != INODE_FILE)
    return fail();
  if (inode_index < 0) {
    if (!writable)
      return fail();
    char parent_path[VFS_MAX_PATH];
    char filename[VFS_MAX_PATH];
    if (!split_path(normalized, parent_path, filename))
      return fail();
    const size_t filename_len = str_length(filename);
    if (!filename_len || filename_len > UINT8_MAX ||
        filename_len + 8 > VFS_SECTOR_SIZE)
      return fail();
    const int parent_inode = find_inode_by_path(parent_path);
    if (parent_inode < 0 || g_inodes[parent_inode].flags != INODE_DIR)
      return fail();
    const int new_inode = allocate_inode();
    if (new_inode < 0)
      return fail();
    bytes_zero(&g_inodes[new_inode], sizeof(Inode));
    g_inodes[new_inode].flags = INODE_FILE;
    g_inodes[new_inode].links_count = 1;
    g_inode_dirty[new_inode] = true;
    if (!add_inode_to_dir(static_cast<uint32_t>(parent_inode),
                          static_cast<uint32_t>(new_inode), filename,
                          INODE_FILE)) {
      bytes_zero(&g_inodes[new_inode], sizeof(Inode));
      g_inode_dirty[new_inode] = true;
      free_inode_bitmap(static_cast<uint32_t>(new_inode));
      return fail();
    }
    inode_index = new_inode;
  }

  Inode &inode = g_inodes[inode_index];
  if (truncate) {
    for (uint32_t e = 0; e < inode.extents_count; ++e)
      for (uint32_t b = 0; b < inode.extents[e].count; ++b)
        free_block_bitmap(inode.extents[e].start_block + b);
    bytes_zero(inode.extents, sizeof(inode.extents));
    inode.extents_count = 0;
    inode.size = 0;
    g_inode_dirty[inode_index] = true;
  }
  stream->inode_index = static_cast<uint32_t>(inode_index);
  stream->readable = readable;
  stream->writable = writable;
  stream->append = append;
  // a+ reads from BOF; every write operation still targets the current EOF.
  stream->offset = (append && !readable) ? inode.size : 0;
  return stream;
}

void *VFS::vfs_open(const char *path) { return vfs_open(path, "r+"); }

size_t VFS::vfs_read(void *buffer, size_t size, size_t count, void *stream) {
  VfsLockGuard guard;
  FileStream *s = validate_stream(stream);
  if (!s || !s->readable || !buffer || size == 0 || count == 0)
    return 0;
  if (count > UINT32_MAX / size)
    return 0;
  const uint32_t bytes = static_cast<uint32_t>(size * count);
  VFSFile file{};
  file.exists = true;
  file.entry_index = s->inode_index;
  const int got = read_file_internal(&file, buffer, bytes, s->offset);
  if (got <= 0)
    return 0;
  s->offset += static_cast<uint32_t>(got);
  return static_cast<size_t>(got) / size;
}

size_t VFS::vfs_write(const void *buffer, size_t size, size_t count,
                      void *stream) {
  VfsLockGuard guard;
  FileStream *s = validate_stream(stream);
  if (!s || !s->writable || !buffer || size == 0 || count == 0)
    return 0;
  if (count > UINT32_MAX / size)
    return 0;
  const uint32_t bytes = static_cast<uint32_t>(size * count);
  Inode &inode = g_inodes[s->inode_index];
  if (inode.flags != INODE_FILE)
    return 0;
  if (s->append)
    s->offset = inode.size;
  if (bytes > UINT32_MAX - s->offset)
    return 0;
  if (s->offset > inode.size) {
    if (!zero_file_range(inode, inode.size, s->offset))
      return 0;
    inode.size = s->offset;
    g_inode_dirty[s->inode_index] = true;
  }

  uint32_t written = 0;
  const auto *source = static_cast<const uint8_t *>(buffer);
  while (written < bytes) {
    uint32_t physical = 0;
    const uint32_t sector_index = s->offset / VFS_SECTOR_SIZE;
    const uint32_t byte_in_sector = s->offset % VFS_SECTOR_SIZE;
    if (!ensure_file_block(inode, sector_index, physical))
      break;
    uint32_t chunk = VFS_SECTOR_SIZE - byte_in_sector;
    if (chunk > bytes - written)
      chunk = bytes - written;
    if (byte_in_sector == 0 && chunk == VFS_SECTOR_SIZE) {
      if (!cached_write_full_sector(physical, source + written))
        break;
    } else {
      uint8_t *sector = cached_read_sector_ptr(physical);
      if (!sector)
        break;
      bytes_copy(sector + byte_in_sector, source + written, chunk);
      cached_mark_dirty(physical);
    }
    written += chunk;
    s->offset += chunk;
    if (s->offset > inode.size)
      inode.size = s->offset;
  }
  g_inode_dirty[s->inode_index] = true;
  return written / size;
}

int VFS::vfs_close(void *stream) {
  VfsLockGuard guard;
  FileStream *s = validate_stream(stream);
  if (!s)
    return -1;
  const uint32_t inode_index = s->inode_index;
  s->used = false;
  bool ok = sync_internal();
  if (inode_index < VFS_MAX_INODES && g_inodes[inode_index].flags == INODE_FILE &&
      g_inodes[inode_index].links_count == 0 &&
      !inode_has_open_stream(inode_index))
    release_inode(inode_index);
  if (ok)
    ok = sync_internal();
  return ok ? 0 : -1;
}

long VFS::vfs_tell(void *stream) {
  VfsLockGuard guard;
  FileStream *s = validate_stream(stream);
  return s ? static_cast<long>(s->offset) : -1;
}

int VFS::vfs_seek(void *stream, long offset, int whence) {
  VfsLockGuard guard;
  FileStream *s = validate_stream(stream);
  if (!s || s->inode_index >= VFS_MAX_INODES)
    return -1;
  int64_t base = 0;
  switch (whence) {
  case 0: base = 0; break;
  case 1: base = s->offset; break;
  case 2: base = g_inodes[s->inode_index].size; break;
  default: return -1;
  }
  const int64_t target = base + static_cast<int64_t>(offset);
  if (target < 0 || target > static_cast<int64_t>(UINT32_MAX))
    return -1;
  s->offset = static_cast<uint32_t>(target);
  return 0;
}

size_t VFS::vfs_size(void *stream) {
  VfsLockGuard guard;
  FileStream *s = validate_stream(stream);
  return s && s->inode_index < VFS_MAX_INODES
             ? g_inodes[s->inode_index].size : 0;
}

bool VFS::vfs_readable(void *stream) {
  VfsLockGuard guard;
  FileStream *s = validate_stream(stream);
  return s && s->readable;
}

bool VFS::vfs_writable(void *stream) {
  VfsLockGuard guard;
  FileStream *s = validate_stream(stream);
  return s && s->writable;
}

int VFS::vfs_mkdir(const char *pathname, int mode) {
  VfsLockGuard guard;
  (void)mode;
  if (!g_mounted)
    return -1;
  char normalized[VFS_MAX_PATH];
  if (!normalize_path(pathname, normalized) || str_equal(normalized, "/"))
    return -1;
  if (find_inode_by_path(normalized) >= 0)
    return -1;
  char parent_path[VFS_MAX_PATH];
  char dirname[VFS_MAX_PATH];
  if (!split_path(normalized, parent_path, dirname))
    return -1;
  const size_t dirname_len = str_length(dirname);
  if (dirname_len == 0 || dirname_len >= VFS_MAX_PATH ||
      dirname_len > UINT8_MAX || dirname_len + 8 > VFS_SECTOR_SIZE)
    return -1;
  const int parent_ino = find_inode_by_path(parent_path);
  if (parent_ino < 0 || g_inodes[parent_ino].flags != INODE_DIR)
    return -1;
  const int ino = allocate_inode();
  if (ino < 0)
    return -1;
  bytes_zero(&g_inodes[ino], sizeof(Inode));
  g_inodes[ino].flags = INODE_DIR;
  g_inodes[ino].links_count = 1;
  g_inode_dirty[ino] = true;
  if (!add_inode_to_dir(static_cast<uint32_t>(parent_ino),
                        static_cast<uint32_t>(ino), dirname, INODE_DIR)) {
    free_inode_bitmap(static_cast<uint32_t>(ino));
    bytes_zero(&g_inodes[ino], sizeof(Inode));
    g_inode_dirty[ino] = true;
    return -1;
  }
  return 0;
}

int VFS::vfs_remove(const char *pathname) {
  VfsLockGuard guard;
  if (!g_mounted)
    return -1;
  char normalized[VFS_MAX_PATH];
  if (!normalize_path(pathname, normalized) || str_equal(normalized, "/"))
    return -1;
  const int ino = find_inode_by_path(normalized);
  if (ino <= 0 || ino >= static_cast<int>(VFS_MAX_INODES))
    return -1;
  Inode &inode = g_inodes[ino];
  if (inode.flags == INODE_DIR && !directory_is_empty(static_cast<uint32_t>(ino)))
    return -1;
  char parent_path[VFS_MAX_PATH], name[VFS_MAX_PATH];
  if (!split_path(normalized, parent_path, name))
    return -1;
  const int parent = find_inode_by_path(parent_path);
  if (parent < 0 || !remove_inode_from_dir(static_cast<uint32_t>(parent),
                                            static_cast<uint32_t>(ino), name))
    return -1;
  if (inode.flags == INODE_FILE && inode.links_count)
    --inode.links_count;
  else
    inode.links_count = 0;
  g_inode_dirty[ino] = true;
  if (!inode_has_open_stream(static_cast<uint32_t>(ino)))
    release_inode(static_cast<uint32_t>(ino));
  return 0;
}

int VFS::vfs_rename(const char *oldpath, const char *newpath) {
  VfsLockGuard guard;
  if (!g_mounted)
    return -1;
  char old_normalized[VFS_MAX_PATH], new_normalized[VFS_MAX_PATH];
  if (!normalize_path(oldpath, old_normalized) ||
      !normalize_path(newpath, new_normalized) ||
      str_equal(old_normalized, "/") || str_equal(new_normalized, "/"))
    return -1;
  if (str_equal(old_normalized, new_normalized))
    return 0;

  const int source_ino = find_inode_by_path(old_normalized);
  if (source_ino <= 0 || source_ino >= static_cast<int>(VFS_MAX_INODES))
    return -1;
  Inode &source = g_inodes[source_ino];
  if (source.flags != INODE_FILE && source.flags != INODE_DIR)
    return -1;
  if (source.flags == INODE_DIR &&
      path_has_prefix(new_normalized, old_normalized,
                      str_length(old_normalized)))
    return -1;

  char old_parent_path[VFS_MAX_PATH], old_name[VFS_MAX_PATH];
  char new_parent_path[VFS_MAX_PATH], new_name[VFS_MAX_PATH];
  if (!split_path(old_normalized, old_parent_path, old_name) ||
      !split_path(new_normalized, new_parent_path, new_name))
    return -1;
  const int old_parent = find_inode_by_path(old_parent_path);
  const int new_parent = find_inode_by_path(new_parent_path);
  if (old_parent < 0 || new_parent < 0 ||
      g_inodes[old_parent].flags != INODE_DIR ||
      g_inodes[new_parent].flags != INODE_DIR)
    return -1;

  const int destination_ino = find_inode_by_path(new_normalized);
  if (destination_ino == source_ino)
    return 0;
  int destination_parent = -1;
  char destination_name[VFS_MAX_PATH]{};
  bool destination_unlinked = false;
  if (destination_ino >= 0) {
    if (destination_ino <= 0 || destination_ino >= static_cast<int>(VFS_MAX_INODES))
      return -1;
    Inode &destination = g_inodes[destination_ino];
    if ((source.flags == INODE_DIR) != (destination.flags == INODE_DIR) ||
        (destination.flags == INODE_DIR &&
         !directory_is_empty(static_cast<uint32_t>(destination_ino))))
      return -1;
    char destination_parent_path[VFS_MAX_PATH];
    if (!split_path(new_normalized, destination_parent_path, destination_name))
      return -1;
    destination_parent = find_inode_by_path(destination_parent_path);
    if (destination_parent < 0 ||
        !remove_inode_from_dir(static_cast<uint32_t>(destination_parent),
                               static_cast<uint32_t>(destination_ino),
                               destination_name))
      return -1;
    destination_unlinked = true;
  }

  auto restore_destination = [&]() {
    if (destination_unlinked)
      (void)add_inode_to_dir(static_cast<uint32_t>(destination_parent),
                             static_cast<uint32_t>(destination_ino),
                             destination_name,
                             static_cast<uint8_t>(g_inodes[destination_ino].flags));
  };

  if (!add_inode_to_dir(static_cast<uint32_t>(new_parent),
                        static_cast<uint32_t>(source_ino), new_name,
                        static_cast<uint8_t>(source.flags))) {
    restore_destination();
    return -1;
  }

  if (!remove_inode_from_dir(static_cast<uint32_t>(old_parent),
                             static_cast<uint32_t>(source_ino), old_name)) {
    (void)remove_inode_from_dir(static_cast<uint32_t>(new_parent),
                                static_cast<uint32_t>(source_ino), new_name);
    restore_destination();
    return -1;
  }

  if (destination_unlinked) {
    Inode &destination = g_inodes[destination_ino];
    if (destination.flags == INODE_FILE && destination.links_count)
      --destination.links_count;
    else
      destination.links_count = 0;
    g_inode_dirty[destination_ino] = true;
    if (!inode_has_open_stream(static_cast<uint32_t>(destination_ino)))
      release_inode(static_cast<uint32_t>(destination_ino));
  }
  return 0;
}

bool VFS::vfs_exists(const char *path) {
  VfsLockGuard guard;
  if (!g_mounted)
    return false;
  char normalized[VFS_MAX_PATH];
  return normalize_path(path, normalized) &&
         find_inode_by_path(normalized) >= 0;
}

bool VFS::get_file_at(uint32_t index, VFSFile *out_file) {
  VfsLockGuard guard;
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
  VfsLockGuard guard;
  return sync_internal();
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
