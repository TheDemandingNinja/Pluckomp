# Building

## Toolchain (Windows)

- Python 3.12+ in `.venv` (`python -m venv .venv`, then `.venv\Scripts\pip install -r requirements.txt`)
- MSYS2 UCRT64 Clang and Ninja (`C:\msys64\ucrt64\bin`); generated C uses GNU extensions, so MSVC `cl.exe` is out
- MSYS2 CLANG64 packages for the Vulkan back end: `mingw-w64-clang-x86_64-vulkan-devel` (headers, loader import
  library), `-sdl3`, `-vulkan-memory-allocator`, `-shaderc` (`glslc`) and `-spirv-tools` (`spirv-val`); and `-libvpx`
  for the movies (VP9, linked statically from `libvpx.a`).
  `tools/build_game.py` reads their headers and libraries from `C:\msys64\clang64` (after the compiler's own headers)
  and copies `SDL3.dll` and `libiconv-2.dll` next to the executable. A Vulkan 1.3 driver is needed to run.
- .NET 10 SDK and the Visual Studio Build Tools (C++), for the NativeAOT shader translator
- Git

Linux: the same with the distribution's clang, ninja, Python and Vulkan SDK.

## One-time setup

```
git config core.hooksPath .githooks
copy config.example.toml config.local.toml
```

Edit `config.local.toml` to point at your own game dump and keys.

## Steps

1. `python tools/extract.py` - unpack ExeFS and RomFS of the base game and each update into
   `build/extract/<title id>/v<version>/`, and check the ExeFS against `tools/data/exefs_hashes.json`. The RomFS
   (11 GB) is written for the newest version only; an update's RomFS is rebuilt from the base game, so list the base
   game too. `--verify` checks the RomFS against its hash tree first, `--no-romfs` skips it. NSZ / XCZ inputs must be
   decompressed first.

2. `python recomp/gen.py` - lift `main`, `subsdk0` and `sdk` of the newest extracted version to C in
   `generated/<title id>-v<version>/`, one chunk file per few thousand functions plus a function table per module.
   `--exefs` / `--out` pick another version; `--jobs` sets the worker count (default: by free memory, about 1.5 GB
   per worker). Unchanged chunks are reused on the next run (`--no-cache` lifts all).

3. `python tools/build_game.py` - compile the generated C and `runtime/` into
   `build/<title id>-v<version>/pluckomp.exe` with clang and Ninja (MSYS2 UCRT64 when installed, else from PATH, such as
   llvm-mingw). The first build compiles about 2 GB of C and takes a while; later builds recompile what changed.
   `--jobs` sets the parallel compiles of generated code (default: by free memory).

4. `python tools/build_xlate.py` - build the shader translator `build/xlate/pluckomp_xlate.exe` from
   `tools/vendor/ryujinx` with `tools/xlate/patches/` applied to a copy under `build/xlate/`. Takes under a minute.

## Running

`pluckomp.exe` reads the ExeFS and RomFS of the version it was built from (`--exefs DIR`, `--romfs DIR` pick others)
and keeps saves under `build/profile` (`--profile DIR`). It writes its log to stderr and to `logs/pluckomp.log` next to
the executable.

- `pluckomp.exe --selftest` - layout, dispatch table and host functions, without game files
- `pluckomp.exe --probe-loader` - load and relocate the modules, check the bindings
- `pluckomp.exe --boot` - run the game to its first NVN call and exit 0
- `pluckomp.exe --frames N` - run the game to its Nth present and exit 0. A window opens; shaders are translated on
  first use (the translator must be built) and cached in `shader-cache/` next to the executable, pipelines in
  `pipeline-cache.bin` and `pipeline-keys.bin`
- `--null-gfx` - the null graphics back end: no GPU and no window, presents are logged
- `--shot PATH` - with `--frames N`, save the Nth presented picture as a PNG
- `PLUCKOMP_VK_VALIDATION=1` - the Khronos validation layer, when installed; `PLUCKOMP_TRACE=gfx` adds a line per frame
  with its submits, passes, draws recorded of those seen, clears, copies and dispatches; `PLUCKOMP_TRACE=perf` the
  frame timing every 60 presents
- `PLUCKOMP_TRACE=svc,host,ipc,fs,gfx` - log every SVC, host-function call, IPC request, file open, or the first call
  of each NVN proc

## Checks

- `python tools/check_content.py` - the content guard CI runs
- `python tests/check_content_test.py` - unit checks for the guard
- `python tests/extract_test.py` - round trips of the extractor on synthetic NSP / XCI images
- `python tools/build_shaders.py --check` - the runtime's own GLSL shaders match their committed SPIR-V headers
  (`python tools/build_shaders.py` rebuilds them after a shader edit)
- `python tests/xlate_smoke.py` - a hand-assembled fragment shader through the translator's `--serve` loop, SPIR-V
  checked with `spirv-val`

Differential tests of the lifter against Unicorn (a Cortex-A57). They need clang and, except `atomic.py`, an
extracted ExeFS (`--exefs`, default the newest version under `build/extract/`). Every lifter change passes them.

- `python tests/diff/lift.py` - single instructions sampled from the game's own code, from random states
- `python tests/diff/simd.py` - FP / AdvSIMD encodings with edge-case operands
- `python tests/diff/atomic.py` - exclusives and LSE atomics
- `python tests/diff/func.py` - whole generated functions of the game, run until they return
