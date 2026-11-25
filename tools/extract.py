"""Unpack ExeFS and RomFS from the user's own NSP / XCI files into the work folder.

Reads the inputs and the keys path from config.local.toml. For every Application or Patch title found it writes

    <work>/extract/<title id>/v<version>/exefs/    main, main.npdm, rtld, sdk, subsdk*
    <work>/extract/<title id>/v<version>/romfs/    the RomFS tree (base game only; see below)
    <work>/extract/<title id>/v<version>/manifest.json

and checks the SHA-256 of each ExeFS module against tools/data/exefs_hashes.json.

An update's RomFS is a BKTR patch on top of the base RomFS and is not applied yet; its ExeFS is complete and is
extracted normally.
"""

import argparse
import hashlib
import json
import shutil
import sys
import tomllib
from pathlib import Path

from nx import FormatError
from nx.cnmt import CONTENT_PROGRAM, ContentMeta, read_cnmt
from nx.keys import Keys
from nx.nca import ENC_BKTR, FS_PFS0, FS_ROMFS, Nca
from nx.pfs import read_partition
from nx.romfs import iter_files
from nx.storage import FileStorage, Storage
from nx.ticket import read_ticket
from nx.xci import read_xci

ROOT = Path(__file__).resolve().parent.parent
HASHES = ROOT / "tools" / "data" / "exefs_hashes.json"
EXTRACTED_TYPES = ("Application", "Patch")


def open_container(storage: FileStorage) -> dict[str, Storage]:
    path = storage.path
    suffix = path.suffix.lower()
    if suffix in (".nsz", ".xcz"):
        raise FormatError(f"{path.name}: compressed {suffix} input is not supported yet; decompress it first")
    if suffix == ".xci":
        partitions = read_xci(storage)
        if "secure" not in partitions:
            raise FormatError(f"{path.name}: XCI has no secure partition")
        return partitions["secure"]
    return read_partition(storage)


def title_keys(files: dict[str, Storage]) -> dict[bytes, bytes]:
    keys = {}
    for name, storage in files.items():
        if name.lower().endswith(".tik"):
            rights_id, key = read_ticket(storage.read(0, storage.size))
            keys[rights_id] = key
    return keys


def content_metas(files: dict[str, Storage], keys: Keys, tks: dict[bytes, bytes]) -> list[ContentMeta]:
    metas = []
    for name, storage in files.items():
        if not name.lower().endswith(".cnmt.nca"):
            continue
        nca = Nca(storage, keys, tks)
        for info in nca.sections:
            for inner, data in read_partition(nca.data(info)).items():
                if inner.lower().endswith(".cnmt"):
                    metas.append(read_cnmt(data.read(0, data.size)))
    return metas


def program_nca(files: dict[str, Storage], meta: ContentMeta, keys: Keys, tks: dict[bytes, bytes]) -> Nca:
    ids = meta.contents.get(CONTENT_PROGRAM, [])
    if not ids:
        raise FormatError(f"title {meta.title_id:016X} lists no Program NCA")
    for nca_id in ids:
        if nca_id + ".nca" in files:
            return Nca(files[nca_id + ".nca"], keys, tks)
        if nca_id + ".ncz" in files:
            raise FormatError(f"{nca_id}.ncz is compressed; decompress the input first")
    raise FormatError(f"Program NCA {ids[0]}.nca is missing from the input")


def sha256_of(storage: Storage) -> str:
    h = hashlib.sha256()
    pos = 0
    while pos < storage.size:
        n = min(8 << 20, storage.size - pos)
        h.update(storage.read(pos, n))
        pos += n
    return h.hexdigest()


def extract_program(nca: Nca, out: Path, with_romfs: bool) -> dict:
    report: dict = {"exefs": {}, "romfs": "absent"}
    for info in nca.sections:
        if info.fs_type == FS_PFS0:
            pfs = read_partition(nca.data(info))
            if "main.npdm" not in pfs:
                continue  # the logo section is a PFS0 too
            (out / "exefs").mkdir(parents=True)
            for name, data in sorted(pfs.items()):
                data.copy_to(out / "exefs" / name)
                report["exefs"][name] = sha256_of(data)
        elif info.fs_type == FS_ROMFS:
            if info.encryption == ENC_BKTR:
                report["romfs"] = "update patch (BKTR), not applied yet"
            elif not with_romfs:
                report["romfs"] = "skipped (--no-romfs)"
            else:
                report["romfs"] = extract_romfs(nca.data(info), out / "romfs")
    if not report["exefs"]:
        raise FormatError("the Program NCA has no ExeFS")
    return report


def extract_romfs(storage: Storage, out: Path) -> dict:
    count = total = 0
    for path, data in iter_files(storage):
        target = out / path
        target.parent.mkdir(parents=True, exist_ok=True)
        data.copy_to(target)
        count += 1
        total += data.size
        if count % 500 == 0:
            print(f"    {count} files, {total / 2**30:.2f} GiB")
    return {"files": count, "bytes": total}


def check_hashes(meta: ContentMeta, exefs: dict[str, str], known: dict) -> bool:
    """Compare against the known-good table. Returns False on a mismatch."""
    expected = known.get(f"{meta.title_id:016X}", {}).get(str(meta.version))
    if expected is None:
        print(f"  v{meta.version} is not in {HASHES.name}; its hashes are in the manifest. "
              "Add them if this is a clean dump of a released version.")
        return True
    ok = True
    for name in sorted(set(expected) | set(exefs)):
        if expected.get(name) != exefs.get(name):
            print(f"  MISMATCH {name}: expected {expected.get(name)}, got {exefs.get(name)}")
            ok = False
    if ok:
        print(f"  ExeFS matches the known-good hashes for v{meta.version}")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", type=Path, default=ROOT / "config.local.toml")
    parser.add_argument("--no-romfs", action="store_true", help="extract ExeFS only")
    parser.add_argument("--force", action="store_true", help="extract again over a finished extraction")
    args = parser.parse_args()

    if not args.config.is_file():
        print(f"{args.config} not found: copy config.example.toml to config.local.toml and fill it in.")
        return 2
    config = tomllib.loads(args.config.read_text(encoding="utf-8"))
    inputs = [Path(config["input"]["base"])] + [Path(p) for p in config["input"].get("updates", [])]
    work = ROOT / config.get("work", {}).get("dir", "build")
    keys = Keys.load(Path(config["input"]["keys"]))
    known = json.loads(HASHES.read_text(encoding="utf-8"))

    ok = True
    for path in inputs:
        print(f"{path.name}")
        storage = None
        try:
            storage = FileStorage(path)
            files = open_container(storage)
            tks = title_keys(files)
            for meta in content_metas(files, keys, tks):
                print(f"  {meta.type_name} {meta.title_id:016X} v{meta.version}")
                if meta.type_name not in EXTRACTED_TYPES:
                    continue
                out = work / "extract" / f"{meta.title_id:016X}" / f"v{meta.version}"
                if (out / "manifest.json").is_file() and not args.force:
                    print(f"  already extracted to {out}")
                    manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
                    ok &= check_hashes(meta, manifest["exefs"], known)
                    continue
                partial = out.with_name(out.name + ".partial")
                shutil.rmtree(partial, ignore_errors=True)
                report = extract_program(program_nca(files, meta, keys, tks), partial, not args.no_romfs)
                manifest = {"title_id": f"{meta.title_id:016X}", "version": meta.version,
                            "type": meta.type_name, "source": path.name, **report}
                (partial / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
                shutil.rmtree(out, ignore_errors=True)
                partial.rename(out)
                print(f"  extracted to {out}")
                ok &= check_hashes(meta, report["exefs"], known)
        except (FormatError, OSError) as e:
            print(f"  error: {e}")
            ok = False
        finally:
            if storage is not None:
                storage.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
