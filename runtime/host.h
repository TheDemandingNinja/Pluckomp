// Adapted from TotK Recomp runtime/host.h @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// Interface of the host runtime: guest memory, modules, kernel, IPC, file system and boot.
//
// Guest addresses are host addresses. Every guest-visible region lies below 2^39 (IPC buffer descriptors carry 39
// address bits). Fixed parts of the guest address map:
//   modules           MOD_<X>_BASE from layout.h, from 0x70_0000_0000
//   kernel region     RT_KERNEL_BASE, 256 MiB: TLR, TLS blocks, host objects the guest reads (mem_kernel_alloc)
//   host functions    RT_HOSTFN_BASE, 1 MiB, never accessible: synthetic addresses of host functions
// The stack, heap and alias regions are placed at start-up (mem_layout).
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "dispatch.h"

// ---- logging and results
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // "[host] <msg>\n" to the log, one write
// The log is stderr plus, in a game run whose stderr is not a file, logs\pluckomp.log next to pluckomp.exe (main.c log_open).
// rt_logf and rt_log_raw write to both without the prefix and the newline: crash reports, fatal errors, guest output.
void rt_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_vlogf(const char *fmt, va_list ap);
void rt_log_raw(const char *p, size_t n);
const char *rt_log_path(void);                  // the log file, NULL when this run writes none
enum { RT_TRACE_SVC = 1, RT_TRACE_HOSTCALL = 2, RT_TRACE_IPC = 4, RT_TRACE_FS = 8, RT_TRACE_GFX = 16, RT_TRACE_PERF = 32,
       RT_TRACE_SCHED = 64, RT_TRACE_AUDIO = 128, RT_TRACE_PRESENT = 256,
       RT_TRACE_HOSTCOUNT = 512,     // no PLUCKOMP_TRACE name of its own: hostfn_count switches it (host.h)
       RT_TRACE_PLUGIN = 1024 };     // no name either: the plugin service run (plugin_init.c) sees every SVC through
                                     // rt_plugin_svc while it runs a plugin's init
extern unsigned rt_trace;                       // from PLUCKOMP_TRACE=svc,host,ipc,fs,gfx,perf,sched,audio,present (or all)
#define RT_RESULT(module, desc) ((uint32_t)(module) | ((uint32_t)(desc) << 9))
void rt_sha256(const void *p, size_t n, uint8_t out[32]);   // BCrypt; fatal if the provider fails

// ---- guest address map (fixed part)
#define RT_ADDRESS_LIMIT (1ull << 39)
#define RT_KERNEL_BASE 0x7010000000ull
#define RT_KERNEL_SIZE 0x10000000ull
#define RT_HOSTFN_BASE 0x7020000000ull
#define RT_HOSTFN_SIZE 0x100000ull

// ---- AAPCS64 argument access for host functions (section 2.2)
static inline uint64_t arg_x(const Cpu *c, int i) { return i < 8 ? c->x[i] : LD64(c->sp + 8 * (uint64_t)(i - 8)); }
static inline void *arg_p(const Cpu *c, int i) { return (void *)(uintptr_t)arg_x(c, i); }
static inline float arg_s(const Cpu *c, int i) { uint32_t b = (uint32_t)c->v[i]; float f; memcpy(&f, &b, 4); return f; }
static inline double arg_d(const Cpu *c, int i) { uint64_t b = (uint64_t)c->v[i]; double d; memcpy(&d, &b, 8); return d; }
static inline void ret_x(Cpu *c, uint64_t v) { c->x[0] = v; }
static inline void ret_s(Cpu *c, float f) { uint32_t b; memcpy(&b, &f, 4); c->v[0] = b; }
static inline void ret_d(Cpu *c, double d) { uint64_t b; memcpy(&b, &d, 8); c->v[0] = b; }

// ---- host functions (hostfn.c)
// Every host function is a GuestFn at a synthetic address RT_HOSTFN_BASE + 16*i. It runs on the calling guest thread,
// may clobber x0-x18, v0-v7, v16-v31, must preserve x19-x29, sp, v8-v15, tpidr, and returns to the guest BL site with
// a C return. On entry c->pc holds the function's own synthetic address.
typedef struct { const char *name; GuestFn fn; } RtHostEntry;
#define RT_CAT2(a, b) a##b
#define RT_CAT(a, b) RT_CAT2(a, b)
// File-scope declarations; fn must be declared before. The loader binds them by symbol name. The entries of every
// object file meet in one section per kind, which hostfn.c walks: on PE/COFF ".ovr$m", which the linker sorts between
// hostfn.c's ".ovr$a" and ".ovr$z" markers; on ELF "pluckomp_ovr", which the linker brackets with __start_pluckomp_ovr and
// __stop_pluckomp_ovr.
#ifdef _WIN32
#define RT_HOST_SECTION(kind) section("." #kind "$m")
#else
#define RT_HOST_SECTION(kind) section("pluckomp_" #kind)
#endif
#define HOST_OVERRIDE(sym, fn) /* replaces sym in every GOT/GLOB_DAT/ABS64 of every module */                  \
    __attribute__((used, RT_HOST_SECTION(ovr))) static const RtHostEntry RT_CAT(rt_ovr_, __COUNTER__) = {sym, fn}
#define HOST_OBSERVER(sym, fn) /* JUMP_SLOT only; fn calls rt_original(c) */                                    \
    __attribute__((used, RT_HOST_SECTION(obs))) static const RtHostEntry RT_CAT(rt_obs_, __COUNTER__) = {sym, fn}
uint64_t hostfn_register(const char *name, GuestFn fn);    // synthetic address; before hostfn_seal only. The same
                                                           // (name, fn) pair returns the same address
GuestFn hostfn_override(const char *name);                 // NULL if none
GuestFn hostfn_observer(const char *name);                 // NULL if none
void hostfn_add_override(const char *name, GuestFn fn);    // an override outside the linker table (probes); before
                                                           // ldr_load
const char *hostfn_name(uint64_t addr);                    // name registered at a synthetic address, NULL if none
void hostfn_seal(void);                                    // build the dispatch table (rt_dispatch_init over every
                                                           // linked module); idempotent; registration ends here
uint64_t hostfn_calls(void);                               // host-function calls counted so far, all threads: only
                                                           // while hostfn_count is on or under PLUCKOMP_TRACE=host
void hostfn_count(int on);                                 // count host-function calls (probes); off by default
void hostfn_report(void);                                  // call count per host function to stderr, busiest first
int hostfn_table_size(int observers);                      // linked HOST_OVERRIDE (0) / HOST_OBSERVER (1) entries
const char *hostfn_table_name(int observers, int i);       // symbol of linked entry i, NULL past the end
uint64_t rt_original(Cpu *c);                              // inside an observer: call the guest definition with the
                                                           // current x0..x7, return x0. Uses c->pc, so call it before
                                                           // anything that changes c->pc (rt_call restores it)
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...);         // host -> guest, section 2.3. n <= 8 arguments, each read
                                                           // as uint64_t: cast every argument. Seals the dispatch
                                                           // table on first use. Sets x30 = RT_RET_HOST, restores
                                                           // x30 and pc, returns x0. Does not touch sp
#define RT_RET_HOST 0x70200ff000ull                        // x30 sentinel inside the host-function window; a jump to
                                                           // it is a named fatal

// ---- guest memory (memory.c)
typedef struct { uint64_t base, size; } RtRange;
typedef struct { RtRange aslr, stack, heap, alias, kernel, hostfn; uint64_t pool; } RtGuestLayout;
typedef struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } RtMemoryInfo;   // Horizon layout
void mem_init(void);                                       // reserve every region; on a collision re-run the process
                                                           // (up to 20 times, PLUCKOMP_REEXEC), then fatal
const RtGuestLayout *mem_layout(void);
uint32_t mem_set_heap_size(uint64_t size, uint64_t *addr);                      // 0x01
uint32_t mem_set_permission(uint64_t a, uint64_t n, uint32_t perm);             // 0x02
uint32_t mem_set_attribute(uint64_t a, uint64_t n, uint32_t mask, uint32_t at); // 0x03
uint32_t mem_map(uint64_t dst, uint64_t src, uint64_t n);                       // 0x04
uint32_t mem_unmap(uint64_t dst, uint64_t src, uint64_t n);                     // 0x05
uint32_t mem_query(uint64_t a, RtMemoryInfo *out);                              // 0x06
extern void (*mem_query_hook)(uint64_t a, RtMemoryInfo *r);   // plugin service run only: adjusts mem_query's answer
uint32_t mem_map_physical(uint64_t a, uint64_t n);                              // 0x2C
uint32_t mem_unmap_physical(uint64_t a, uint64_t n);                            // 0x2D
int mem_info(uint32_t id, uint64_t *out);                  // GetInfo memory ids 2-7, 12-17, 21, 22, 28; 0 if not a memory id
void *mem_kernel_alloc(uint64_t size, uint64_t align);     // zeroed, guest-visible, in the kernel region; align <= 64 KiB
uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max);
                                                           // GetWriteWatch(RESET): fills pages[] (4 KiB addresses)
                                                           // with the pages of [addr, addr+size) written since the
                                                           // last call, returns their count (at most max; a page
                                                           // past max stays reported next time). The range must lie
                                                           // inside the stack/heap block or the alias region
                                                           // (write watch); otherwise returns UINT64_MAX
uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max);
                                                           // the same without the reset: the bits stay for the next
                                                           // mem_written_pages (verify checks only)
int mem_freeze(uint64_t a);                                // makes the 2 MiB region at a (2 MiB aligned, watched,
                                                           // committed PAGE_READWRITE) read-only until a write or
                                                           // mem_thaw thaws it; 1 frozen (or already), 0 refused
int mem_frozen(uint64_t a);                                // the 2 MiB region of a is frozen: no page in it changed
                                                           // since its freeze
void mem_thaw(uint64_t a, uint64_t n);                     // makes the frozen regions over [a, a+n) writable again;
                                                           // before a kernel write (file read) into guest memory
void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws);
                                                           // regions frozen now; freezes and thaws since the last call
void mem_kernel_free(void *p, uint64_t size);              // TLR/TLS reuse after thread exit
int mem_guest_readable(uint64_t a, uint64_t n);            // [a, a+n) can be read without a fault: inside a loaded
                                                           // module or the committed heap answered from the layout,
                                                           // anything else by plat_mem_readable (an OS query)

// ---- short out-of-memory spells (oom.c)
enum { RT_OOM_COMMIT, RT_OOM_STAGING, RT_OOM_IMAGE, RT_OOM_CHUNK, RT_OOM_SITES };
extern volatile int32_t rt_oom_req;                        // set by rt_oom_request; vk_texture_new_submit takes it
void rt_oom_request(void);                                 // asks the Vulkan back end to give memory back at its next
                                                           // submit (released textures, headroom blocks); any thread,
                                                           // any lock held
int rt_oom_wait(const char *what, uint64_t bytes, int (*retry)(void *arg), void *arg);
                                                           // after a failed allocation: requests a release, then calls
                                                           // retry every 10 ms for up to 30 s; 1 once retry returned 1,
                                                           // 0 after the wait (the caller ends the game). Logs the
                                                           // first 8 with the commit numbers
void *rt_realloc_wait(void *old, size_t n, const char *what);   // realloc, NULL only after rt_oom_wait gave up
int rt_oom_forced(int site);                               // 1: fail this attempt at site (PLUCKOMP_TEST_OOM=<site>:<n>,
                                                           // sites commit, staging, image, chunk; or rt_oom_force)
void rt_oom_force(int site, int n);                        // probes: fail the next n attempts at site
int rt_oom_forced_left(int site);                          // forced failures still to come at site

// ---- modules (loader.c)
typedef struct {
    const char *name; uint64_t base, size, text_end, data_off;   // text_end, data_off: offsets from base
    uint64_t init, init_array, init_array_size, ctor_flag;  // absolute guest addresses; init_array_size in bytes
    int plugin;                                             // a mod's code module (layout plugin<N>), not a game module
} RtLoaded;
void ldr_load(const char *exefs_dir);                      // load, relocate (overrides applied), seal; once
extern GuestFn ldr_plugin_import;                          // set before ldr_load (plugin_init.c): every JUMP_SLOT
                                                           // import of a plugin binds to this observer
// The module tables of the runtime library: it is built once per release (tools/build_game.py --make-runtime-lib),
// before an install's layout (recomp/layout.py) adds its mod plugins after sdk, so they do not use that MOD_COUNT.
#define RT_MAX_MODULES 32
int ldr_count(void);
int ldr_plugins_gone(uint32_t *key);                       // layout plugins whose file is gone; mixes their paths into key
const RtLoaded *ldr_module(int i);                         // order: main, subsdk0, sdk
uint64_t ldr_symbol(const char *name);                     // exported definition, 0 if none
uint64_t ldr_plt_stub(const char *module, const char *name); // PLT stub whose GOT slot holds name, 0 if none
const char *ldr_describe(uint64_t a, char *buf, int n);    // "main+0x1234 (symbol+0x10)", "host <name>", or the address
uint64_t ldr_nso_size(const char *dir, const char *file);   // image size of an ExeFS NSO (SHA-256 checked)
void ldr_map_image(const char *dir, const char *file, uint64_t base, uint64_t *text_end, uint64_t *data_off);
                                                           // its segments at base (reserved by the caller), not
                                                           // linked: the plugin service run's rtld (plugin_init.c)
uint8_t *ldr_read_verified(const char *dir, const char *file, size_t *len);   // whole file, SHA-256 checked against
                                                           // modules.c (fatal on mismatch); free() it

// Written by tools/build_game.py into build/<version>/modules.c.
typedef struct { const char *file, *sha256; } RtFileHash;
extern const RtFileHash rt_exefs_sha256[];                 // tools/data/expected-<version>.json "exefs_sha256"
extern const int rt_exefs_sha256_count;
extern const char rt_default_exefs[], rt_default_romfs[], rt_default_profile[];
extern const char rt_port_version[];                       // VERSION at the build's root: 0.1.0, 0.1.0+973c626 in a release

// ---- kernel (kernel.c)
typedef struct RtThread RtThread;
Cpu *kern_cpu(void);                                       // current guest thread's Cpu, NULL on host threads
void kern_free_cores_note(void);                           // for crash reports: logs that free_cores = 1 lifted the
                                                           // virtual core gate, nothing when the gate is on
uint32_t kern_main_thread(Cpu **cpu);                      // main thread object and Cpu for the calling host thread
                                                           // (main.npdm: 1 MiB stack at the top of the stack region,
                                                           // priority 44, core 0); on the main thread: its handle
void kern_main_stack(uint64_t *base, uint64_t *size);      // the main thread's stack, size 0 before kern_main_thread;
                                                           // svc.c reports it to QueryMemory, memory.c does not know it
void kern_run_on_guest_main(void (*fn)(Cpu *c));           // start the guest main host thread, run fn there, join
int kern_core_away(void);                                  // give the virtual core up around a blocking host wait
void kern_core_back(int *away);
void kern_checkpoint(void);                                // preemption point, at every SVC and indirect call
// A preempt request is pending for the thread (Cpu.thread): its RtThread starts with the flag (kernel.c).
static inline int kern_preempt_pending(const void *thread) { return __atomic_load_n((const int *)thread, __ATOMIC_RELAXED); }
// Set once before guest threads start (PLUCKOMP_CORE_DIAG, PLUCKOMP_ASYNC_PREEMPT): host calls (hostfn.c) and SVCs (svc.c) count their depth in
// the thread's second int, so the kernel knows whether a core owner runs game code at the top level.
extern int kern_host_track;
static inline int *kern_host_depth(void *thread) { return (int *)thread + 1; }
void kern_host_left(void *thread);                         // back at depth 0 with a preempt request pending
int kern_core_diag_on(void);                               // PLUCKOMP_CORE_DIAG
void kern_core_diag_frame(uint64_t frame, uint64_t interval_ns);   // perf_present, every present (also logs the
                                                           // asynchronous preemption counters)
void kern_sched_report(uint64_t frame);                    // PLUCKOMP_TRACE=sched: per-thread time split, on present
uint32_t kern_handle_new(int kind, void *obj);             // kind: KERN_SESSION or KERN_TMEM (ipc.c)
void *kern_handle_get(uint32_t h, int kind);
void kern_handle_close(uint32_t h);                        // CloseHandle from the host side, any thread; fatal on a
                                                           // bad handle
enum { KERN_SESSION = 4, KERN_TMEM = 5 };
uint32_t kern_event_new(void);                             // writable handle
uint32_t kern_event_readable(uint32_t writable);
void kern_event_signal(uint32_t writable);
void kern_bind_system_event(Cpu *c, uint64_t ev, int autoclear);   // guest thread only
void kern_signal_system_event(uint64_t ev);                // any thread
int kern_svc(Cpu *c, uint32_t imm);                        // thread/sync SVCs; 1 if handled
uint64_t kern_now_ns(void);
extern uint64_t rt_guest_start_ns;   // kern_now_ns when the guest started (boot.c), 0 before
typedef struct { uint64_t svcs, threads, host_calls; } RtStats;
RtStats rt_stats(void);                                    // host_calls: hostfn_calls()

// ---- IPC (ipc.c)
int ipc_svc(Cpu *c, uint32_t imm);                         // 0x15, 0x1F, 0x21, 0x22; 1 if handled
void ipc_handle_closed(int kind, void *obj);               // CloseHandle on a KERN_SESSION/KERN_TMEM handle

// ---- file system (romfs.c / fs_host.c)
void fs_init(void);                                        // idempotent; paths from rt_config()

// ---- account (ov_system.c)
void account_uid(uint64_t out[2]);                         // the local user's nn::account::Uid {lo, hi}, persistent in
                                                           // <profile>/account.uid; fs_host.c names save data after it

// ---- boot (boot.c, main.c)
typedef struct {
    const char *exefs, *romfs, *profile;                   // ExeFS directory, extracted RomFS, save profile
    int boot;                                              // --boot: exit 0 at the first NVN call
    uint64_t frames;                                       // --frames N: exit 0 at the Nth present (0 = no limit)
} RtConfig;
const RtConfig *rt_config(void);
void rt_boot_event(const char *what, Cpu *c, const char *detail);   // checkpoint log; "nvn" ends --boot
void rt_boot(void);                                        // boot.c
// Modes of pluckomp.exe. main.c has run mem_init and parsed the paths; each mode calls ldr_load itself if it needs the
// modules, registers its host functions, then seals (hostfn_seal, or implicitly at the first rt_call). Return the
// process exit code.
int probe_loader(void), probe_kernel(void), probe_ipc(void), probe_services(void), probe_fs(void);
