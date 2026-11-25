"""Round-trip checks for tools/extract.py and tools/nx on synthetic containers built here with random keys.

The builders below write NSP / XCI / NCA / RomFS images the way the readers expect them, so these tests catch
regressions in parsing and plumbing. They cannot prove the readers match retail dumps; that needs a real dump.

Needs the `cryptography` package (requirements.txt).
"""

import contextlib
import hashlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # noqa: E402

import extract  # noqa: E402
from nx import FormatError  # noqa: E402
from nx.nca import ctr_crypt  # noqa: E402

TITLE_ID = 0x0100AAAA00000000
VERSION = 0
GEN = 0x10  # key generation 0x10 -> master key 0x0f, so key names need two hex digits


def align(n: int, a: int) -> int:
    return -(-n // a) * a


def ecb_encrypt(key: bytes, data: bytes) -> bytes:
    enc = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return enc.update(data) + enc.finalize()


def xts_encrypt(key: bytes, data: bytes) -> bytes:
    out = b""
    for i in range(0, len(data), 0x200):
        enc = Cipher(algorithms.AES(key), modes.XTS((i // 0x200).to_bytes(16, "big"))).encryptor()
        out += enc.update(data[i:i + 0x200]) + enc.finalize()
    return out


def partition(files: dict[str, bytes], magic: bytes = b"PFS0") -> bytes:
    entry_size = 0x18 if magic == b"PFS0" else 0x40
    strings = b""
    entries = b""
    data = b""
    for name, content in files.items():
        entry = struct.pack("<QQI", len(data), len(content), len(strings))
        entries += entry.ljust(entry_size, b"\0")
        strings += name.encode() + b"\0"
        data += content
    strings = strings.ljust(align(len(strings) + 0x10, 0x20) - 0x10, b"\0")
    return magic + struct.pack("<III", len(files), len(strings), 0) + entries + strings + data


def romfs(files: dict[str, bytes]) -> bytes:
    """RomFS image with empty hash tables (the reader walks the metadata tables only)."""
    dirs: dict[str, dict] = {"": {"dirs": [], "files": []}}
    for path in files:
        parts = path.split("/")
        for depth in range(1, len(parts)):
            d, parent = "/".join(parts[:depth]), "/".join(parts[:depth - 1])
            if d not in dirs:
                dirs[d] = {"dirs": [], "files": []}
                dirs[parent]["dirs"].append(d)
        dirs["/".join(parts[:-1])]["files"].append(path)

    def name(p):
        return p.rsplit("/", 1)[-1].encode()

    dir_off, at = {}, 0
    for d in dirs:
        dir_off[d] = at
        at += 0x18 + align(len(name(d)) if d else 0, 4)
    file_off, at, data_off, data = {}, 0, {}, b""
    for p, content in files.items():
        file_off[p] = at
        at += 0x20 + align(len(name(p)), 4)
        data_off[p] = len(data)
        data += content.ljust(align(len(content), 0x10), b"\0")

    empty = 0xFFFFFFFF
    dir_table = b""
    for d, entry in dirs.items():
        parent = dir_off[d.rsplit("/", 1)[0] if "/" in d else ""]
        siblings = dirs[d.rsplit("/", 1)[0] if "/" in d else ""]["dirs"] if d else [d]
        i = siblings.index(d)
        sibling = dir_off[siblings[i + 1]] if i + 1 < len(siblings) else empty
        child_dir = dir_off[entry["dirs"][0]] if entry["dirs"] else empty
        child_file = file_off[entry["files"][0]] if entry["files"] else empty
        n = name(d) if d else b""
        dir_table += struct.pack("<6I", parent, sibling, child_dir, child_file, empty, len(n)) + n.ljust(
            align(len(n), 4), b"\0")
    file_table = b""
    for p in files:
        parent_dir = p.rsplit("/", 1)[0] if "/" in p else ""
        siblings = dirs[parent_dir]["files"]
        i = siblings.index(p)
        sibling = file_off[siblings[i + 1]] if i + 1 < len(siblings) else empty
        n = name(p)
        file_table += struct.pack("<IIQQII", dir_off[parent_dir], sibling, data_off[p], len(files[p]), empty,
                                  len(n)) + n.ljust(align(len(n), 4), b"\0")

    dir_meta = 0x50
    file_meta = dir_meta + len(dir_table)
    data_start = align(file_meta + len(file_table), 0x10)
    header = struct.pack("<10Q", 0x50, dir_meta, 0, dir_meta, len(dir_table), file_meta, 0, file_meta,
                         len(file_table), data_start)
    return (header + dir_table + file_table).ljust(data_start, b"\0") + data


BLOCK = 0x4000  # IVFC block size used by the builders


def pfs0_section(image: bytes):
    """(fs header, plain section): one SHA-256 layer (dummy) then the PFS0 at 0x200."""
    fs = bytearray(0x200)
    fs[2], fs[3], fs[4] = 1, 2, 3
    struct.pack_into("<II", fs, 0x8 + 0x20, 0x1000, 2)
    struct.pack_into("<QQQQ", fs, 0x8 + 0x28, 0, 0x20, 0x200, len(image))
    return fs, (bytes(0x200) + image).ljust(align(0x200 + len(image), 0x200), b"\0")


def romfs_section(image: bytes):
    """(fs header, plain section) with a real 3-layer IVFC tree: master hash -> level 0 hashes -> RomFS data.

    The last data block is hashed zero-padded, like retail."""
    blocks = [image[i:i + BLOCK].ljust(BLOCK, b"\0") for i in range(0, len(image), BLOCK)]
    level0 = b"".join(hashlib.sha256(b).digest() for b in blocks)
    fs = bytearray(0x200)
    fs[2], fs[3], fs[4] = 0, 3, 3
    fs[0x8:0xC] = b"IVFC"
    struct.pack_into("<III", fs, 0xC, 0x20000, 0x20, 3)
    struct.pack_into("<QQI", fs, 0x8 + 0x10, 0, len(level0), 14)
    struct.pack_into("<QQI", fs, 0x8 + 0x10 + 0x18, BLOCK, len(image), 14)
    fs[0x8 + 0xC0:0x8 + 0xE0] = hashlib.sha256(level0.ljust(BLOCK, b"\0")).digest()
    plain = level0.ljust(BLOCK, b"\0") + image
    return fs, plain.ljust(align(len(plain), 0x200), b"\0")


def bucket_tree(entries: list[bytes], starts: list[int], entry_size: int, end: int) -> tuple[bytes, bytes]:
    """(table, 0x10-byte header) of a single-level bucket tree."""
    per_set = (0x4000 - 0x10) // entry_size
    sets = [range(i, min(i + per_set, len(entries))) for i in range(0, len(entries), per_set)]
    root = struct.pack("<IIQ", 0, len(sets), end) + b"".join(struct.pack("<Q", starts[s[0]]) for s in sets)
    table = root.ljust(0x4000, b"\0")
    for n, s in enumerate(sets):
        set_end = starts[s[-1] + 1] if s[-1] + 1 < len(starts) else end
        node = struct.pack("<IIQ", n, len(s), set_end) + b"".join(entries[i] for i in s)
        table += node.ljust(0x4000, b"\0")
    return table, b"BKTR" + struct.pack("<II", 1, len(entries)) + bytes(4)


def patch_section(base_plain: bytes, new_image: bytes, shared: bytes):
    """(fs header, encrypt function) of an update RomFS section rebuilding `new_image` on top of `base_plain`.

    The bytes of `shared` come from the base section; everything else comes from the update's own data, split into
    0x40-byte indirect ranges (more than one entry set) and three AesCtrEx ranges (two generations, one plain)."""
    fs, virtual = romfs_section(new_image)
    fs[4] = 4
    v_at, b_at = virtual.find(shared), base_plain.find(shared)
    assert v_at >= 0 and b_at >= 0
    starts = sorted(set(range(0, len(virtual), 0x40)) | {v_at, v_at + len(shared)})
    indirect = []
    for o in starts:
        if v_at <= o < v_at + len(shared):
            indirect.append(struct.pack("<QQI", o, b_at + o - v_at, 0))
        else:
            indirect.append(struct.pack("<QQI", o, o, 1))
    data = virtual  # the update's own data, laid out at the virtual offsets for simplicity
    ex_starts = [0, align(len(data) // 2, 0x10), align(len(data) * 3 // 4, 0x10)]
    ex = [struct.pack("<QB3xI", ex_starts[0], 0, 1), struct.pack("<QB3xI", ex_starts[1], 1, 0),
          struct.pack("<QB3xI", ex_starts[2], 0, 2)]
    ind_table, ind_header = bucket_tree(indirect, starts, 0x14, len(virtual))
    ex_table, ex_header = bucket_tree(ex, ex_starts, 0x10, len(data))
    ind_at = align(len(data), 0x10)
    ex_at = ind_at + len(ind_table)
    struct.pack_into("<QQ", fs, 0x100, ind_at, len(ind_table))
    fs[0x110:0x120] = ind_header
    struct.pack_into("<QQ", fs, 0x120, ex_at, len(ex_table))
    fs[0x130:0x140] = ex_header

    def encrypt(start: int, key: bytes, upper: bytes) -> bytes:
        out = b""
        bounds = ex_starts + [len(data)]
        for i, (_, plain_flag, generation) in enumerate(struct.unpack("<QB3xI", e) for e in ex):
            piece = data[bounds[i]:bounds[i + 1]]
            if plain_flag:
                out += piece
            else:
                out += ctr_crypt(key, upper[:4] + generation.to_bytes(4, "big"), start + bounds[i], piece)
        out = out.ljust(ind_at, b"\0")
        tables = ind_table + ex_table
        out += ctr_crypt(key, upper, start + ind_at, tables)
        return out.ljust(align(len(out), 0x200), b"\0")

    return fs, encrypt


def nca(keys: dict[str, bytes], content_type: int, sections: list, title_key: bytes | None = None,
        title_id: int = TITLE_ID) -> bytes:
    """sections: (fs header, plain bytes) or (fs header, encrypt(start, key, upper) -> bytes)."""
    ctr_key = title_key or os.urandom(16)
    header = bytearray(0xC00)
    header[0x200:0x204] = b"NCA3"
    header[0x205] = content_type
    header[0x206] = 2
    header[0x220] = GEN
    struct.pack_into("<Q", header, 0x210, title_id)
    if title_key is not None:
        header[0x230:0x240] = rights_id(title_id)
    else:
        key_area = os.urandom(0x20) + ctr_key + os.urandom(0x10)
        header[0x300:0x340] = ecb_encrypt(keys["key_area_key_application_0f"], key_area)

    body = b""
    for i, (fs, content) in enumerate(sections):
        start = 0xC00 + len(body)
        fs = bytearray(fs)
        struct.pack_into("<H", fs, 0, 2)
        raw_upper = os.urandom(8)
        fs[0x140:0x148] = raw_upper
        upper = raw_upper[::-1]
        if callable(content):
            encrypted = content(start, ctr_key, upper)
        else:
            encrypted = ctr_crypt(ctr_key, upper, start, content)
        header[0x400 + i * 0x200:0x600 + i * 0x200] = fs
        header[0x280 + i * 0x20:0x2A0 + i * 0x20] = hashlib.sha256(fs).digest()
        struct.pack_into("<II", header, 0x240 + i * 0x10, start // 0x200, (start + len(encrypted)) // 0x200)
        body += encrypted
    struct.pack_into("<Q", header, 0x208, 0xC00 + len(body))
    return xts_encrypt(keys["header_key"], bytes(header)) + body


def rights_id(title_id: int) -> bytes:
    return struct.pack(">Q", title_id) + bytes(7) + bytes([GEN])


def ticket(keys: dict[str, bytes], rid: bytes, title_key: bytes) -> bytes:
    body = bytearray(0x180)
    body[0x40:0x50] = ecb_encrypt(keys["titlekek_0f"], title_key)
    body[0x141] = 0
    body[0x145] = GEN
    body[0x160:0x170] = rid
    return struct.pack("<I", 0x10004) + bytes(0x13C) + bytes(body)


def cnmt(program_id_hex: str, title_id: int = TITLE_ID, version: int = VERSION, meta_type: int = 0x80) -> bytes:
    header = struct.pack("<QIBBHHH", title_id, version, meta_type, 0, 0x10, 1, 0).ljust(0x20, b"\0")
    record = bytes(0x20) + bytes.fromhex(program_id_hex) + (0x1234).to_bytes(6, "little") + bytes([1, 0])
    return header + bytes(0x10) + record


EXEFS = {"main": os.urandom(5000), "main.npdm": os.urandom(300), "rtld": os.urandom(100), "sdk": os.urandom(7000),
         "subsdk0": os.urandom(33)}
ROMFS = {"Pikmin4/Content/Paks/pakchunk0.pak": os.urandom(70000),
         "Pikmin4/Content/Paks/global.utoc": os.urandom(17),
         "Pikmin4/Content/Movies/a.bin": b"",
         "Engine/x.txt": b"hello",
         "root.txt": os.urandom(1)}
PATCH_ID = TITLE_ID | 0x800
PATCH_VERSION = 65536
EXEFS_V2 = {**EXEFS, "main": os.urandom(5100)}
# The update keeps the big pak (read from the base game), changes one file and adds one.
ROMFS_V2 = {"Pikmin4/Content/Paks/pakchunk0.pak": ROMFS["Pikmin4/Content/Paks/pakchunk0.pak"],
            "Pikmin4/Content/Paks/global.utoc": os.urandom(40),
            "Pikmin4/Content/Paks/pakchunk1.pak": os.urandom(9000),
            "Engine/x.txt": b"hello again",
            "root.txt": ROMFS["root.txt"]}


def title_files(keys: dict[str, bytes], title_id: int, version: int, meta_type: int, program_sections: list,
                ) -> dict[str, bytes]:
    title_key = os.urandom(16)
    program_id = os.urandom(16).hex()
    program = nca(keys, 0, program_sections, title_key, title_id)
    kind = "Application" if meta_type == 0x80 else "Patch"
    meta = nca(keys, 1, [pfs0_section(partition({f"{kind}_{title_id:016x}.cnmt": cnmt(program_id, title_id, version,
                                                                                        meta_type)}))],
               title_id=title_id)
    rid = rights_id(title_id)
    return {program_id + ".nca": program, os.urandom(16).hex() + ".cnmt.nca": meta,
            rid.hex() + ".tik": ticket(keys, rid, title_key), rid.hex() + ".cert": b"cert"}


def game_files(keys: dict[str, bytes]) -> dict[str, bytes]:
    return title_files(keys, TITLE_ID, VERSION, 0x80, [
        pfs0_section(partition(EXEFS)), romfs_section(romfs(ROMFS)),
        pfs0_section(partition({"NintendoLogo.png": b"logo"}))])


def game_and_update_files(keys: dict[str, bytes]) -> tuple[dict[str, bytes], dict[str, bytes]]:
    base_fs, base_plain = romfs_section(romfs(ROMFS))
    base = title_files(keys, TITLE_ID, VERSION, 0x80, [pfs0_section(partition(EXEFS)), (base_fs, base_plain)])
    update = title_files(keys, PATCH_ID, PATCH_VERSION, 0x81, [
        pfs0_section(partition(EXEFS_V2)),
        patch_section(base_plain, romfs(ROMFS_V2), ROMFS["Pikmin4/Content/Paks/pakchunk0.pak"])])
    return base, update


def xci(files: dict[str, bytes], key_area: bool) -> bytes:
    root = partition({"update": partition({}, b"HFS0"), "secure": partition(files, b"HFS0")}, b"HFS0")
    header = bytearray(0x200)
    header[0x100:0x104] = b"HEAD"
    struct.pack_into("<QQ", header, 0x130, 0xF000, 0x200)
    image = bytes(header).ljust(0xF000, b"\0") + root
    return (bytes(0x1000) + image) if key_area else image


class ExtractTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)  # registered first, so it runs after the cleanups that close files
        self.dir = Path(tmp.name)
        self.keys = {"header_key": os.urandom(32), "key_area_key_application_0f": os.urandom(16),
                     "titlekek_0f": os.urandom(16)}
        self.keys_path = self.dir / "prod.keys"
        self.keys_path.write_text("".join(f"{k} = {v.hex()}\n" for k, v in self.keys.items()))
        self.hashes = self.dir / "hashes.json"
        self.hashes.write_text("{}")
        self._old_hashes = extract.HASHES
        extract.HASHES = self.hashes

    def tearDown(self):
        extract.HASHES = self._old_hashes

    def run_extract(self, game: Path, *flags: str, updates: tuple[Path, ...] = ()) -> tuple[int, str]:
        config = self.dir / "config.toml"
        work = (self.dir / "work").as_posix()
        listed = ", ".join(f"'{u.as_posix()}'" for u in updates)
        config.write_text(f"[input]\nbase = '{game.as_posix()}'\nupdates = [{listed}]\n"
                          f"keys = '{self.keys_path.as_posix()}'\n[work]\ndir = '{work}'\n")
        out = io.StringIO()
        argv = sys.argv
        sys.argv = ["extract.py", "--config", str(config), *flags]
        try:
            with contextlib.redirect_stdout(out):
                code = extract.main()
        finally:
            sys.argv = argv
        return code, out.getvalue()

    def out_dir(self, title_id: int = TITLE_ID, version: int = VERSION) -> Path:
        return self.dir / "work" / "extract" / f"{title_id:016X}" / f"v{version}"

    def assert_extracted(self):
        out = self.out_dir()
        for name, content in EXEFS.items():
            self.assertEqual((out / "exefs" / name).read_bytes(), content, name)
        for path, content in ROMFS.items():
            self.assertEqual((out / "romfs" / path).read_bytes(), content, path)
        manifest = json.loads((out / "manifest.json").read_text())
        self.assertEqual(manifest["exefs"]["main"], hashlib.sha256(EXEFS["main"]).hexdigest())
        self.assertEqual(manifest["romfs"]["files"], len(ROMFS))
        self.assertFalse(out.with_name(out.name + ".partial").exists())

    def test_nsp(self):
        game = self.dir / "game.nsp"
        game.write_bytes(partition(game_files(self.keys)))
        code, log = self.run_extract(game)
        self.assertEqual(code, 0, log)
        self.assert_extracted()

    def test_xci_with_and_without_key_area(self):
        for key_area in (False, True):
            with self.subTest(key_area=key_area):
                game = self.dir / "game.xci"
                game.write_bytes(xci(game_files(self.keys), key_area))
                code, log = self.run_extract(game, "--force")
                self.assertEqual(code, 0, log)
                self.assert_extracted()

    def test_known_hashes(self):
        game = self.dir / "game.nsp"
        game.write_bytes(partition(game_files(self.keys)))
        good = {n: hashlib.sha256(c).hexdigest() for n, c in EXEFS.items()}
        self.hashes.write_text(json.dumps({f"{TITLE_ID:016X}": {str(VERSION): good}}))
        code, log = self.run_extract(game, "--no-romfs")
        self.assertEqual(code, 0, log)
        self.assertIn("matches the known-good hashes", log)
        self.hashes.write_text(json.dumps({f"{TITLE_ID:016X}": {str(VERSION): {**good, "sdk": "0" * 64}}}))
        code, log = self.run_extract(game)
        self.assertEqual(code, 1)
        self.assertIn("MISMATCH sdk", log)

    def test_wrong_header_key_fails_without_printing_keys(self):
        game = self.dir / "game.nsp"
        game.write_bytes(partition(game_files(self.keys)))
        self.keys["header_key"] = os.urandom(32)
        self.keys_path.write_text("".join(f"{k} = {v.hex()}\n" for k, v in self.keys.items()))
        code, log = self.run_extract(game)
        self.assertEqual(code, 1)
        self.assertIn("expected NCA3", log)
        for value in self.keys.values():
            self.assertNotIn(value.hex(), log.lower())

    def test_unaligned_section_reads(self):
        files = game_files(self.keys)
        container = extract.FileStorage(self._write(partition(files)))
        self.addCleanup(container.close)
        storage = extract.read_partition(container)
        tks = extract.title_keys(storage)
        program = next(n for n in storage if n.endswith(".nca") and not n.endswith(".cnmt.nca"))
        nx_nca = extract.Nca(storage[program], extract.Keys.load(self.keys_path), tks)
        data = nx_nca.data(nx_nca.sections[1])
        image = romfs(ROMFS)
        for offset, size in ((0, 1), (1, 15), (7, 33), (0x50, 0x101), (len(image) - 3, 3)):
            self.assertEqual(data.read(offset, size), image[offset:offset + size], (offset, size))

    def write_game_and_update(self) -> tuple[Path, Path]:
        base, update = game_and_update_files(self.keys)
        base_path, update_path = self.dir / "game.xci", self.dir / "update.nsp"
        base_path.write_bytes(xci(base, False))
        update_path.write_bytes(partition(update))
        return base_path, update_path

    def test_update_romfs_is_rebuilt_on_the_base_game(self):
        base, update = self.write_game_and_update()
        code, log = self.run_extract(base, "--verify", updates=(update,))
        self.assertEqual(code, 0, log)
        out = self.out_dir(PATCH_ID, PATCH_VERSION)
        for name, content in EXEFS_V2.items():
            self.assertEqual((out / "exefs" / name).read_bytes(), content, name)
        extracted = {p.relative_to(out / "romfs").as_posix() for p in (out / "romfs").rglob("*") if p.is_file()}
        self.assertEqual(extracted, set(ROMFS_V2))
        for path, content in ROMFS_V2.items():
            self.assertEqual((out / "romfs" / path).read_bytes(), content, path)
        manifest = json.loads((out / "manifest.json").read_text())
        self.assertTrue(manifest["romfs"]["verified"])
        # Only the newest version gets a RomFS; the base game keeps its ExeFS.
        base_manifest = json.loads((self.out_dir() / "manifest.json").read_text())
        self.assertEqual(base_manifest["romfs"], f"skipped (v{PATCH_VERSION} is newer)")
        self.assertFalse((self.out_dir() / "romfs").exists())
        self.assertEqual((self.out_dir() / "exefs" / "main").read_bytes(), EXEFS["main"])

    def test_update_without_base_game_fails(self):
        _, update = self.write_game_and_update()
        code, log = self.run_extract(update)
        self.assertEqual(code, 1)
        self.assertIn(f"base game {TITLE_ID:016X} is not among the inputs", log)

    def test_verify_catches_a_bad_block(self):
        container = extract.FileStorage(self._write(partition(game_files(self.keys))))
        self.addCleanup(container.close)
        files = extract.read_partition(container)
        tks = extract.title_keys(files)
        meta = extract.content_metas(files, extract.Keys.load(self.keys_path), tks)[0]
        nx_nca = extract.program_nca(files, meta, extract.Keys.load(self.keys_path), tks)
        info = extract.romfs_section(nx_nca)
        self.assertEqual(nx_nca.verify_ivfc(info), 0)

        class Flip(extract.Storage):
            def __init__(self, inner, at):
                self.inner, self.at, self.size = inner, at, inner.size

            def read(self, offset, size):
                data = bytearray(self.inner.read(offset, size))
                if offset <= self.at < offset + size:
                    data[self.at - offset] ^= 1
                return bytes(data)

        # One flipped bit in the second data block (the data level starts at BLOCK).
        self.assertEqual(nx_nca.verify_ivfc(info, Flip(nx_nca.open_section(info), BLOCK * 2 + 5)), 1)

    def test_bad_input_is_a_format_error(self):
        container = extract.FileStorage(self._write(b"NOPE" + bytes(60)))
        self.addCleanup(container.close)
        with self.assertRaises(FormatError):
            extract.read_partition(container)

    def _write(self, data: bytes) -> Path:
        path = self.dir / f"blob{len(data)}.bin"
        path.write_bytes(data)
        return path


if __name__ == "__main__":
    unittest.main(verbosity=2)
