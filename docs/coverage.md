# SVC and IPC coverage

What Pikmin 4 v2.0.0 asks of the Switch OS, and how the host runtime answers it. Measured with
`PLUCKOMP_TRACE=svc,host,ipc pluckomp.exe --boot`, which runs from `nn::init::Start` to the game's first NVN call
(`nvnBootstrapLoader("nvnDeviceInitialize")`, in UE4's NVN RHI bring-up).

## How the OS boundary is cut

As in TotK Recomp, the boundary sits at the SDK's `nn::*` functions, not at IPC: the loader binds a host function
(`HOST_OVERRIDE`) in place of every GOT, GLOB_DAT and ABS64 relocation that names an overridden symbol. The SDK code
behind an override never runs, so the Horizon service it would have opened is never asked for. What is not overridden
runs lifted; if it reaches a service, `sm:` stops with the service's name (`ipc.c`), which is how each gap below was
found.

## Boot path, v2.0.0

The run passes the three module constructors (`main` 970 entries, `subsdk0` 45, `sdk` 110), `nnMain` and UE4's
engine pre-init: 4257 SVCs on 20 guest threads, no IPC session other than `sm:`.

### SVCs reached

| SVC | Name | Calls | Served by |
|---|---|---|---|
| 0x01 | SetHeapSize | 1 | `memory.c` |
| 0x04 | MapMemory | 19 | `memory.c` (thread stacks) |
| 0x06 | QueryMemory | 59 | `memory.c`, `svc.c` |
| 0x08 | CreateThread | 19 | `kernel.c` |
| 0x09 | StartThread | 19 | `kernel.c` |
| 0x0B | SleepThread | 1 | `kernel.c` |
| 0x0C | GetThreadPriority | 1 | `kernel.c` |
| 0x0D | SetThreadPriority | 18 | `kernel.c` |
| 0x0F | SetThreadCoreMask | 20 | `kernel.c` |
| 0x18 | WaitSynchronization | 2 | `kernel.c` |
| 0x1A | ArbitrateLock | 4 | `kernel.c` |
| 0x1B | ArbitrateUnlock | 66 | `kernel.c` |
| 0x1C | WaitProcessWideKeyAtomic | 78 | `kernel.c` |
| 0x1D | SignalProcessWideKey | 681 | `kernel.c` |
| 0x25 | GetThreadId | 20 | `kernel.c` |
| 0x29 | GetInfo | 22 | `svc.c`, `memory.c` |
| 0x2C | MapPhysicalMemory | 3234 | `memory.c` (`nn::os::AllocateMemoryPages`) |
| 0x45 | CreateEvent | 1 | `kernel.c` |

GetSystemTick (0x1E) never shows: the lifter reads `CNTPCT_EL0` through `rt_ticks` instead. `svc.c` serves more SVCs
than this list (the rest of TotK Recomp's set); an SVC it does not know stops with its number and caller.

### IPC services reached

None besides `sm:`. Every service the boot path would open (fsp-srv, bsd:u, hid, nvdrv, time, appletOE, acc:u0, aoc:u,
nifm:u, ...) is answered by an override before the SDK gets to it.

## Imports and overrides

`main` and `subsdk0` import 1211 symbols, 530 of them from `nn::*`, `nv::` and the NVN / nvgr libraries. 165 of those
are bound to an override; the rest run lifted. The counts below are overrides the runtime defines, some of them (from
TotK Recomp) for functions this version does not import. Most of what runs lifted is pure library code (`nn::os` locks
and events over SVCs, `nn::util`, `nn::mem`, `nn::lmem`, `nn::cstd`, the `nn::prepo` report builders, the hid vibration
mixer classes); the ones below would reach a service once the game calls them.

| Area | Overrides defined (file) | Answer | Still lifted, reaches a service when called |
|---|---|---|---|
| `nn::fs` | 45 (`fs_host.c`, `romfs.c`) | RomFS from the extracted directory, saves under the profile directory | none |
| `nn::oe`, `applet`, `am` | 31 (`ov_system.c`, `ov_services.c`) | docked, focused, version 2.0.0; `ExecuteProgram` is a named fatal | none |
| `nn::time` | 16 (`ov_system.c`) | host clock and zone | none |
| `nn::account` | 11 (`ov_system.c`, `ov_services.c`) | one local user; no network service account | none |
| `nn::aoc` | 4 (`ov_system.c`, `ov_services.c`) | no add-on content | none |
| `nn::hid` | 30 (`ov_hid.c`) | no controller, keyboard, mouse or touch input (input: Milestone 5); `ShowControllerSupport` is a named fatal | `VibrationTarget` |
| `nn::socket` | 25 (`ov_services.c`) | no network: `Socket` fails with ENETDOWN | none |
| `nn::nifm`, `nn::bcat`, `nn::ldn` | 14 (`ov_services.c`) | no network, no delivery cache, no local wireless | `nn::nsd::ResolveEx`, `nn::ssl` |
| `nn::vi`, `nv` | 8 (`ov_services.c`) | display and layer identities, 60 Hz vsync event, graphics allocators recorded | NVN (next step) |
| `nn::prepo`, `nn::err`, `nn::ec`, `nn::ro`, `nn::pctl`, `nn::diag`, `nn::nfp`, `nn::pl` | (`ov_system.c`, `ov_services.c`) | dropped reports, no shop, no NFC reader, no shared font | `nn::ngc::ProfanityFilter` (system data) |
| `nn::audio` | none | | 70 imports: `audren:u` (TotK Recomp's audio renderer, Milestone 5) |
| `nn::swkbd`, `nn::web`, `nn::album` | none | | applets, not reached at boot |
| NVN, `nvgr_*` | `nvnBootstrapLoader` only (`boot.c`) | `--boot` ends there | the NVN front end and null back end |
