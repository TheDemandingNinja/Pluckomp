"""XCI game card images: the card header points at a root HFS0 whose entries (update, normal, secure, logo) are
HFS0 partitions themselves. Game NCAs live in `secure`."""

import struct

from . import FormatError
from .pfs import read_partition
from .storage import Storage


def read_xci(storage: Storage) -> dict[str, dict[str, Storage]]:
    # Some dumps prepend the 0x1000-byte card key area; the header then starts 0x1000 later.
    for base in (0, 0x1000):
        if storage.size >= base + 0x200 and storage.read(base + 0x100, 4) == b"HEAD":
            break
    else:
        raise FormatError("not an XCI: no HEAD magic at 0x100 or 0x1100")
    root_offset, root_header_size = struct.unpack("<QQ", storage.read(base + 0x130, 16))
    root = read_partition(storage.sub(base + root_offset, storage.size - base - root_offset))
    return {name: read_partition(part) for name, part in root.items()}
