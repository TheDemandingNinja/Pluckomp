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

## Milestone 2 - Lifter

- Bring in an MIT-licensed ARM64-to-C lifter (TotK Recomp's `recomp/`, LAS Recomp) and note it in
  `THIRD_PARTY_NOTICES.md`
- Lift every function in `main`; differential tests pass

## Milestone 3 - Boot

- Host runtime gets the game to its first frame with a null renderer (logs only)
- SVC and IPC coverage list for Pikmin 4

## Milestone 4 - Picture

- NVN on Vulkan draws the title screen

## Milestone 5 - Playable

- Input, audio, saves; first tester build through the installer
