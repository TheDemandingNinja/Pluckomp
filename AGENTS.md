# Pluckomp

Static recompilation of *Pikmin 4* (Switch, ARM64, Unreal Engine 4) into a native x86-64 program with a Vulkan
renderer. Design: [docs/architecture.md](docs/architecture.md). Toolchain, build commands and tests:
[docs/building.md](docs/building.md).

## Game content boundary

This repository is public. Game data lives outside it and every derived file stays in ignored folders.

- The user's game and key paths are in `config.local.toml` (ignored). Pass the keys file path to tools; the tools read
  it themselves. Key values stay out of your output, logs, commands and files.
- Everything extracted or generated goes under `build/` or `generated/`.
- Every file the code generator emits carries the marker defined as `GENERATED_MARKER` in `tools/check_content.py`.
- Run `python tools/check_content.py --staged` before proposing a commit. It is the same check CI runs.

## Reusing code

`refs/` holds upstream checkouts (`tools/fetch_refs.sh` recreates it). Read-only.

- Copy only MIT, BSD, ISC, zlib or Apache-2.0 code, keep its header, and add a row to `THIRD_PARTY_NOTICES.md`.
- yuzu, suyu and other GPL code: read for behaviour, write our own.

## Code

- Generated C is never edited by hand: change the generator (`recomp/`).
- Comments say what the code does and why. Cite our code by file plus function, upstream code by project, file and
  function.

## Writing

Code, comments, docs and commit messages are in English. Commit messages are plain sentences without type prefixes.
