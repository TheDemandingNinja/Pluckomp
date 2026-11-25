# Building

## Toolchain (Windows)

- Python 3.12+ in `.venv` (`python -m venv .venv`, then `.venv\Scripts\pip install -r requirements.txt`)
- MSYS2 UCRT64 Clang and Ninja (`C:\msys64\ucrt64\bin`); generated C uses GNU extensions, so MSVC `cl.exe` is out
- Vulkan SDK (for `glslc` and validation layers)
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

Not implemented yet:

3. `python tools/build_game.py` - compile the generated C and `runtime/` into `build/bin/`

## Checks

- `python tools/check_content.py` - the content guard CI runs
- `python tests/check_content_test.py` - unit checks for the guard
- `python tests/extract_test.py` - round trips of the extractor on synthetic NSP / XCI images

Differential tests of the lifter against Unicorn (a Cortex-A57). They need clang and, except `atomic.py`, an
extracted ExeFS (`--exefs`, default the newest version under `build/extract/`). Every lifter change passes them.

- `python tests/diff/lift.py` - single instructions sampled from the game's own code, from random states
- `python tests/diff/simd.py` - FP / AdvSIMD encodings with edge-case operands
- `python tests/diff/atomic.py` - exclusives and LSE atomics
- `python tests/diff/func.py` - whole generated functions of the game, run until they return
