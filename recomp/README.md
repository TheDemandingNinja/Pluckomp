# recomp/

The ARM64-to-C generator, adapted from [TotK Recomp](https://github.com/valentderah/totk-pc-recomp)'s `recomp/`
(MIT), which builds on [LAS Recomp](https://github.com/peiikai/LASRecomp) (MIT). Every file it emits contains
`GENERATED_MARKER` from `tools/check_content.py`.

| File | What |
|---|---|
| `gen.py` | Driver: function discovery, chunking, parallel emission, lift cache, manifest |
| `lifter.py` | One AArch64 instruction to C (integer, memory, branches, system) |
| `simd_c.py` | FP / AdvSIMD / crypto / CRC32 to portable C over `runtime/simd.h` |
| `jumptables.py` | Static resolution of the jump tables behind computed `br` |
| `flags.py` | NZCV in function locals |
| `regs.py` | X registers in function locals |
| `vlocals.py` | V registers in function locals |
| `module.py`, `nso.py` | NSO loading, function seeds |
| `layout.py` | Module bases, shared by the generator and the runtime |

Left out from TotK Recomp: its code patch tables, exlaunch plugins, spin-wait gates and function-entry hooks. They
can come back when Pikmin 4 needs them.

Run: `python recomp/gen.py` (docs/building.md). Tests: `tests/diff/` against Unicorn.
