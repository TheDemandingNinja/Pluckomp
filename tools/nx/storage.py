"""Random-access byte ranges, so nested containers (an NCA inside an NSP) are read in place without copying."""

import os
from pathlib import Path

from . import FormatError

COPY_CHUNK = 8 << 20


class Storage:
    size: int

    def read(self, offset: int, size: int) -> bytes:
        raise NotImplementedError

    def sub(self, offset: int, size: int) -> "Storage":
        return SubStorage(self, offset, size)

    def copy_to(self, path: Path) -> None:
        """Write the whole range to `path` in chunks, so multi-gigabyte files never sit in memory."""
        with open(path, "wb") as out:
            pos = 0
            while pos < self.size:
                n = min(COPY_CHUNK, self.size - pos)
                out.write(self.read(pos, n))
                pos += n


class FileStorage(Storage):
    def __init__(self, path: Path):
        self._file = open(path, "rb")
        self.size = os.fstat(self._file.fileno()).st_size
        self.path = path

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.size:
            raise FormatError(f"{self.path.name}: read of {size:#x} at {offset:#x} past the end of the file")
        self._file.seek(offset)
        return self._file.read(size)

    def close(self) -> None:
        self._file.close()


class SubStorage(Storage):
    def __init__(self, parent: Storage, offset: int, size: int):
        if offset < 0 or size < 0 or offset + size > parent.size:
            raise FormatError(f"range {offset:#x}+{size:#x} lies outside its container ({parent.size:#x} bytes)")
        self._parent = parent
        self._offset = offset
        self.size = size

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.size:
            raise FormatError(f"read of {size:#x} at {offset:#x} past the end of a {self.size:#x}-byte range")
        return self._parent.read(self._offset + offset, size)
