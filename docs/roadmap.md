# Roadmap

## Milestone 0 - Repository (done)

- Layout, license, content guard, CI, issue and pull request templates

## Milestone 1 - Input

- [x] `tools/extract.py`: ExeFS and RomFS out of the user's NSP / XCI with their own keys
- [x] Run against a real dump (base XCI v0 with bundled v1.0.1, update NSP v2.0.0)
- [x] Known-good ExeFS hashes per game version in `tools/data/exefs_hashes.json`
- [x] Update RomFS: apply BKTR patches on top of the base RomFS (v2.0.0 passes its full IVFC hash check)
- [ ] NSZ / XCZ input
- [ ] Identify which updates exist and pick the primary target version

## Milestone 2 - Lifter (done)

- [x] TotK Recomp's `recomp/` (which builds on LAS Recomp) in `recomp/`, without its TotK-only patch, plugin,
  spin-wait and hook machinery; noted in `THIRD_PARTY_NOTICES.md`
- [x] Lift every function in `main`, `subsdk0` and `sdk` of v2.0.0
- [x] Differential tests against Unicorn pass on Pikmin 4's own code (`tests/diff/`)
- [ ] Callee-saved registers across setjmp / longjmp and C++ exceptions, which Pikmin 4's `main` uses
  (recomp/regs.py; needs the runtime of Milestone 3)

## Milestone 3 - Boot

- [x] `tools/build_game.py`: compile the generated C and `runtime/` into `pluckomp.exe`
- [x] Host runtime core from TotK Recomp: loader and dynamic linking, guest memory, kernel, SVCs, in-process IPC,
  nn::fs with the RomFS served from the extracted directory, system service overrides, crash reporter
- [x] `--boot` reaches the first NVN call (`nvnBootstrapLoader`) through all module constructors, nnMain and UE4's
  engine pre-init
- [x] SVC and IPC coverage list for Pikmin 4 ([coverage.md](coverage.md))
- [ ] NVN front end with a null back end (logs only), so the game runs on to its first present

## Milestone 4 - Picture

- NVN on Vulkan draws the title screen

## Milestone 5 - Playable

- Input, audio, saves; first tester build through the installer
