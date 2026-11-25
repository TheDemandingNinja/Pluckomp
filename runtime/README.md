# runtime/

The host runtime: kernel, IPC, file system, system services, the NVN front end with a null back end, silent audio
services and, later, input, audio output and the NVN-on-Vulkan renderer.
Most of it comes from TotK Recomp (MIT, see THIRD_PARTY_NOTICES.md), without its TotK-only parts.

- `cpu.h`, `simd.h`, `simd_fp.h`, `fma.c`: the CPU support the generated C includes
- `main.c`: entry, modes, logging, crash reporter; `boot.c`: `nn::init::Start` and the checkpoints (`--boot`,
  `--frames N`)
- `loader.c`, `hostfn.c`, `dispatch.c`: NSO loading and linking, host overrides, the indirect-branch table
- `memory.c`, `kernel.c`, `svc.c`, `ipc.c`: guest memory, threads and synchronisation, SVCs, in-process IPC
- `fs_host.c`, `romfs.c`: nn::fs (saves under the profile directory, RomFS from the extracted directory)
- `ov_system.c`, `ov_services.c`, `ov_hid.c`, `ov_movie.c`: the other `nn::*` overrides
  ([docs/coverage.md](../docs/coverage.md))
- `nvn_*.c`, `nvn*.h`, `gfx.h`, `gfx_null.c`: the NVN front end and the null graphics back end
- `audren.c`, `audren.h`, `audio_null.c`, `audout.c`, `hwopus.c`: `audren:u`, `audout:u` and `hwopus`, served in
  process (`ipc.c` holds the service table and CMIF domains)
- `cfg.c`: `PLUCKOMP_*` settings; `platform/`: Windows and Linux OS calls
