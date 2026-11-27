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
- [x] NVN front end from TotK Recomp with a null back end (logs only): `--frames N` runs to the Nth present (600 pass)
- [x] Audio services the SDK reaches on the way (`audren:u`, `audout:u`, `hwopus`), silent; CMIF domains in `ipc.c`
- [x] A host movie decoder: VP9 with libvpx into the game's texture (`runtime/ov_movie.c`, [coverage.md](coverage.md))

## Milestone 4 - Picture

NVN on Vulkan draws the title screen. The Vulkan back end and shader translator of TotK Recomp come over without
their TotK-only parts (DLAA, FSR, frame generation, HDR, motion vectors, contact shadows, colour grading, resolution
patches, frame-ahead sync).

- [x] Shader translator: Ryujinx 1.3.2's Maxwell-to-SPIR-V translator with TotK Recomp's generic patches, built with
  .NET NativeAOT (`tools/build_xlate.py`, smoke test `tests/xlate_smoke.py`)
- [x] Window and swapchain (SDL3), present of the game's window texture
- [x] Vulkan device, memory pools, textures (block-linear deswizzle, ASTC), samplers, sync and fences
- [x] Replay of the recorded NVN command streams: transfers, clears, draws and dispatches, pipelines and the shader
  cache (`runtime/gfx_shader.c`); programs bound per stage, as UE4 binds them
- [x] `--frames N` with the Vulkan back end (600 pass, every draw recorded, no shader failures), `--null-gfx`, and
  `--shot PATH`
- [x] One connected controller without input (`runtime/ov_hid.c`), so the controller applet returns at once
- [x] Boot to the title state: the boot sequence used to wait forever in `WaitPreSyncBCAT` after the startup logo, a
  race in the game's BCAT pre-sync thread that a nifm request resolving at once exposed; a request now stays on hold for
  100 ms (`runtime/ov_services.c`, docs/coverage.md), and every run reaches `OnMenuTitle` / `MenuTitle` at frame 55
- [x] The title screen drawn (`--frames 600 --shot`): the background movie decoded on the host with libvpx
  (`runtime/ov_movie.c`); textures whose handle the translator packs from a separate texture and a separate sampler,
  which bound the zero texture before (every UE4 draw: the picture was black from the startup logo on); buffer textures
  through texel buffer views (UE4's vertex data); and a geometry stage for UE4's volume colour-grading LUT
- [ ] Colour banding in the title's smooth gradients (sky, fog): steps where the console shows none, cause not found
- [ ] Vulkan validation layer runs; the recorder thread (`vk_rec.c`)

## Milestone 5 - Playable

- Input into the connected controller, audio output, saves; first tester
  build through the installer
