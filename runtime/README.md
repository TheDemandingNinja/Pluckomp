# runtime/

The host runtime: kernel, IPC, file system, input, audio and the NVN-on-Vulkan renderer.
So far only the CPU support the generated C includes, from TotK Recomp (MIT):

- `cpu.h`: the guest `Cpu` struct, memory access, exclusives, atomics, flags
- `simd.h`, `simd_fp.h`: the portable FP / AdvSIMD helpers `recomp/simd_c.py` emits calls to
- `fma.c`: correctly rounded `fma` / `fmaf` for builds without FMA3 (from musl, MIT)

The rest arrives with Milestone 3 (docs/roadmap.md).
