"""Content meta (.cnmt inside the Meta NCA): which NCAs make up a title, and its id and version (Switchbrew, CNMT)."""

import struct
from dataclasses import dataclass

from . import FormatError

META_TYPES = {0x80: "Application", 0x81: "Patch", 0x82: "AddOnContent", 0x83: "Delta"}
CONTENT_PROGRAM = 1


@dataclass
class ContentMeta:
    title_id: int
    version: int
    meta_type: int
    contents: dict[int, list[str]]  # content type -> NCA ids (hex)

    @property
    def type_name(self) -> str:
        return META_TYPES.get(self.meta_type, f"{self.meta_type:#x}")


def read_cnmt(data: bytes) -> ContentMeta:
    if len(data) < 0x20:
        raise FormatError("CNMT is truncated")
    title_id, version, meta_type = struct.unpack_from("<QIB", data, 0)
    ext_size, count = struct.unpack_from("<HH", data, 0xE)
    contents: dict[int, list[str]] = {}
    base = 0x20 + ext_size
    for i in range(count):
        record = data[base + i * 0x38:base + (i + 1) * 0x38]
        if len(record) != 0x38:
            raise FormatError("CNMT content records are truncated")
        contents.setdefault(record[0x36], []).append(record[0x20:0x30].hex())
    return ContentMeta(title_id, version, meta_type, contents)
