# Contributing

## The one hard rule

Nothing that came out of the game goes into the repository: no game files, keys, extracted data, recompiled C,
compiled binaries, shaders, shader caches, fonts, audio, movies or Unreal `.pak` / `.utoc` / `.ucas` / `.uasset`
files. Not in commits, not in issues, not in pull requests. Only information *about* the game goes in: addresses,
hashes, patch code we wrote, shader identifiers.

Two checks enforce this:

- CI runs `tools/check_content.py` on every push and pull request.
- The same check runs before each local commit once you enable the hook:

  ```
  git config core.hooksPath .githooks
  ```

## Issues

Use the issue templates. Attach logs whole; never attach game files, keys, firmware or builds. Issues that do are
closed.

## Pull requests

- Target `dev` once it exists; `main` until then.
- Fill in the pull request template: the problem, the change, where it reproduces, proof, and the checks you ran.
- One change per pull request.
