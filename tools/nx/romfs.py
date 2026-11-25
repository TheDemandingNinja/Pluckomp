"""RomFS: directory and file metadata tables plus a data blob (Switchbrew, RomFS)."""

import struct
from collections.abc import Iterator

from . import FormatError
from .storage import Storage

EMPTY = 0xFFFFFFFF


def _name(table: bytes, at: int, length: int) -> str:
    name = table[at:at + length].decode("utf-8")
    if name in ("", ".", "..") or "/" in name or "\\" in name or "\0" in name or ":" in name:
        raise FormatError(f"RomFS entry has an unsafe name {name!r}")
    return name


def iter_files(storage: Storage) -> Iterator[tuple[str, Storage]]:
    """Yield (path with / separators, file bytes) for every file, depth first."""
    (_, _, _, dir_meta_off, dir_meta_size, _, _, file_meta_off, file_meta_size,
     data_off) = struct.unpack("<10Q", storage.read(0, 0x50))
    dirs = storage.read(dir_meta_off, dir_meta_size)
    files = storage.read(file_meta_off, file_meta_size)

    stack = [(0, "")]
    seen = set()
    while stack:
        dir_at, prefix = stack.pop()
        if dir_at in seen:
            raise FormatError("RomFS directory table has a cycle")
        seen.add(dir_at)
        _, _, child_dir, child_file, _, _ = struct.unpack_from("<6I", dirs, dir_at)
        file_at = child_file
        while file_at != EMPTY:
            _, sibling, offset, size, _, name_len = struct.unpack_from("<IIQQII", files, file_at)
            yield prefix + _name(files, file_at + 0x20, name_len), storage.sub(data_off + offset, size)
            file_at = sibling
        sub = child_dir
        while sub != EMPTY:
            _, sibling, _, _, _, name_len = struct.unpack_from("<6I", dirs, sub)
            stack.append((sub, prefix + _name(dirs, sub + 0x18, name_len) + "/"))
            sub = sibling
