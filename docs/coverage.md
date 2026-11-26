# SVC, IPC and NVN coverage

What Pikmin 4 v2.0.0 asks of the Switch OS and GPU driver, and how the host runtime answers it. Two runs:

- `PLUCKOMP_TRACE=svc,host,ipc pluckomp.exe --boot`: from `nn::init::Start` to the game's first NVN call
  (`nvnBootstrapLoader("nvnDeviceInitialize")` from main+0x60bc994, in UE4's NVN RHI bring-up).
- `PLUCKOMP_TRACE=svc,ipc,gfx pluckomp.exe --frames 600`: on to the 600th present with the null renderer (about 23 s
  on a 16-thread desktop).

## How the OS boundary is cut

As in TotK Recomp, the boundary sits at the SDK's `nn::*` functions, not at IPC: the loader binds a host function
(`HOST_OVERRIDE`) in place of every GOT, GLOB_DAT and ABS64 relocation that names an overridden symbol. The SDK code
behind an override never runs, so the Horizon service it would have opened is never asked for. What is not overridden
runs lifted; if it reaches a service, `sm:` stops with the service's name (`ipc.c`), which is how each gap below was
found. Where the SDK client is a thin protocol layer (audio, Opus), the service itself is served in process instead,
so the SDK's own client code runs unchanged.

## Boot path, v2.0.0

The run passes the three module constructors (`main` 970 entries, `subsdk0` 45, `sdk` 110), `nnMain` and UE4's
engine pre-init: about 4260 SVCs on 20 guest threads, no IPC session other than `sm:`.

## First frames, v2.0.0

With `--frames 600` the game creates its NVN device, a 1280x720 RGBA8 window for the first three presents, then a
1920x1080 RGBA8 window (docked). Wwise brings up its audio on `audren:u`, `audout:u` and `hwopus`. The game then shows
the controller applet over and over (no controller is connected, see `nn::hid` below) and keeps presenting: 600
presents, 454 000 SVCs, 47 guest threads, 955 000 NVN calls to 170 distinct procs, exit 0.

### SVCs reached

| SVC | Name | Calls (`--boot`) | Calls (600 frames) | Served by |
|---|---|---|---|---|
| 0x01 | SetHeapSize | 1 | 1 | `memory.c` |
| 0x04 | MapMemory | 19 | 46 | `memory.c` (thread stacks) |
| 0x05 | UnmapMemory | | 6 | `memory.c` |
| 0x06 | QueryMemory | 59 | 146 | `memory.c`, `svc.c` |
| 0x08 | CreateThread | 19 | 46 | `kernel.c` |
| 0x09 | StartThread | 19 | 46 | `kernel.c` |
| 0x0A | ExitThread | | 7 | `kernel.c` |
| 0x0B | SleepThread | 1 | 197 960 | `kernel.c` |
| 0x0C | GetThreadPriority | 1 | 1 | `kernel.c` |
| 0x0D | SetThreadPriority | 18 | 40 | `kernel.c` |
| 0x0F | SetThreadCoreMask | 20 | 48 | `kernel.c` |
| 0x15 | CreateTransferMemory | | 1 | `ipc.c` (audio renderer work buffer) |
| 0x16 | CloseHandle | | 28 | `kernel.c` |
| 0x17 | ResetSignal | | 535 | `kernel.c` |
| 0x18 | WaitSynchronization | 2 | 18 | `kernel.c` |
| 0x1A | ArbitrateLock | 4 | 37 275 | `kernel.c` |
| 0x1B | ArbitrateUnlock | 66 | 65 920 | `kernel.c` |
| 0x1C | WaitProcessWideKeyAtomic | 78 | 47 766 | `kernel.c` |
| 0x1D | SignalProcessWideKey | 681 | 51 996 | `kernel.c` |
| 0x1F | ConnectToNamedPort | | 1 | `ipc.c` (`sm:` only) |
| 0x21 | SendSyncRequest | | 6550 | `ipc.c` |
| 0x25 | GetThreadId | 20 | 47 | `kernel.c` |
| 0x29 | GetInfo | 22 | 23 | `svc.c`, `memory.c` |
| 0x2C | MapPhysicalMemory | 3234 | 45 702 | `memory.c` (`nn::os::AllocateMemoryPages`) |
| 0x45 | CreateEvent | 1 | 2 | `kernel.c` |

GetSystemTick (0x1E) never shows: the lifter reads `CNTPCT_EL0` through `rt_ticks` instead. `svc.c` serves more SVCs
than this list (the rest of TotK Recomp's set); an SVC it does not know stops with its number and caller.

### IPC services reached

| Service | Reached through | Served by | Commands seen (600 frames) |
|---|---|---|---|
| `audren:u` | `nn::audio` (Wwise `CAkLEngine::InitPlatformContext`, `CAkSink`) | `audren.c` (TotK Recomp), silent mixer `audio_null.c` | manager 0, 1, 4; `IAudioRenderer` 4 (RequestUpdate, 540), 5, 7; `IAudioDevice` 5, 10, 12 |
| `audout:u` | `nn::audio::OpenDefaultAudioOut` (Wwise `CAkSink::Init`) | `audout.c`: buffers given back in real time, no output | manager 1; `IAudioOut` 0, 1, 2, 7 (append, 938), 8 (released, 4993) |
| `hwopus` | `nn::codec::HardwareOpusDecoder` (Wwise `CAkOpusDecodingMgr::Init`) | `hwopus.c`: decodes to silence of the packet's length; the SDK client converts the session to a CMIF domain (`ipc.c`) | manager 1 (GetWorkBufferSize, 10); no decoder opened yet |

Every other service the game would open (fsp-srv, bsd:u, hid, nvdrv, time, appletOE, acc:u0, aoc:u, nifm:u, ...) is
answered by an override before the SDK gets to it.

### NVN procs reached

The game loads 519 NVN procs by name; `runtime/nvn_procs.inc` (TotK 1.2.1's list) has all but two of them,
`nvnDeviceApplyDeferredFinalizes` and `nvnQueueAcquireTexture`, which get NULL and are logged by name, as on hardware
for an unknown name. A proc that is in the table but has no implementation stops with its name when called. The 170
reached in 600 frames, by object:

| Object | Procs |
|---|---|
| Device, DeviceBuilder | Initialize, GetInteger, SetDepthMode, SetWindowOriginMode, GetSeparateSamplerHandle, GetSeparateTextureHandle, GetImageHandle, RegisterFastClearColor, RegisterFastClearDepth; SetDefaults, SetFlags |
| Queue, QueueBuilder | Initialize, SubmitCommands, Flush, Finish, WaitSync, PresentTexture; SetDefaults, SetDevice, SetCommandFlushThreshold, SetControlMemorySize |
| MemoryPool, MemoryPoolBuilder | Initialize, GetFlags, Map, GetBufferAddress, GetSize, Finalize; SetDefaults, SetDevice, SetFlags, SetStorage |
| Texture, TextureBuilder, TextureView, TexturePool | 9, 14, 5 and 3 procs (RegisterTexture, RegisterImage), including SetDepthStencilMode |
| Sampler, SamplerBuilder, SamplerPool | Initialize; 9 setters; Initialize, RegisterSampler |
| Program | Initialize, SetShaders |
| Window, WindowBuilder | Initialize, AcquireTexture, Get/SetPresentInterval, Finalize; SetDefaults, SetDevice, SetNativeWindow, SetTextures |
| Sync, Event, EventBuilder | Initialize, Wait, Finalize; Initialize, Signal, GetValue; SetStorage |
| CommandBuffer | 53 procs: recording and its memory, render targets, clears, viewports, binds, copies, barriers, counters, `FenceSync` |
| State objects | Blend 4, ChannelMask 2, Color 2, DepthStencil 7, Multisample 1, Polygon 3, VertexAttrib 3, VertexStream 3 |

Added for Pikmin 4 on top of TotK Recomp's front end: `nvnTexturePoolRegisterImage` (images share the texture pool's
ids), `nvnCommandBufferFenceSync` (a record the back end fences with its submission's value), `nvnQueueWaitSync`, and
stencil-plane sampling kept in the texture description instead of stopping.

## Imports and overrides

`main` and `subsdk0` import 1211 symbols, 530 of them from `nn::*`, `nv::` and the NVN / nvgr libraries. The counts
below are overrides the runtime defines, some of them (from TotK Recomp) for functions this version does not import.
Most of what runs lifted is pure library code (`nn::os` locks and events over SVCs, `nn::util`, `nn::mem`, `nn::lmem`,
`nn::cstd`, the `nn::prepo` report builders, the hid vibration mixer classes); the ones below would reach a service
once the game calls them.

| Area | Overrides defined (file) | Answer | Still lifted, reaches a service when called |
|---|---|---|---|
| `nn::fs` | 45 (`fs_host.c`, `romfs.c`) | RomFS from the extracted directory, saves under the profile directory | none |
| `nn::oe`, `applet`, `am` | 31 (`ov_system.c`, `ov_services.c`) | docked, focused, version 2.0.0; `ExecuteProgram` is a named fatal | none |
| `nn::time` | 16 (`ov_system.c`) | host clock and zone | none |
| `nn::account` | 11 (`ov_system.c`, `ov_services.c`) | one local user; no network service account | none |
| `nn::aoc` | 4 (`ov_system.c`, `ov_services.c`) | no add-on content | none |
| `nn::hid` | 30 (`ov_hid.c`) | one connected Pro Controller (player 1) with every button up; no keyboard, mouse or touch input (input: Milestone 5); `ShowControllerSupport` returns at once | `VibrationTarget` |
| `nn::socket` | 25 (`ov_services.c`) | no network: `Socket` fails with ENETDOWN | none |
| `nn::nifm`, `nn::bcat`, `nn::ldn` | 14 (`ov_services.c`) | no network: a submitted request stays on hold for 100 ms, then resolves unavailable; no delivery cache, no local wireless | `nn::nsd::ResolveEx`, `nn::ssl` |
| `nn::vi`, `nv` | 8 (`ov_services.c`) | display and layer identities, 60 Hz vsync event, graphics allocators recorded | none |
| `nn::prepo`, `nn::err`, `nn::ec`, `nn::ro`, `nn::pctl`, `nn::diag`, `nn::nfp`, `nn::pl` | (`ov_system.c`, `ov_services.c`) | dropped reports, no shop, no NFC reader, no shared font | `nn::ngc::ProfanityFilter` (system data; not reached in 600 frames) |
| `nn::audio`, `nn::codec` | none: served as `audren:u`, `audout:u`, `hwopus` | silent | none |
| `movie::Decoder` | 1 (`ov_movie.c`) | `Configure` answers unsupported (0x2001), so the library never opens NVDEC through NvRm | none |
| `nn::swkbd`, `nn::web`, `nn::album` | none | | applets, not reached |
| NVN, `nvgr_*` | `nvnBootstrapLoader` and every proc (`nvn_procs.c`, `nvn_api_*.c`, `nvn_ext.c`) | null back end: no GPU, presents logged | none |

The movie decoder needs a word: once, on a run before `ov_movie.c` existed, a movie decoder started (Android's ACodec
in subsdk0) about 60 frames in and aborted in `NvOsDrvOpen`, as TotK Recomp found for its movies. Since the boot
reaches its title (about frame 55) every run builds two decoders for the title's background movie, VP9 (type 3, output
format 3, mode 2) and its audio track (type 5, format 1, mode 1), and both get the unsupported answer. The game stays in
its title state with a black picture; a host VP9 decoder (TotK Recomp's libvpx one serves type 3 in buffer mode 1 only)
is the next step.

The nifm hold needs one too. Pikmin 4's BCAT pre-sync runnable (constructed at main+0x206a6d0) stores its status 1
after `FRunnableThread::Create` returns, while the thread's `Run` (main+0x206a830) submits a network request, polls
`IsNetworkRequestOnHold` every 10 ms and stores status 6 when the request resolves without a network. A request that
resolved at once let `Run` finish first in most runs; status 1 then overwrote the 6 and the boot sequence waited in its
`WaitPreSyncBCAT` state forever. On a console the request is an IPC round trip and stays on hold while nifm evaluates
it, so the creator always stores first.
