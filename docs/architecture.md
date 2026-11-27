# Architecture

How the Switch build of *Pikmin 4* is meant to become a native x86-64 program. This is the plan; sections are filled
in as each stage lands.

## Differences from TotK Recomp

TotK Recomp is the model for this project, but the game is built differently:

| | Tears of the Kingdom | Pikmin 4 |
|---|---|---|
| Engine | Nintendo in-house | Unreal Engine 4 |
| Data | SARC / BFRES / BNTX, zstd | `.pak` / IoStore (`.utoc` / `.ucas`) |
| Graphics API | NVN | NVN, through UE4's Switch RHI |
| Audio | Nintendo audio renderer | Wwise on `audren:u`, `audout:u` and `hwopus` (Opus) |

The CPU half (lifting ARM64 to C) and the OS half (Horizon SVCs, IPC, `nn::*` SDK functions, NVN on Vulkan) carry
over in design. The game-specific half (patches, version tables, renderer quirks) is new work.

## Pipeline

### 1. Input (`tools/`)

- Base game and update, NSP or XCI, plus the keys of the user's console, from `config.local.toml`.
- Extract ExeFS (`main`, `sdk`, `subsdk*`) and RomFS into `build/`.
- Verify the dump by SHA-256 of each ExeFS module; record known-good hashes per game version in `tools/data/`.

### 2. CPU: lift ARM64 to C (`recomp/`)

- One C function per guest function, direct calls for `BL`, an address-keyed table per module (`<mod>_table.c`) for
  indirect branches. Guest addresses are host addresses: `recomp/layout.py` places the modules from 0x7000000000.
- Function starts come from `.eh_frame_hdr`, init arrays, symbols, relocations, `BL` targets and materialised code
  addresses, then out-of-function branch targets until a fixpoint (about 432 000 functions in v2.0.0's `main`).
- Jump tables behind `br` are resolved statically into `switch` statements; flags, X registers and the most used V
  registers live in C locals.
- FP / AdvSIMD lowers to portable C (`runtime/simd.h`), bit-exact to the A57 including FPCR rounding and NaNs.
- Every emitted file carries `GENERATED_MARKER` so the content guard keeps it out of git.
- Differential tests against Unicorn (`tests/diff/`, test-only, never linked into the game).
- Unlike TotK, Pikmin 4's `main` imports `setjmp` / `longjmp` and the C++ exception runtime; generated code trusts
  AAPCS64 callee-saved registers across calls (`recomp/regs.py`), which the runtime must uphold for those paths.

### 3. Host (`runtime/`)

- The core of TotK Recomp's host runtime, without its TotK-only parts: the loader maps the NSOs at the layout bases,
  links them and binds host overrides by symbol (`loader.c`, `hostfn.c`); guest memory, threads on virtual cores,
  synchronisation and SVCs (`memory.c`, `kernel.c`, `svc.c`); in-process IPC with `sm:` (`ipc.c`); OS calls behind
  `runtime/platform/`.
- The OS boundary is the SDK's `nn::*` functions: `fs_host.c`, `romfs.c`, `ov_system.c`, `ov_services.c`,
  `ov_hid.c` and `ov_movie.c` answer them on the host, and the Horizon services behind them are never opened. Audio is
  the exception: Wwise drives `nn::audio` and `nn::codec`, whose SDK clients are thin protocol layers, so their
  services run in process (`audren.c` with the silent mixer `audio_null.c`, `audout.c`, `hwopus.c`; CMIF domains in
  `ipc.c`). What is reached and how it is answered: [coverage.md](coverage.md).
- File system serves the extracted RomFS directory in place, so the engine's pak loader reads the user's own files.
- `boot.c` runs the game's `nn::init::Start` with rtld's three callbacks, the second of which runs every module's
  constructors, and ends the checkpoint modes: `--boot` at the first NVN call, `--frames N` at the Nth present.
- Window, input and audio output through SDL3 (later milestones).

### 4. Graphics (`runtime/`)

- NVN front end from TotK Recomp (`nvn_api_core.c`, `nvn_api_obj.c`, `nvn_api_cmd.c`, `nvn_ext.c`, `nvn_procs.c`):
  `nvnBootstrapLoader` and `nvnDeviceGetProcAddress` hand out synthetic host addresses for every proc; objects live in
  the game's own NVN object memory; command buffers record into host op streams that a back end replays at submit.
- Back ends implement `GfxBackend` (`gfx.h`); `gfx_backend()` picks Vulkan, or the null one with `--null-gfx`.
- Null back end (`gfx_null.c`): no GPU, every submit completes at once with its CPU-visible side effects (event
  signals, counter reports, buffer copies and fills, uniform updates, fence syncs), and each present is logged with its
  frame number, size and format.
- Vulkan back end, TotK Recomp's without its TotK-only parts (`vk_*.c`; Vulkan 1.3, VMA for memory):
  - Memory (`vk_memory.c`): CPU-visible pools are guest memory imported as Vulkan buffers
    (`VK_EXT_external_memory_host`); on a discrete GPU pools used as buffers get a device-local mirror kept in sync
    from the write watch; physical pools are device memory, virtual pools a page table over them.
  - Textures (`vk_texture.c`): images keyed by storage address and descriptor, uploaded on first use and again when
    their pages were written; block-linear storage is deswizzled by a compute shader (`shaders/bl_deswizzle.comp`), ASTC
    decoded by another (`shaders/astc_decode.comp`) where the GPU does not sample it. GPU-written targets go back to
    their storage when the CPU reads it or waits for their fence.
  - Replay (`vk_exec.c`, `vk_exec_draw.c`, `vk_exec_xfer.c`): each submission's op streams are recorded into Vulkan
    command buffers with dynamic rendering; draws resolve their shaders through the translator and their pipelines
    through `vk_pipeline.c` (graphics pipeline libraries where the driver fast-links them; pipelines of earlier runs are
    rebuilt at boot from `pipeline-keys.bin`). Uniform ranges are snapshotted at submit (`vk_usnap.c`). A program is
    bound per stage (`vk_exec_draw.c` bind_program): Pikmin 4's UE4 binds its vertex and fragment programs apart.
    What UE4 uses beyond TotK: texture handles the translator packs from a separate texture and a separate sampler
    (`vk_exec_draw.c` sep_handle), buffer textures read through texel buffer views (texel_view), and a geometry stage
    (its volume colour-grading LUT), built as a full pipeline without the pipeline-library path.
  - Submission and fences (`vk_sync.c`): one timeline semaphore; its values are the NVN fence values. The recorder
    thread of TotK Recomp is not ported: `vk_rec.h` makes every Vulkan call on the replaying thread.
  - Window and present (`gfx_window_sdl.c`, `gfx_present.c`): an SDL3 window on its own thread, a swapchain the game's
    window texture is blitted into (letterboxed), presented at the pacer's time by a present thread (`vk_backend.c`).
- Shader cache (`gfx_shader.c`): the translator runs as child processes on first use of each (program, answers) pair;
  SPIR-V and reflection are kept under `shader-cache/` next to the executable, keyed by `pluckomp_xlate.id`.
- Shaders: Ryujinx 1.3.2's translator (`tools/vendor/ryujinx`, unmodified) with TotK Recomp's generic patches
  (`tools/xlate/patches/`; its shadow-filter and motion-vector patches are left out), built with .NET NativeAOT as
  `pluckomp_xlate.exe` and run as a separate process that answers one job per line (`tools/xlate/Program.cs`). It
  writes SPIR-V for Vulkan and a `res.json` with the bindings. `pluckomp_xlate.id` hashes its inputs to key a shader
  cache.

### 5. Installer (`installer/`)

- Takes the user's files, runs extraction, generation and the C build on their machine, and writes the game into the
  install folder. Nothing game-derived ever ships in a release.
