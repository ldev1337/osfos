#pragma once
#include <cstddef>
#include <cstdint>

namespace Kernel::FS {

#ifndef VFS_START_LBA
#define VFS_START_LBA 0u
#endif
#ifndef VFS_MAX_SECTORS
#define VFS_MAX_SECTORS 131072u
#endif

constexpr uint32_t VFS_SECTOR_SIZE = 512;
constexpr uint32_t VFS_MAX_PATH = 112;
constexpr uint32_t VFS_MAX_OPEN_FILES = 32;
constexpr uint32_t VFS_MAX_INODES = 1024;

// ── Sector cache configuration ──────────────────────────────────────────────
#ifndef VFS_CACHE_SLOTS
#define VFS_CACHE_SLOTS 64u
#endif

struct VFSFile {
  char name[VFS_MAX_PATH];
  uint32_t size;
  uint32_t start_lba; // Primary extent start or inode number context
  bool exists;
  uint32_t entry_index; // Inode index
  bool is_directory;
};

class VFS {
public:
  static void init();
  static bool mount();
  static bool format(uint32_t total_sectors);
  static bool is_mounted();

  static bool find_file(const char *filename, VFSFile *out_file);
  static int read_file(const VFSFile *file, void *buffer, uint32_t length,
                       uint32_t offset);

  static void *vfs_open(const char *path, const char *mode);
  static void *vfs_open(const char *path);
  static size_t vfs_read(void *buffer, size_t size, size_t count, void *stream);
  static size_t vfs_write(const void *buffer, size_t size, size_t count,
                          void *stream);
  static int vfs_close(void *stream);
  static long vfs_tell(void *stream);
  static int vfs_seek(void *stream, long offset, int whence);
  static size_t vfs_size(void *stream);
  static int vfs_mkdir(const char *pathname, int mode);
  static int vfs_remove(const char *pathname);
  static int vfs_rename(const char *oldpath, const char *newpath);
  static bool vfs_exists(const char *path);
  static bool get_file_at(uint32_t index, VFSFile *out_file);

  // Flush all deferred writes (cache, bitmaps, dirty inodes) to disk.
  static bool vfs_sync();
};

} // namespace Kernel::FS

extern "C" {
void *kernel_vfs_open(const char *path, const char *mode);
size_t kernel_vfs_read(void *ptr, size_t size, size_t nmemb, void *stream);
size_t kernel_vfs_write(const void *ptr, size_t size, size_t nmemb,
                        void *stream);
int kernel_vfs_close(void *stream);
long kernel_vfs_tell(void *stream);
int kernel_vfs_seek(void *stream, long offset, int whence);
size_t kernel_vfs_size(void *stream);
int kernel_vfs_mkdir(const char *path, int mode);
int kernel_vfs_remove(const char *path);
int kernel_vfs_rename(const char *oldpath, const char *newpath);
bool kernel_vfs_exists(const char *path);
int kernel_vfs_sync();
}