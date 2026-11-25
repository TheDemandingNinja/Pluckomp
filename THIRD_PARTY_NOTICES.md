# Third-party notices

Code copied or adapted from other projects, with its license. Add a row whenever you bring code in.

| Project | What | License | Where |
|---|---|---|---|
| [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) | Repository layout; design of the content guard, pre-commit hook and CI workflow | MIT, Copyright (c) the TotK Recomp contributors | `tools/check_content.py`, `.githooks/`, `.github/` |
| [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) @ `afae80c908ab9fc9cdbafa9ed5295c946d80aa21` | ARM64-to-C lifter and generator, jump-table resolver, flag / register / V-register locals, CPU and FP/SIMD headers, differential tests (patch, plugin, spin-wait and hook machinery removed) | MIT, Copyright (c) 2026 Valiantsin Dzerakh | `recomp/*.py`, `runtime/cpu.h`, `runtime/simd.h`, `runtime/simd_fp.h`, `runtime/fma.c`, `tests/diff/` |
| [LAS Recomp](https://github.com/peiikai/LASRecomp) @ `da35b83`, through TotK Recomp | The lifter, FP/SIMD lowering, NSO loader, generator, CPU headers and differential tests TotK Recomp adapted | MIT, Copyright (c) 2026 Peiikai | `recomp/lifter.py`, `recomp/simd_c.py`, `recomp/nso.py`, `recomp/gen.py`, `recomp/module.py`, `runtime/cpu.h`, `runtime/simd.h`, `runtime/simd_fp.h`, `tests/diff/` |
| [musl](https://git.musl-libc.org/cgit/musl) @ `5a56ad13e1e0790c06122978aad2a14d6b9efc1a`, through TotK Recomp | `fma.c`, `fmaf.c` and `scalbn.c` in one file: correctly rounded fma for builds without FMA3 | MIT, Copyright (c) 2005-2020 Rich Felker and contributors | `runtime/fma.c` |

## MIT License (TotK Recomp, LAS Recomp, musl)

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
