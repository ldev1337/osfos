#!/usr/bin/env python3
"""Create an OSFOSFS v1 disk image and optionally preload files."""
import argparse
import struct
import sys
from pathlib import Path

SECTOR = 512
MAX_SECTORS = 131072  # must match VFS_MAX_SECTORS in kernel/fs/vfs.h
MAX_ENTRIES = 256
PATH_SIZE = 112
MAGIC = b"OSFOSFS1"
VERSION = 1
FILE = 1
DIRECTORY = 2
EOC = 0xFFFFFFFF

def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h

def norm(path: str) -> str:
    path = path.replace("\\", "/")
    parts = []
    for part in path.split("/"):
        if not part or part == ".":
            continue
        if part == "..":
            if not parts:
                raise ValueError(f"path escapes root: {path}")
            parts.pop()
        else:
            try:
                part.encode("ascii")
            except UnicodeEncodeError as exc:
                raise ValueError("OSFOSFS v1 paths must be ASCII") from exc
            parts.append(part.lower())
    result = "/" + "/".join(parts)
    if result == "/":
        return result
    if len(result.encode("ascii")) >= PATH_SIZE:
        raise ValueError(f"path too long (maximum {PATH_SIZE - 1} bytes): {path}")
    return result

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="output disk image; its previous contents are destroyed")
    parser.add_argument("--size-mib", type=int, default=64, help="volume size in MiB (2..64, default: 64)")
    parser.add_argument("--add", action="append", default=[], metavar="VFS_PATH=HOST_FILE",
                        help="add a file, e.g. --add /doom1.wad=./doom1.wad (repeatable)")
    args = parser.parse_args()
    if args.size_mib < 2 or args.size_mib > 64:
        parser.error("--size-mib must be between 2 and 64")

    total = args.size_mib * 2048
    fat_start = 1
    fat_sectors = (total + 127) // 128
    dir_start = fat_start + fat_sectors
    dir_sectors = (MAX_ENTRIES * 128) // SECTOR
    data_start = dir_start + dir_sectors
    if data_start + 1 >= total:
        parser.error("image is too small for metadata")

    # Each entry is (path, byte_size, first_cluster, flags).
    entries: list[tuple[str, int, int, int]] = []
    by_path: dict[str, int] = {}
    fat = [0] * total
    for i in range(data_start):
        fat[i] = EOC
    next_free = data_start
    payloads: list[tuple[list[int], bytes]] = []

    def add_dir(path: str) -> None:
        if path == "/":
            return
        if path in by_path:
            if entries[by_path[path]][3] != DIRECTORY:
                raise ValueError(f"path already exists as a file: {path}")
            return
        parent = str(Path(path).parent).replace("\\", "/")
        if parent == ".":
            parent = "/"
        add_dir(norm(parent))
        if len(entries) >= MAX_ENTRIES:
            raise ValueError(f"directory table full (max {MAX_ENTRIES} entries)")
        by_path[path] = len(entries)
        entries.append((path, 0, 0, DIRECTORY))

    try:
        for spec in args.add:
            if "=" not in spec:
                raise ValueError(f"--add requires VFS_PATH=HOST_FILE: {spec}")
            raw_vfs_path, raw_host_path = spec.split("=", 1)
            vpath = norm(raw_vfs_path)
            host = Path(raw_host_path)
            if vpath == "/":
                raise ValueError("cannot add a file at root")
            if vpath in by_path:
                raise ValueError(f"duplicate VFS path: {vpath}")
            if not host.is_file():
                raise ValueError(f"host file does not exist: {host}")
            data = host.read_bytes()
            parent = str(Path(vpath).parent).replace("\\", "/")
            if parent == ".":
                parent = "/"
            add_dir(norm(parent))
            if len(entries) >= MAX_ENTRIES:
                raise ValueError(f"directory table full (max {MAX_ENTRIES} entries)")
            need = (len(data) + SECTOR - 1) // SECTOR
            if need > total - next_free:
                raise ValueError(f"not enough filesystem space for {vpath}")
            clusters = list(range(next_free, next_free + need))
            next_free += need
            for j, cluster in enumerate(clusters):
                fat[cluster] = clusters[j + 1] if j + 1 < len(clusters) else EOC
            first = clusters[0] if clusters else 0
            by_path[vpath] = len(entries)
            entries.append((vpath, len(data), first, FILE))
            payloads.append((clusters, data))

        # File table is 256 * 128 bytes = 64 sectors.
        directory = bytearray(dir_sectors * SECTOR)
        for i, (path, size, first, flags) in enumerate(entries):
            p = path.encode("ascii") + b"\0"
            directory[i * 128:(i + 1) * 128] = struct.pack("<112sIIII", p, size, first, flags, 0)

        fat_bytes = bytearray(fat_sectors * SECTOR)
        for i, value in enumerate(fat):
            struct.pack_into("<I", fat_bytes, i * 4, value)

        # Checksum covers the fixed 44-byte header before the checksum field.
        header_prefix = struct.pack("<8s9I", MAGIC, VERSION, SECTOR, total, fat_start,
                                    fat_sectors, dir_start, dir_sectors, data_start, MAX_ENTRIES)
        checksum = fnv1a(header_prefix)
        superblock = header_prefix + struct.pack("<I", checksum) + bytes(SECTOR - 48)
        if len(superblock) != SECTOR:
            raise AssertionError("superblock layout mismatch")

        args.image.parent.mkdir(parents=True, exist_ok=True)
        with args.image.open("wb") as image:
            image.truncate(total * SECTOR)
            image.seek(0)
            image.write(superblock)
            image.seek(fat_start * SECTOR)
            image.write(fat_bytes)
            image.seek(dir_start * SECTOR)
            image.write(directory)
            for clusters, data in payloads:
                pos = 0
                for cluster in clusters:
                    chunk = data[pos:pos + SECTOR]
                    image.seek(cluster * SECTOR)
                    image.write(chunk)
                    if len(chunk) < SECTOR:
                        image.write(bytes(SECTOR - len(chunk)))
                    pos += len(chunk)

        print(f"Created {args.image}: {args.size_mib} MiB, {len(entries)} entries, "
              f"{next_free - data_start} data sectors used, data starts at LBA {data_start}.")
        for path, size, _, flags in entries:
            if flags == FILE:
                print(f"  {path}: {size} bytes")
        return 0
    except (OSError, ValueError) as exc:
        print(f"mkofsfs: error: {exc}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    raise SystemExit(main())
