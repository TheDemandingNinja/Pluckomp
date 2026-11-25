# Roadmap

## Milestone 0 - Repository (done)

- Layout, license, content guard, CI, issue and pull request templates

## Milestone 1 - Input

- `tools/extract.py`: ExeFS and RomFS out of the user's NSP / XCI with their own keys
- Known-good hashes per game version in `tools/data/`
- Identify which updates exist and pick the primary target version

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
