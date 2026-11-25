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
   `build/extract/<title id>/v<version>/`, and check the ExeFS against `tools/data/exefs_hashes.json`. `--no-romfs`
   skips the multi-gigabyte RomFS. Updates' RomFS (a BKTR patch on the base RomFS) is not applied yet; NSZ / XCZ
   inputs must be decompressed first.

Not implemented yet:

2. `python recomp/gen.py` - lift `main` / `sdk` / `subsdk*` to C in `generated/`
3. `python tools/build_game.py` - compile the generated C and `runtime/` into `build/bin/`

## Checks

- `python tools/check_content.py` - the content guard CI runs
- `python tests/check_content_test.py` - unit checks for the guard
- `python tests/extract_test.py` - round trips of the extractor on synthetic NSP / XCI images
