"""Update (patch) RomFS: BKTR bucket trees that rebuild an update's RomFS section from the base game's section.

An update's RomFS section has encryption type 4 (AesCtrEx). Its fs header carries a patch info block (0x100) with two
bucket trees, both stored inside the section and encrypted with the section's ordinary AES-CTR counter:

- the indirect table maps each virtual range of the patched section to the base section (storage 0) or to the
  update's own data (storage 1), which is the start of the update section;
- the AesCtrEx table splits the update's own data into ranges, each encrypted with the section counter whose
  generation half (counter bytes 4..7) is replaced by the range's generation.

The patched section then has the IVFC layout described by the update's fs header. Layout from Switchbrew (NCA) and
LibHac (BSD-2), BucketTree, IndirectStorage and AesCtrCounterExtendedStorage; behaviour written from those, no code
copied.
"""

import bisect
import struct

from . import FormatError
from .storage import Storage

NODE_SIZE = 0x4000
NODE_HEADER = 0x10
INDIRECT_ENTRY = 0x14
AES_CTR_EX_ENTRY = 0x10
STORAGE_BASE, STORAGE_PATCH = 0, 1
ENCRYPTED = 0


def read_bucket_tree(storage: Storage, offset: int, size: int, header: bytes, entry_size: int) -> tuple[list[bytes], int]:
    """Return the raw entries of a single-level bucket tree, in order, and the end offset it covers."""
    magic, version, count = struct.unpack_from("<4sII", header)
    if magic != b"BKTR":
        raise FormatError(f"bucket tree magic is {magic!r}, expected BKTR")
    if count == 0:
        return [], 0
    per_set = (NODE_SIZE - NODE_HEADER) // entry_size
    sets = -(-count // per_set)
    if sets > (NODE_SIZE - NODE_HEADER) // 8:
        raise FormatError(f"bucket tree with {count} entries needs a second node level, which is not supported")
    if size < NODE_SIZE * (1 + sets):
        raise FormatError(f"bucket tree table is {size:#x} bytes, too small for {sets} entry sets")
    table = storage.read(offset, NODE_SIZE * (1 + sets))
    index, node_count, end = struct.unpack_from("<IIQ", table, 0)
    if index != 0 or node_count != sets:
        raise FormatError(f"bucket tree root lists {node_count} entry sets, expected {sets} "
                          "(wrong key or counter for the patch tables)")
    entries = []
    for s in range(sets):
        base = NODE_SIZE * (1 + s)
        set_index, n, _ = struct.unpack_from("<IIQ", table, base)
        if set_index != s or n > per_set:
            raise FormatError(f"bucket tree entry set {s} has a bad header")
        entries += [table[base + NODE_HEADER + i * entry_size:base + NODE_HEADER + (i + 1) * entry_size]
                    for i in range(n)]
    if len(entries) != count:
        raise FormatError(f"bucket tree holds {len(entries)} entries, its header says {count}")
    return entries, end


class _Ranges:
    """Sorted start offsets plus an end, for finding the entry that covers an offset."""

    def __init__(self, starts: list[int], end: int):
        if not starts or starts[0] != 0 or any(b <= a for a, b in zip(starts, starts[1:])) or end <= starts[-1]:
            raise FormatError("bucket tree entries are not sorted ranges starting at 0")
        self.starts = starts
        self.end = end

    def pieces(self, offset: int, size: int):
        """Yield (entry index, offset inside the entry's range, length) covering [offset, offset + size)."""
        i = bisect.bisect_right(self.starts, offset) - 1
        while size > 0:
            range_end = self.starts[i + 1] if i + 1 < len(self.starts) else self.end
            n = min(size, range_end - offset)
            yield i, offset - self.starts[i], n
            offset += n
            size -= n
            i += 1


class AesCtrExStorage(Storage):
    """The update's own data, decrypted range by range with each range's counter generation."""

    def __init__(self, nca: Storage, section_start: int, key: bytes, upper: bytes, entries: list[bytes], end: int):
        from .nca import ctr_read  # nca imports this module

        self._read = ctr_read
        self._nca = nca
        self._start = section_start
        self._key = key
        self._upper = upper
        parsed = [struct.unpack_from("<QB3xI", e) for e in entries]
        self._ranges = _Ranges([p[0] for p in parsed], end)
        self._entries = parsed
        self.size = end

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.size:
            raise FormatError(f"read of {size:#x} at {offset:#x} past the end of the update data ({self.size:#x})")
        out = bytearray()
        for i, inner, n in self._ranges.pieces(offset, size):
            start, encryption, generation = self._entries[i]
            absolute = self._start + start + inner
            if encryption == ENCRYPTED:
                upper = self._upper[:4] + generation.to_bytes(4, "big")
                out += self._read(self._nca, self._key, upper, absolute, n)
            else:
                out += self._nca.read(absolute, n)
        return bytes(out)


class IndirectStorage(Storage):
    """The patched section: each range comes from the base section or from the update's data."""

    def __init__(self, entries: list[bytes], end: int, base: Storage, patch: Storage):
        parsed = [struct.unpack_from("<QQI", e) for e in entries]
        for _, _, storage_index in parsed:
            if storage_index not in (STORAGE_BASE, STORAGE_PATCH):
                raise FormatError(f"indirect entry refers to storage {storage_index}")
        self._ranges = _Ranges([p[0] for p in parsed], end)
        self._entries = parsed
        self._storages = (base, patch)
        self.size = end

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.size:
            raise FormatError(f"read of {size:#x} at {offset:#x} past the end of the patched section ({self.size:#x})")
        out = bytearray()
        for i, inner, n in self._ranges.pieces(offset, size):
            _, physical, storage_index = self._entries[i]
            out += self._storages[storage_index].read(physical + inner, n)
        return bytes(out)
