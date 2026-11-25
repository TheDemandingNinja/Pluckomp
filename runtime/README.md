# runtime/

The host runtime: kernel, IPC, file system, system services and, later, input, audio and the NVN-on-Vulkan renderer.
Most of it comes from TotK Recomp (MIT, see THIRD_PARTY_NOTICES.md), without its TotK-only parts.

- `cpu.h`, `simd.h`, `simd_fp.h`, `fma.c`: the CPU support the generated C includes
- `main.c`: entry, modes, logging, crash reporter; `boot.c`: `nn::init::Start` and the boot checkpoints
- `loader.c`, `hostfn.c`, `dispatch.c`: NSO loading and linking, host overrides, the indirect-branch table
- `memory.c`, `kernel.c`, `svc.c`, `ipc.c`: guest memory, threads and synchronisation, SVCs, in-process IPC
- `fs_host.c`, `romfs.c`: nn::fs (saves under the profile directory, RomFS from the extracted directory)
- `ov_system.c`, `ov_services.c`, `ov_hid.c`: the other `nn::*` overrides ([docs/coverage.md](../docs/coverage.md))
- `cfg.c`: `PLUCKOMP_*` settings; `platform/`: Windows and Linux OS calls
