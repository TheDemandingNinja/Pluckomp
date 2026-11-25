"""Build the shader translator: build/xlate/pluckomp_xlate.exe (Ryujinx 1.3.2 + patches, NativeAOT, SPIR-V for Vulkan).

Adapted from TotK Recomp tools/build_xlate.py @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh), without its
comparison against a MacNativeGames checkout and its Linux release options.

Copies tools/vendor/ryujinx to build/xlate/ryujinx, applies tools/xlate/patches/*.patch there (tools/vendor stays
byte-identical to upstream), then runs `dotnet publish`. Needs the .NET 10 SDK and, on Windows, the MSVC build tools
with vswhere.exe (NativeAOT links with them; docs/building.md). On Linux NativeAOT links with the system's clang.

Next to the exe it writes pluckomp_xlate.id: the translator identity, a sha256 over everything the build reads (this
script, Program.cs, the csproj, the patches, the vendor tree), so the runtime's shader cache can be keyed by it.

  python tools/build_xlate.py [--id-only] [--out DIR]
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "tools" / "vendor" / "ryujinx"
XLATE = ROOT / "tools" / "xlate"
BUILD = ROOT / "build" / "xlate"
WIN = os.name == "nt"
EXE = "pluckomp_xlate.exe" if WIN else "pluckomp_xlate"
PROJECT = "pluckomp_xlate.csproj"
VSWHERE_DIR = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer"


def tree_hashes(base: Path) -> dict[str, str]:
    """Hash every source file; src/*/obj and src/*/bin are build output and left out."""
    def build_output(rel: Path) -> bool:
        return len(rel.parts) > 2 and rel.parts[0] == "src" and rel.parts[2] in ("obj", "bin")

    return {
        p.relative_to(base).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(base.rglob("*"))
        if p.is_file() and not build_output(p.relative_to(base))
    }


def prepare_sources() -> Path:
    src = BUILD / "ryujinx"
    shutil.rmtree(src, ignore_errors=True)
    shutil.copytree(VENDOR, src)
    for junk in list(src.glob("src/*/obj")) + list(src.glob("src/*/bin")):
        shutil.rmtree(junk)
    patches = sorted((XLATE / "patches").glob("*.patch"))
    if not patches:
        sys.exit("no patches in tools/xlate/patches")
    for patch in patches:
        # git apply works outside a repository when given a directory; --check first so a failure names the patch.
        for extra in (["--check"], []):
            r = subprocess.run(
                ["git", "apply", *extra, "--whitespace=nowarn", "--directory", str(src.relative_to(BUILD)), str(patch)],
                cwd=BUILD, capture_output=True, text=True,
            )
            if r.returncode != 0:
                sys.exit(f"patch does not apply: {patch.name}\n{r.stdout}{r.stderr}")
        print(f"applied {patch.name}")
    return src


def identity() -> str:
    """sha256 over the translator's inputs, in a fixed order: name and content hash of each file."""
    h = hashlib.sha256()
    files = [Path(__file__).resolve(), XLATE / "Program.cs", XLATE / PROJECT,
             *sorted((XLATE / "patches").glob("*.patch"))]
    for f in files:
        h.update(f"{f.name}\0{hashlib.sha256(f.read_bytes()).hexdigest()}\n".encode())
    for rel, digest in tree_hashes(VENDOR).items():
        h.update(f"vendor/{rel}\0{digest}\n".encode())
    return h.hexdigest()


def write_identity(exe: Path) -> None:
    ident = identity()
    exe.with_suffix(".id").write_text(ident + "\n", encoding="ascii")
    print(f"translator identity {ident[:16]} -> {exe.with_suffix('.id')}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--id-only", action="store_true", help="write pluckomp_xlate.id for the exe already built")
    parser.add_argument("--out", type=Path, help="build directory instead of build/xlate")
    a = parser.parse_args()
    if a.out:
        global BUILD
        BUILD = a.out.resolve()
    if a.id_only:
        write_identity(BUILD / EXE)
        return
    started = time.time()
    dotnet = shutil.which("dotnet")
    if not dotnet:
        sys.exit("missing: dotnet (.NET 10 SDK)")
    env = dict(os.environ)
    if WIN:
        if not (VSWHERE_DIR / "vswhere.exe").is_file() and not shutil.which("vswhere"):
            sys.exit(f"missing: vswhere.exe (expected in {VSWHERE_DIR}); NativeAOT cannot link without it")
        env["PATH"] = str(VSWHERE_DIR) + os.pathsep + env["PATH"]

    BUILD.mkdir(parents=True, exist_ok=True)
    src = prepare_sources()
    work = BUILD / "xlate"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    for name in ("Program.cs", PROJECT):
        shutil.copy2(XLATE / name, work / name)
    out = BUILD / "publish"
    shutil.rmtree(out, ignore_errors=True)
    cmd = [
        dotnet, "publish", str(work / PROJECT), "-c", "Release", "-r", "win-x64" if WIN else "linux-x64",
        "-p:PublishAot=true", "-p:InvariantGlobalization=true", f"-p:RyuSrc={src}", "-o", str(out),
    ]
    if subprocess.run(cmd, env=env).returncode != 0:
        sys.exit("dotnet publish failed")
    exe = BUILD / EXE
    shutil.copy2(out / EXE, exe)
    write_identity(exe)
    print(f"built {exe} ({exe.stat().st_size / 1e6:.1f} MB) in {time.time() - started:.0f} s")


if __name__ == "__main__":
    main()
