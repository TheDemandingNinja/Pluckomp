"""Compile the generated C of one game version and link it with runtime/ into build/<version>/pluckomp.exe.

Writes build/<version>/build.ninja and runs Ninja on it. Ninja keeps the build incremental: a chunk is recompiled
only when it, a header it includes or its flags change.

    python tools/build_game.py                    # newest generated/<title>-v<version> -> build/<title>-v<version>/
    python tools/build_game.py --jobs 8
    python tools/build_game.py --runtime-only     # compile runtime/ only (no link): a quick syntax check

build/<version>/modules.c carries the module layout, the linked function tables (runtime/dispatch.h), the expected
SHA-256 of each ExeFS file (tools/data/exefs_hashes.json) and the default ExeFS and RomFS paths, which pluckomp.exe
uses unless its own command line names others.

Compile jobs share a memory budget: the default job count is the smaller of the CPU count and
(free RAM - 2 GiB) / JOB_MEM.

Design and flags from TotK Recomp tools/build_game.py (MIT); see THIRD_PARTY_NOTICES.md. Standard library only.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_shaders import stale as stale_shaders  # noqa: E402
from check_content import GENERATED_MARKER  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
WIN = os.name == 'nt'
EXE = 'pluckomp.exe' if WIN else 'pluckomp'
UCRT = Path(r'C:\msys64\ucrt64\bin')
# The Vulkan headers and loader import library, SDL3, the Vulkan Memory Allocator header, glslc and spirv-val come from
# MSYS2 CLANG64 (packages mingw-w64-clang-x86_64-vulkan-devel, -sdl3, -vulkan-memory-allocator, -shaderc,
# -spirv-tools). Its headers are searched after the compiler's own (-idirafter), so its C library headers never
# replace those of the compiler in use.
CLANG64 = Path(r'C:\msys64\clang64')
# DLLs pluckomp.exe loads from its own folder: SDL3 and the libiconv it imports.
RUNTIME_DLLS = [CLANG64 / 'bin' / 'SDL3.dll', CLANG64 / 'bin' / 'libiconv-2.dll'] if WIN else []

# Flags for generated code as TotK Recomp builds it (LAS Recomp's GENFLAGS plus -march for the FP/SIMD lowering).
GEN_FLAGS = ['-O1', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-fvisibility=hidden',
             '-march=x86-64-v3']
RT_FLAGS = ['-O2', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing', '-march=x86-64-v3', '-std=gnu11']
if WIN:
    RT_FLAGS.append('-D_WIN32_WINNT=0x0A00')   # Windows 10 APIs (CPU sets, stack limits); llvm-mingw defaults lower
LINK_FLAGS = ['-fuse-ld=lld'] + (['-Wl,--stack,8388608'] if WIN else ['-pthread'])
CXX_FLAGS = ['-O2', '-g', '-march=x86-64-v3', '-std=c++17', '-Wno-nullability-completeness']   # vk_vma.cpp only
if WIN:
    RT_FLAGS += ['-idirafter', str(CLANG64 / 'include'), '-idirafter', str(CLANG64 / 'include' / 'vma')]
    CXX_FLAGS += ['-idirafter', str(CLANG64 / 'include'), '-idirafter', str(CLANG64 / 'include' / 'vma')]
    LINK_FLAGS += ['-L', str(CLANG64 / 'lib')]
LINK_LIBS = (['-lbcrypt', '-lsynchronization', '-lntdll', '-lwinmm', '-lvulkan-1', '-lSDL3', '-luser32', '-lgdi32',
              '-lshell32', '-lole32', '-Wl,-Bstatic', '-lc++', '-Wl,-Bdynamic'] if WIN
             else ['-lvulkan', '-lSDL3', '-l:libstdc++.a', '-lm', '-ldl'])
# Peak commit of one generated-chunk compile at -O1 (TotK Recomp's measurement: 120-180 MiB per ~3 MiB chunk, more for
# the function tables), with headroom.
JOB_MEM = 400 * 2**20


def tool(name):
    """A tool from MSYS2 UCRT64 when it is there, else from PATH."""
    exe = name + ('.exe' if WIN else '')
    if WIN and (UCRT / exe).exists():
        return str(UCRT / exe)
    found = shutil.which(name)
    if not found:
        sys.exit(f'{name} not found: install it (docs/building.md) or put it on PATH')
    return found


def ninja_path(p):
    return str(p).replace('\\', '/').replace(':', '$:').replace(' ', '$ ')


def cmd_path(p):
    s = str(p).replace('\\', '/')
    return f'"{s}"' if ' ' in s else s


def c_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def newest_generated():
    gens = sorted((p for p in (ROOT / 'generated').glob('*-v*') if (p / 'layout.json').is_file()),
                  key=lambda p: int(p.name.rsplit('-v', 1)[1]))
    if not gens:
        sys.exit('no generated/<title>-v<version>/ with layout.json: run recomp/gen.py first')
    return gens[-1]


def read_layout(gen):
    data = json.loads((gen / 'layout.json').read_text())
    return [(m['name'], int(m['base'], 16), int(m['size'], 16)) for m in data['modules']]


def chunk_files(gen, mod):
    """The C files of a module, in the order of its manifest: chunks, then the table."""
    man = json.loads((gen / mod / 'manifest.json').read_text())
    files = [gen / mod / f for f in man['chunks']] + [gen / mod / f'{mod}_table.c']
    missing = [f for f in files if not f.is_file()]
    if missing:
        sys.exit(f'{mod}: {len(missing)} generated files missing, first {missing[0]}: run recomp/gen.py again')
    return files


def exefs_hashes(title, version):
    """{file: sha256} of the ExeFS this build expects, from tools/data/exefs_hashes.json."""
    data = json.loads((ROOT / 'tools' / 'data' / 'exefs_hashes.json').read_text())
    return data.get(title, {}).get(str(version), {})


def module_glue(layout, linked, hashes, paths):
    out = [f'// Module glue written by tools/build_game.py; do not edit. {GENERATED_MARKER}',
           '#include "dispatch.h"', '#include "host.h"', '#include "layout.h"', '']
    for name, base, size in layout:
        u = name.upper()
        out.append(f'_Static_assert(MOD_{u}_BASE == {base:#x}ull && MOD_{u}_SIZE == {size:#x}ull, '
                   f'"layout.h and layout.json disagree on {name}");')
    out += ['', 'const RtLayoutEntry rt_layout[] = {']
    out += [f'    {{"{n}", MOD_{n.upper()}_BASE, MOD_{n.upper()}_SIZE}},' for n, _, _ in layout]
    out += ['};', f'const int rt_layout_count = {len(layout)};', '']
    for name in linked:
        out += [f'extern const uint64_t {name}_fn_addrs[];', f'extern const GuestFn {name}_fn_ptrs[];',
                f'extern const uint64_t {name}_fn_count;']
    out += ['', 'int rt_linked_modules(RtModule *out, int max) {', '    int n = 0;']
    for name in linked:
        u = name.upper()
        out.append(f'    if (n < max) out[n++] = (RtModule){{"{name}", MOD_{u}_BASE, MOD_{u}_SIZE, {name}_fn_addrs, '
                   f'{name}_fn_ptrs, {name}_fn_count}};')
    out += ['    return n;', '}', '', 'const RtFileHash rt_exefs_sha256[] = {']
    out += [f'    {{{c_string(f)}, {c_string(h)}}},' for f, h in sorted(hashes.items())]
    out += ['    {0, 0}', '};', f'const int rt_exefs_sha256_count = {len(hashes)};', '']
    for name in ('exefs', 'romfs'):
        out.append(f'const char rt_default_{name}[] = {c_string(paths[name].as_posix())};')
    version = (ROOT / 'VERSION').read_text().strip() if (ROOT / 'VERSION').is_file() else 'unknown'
    out += [f'const char rt_port_version[] = {c_string(version)};', '']
    return '\n'.join(out)


def runtime_sources():
    other = 'linux.c' if WIN else 'win32.c'
    return sorted(p for p in list(RUNTIME.rglob('*.c')) + list(RUNTIME.glob('*.cpp'))
                  if p.name != other and 'third_party' not in p.parts and not p.name.endswith('_test.c'))


def write_if_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8')


def ninja_file(gen, out, mods, depth, cc, cxx, runtime_only):
    inc = f'-I {cmd_path(RUNTIME)} -I {cmd_path(gen)}'
    lines = [
        '# Written by tools/build_game.py; do not edit.',
        'ninja_required_version = 1.10',
        f'builddir = {ninja_path(out)}',
        f'cc = {cmd_path(cc)}',
        f'genflags = {" ".join(GEN_FLAGS)} {inc}',
        f'rtflags = {" ".join(cmd_path(f) for f in RT_FLAGS)} {inc}',
        f'cxx = {cmd_path(cxx)}',
        f'cxxflags = {" ".join(cmd_path(f) for f in CXX_FLAGS)} {inc}',
        f'ldflags = {" ".join(cmd_path(f) for f in LINK_FLAGS)}',
        f'libs = {" ".join(LINK_LIBS)}',
        '', 'pool gen', f'  depth = {depth}', '',
        'rule cc_gen', '  command = $cc $genflags -MD -MF $out.d -c $in -o $out', '  deps = gcc',
        '  depfile = $out.d', '  description = CC $in', '',
        'rule cc_rt', '  command = $cc $rtflags -MD -MF $out.d -c $in -o $out', '  deps = gcc',
        '  depfile = $out.d', '  description = CC $in', '',
        'rule cxx_rt', '  command = $cxx $cxxflags -MD -MF $out.d -c $in -o $out', '  deps = gcc',
        '  depfile = $out.d', '  description = CXX $in', '',
        'rule link', '  command = $cc $ldflags @$out.rsp $libs -o $out', '  rspfile = $out.rsp',
        '  rspfile_content = $in_newline', '  description = LINK $out', '',
    ]
    gen_objs, rt_objs = [], []
    if not runtime_only:
        for mod, files in mods.items():
            for f in files:
                o = out / 'obj' / mod / (f.stem + '.o')
                lines += [f'build {ninja_path(o)}: cc_gen {ninja_path(f)}', '  pool = gen']
                gen_objs.append(o)
    for src in runtime_sources() + ([] if runtime_only else [out / 'modules.c']):
        rel = src.relative_to(RUNTIME) if src.is_relative_to(RUNTIME) else Path(src.name)
        o = out / 'obj' / 'runtime' / rel.with_suffix('.o')
        rule = 'cxx_rt' if src.suffix == '.cpp' else 'cc_rt'
        lines.append(f'build {ninja_path(o)}: {rule} {ninja_path(src)}')
        rt_objs.append(o)
    every = gen_objs + rt_objs
    lines.append('build runtime: phony ' + ' '.join(ninja_path(o) for o in rt_objs))
    if runtime_only:
        lines += ['default runtime', '']
    else:
        exe = out / EXE
        lines += [f'build {ninja_path(exe)}: link ' + ' '.join(ninja_path(o) for o in every),
                  'build generated: phony ' + ' '.join(ninja_path(o) for o in gen_objs),
                  f'default {ninja_path(exe)}', '']
    return '\n'.join(lines)


def free_ram():
    if WIN:
        import ctypes

        class MemStatus(ctypes.Structure):
            _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong),
                        ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                        ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                        ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                        ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
        st = MemStatus()
        st.dwLength = ctypes.sizeof(st)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st))
        return st.ullAvailPhys
    return os.sysconf('SC_AVPHYS_PAGES') * os.sysconf('SC_PAGE_SIZE')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--gen', type=Path, help='generated C directory (default: the newest generated/<title>-v<version>)')
    ap.add_argument('--out', type=Path, help='build directory (default build/<title>-v<version>)')
    ap.add_argument('--exefs', type=Path, help='default ExeFS directory baked into the exe')
    ap.add_argument('--romfs', type=Path, help='default RomFS directory baked into the exe')
    ap.add_argument('--jobs', type=int, help='parallel generated-code compiles (default: by free memory)')
    ap.add_argument('--runtime-only', action='store_true', help='compile runtime/ only, no generated code, no link')
    ap.add_argument('--keep-going', action='store_true', help='build what does not depend on a failed step')
    ap.add_argument('--target',help='ninja target to build instead of the default (generated, runtime)')
    args = ap.parse_args()

    gen = (args.gen or newest_generated()).resolve()
    title, version = gen.name.rsplit('-v', 1)
    out = (args.out or ROOT / 'build' / gen.name).resolve()
    layout = read_layout(gen)
    mods = {name: chunk_files(gen, name) for name, _, _ in layout} if not args.runtime_only else {}
    extract = ROOT / 'build' / 'extract' / title / f'v{version}'
    paths = {'exefs': (args.exefs or extract / 'exefs').resolve(), 'romfs': (args.romfs or extract / 'romfs').resolve()}
    if not args.runtime_only:
        write_if_changed(out / 'modules.c', module_glue(layout, list(mods), exefs_hashes(title, version), paths))
    cpus = os.cpu_count() or 4
    jobs = args.jobs or max(1, min(cpus, int((free_ram() - 2 * 2**30) // JOB_MEM)))
    cc, cxx = tool('clang'), tool('clang++')
    stale = stale_shaders(RUNTIME)
    if stale:
        sys.exit(f'{stale[0]}: its SPIR-V header no longer matches; run python tools/build_shaders.py')
    write_if_changed(out / 'build.ninja', ninja_file(gen, out, mods, jobs, cc, cxx, args.runtime_only))
    cmd = [tool('ninja'), '-C', str(out), '-j', str(max(jobs, cpus))]
    if args.keep_going:
        cmd += ['-k', '0']
    if args.target:
        cmd.append(args.target)
    print(f'build: {gen.name} -> {out} ({jobs} generated-code jobs, {cc})', flush=True)
    rc = subprocess.call(cmd)
    if rc == 0 and not args.runtime_only:
        for dll in RUNTIME_DLLS:   # loaded from the exe's folder; copied when missing or changed
            dst = out / dll.name
            if not dll.is_file():
                sys.exit(f'{dll} not found: install the MSYS2 CLANG64 packages (docs/building.md)')
            if not dst.is_file() or dst.stat().st_size != dll.stat().st_size or dst.stat().st_mtime < dll.stat().st_mtime:
                shutil.copy2(dll, dst)
    sys.exit(rc)


if __name__ == '__main__':
    main()
