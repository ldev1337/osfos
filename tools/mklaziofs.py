#!/usr/bin/env python3
"""Create a LazioFS v3 (Extent & Inode Based) disk image and optionally preload files."""
import argparse
import struct
import sys
from pathlib import Path

SECTOR = 512
MAX_SECTORS = 131072  # matches VFS_MAX_SECTORS
VFS_MAX_INODES = 1024
MAX_EXTENTS_PER_INODE = 14
FS_MAGIC = b"LAZIOEXT"
FS_VERSION = 2
INODE_FREE = 0
INODE_FILE = 1
INODE_DIR = 2

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
                raise ValueError("LazioFS paths must be ASCII") from exc
            parts.append(part.lower())
    return "/" + "/".join(parts)

def pack_inode(flags: int, size: int, links_count: int, extents: list[tuple[int, int]]) -> bytes:
    extents_count = len(extents)
    if extents_count > MAX_EXTENTS_PER_INODE:
        raise ValueError(f"Too many extents per inode (max {MAX_EXTENTS_PER_INODE})")
    ext_flat = []
    for start_block, count in extents:
        ext_flat.extend([start_block, count])
    while len(ext_flat) < MAX_EXTENTS_PER_INODE * 2:
        ext_flat.append(0)
    
    inode_data = struct.pack(
        f"<4I{MAX_EXTENTS_PER_INODE * 2}I",
        flags,
        size,
        links_count,
        extents_count,
        *ext_flat,
    )
    assert len(inode_data) == 128
    return inode_data

def pack_dir_entry(inode_index: int, file_type: int, name: str, rec_len: int) -> bytes:
    name_bytes = name.encode("ascii")
    name_len = len(name_bytes)
    header = struct.pack("<IHBB", inode_index, rec_len, name_len, file_type)
    entry_data = header + name_bytes + b"\0"
    if len(entry_data) < rec_len:
        entry_data += b"\0" * (rec_len - len(entry_data))
    return entry_data

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="output disk image")
    parser.add_argument("--size-mib", type=int, default=64, help="volume size in MiB (default: 64)")
    parser.add_argument("--add", action="append", default=[], metavar="VFS_PATH=HOST_FILE",
                        help="add a file, e.g. --add /file.txt=./host.txt (repeatable)")
    args = parser.parse_args()

    if args.size_mib < 2 or args.size_mib > 64:
        parser.error("--size-mib must be between 2 and 64")

    total_sectors = args.size_mib * 2048

    # Layout calculations matching laziofs.cpp format()
    inode_bitmap_start = 1
    inode_bitmap_sectors = 1
    block_bitmap_start = inode_bitmap_start + inode_bitmap_sectors
    block_bitmap_sectors = (total_sectors + 4095) // 4096
    inode_table_start = block_bitmap_start + block_bitmap_sectors
    inode_table_sectors = (VFS_MAX_INODES * 128 + SECTOR - 1) // SECTOR
    data_start = inode_table_start + inode_table_sectors

    if data_start + 1 >= total_sectors:
        parser.error("image is too small for metadata")

    # Initialize bitmaps and inode table
    inode_bitmap = bytearray((VFS_MAX_INODES + 7) // 8)
    block_bitmap = bytearray((total_sectors + 7) // 8)

    # Mark metadata blocks as used in block bitmap
    for b in range(data_start):
        block_bitmap[b // 8] |= (1 << (b % 8))

    # Inode 0: Root directory
    inode_bitmap[0] |= (1 << 0)
    root_block = data_start
    block_bitmap[root_block // 8] |= (1 << (root_block % 8))

    next_free_inode = 1
    next_free_block = data_start + 1

    dir_entries: dict[str, list[tuple[str, int, int]]] = {"/": []}
    file_payloads: list[tuple[int, bytes]] = []
    inode_records: dict[int, tuple[int, int, int, list[tuple[int, int]]]] = {}

    # Root inode initial record
    inode_records[0] = (INODE_DIR, 0, 1, [(root_block, 1)])

    def ensure_dir(vpath: str) -> int:
        nonlocal next_free_inode, next_free_block
        if vpath == "/":
            return 0
        norm_vpath = norm(vpath)
        
        parent_path = str(Path(norm_vpath).parent).replace("\\", "/")
        if parent_path == "." or not parent_path.startswith("/"):
            parent_path = "/"
        ensure_dir(parent_path)

        dir_name = Path(norm_vpath).name.lower()
        for name, ino, _ in dir_entries.get(parent_path, []):
            if name == dir_name:
                return ino

        if next_free_inode >= VFS_MAX_INODES:
            raise ValueError("maximum inodes exceeded")
        
        ino = next_free_inode
        next_free_inode += 1

        dblock = next_free_block
        next_free_block += 1
        if dblock >= total_sectors:
            raise ValueError("filesystem out of space")
        
        block_bitmap[dblock // 8] |= (1 << (dblock % 8))
        inode_bitmap[ino // 8] |= (1 << (ino % 8))

        inode_records[ino] = (INODE_DIR, 0, 1, [(dblock, 1)])
        dir_entries[norm_vpath] = []
        dir_entries.setdefault(parent_path, []).append((dir_name, ino, INODE_DIR))
        return ino

    try:
        for spec in args.add:
            if "=" not in spec:
                raise ValueError(f"--add requires VFS_PATH=HOST_FILE: {spec}")
            raw_vpath, raw_host = spec.split("=", 1)
            vpath = norm(raw_vpath)
            host = Path(raw_host)
            if vpath == "/":
                raise ValueError("cannot add a file at root")
            if not host.is_file():
                raise ValueError(f"host file does not exist: {host}")
            
            data = host.read_bytes()
            parent_path = str(Path(vpath).parent).replace("\\", "/")
            if parent_path == "." or not parent_path.startswith("/"):
                parent_path = "/"
            ensure_dir(parent_path)

            if next_free_inode >= VFS_MAX_INODES:
                raise ValueError("maximum inodes exceeded")
            
            ino = next_free_inode
            next_free_inode += 1

            need_sectors = (len(data) + SECTOR - 1) // SECTOR
            if need_sectors == 0:
                need_sectors = 1

            if next_free_block + need_sectors > total_sectors:
                raise ValueError(f"not enough space for file {vpath}")

            start_blk = next_free_block
            next_free_block += need_sectors
            for b in range(start_blk, start_blk + need_sectors):
                block_bitmap[b // 8] |= (1 << (b % 8))

            inode_bitmap[ino // 8] |= (1 << (ino % 8))
            inode_records[ino] = (INODE_FILE, len(data), 1, [(start_blk, need_sectors)])
            file_payloads.append((start_blk, data))

            fname = Path(vpath).name.lower()
            dir_entries.setdefault(parent_path, []).append((fname, ino, INODE_FILE))

        # Fill directory blocks for all DIR inodes
        for ino, (flags, _, _, extents) in inode_records.items():
            if flags != INODE_DIR:
                continue
            dpath = "/"
            if ino != 0:
                for p, ents in dir_entries.items():
                    for name, i, _ in ents:
                        if i == ino:
                            found_path = str(Path(p) / name).replace("\\", "/")
                            dpath = "/" + found_path.lstrip("/")

            entries = dir_entries.get(dpath, [])
            block_buf = bytearray(SECTOR)
            offset = 0
            for idx, (name, child_ino, ftype) in enumerate(entries):
                is_last = (idx == len(entries) - 1)
                name_bytes = name.encode("ascii")
                rec_len = (8 + len(name_bytes) + 1 + 3) & ~3
                if is_last:
                    rec_len = SECTOR - offset
                if offset + rec_len > SECTOR:
                    raise ValueError(f"directory block overflow for {dpath}")
                encoded = pack_dir_entry(child_ino, ftype, name, rec_len)
                block_buf[offset:offset + len(encoded)] = encoded
                offset += rec_len

            blk = extents[0][0]
            file_payloads.append((blk, bytes(block_buf)))

        # Build Inode Table
        inode_table_bytes = bytearray(inode_table_sectors * SECTOR)
        for ino in range(VFS_MAX_INODES):
            if ino in inode_records:
                flags, size, links, extents = inode_records[ino]
                inode_bytes = pack_inode(flags, size, links, extents)
            else:
                inode_bytes = pack_inode(INODE_FREE, 0, 0, [])
            inode_table_bytes[ino * 128:(ino + 1) * 128] = inode_bytes

        # Superblock prefix (first 48 bytes)
        prefix = struct.pack(
            "<8s10I",
            FS_MAGIC,
            FS_VERSION,
            SECTOR,
            total_sectors,
            inode_bitmap_start,
            inode_bitmap_sectors,
            block_bitmap_start,
            block_bitmap_sectors,
            inode_table_start,
            inode_table_sectors,
            data_start,
        )
        assert len(prefix) == 48
        checksum = fnv1a(prefix)
        rest = struct.pack("<II", VFS_MAX_INODES, checksum)
        reserved = bytes(SECTOR - len(prefix) - len(rest))
        superblock = prefix + rest + reserved

        # Write disk image
        args.image.parent.mkdir(parents=True, exist_ok=True)
        with args.image.open("wb") as image:
            image.truncate(total_sectors * SECTOR)
            image.seek(0)
            image.write(superblock)

            image.seek(inode_bitmap_start * SECTOR)
            image.write(inode_bitmap)

            image.seek(block_bitmap_start * SECTOR)
            image.write(block_bitmap)

            image.seek(inode_table_start * SECTOR)
            image.write(inode_table_bytes)

            for blk, data in file_payloads:
                image.seek(blk * SECTOR)
                image.write(data)
                if len(data) < SECTOR:
                    image.write(bytes(SECTOR - len(data)))

        print(f"Created LazioFS v3 image {args.image}: {args.size_mib} MiB, "
              f"{next_free_inode} inodes used, {next_free_block - data_start} data sectors used.")
        return 0
    except (OSError, ValueError) as exc:
        print(f"mkosfosfs: error: {exc}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    raise SystemExit(main())