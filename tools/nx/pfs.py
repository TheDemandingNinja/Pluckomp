"""PFS0 (NSP, ExeFS) and HFS0 (XCI partitions): a flat table of named byte ranges."""

import struct

from . import FormatError
from .storage import Storage

ENTRY_SIZES = {b"PFS0": 0x18, b"HFS0": 0x40}


def read_partition(storage: Storage) -> dict[str, Storage]:
    magic = storage.read(0, 4)
    entry_size = ENTRY_SIZES.get(magic)
    if entry_size is None:
        raise FormatError(f"expected a PFS0 or HFS0 partition, found magic {magic!r}")
    count, strings_size = struct.unpack("<II", storage.read(4, 8))
    if count > 0x10000:
        raise FormatError(f"{magic.decode()} claims {count} entries")
    table = storage.read(0x10, count * entry_size + strings_size)
    strings = table[count * entry_size:]
    data_start = 0x10 + len(table)
    files = {}
    for i in range(count):
        offset, size, name_offset = struct.unpack_from("<QQI", table, i * entry_size)
        end = strings.find(b"\0", name_offset)
        name = strings[name_offset:end if end >= 0 else len(strings)].decode("utf-8")
        files[name] = storage.sub(data_start + offset, size)
    return files
