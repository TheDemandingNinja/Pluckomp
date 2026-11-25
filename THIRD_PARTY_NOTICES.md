# Third-party notices

Code copied or adapted from other projects, with its license. Add a row whenever you bring code in.

| Project | What | License | Where |
|---|---|---|---|
| [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) | Repository layout; design of the content guard, pre-commit hook and CI workflow | MIT, Copyright (c) the TotK Recomp contributors | `tools/check_content.py`, `.githooks/`, `.github/` |
| [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) @ `afae80c908ab9fc9cdbafa9ed5295c946d80aa21` | ARM64-to-C lifter and generator, jump-table resolver, flag / register / V-register locals, CPU and FP/SIMD headers, differential tests (patch, plugin, spin-wait and hook machinery removed) | MIT, Copyright (c) 2026 Valiantsin Dzerakh | `recomp/*.py`, `runtime/cpu.h`, `runtime/simd.h`, `runtime/simd_fp.h`, `runtime/fma.c`, `tests/diff/` |
| [LAS Recomp](https://github.com/peiikai/LASRecomp) @ `da35b83`, through TotK Recomp | The lifter, FP/SIMD lowering, NSO loader, generator, CPU headers and differential tests TotK Recomp adapted | MIT, Copyright (c) 2026 Peiikai | `recomp/lifter.py`, `recomp/simd_c.py`, `recomp/nso.py`, `recomp/gen.py`, `recomp/module.py`, `runtime/cpu.h`, `runtime/simd.h`, `runtime/simd_fp.h`, `tests/diff/` |
| [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) @ `afae80c908ab9fc9cdbafa9ed5295c946d80aa21` | Host runtime core: guest memory, NSO loader and dynamic linking with host overrides, dispatch table, host functions, kernel (threads, synchronisation, virtual cores, fibers), SVCs, in-process IPC with `sm:`, nn::fs save storage, system service overrides, boot driver, crash reporter, platform layer; the settings interface; the build script's design and flags. TotK-only parts (mods, patches, graphics, audio, input, menu) removed; RomFS rewritten to serve a directory | MIT, Copyright (c) 2026 Valiantsin Dzerakh | `runtime/{boot,cfg,dispatch,fs_host,hostfn,ipc,kernel,loader,main,memory,oom,ov_system,romfs,sha256,svc}.c`, `runtime/{cfg,dispatch,fs,gfx,host,ipc,kernel,memx,ov,romfs_path,sha256,utf16}.h`, `runtime/platform/`, `tools/build_game.py` |
| [MacNativeGames](https://github.com/ly05010419/MacNativeGames) @ `b75be8cfa704df3701e86130c8ced268e79d6b11` and `33e8fa83052ea1ae09a3583137008e9797e12950`, through TotK Recomp | TotK host runtime TotK Recomp adapted: dynamic linking, identity check, guest memory and SVCs, threads and virtual-core preemption, IPC, system services, RomFS and save storage, boot sequence | MIT, Copyright (c) 2026 MacNativeGames contributors | `runtime/loader.c`, `runtime/memory.c`, `runtime/kernel.c`, `runtime/svc.c`, `runtime/ipc.c`, `runtime/ipc.h`, `runtime/ov_system.c`, `runtime/romfs_path.h`, `runtime/fs_host.c`, `runtime/fs.h`, `runtime/boot.c`, `runtime/hostfn.c` |
| [LAS Recomp](https://github.com/peiikai/LASRecomp) @ `da35b83`, through TotK Recomp | Host runtime pieces TotK Recomp adapted: NSO loader, host-function table, guest-space reservation, guest thread start | MIT, Copyright (c) 2026 Peiikai | `runtime/loader.c`, `runtime/hostfn.c`, `runtime/memory.c`, `runtime/kernel.c`, `runtime/boot.c` |
| [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) @ `53e7c5bb9ca3ea53e2160005be8fbe920cd9cb53` and [nxnative](https://github.com/seedds/nxnative) @ `6d349844880eb0527be77d7e0e4b8d999c0ae2f4`, through MacNativeGames and TotK Recomp | Host thread environment (TLR, TLS, thread object); the spin-wait gate | MIT, Copyright (c) 2026 jhaizhou; Copyright (c) 2026 MacNativeGames contributors, Copyright (c) 2026 seedds | `runtime/kernel.c` |
| [musl](https://git.musl-libc.org/cgit/musl) @ `5a56ad13e1e0790c06122978aad2a14d6b9efc1a`, through TotK Recomp | `fma.c`, `fmaf.c` and `scalbn.c` in one file: correctly rounded fma for builds without FMA3 | MIT, Copyright (c) 2005-2020 Rich Felker and contributors | `runtime/fma.c` |

## MIT License (TotK Recomp, LAS Recomp, MacNativeGames, d2r-mobile, nxnative, musl)

The same text applies to each, under the copyright line in its row above.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
