"""NCA content archives: an XTS-encrypted header followed by up to four AES-CTR encrypted sections.

Layout from Switchbrew, NCA. The header uses AES-XTS with a big-endian sector number as tweak; the CTR counter of a
section is its upper counter (fs header 0x140, a little-endian u64, stored big-endian in the counter) followed by the
big-endian block index (absolute NCA offset / 16). hactool (ISC), nca.c, implements the same.
"""

import hashlib
import struct
from dataclasses import dataclass

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

from . import FormatError
from .bktr import AesCtrExStorage, IndirectStorage, read_bucket_tree, AES_CTR_EX_ENTRY, INDIRECT_ENTRY
from .keys import Keys
from .storage import Storage

HEADER_SIZE = 0xC00
SECTOR = 0x200

CONTENT_TYPES = {0: "Program", 1: "Meta", 2: "Control", 3: "Manual", 4: "Data", 5: "PublicData"}
KEY_AREA_KEYS = {0: "application", 1: "ocean", 2: "system"}

FS_ROMFS, FS_PFS0 = 0, 1
HASH_SHA256, HASH_IVFC = 2, 3
ENC_NONE, ENC_XTS, ENC_CTR, ENC_BKTR, ENC_CTR_SKIP = 1, 2, 3, 4, 5


def xts_decrypt(key: bytes, data: bytes, first_sector: int = 0) -> bytes:
    out = bytearray()
    for i in range(0, len(data), SECTOR):
        tweak = (first_sector + i // SECTOR).to_bytes(16, "big")
        dec = Cipher(algorithms.AES(key), modes.XTS(tweak)).decryptor()
        out += dec.update(data[i:i + SECTOR]) + dec.finalize()
    return bytes(out)


def ecb_decrypt(key: bytes, data: bytes) -> bytes:
    dec = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return dec.update(data) + dec.finalize()


def ctr_crypt(key: bytes, upper: bytes, offset: int, data: bytes) -> bytes:
    """AES-CTR at absolute NCA `offset` (16-byte aligned). Encryption and decryption are the same operation."""
    counter = upper + (offset >> 4).to_bytes(8, "big")
    enc = Cipher(algorithms.AES(key), modes.CTR(counter)).encryptor()
    return enc.update(data) + enc.finalize()


def ctr_read(storage: Storage, key: bytes, upper: bytes, absolute: int, size: int) -> bytes:
    """Read and decrypt `size` bytes at absolute NCA offset `absolute`, which need not be 16-byte aligned."""
    aligned = absolute & ~0xF
    end = min(-(-(absolute + size) // 16) * 16, storage.size)
    plain = ctr_crypt(key, upper, aligned, storage.read(aligned, end - aligned))
    return plain[absolute - aligned:absolute - aligned + size]


class Section(Storage):
    """The decrypted bytes of one NCA section."""

    def __init__(self, nca: Storage, start: int, size: int, encryption: int, key: bytes, upper: bytes):
        self._nca = nca
        self._start = start
        self.size = size
        self._encryption = encryption
        self._key = key
        self._upper = upper

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.size:
            raise FormatError(f"read of {size:#x} at {offset:#x} past the end of a {self.size:#x}-byte section")
        if self._encryption == ENC_NONE:
            return self._nca.read(self._start + offset, size)
        return ctr_read(self._nca, self._key, self._upper, self._start + offset, size)


@dataclass
class SectionInfo:
    index: int
    fs_type: int
    hash_type: int
    encryption: int
    start: int
    size: int
    fs_header: bytes


class Nca:
    def __init__(self, storage: Storage, keys: Keys, title_keys: dict[bytes, bytes]):
        self.storage = storage
        if storage.size < HEADER_SIZE:
            raise FormatError("NCA is smaller than its header")
        header = xts_decrypt(keys.get("header_key", 32), storage.read(0, HEADER_SIZE))
        if header[0x200:0x204] != b"NCA3":
            raise FormatError(f"NCA header magic is {header[0x200:0x204]!r} after decryption, expected NCA3 "
                              "(wrong header_key, or not an NCA)")
        self.content_type = header[0x205]
        self.program_id = struct.unpack_from("<Q", header, 0x210)[0]
        generation = max(header[0x206], header[0x220])
        master = generation - 1 if generation > 0 else 0
        rights_id = header[0x230:0x240]

        if any(rights_id):
            encrypted = title_keys.get(rights_id)
            if encrypted is None:
                raise FormatError(f"no ticket for rights id {rights_id.hex()} in the input")
            self._ctr_key = ecb_decrypt(keys.get(f"titlekek_{master:02x}"), encrypted)
        else:
            kind = KEY_AREA_KEYS.get(header[0x207])
            if kind is None:
                raise FormatError(f"unknown key area key index {header[0x207]}")
            key_area = ecb_decrypt(keys.get(f"key_area_key_{kind}_{master:02x}"), header[0x300:0x340])
            self._ctr_key = key_area[0x20:0x30]

        self.sections: list[SectionInfo] = []
        for i in range(4):
            start, end = struct.unpack_from("<II", header, 0x240 + i * 0x10)
            if start == 0 and end == 0:
                continue
            fs_header = header[0x400 + i * 0x200:0x600 + i * 0x200]
            if hashlib.sha256(fs_header).digest() != header[0x280 + i * 0x20:0x2A0 + i * 0x20]:
                raise FormatError(f"NCA section {i} header hash mismatch")
            self.sections.append(SectionInfo(i, fs_header[2], fs_header[3], fs_header[4],
                                             start * SECTOR, (end - start) * SECTOR, fs_header))

    def open_section(self, info: SectionInfo) -> Section:
        if info.encryption not in (ENC_NONE, ENC_CTR):
            names = {ENC_XTS: "XTS", ENC_BKTR: "BKTR (update patch RomFS)", ENC_CTR_SKIP: "CTR skip-layer"}
            raise FormatError(f"section {info.index}: {names.get(info.encryption, info.encryption)} "
                              "encryption is not supported")
        upper = info.fs_header[0x140:0x148][::-1]
        return Section(self.storage, info.start, info.size, info.encryption, self._ctr_key, upper)

    def patch_section(self, info: SectionInfo, base: Storage) -> Storage:
        """An update's RomFS section rebuilt on top of `base`, the base game's decrypted RomFS section."""
        if info.encryption != ENC_BKTR:
            raise FormatError(f"section {info.index} is not a patch section")
        fs = info.fs_header
        indirect_offset, indirect_size = struct.unpack_from("<QQ", fs, 0x100)
        ex_offset, ex_size = struct.unpack_from("<QQ", fs, 0x120)
        upper = fs[0x140:0x148][::-1]
        # The tables themselves use the section's ordinary counter.
        tables = Section(self.storage, info.start, info.size, ENC_CTR, self._ctr_key, upper)
        ex_entries, ex_end = read_bucket_tree(tables, ex_offset, ex_size, fs[0x130:0x140], AES_CTR_EX_ENTRY)
        if ex_end > info.size:
            raise FormatError("AesCtrEx table covers more than the section")
        patch = AesCtrExStorage(self.storage, info.start, self._ctr_key, upper, ex_entries, ex_end)
        entries, end = read_bucket_tree(tables, indirect_offset, indirect_size, fs[0x110:0x120], INDIRECT_ENTRY)
        return IndirectStorage(entries, end, base, patch)

    def verify_ivfc(self, info: SectionInfo, section: Storage | None = None, progress=None) -> int:
        """Check a RomFS section's IVFC hash tree, from the master hash in the fs header down to every data block.

        Returns the number of blocks whose hash does not match. Each level's blocks are hashed with plain SHA-256 into
        the level above; a short last block is zero-padded to the block size first (checked on retail dumps)."""
        if info.hash_type != HASH_IVFC:
            raise FormatError(f"section {info.index} has no IVFC hash tree")
        if section is None:
            section = self.open_section(info)
        hash_data = info.fs_header[0x8:0x100]
        master_size, layers = struct.unpack_from("<II", hash_data, 0x8)
        parent = hash_data[0xC0:0xC0 + master_size]
        bad = 0
        for level in range(layers - 1):
            offset, size, log2 = struct.unpack_from("<QQI", hash_data, 0x10 + level * 0x18)
            block = 1 << log2
            count = -(-size // block)
            if len(parent) < count * 32:
                raise FormatError(f"IVFC level {level} has {count} blocks but only {len(parent) // 32} hashes")
            keep = level < layers - 2  # every level but the data level holds the next level's hashes
            data = bytearray()
            per_read = max(1, (8 << 20) // block)
            for first in range(0, count, per_read):
                n = min(per_read, count - first)
                chunk = section.read(offset + first * block, min(n * block, size - first * block))
                for i in range(n):
                    piece = chunk[i * block:(i + 1) * block].ljust(block, b"\0")
                    if hashlib.sha256(piece).digest() != parent[(first + i) * 32:(first + i + 1) * 32]:
                        bad += 1
                if keep:
                    data += chunk
                elif progress:
                    progress(first + n, count)
            parent = bytes(data)
        return bad

    def data(self, info: SectionInfo, section: Storage | None = None) -> Storage:
        """The filesystem inside a section, past its hash layers: a PFS0 or a RomFS image.

        `section` replaces the decrypted section, for a patch section rebuilt by patch_section."""
        if section is None:
            section = self.open_section(info)
        hash_data = info.fs_header[0x8:0x100]
        if info.hash_type == HASH_SHA256:
            layers = struct.unpack_from("<I", hash_data, 0x24)[0]
            if not 1 <= layers <= 5:
                raise FormatError(f"section {info.index}: {layers} SHA-256 hash layers")
            offset, size = struct.unpack_from("<QQ", hash_data, 0x28 + (layers - 1) * 0x10)
        elif info.hash_type == HASH_IVFC:
            if hash_data[0:4] != b"IVFC":
                raise FormatError(f"section {info.index}: IVFC magic missing")
            layers = struct.unpack_from("<I", hash_data, 0xC)[0]
            if not 2 <= layers <= 7:
                raise FormatError(f"section {info.index}: {layers} IVFC layers")
            # The master hash counts as a layer, so the data level is entry layers - 2.
            offset, size = struct.unpack_from("<QQ", hash_data, 0x10 + (layers - 2) * 0x18)
        else:
            raise FormatError(f"section {info.index}: unknown hash type {info.hash_type}")
        return section.sub(offset, size)
