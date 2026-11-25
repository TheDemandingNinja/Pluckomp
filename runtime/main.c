// Adapted from TotK Recomp runtime/main.c @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// pluckomp.exe entry: reserves the guest address space, parses the command line and runs one mode.
//
//   pluckomp.exe [--exefs DIR] [--romfs DIR] [--profile DIR] <mode>
//     --selftest        layout, dispatch table and host-function registry; needs no game files
//     --probe-loader    load and relocate the modules
//     --boot            run the game until the first NVN call
//     --frames N        run the game until its Nth present (null renderer: presents are logged)
//     (no mode)         run the game
//
// Also here: the runtime services the generated code calls besides rt_dispatch and rt_svc (rt_brk, rt_udf,
// rt_ticks), logging, SHA-256, and the crash reporter (a vectored exception handler that names the lifted function,
// the host and guest registers, the machine code around the fault, the guest frame-pointer chain and the memory the
// registers point at).
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"
#include "sha256.h"

#define MAX_MODULES 16

// ---- logging, tracing, hashing
unsigned rt_trace;

// Log file. A game run whose stderr is not a file already (started from Explorer, a shortcut or a console) also
// writes its log to logs\pluckomp.log next to pluckomp.exe, so a player has one to attach to a report; run_game.py and
// `2> file` keep their own file. Each start moves the earlier logs to pluckomp.1.log .. pluckomp.4.log. A file stops at 64 MiB
// (PLUCKOMP_TRACE can write that much). Lines from before log_open wait in a static buffer: mem_init runs first so that
// nothing else lands in the guest address space, and an earlier open would put the CRT's heap there.
#define LOG_KEEP 5
#define LOG_CAP (64ull << 20)
enum { LOG_EARLY, LOG_FILE, LOG_OFF };
static int g_log_state;
static PlatAppend *g_log_file;
static char g_log_path[PLAT_PATH_MAX * 3];
static char g_log_early[32768];
static size_t g_log_early_n;
static uint64_t g_log_bytes;

void rt_log_raw(const char *p, size_t n) {
    fwrite(p, 1, n, stderr);
    int state = __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE);
    if (state == LOG_EARLY) {
        size_t at = __atomic_fetch_add(&g_log_early_n, n, __ATOMIC_RELAXED);
        if (at + n <= sizeof g_log_early) memcpy(g_log_early + at, p, n);
    } else if (state == LOG_FILE) {
        uint64_t at = __atomic_fetch_add(&g_log_bytes, n, __ATOMIC_RELAXED);
        if (at + n <= LOG_CAP) {
            plat_append_write(g_log_file, p, n);
        } else if (at <= LOG_CAP) {   // the first write past the cap says so, once
            static const char cut[] = "[host] log: the file reached 64 MiB, the rest of this run goes to stderr only\n";
            plat_append_write(g_log_file, cut, sizeof cut - 1);
        }
    }
}

void rt_vlogf(const char *fmt, va_list ap) {
    char buf[2048];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) rt_log_raw(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}

void rt_log(const char *fmt, ...) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[host] ");
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    n = k < 0 ? n : n + k < (int)sizeof buf - 1 ? n + k : (int)sizeof buf - 2;
    buf[n++] = '\n';
    rt_log_raw(buf, n);
}

const char *rt_log_path(void) {
    return __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_FILE ? g_log_path : NULL;
}

// Opens the log file for a game run (game = 1) and writes the buffered lines to it; anything else drops them. Runs on
// the main thread before the boot starts other threads.
static void log_open(int game) {
    char dir[PLAT_PATH_MAX * 3], from[PLAT_PATH_MAX * 3 + 16], to[PLAT_PATH_MAX * 3 + 16];
    char *cut = NULL;
    if (game && !plat_stderr_path(dir, sizeof dir) && plat_exe_path(dir, sizeof dir)) {
        cut = plat_last_sep(dir);
        char *f = strrchr(dir, '/');
        if (f > cut) cut = f;
    }
    if (cut && (size_t)(cut - dir) + sizeof "/logs" <= sizeof dir) {
        strcpy(cut, "/logs");
        plat_file_mkdir_p(dir);
        for (int i = LOG_KEEP - 1; i > 0; i--) {   // pluckomp.3.log -> pluckomp.4.log, ..., pluckomp.log -> pluckomp.1.log
            if (i > 1) snprintf(from, sizeof from, "%s/pluckomp.%d.log", dir, i - 1);
            else snprintf(from, sizeof from, "%s/pluckomp.log", dir);
            snprintf(to, sizeof to, "%s/pluckomp.%d.log", dir, i);
            if (plat_file_exists(from)) plat_file_replace(from, to);
        }
        snprintf(g_log_path, sizeof g_log_path, "%s/pluckomp.log", dir);
        g_log_file = plat_append_open(g_log_path);
    }
    if (!g_log_file) {
        __atomic_store_n(&g_log_state, LOG_OFF, __ATOMIC_RELEASE);
        return;
    }
    size_t n = __atomic_load_n(&g_log_early_n, __ATOMIC_RELAXED);
    if (n > sizeof g_log_early) n = sizeof g_log_early;
    plat_append_write(g_log_file, g_log_early, n);
    g_log_bytes = n;
    __atomic_store_n(&g_log_state, LOG_FILE, __ATOMIC_RELEASE);
    rt_log("log: %s", g_log_path);
}

static void parse_trace(void) {
    const char *e = cfg_get("PLUCKOMP_TRACE");   // no default: nothing is traced when unset
    if (!e) return;
    static const struct { const char *name; unsigned bit; } names[] = {
        {"svc", RT_TRACE_SVC}, {"host", RT_TRACE_HOSTCALL}, {"ipc", RT_TRACE_IPC}, {"fs", RT_TRACE_FS},
        {"gfx", RT_TRACE_GFX}, {"perf", RT_TRACE_PERF}, {"sched", RT_TRACE_SCHED}, {"audio", RT_TRACE_AUDIO},
        {"present", RT_TRACE_PRESENT}, {"all", ~0u}};
    for (const char *p = e; *p;) {
        size_t n = strcspn(p, ",");
        int known = 0;
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
            if (strlen(names[i].name) == n && !strncmp(p, names[i].name, n)) rt_trace |= names[i].bit, known = 1;
        if (!known && n) rt_log("PLUCKOMP_TRACE: unknown item '%.*s' (svc, host, ipc, fs, gfx, perf, sched, audio, present, all)", (int)n, p);
        p += n + (p[n] == ',');
    }
}

void rt_sha256(const void *p, size_t n, uint8_t out[32]) { sha256(p, n, out); }

// ---- configuration
static RtConfig cfg;
const RtConfig *rt_config(void) { return &cfg; }

// ---- runtime services the generated code calls (cpu.h)

// cpu.h RT_FPCR_NOTE: diagnostic only, the write itself takes effect as before (runtime/simd.h slow paths).
void rt_fpcr_rmode(uint64_t fpcr, uint64_t lr) {
    static int seen;
    if (__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED)) return;
    static const char *const mode[] = {"nearest", "+inf", "-inf", "zero"};
    char at[128];
    rt_log("fpcr: guest code set rounding mode %s (FPCR %#llx), in a function called from %s", mode[(fpcr >> 22) & 3],
           (unsigned long long)fpcr, ldr_describe(lr, at, sizeof at));
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    char at[128], lr[128];
    rt_fatal("brk #%#x at %s (lr %s)", imm, ldr_describe(pc, at, sizeof at), ldr_describe(c->x[30], lr, sizeof lr));
}

void rt_udf(Cpu *c, uint64_t pc) {
    char at[128], lr[128];
    rt_fatal("undefined instruction at %s (lr %s)", ldr_describe(pc, at, sizeof at),
             ldr_describe(c->x[30], lr, sizeof lr));
}

// CNTPCT_EL0: the Switch counter runs at 19.2 MHz. The scale is set before main, so no thread reads it half written.
static PlatScale tick_scale;

__attribute__((constructor)) static void tick_scale_init(void) { tick_scale = plat_scale(19200000u, plat_counter_hz()); }

uint64_t rt_ticks(void) { return plat_scaled(tick_scale, plat_counter()); }

// ---- crash reporter
typedef struct { uintptr_t host; uint64_t guest; } FnRev;
static FnRev *fn_rev;
static uint64_t fn_rev_n;

static int by_host(const void *a, const void *b) {
    uintptr_t x = ((const FnRev *)a)->host, y = ((const FnRev *)b)->host;
    return x < y ? -1 : x > y;
}

// The lifted function that contains a host code address, by the nearest function start below it; 0 outside the lifted
// code. *host, when given, gets the function's host start.
static uint64_t lifted_function_at(uintptr_t rip, uintptr_t *host) {
    if (!fn_rev) {
        static RtModule mods[MAX_MODULES];
        int n = rt_linked_modules(mods, MAX_MODULES);
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += mods[i].count;
        fn_rev = malloc((total ? total : 1) * sizeof *fn_rev);
        if (!fn_rev) return 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < mods[i].count; k++)
                fn_rev[fn_rev_n++] = (FnRev){(uintptr_t)mods[i].fns[k], mods[i].addrs[k]};
        qsort(fn_rev, fn_rev_n, sizeof *fn_rev, by_host);
    }
    // The lifted code is one range with the host code outside it (kernel.c ap_init checks that); an address above the
    // last function start is taken for host code, which costs only the last function's body.
    if (!fn_rev_n || rip > fn_rev[fn_rev_n - 1].host) return 0;
    uint64_t lo = 0, hi = fn_rev_n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (fn_rev[mid].host <= rip) lo = mid + 1;
        else hi = mid;
    }
    if (lo && host) *host = fn_rev[lo - 1].host;
    return lo ? fn_rev[lo - 1].guest : 0;
}

static uint64_t lifted_function(uintptr_t rip) { return lifted_function_at(rip, NULL); }

static int readable(uint64_t a, uint64_t n) { return plat_mem_readable(a, n); }

// The machine code around the faulting instruction: 32 bytes before rip, then 32 from rip. The user's pluckomp.exe is built
// on their machine, so its offsets need not match ours; these bytes and the offset into the lifted function find the
// same instruction in a local build of that version, and with it the guest instruction and the operands' registers.
static void crash_code_bytes(uintptr_t rip) {
    uint8_t b[64];
    int before = 32;
    if (!plat_mem_read(rip - 32, b, sizeof b)) {
        before = 0;   // rip near the start of a mapping: the bytes from rip only
        if (!plat_mem_read(rip, b + 32, 32)) return;
    }
    rt_logf("  host code at rip-%d:", before);
    for (int i = 32 - before; i < 64; i++) rt_logf("%s%02x", i == 32 ? " | " : " ", b[i]);
    rt_log_raw("\n", 1);
}

// 64 bytes around each register value that points at readable memory, as 8-byte words from 16 bytes below the value,
// the word holding the value marked with *. Host and guest registers that land on the same window share a line. A
// guest pointer is a host address here (the guest address space is mapped one to one), so both kinds are read alike.
// The guest registers come from the Cpu, which the lifted code writes back only at calls and returns, so the host
// registers carry the faulting function's live values.
typedef struct { uint64_t start; char names[56]; uint64_t w[8]; } CrashWin;

static void crash_win_add(CrashWin *wins, int *n, int cap, const char *name, uint64_t v) {
    if (v < 0x10000 || v > 0x00007fffffffffffull) return;   // small integers and non-canonical values
    uint64_t start = (v & ~7ull) - 16;
    for (int i = 0; i < *n; i++)
        if (wins[i].start == start) {
            size_t len = strlen(wins[i].names);
            if (len + strlen(name) + 2 < sizeof wins[i].names)
                snprintf(wins[i].names + len, sizeof wins[i].names - len, " %s", name);
            return;
        }
    if (*n == cap || !plat_mem_read(start, wins[*n].w, sizeof wins[*n].w)) return;
    wins[*n].start = start;
    snprintf(wins[*n].names, sizeof wins[*n].names, "%s", name);
    (*n)++;
}

static void crash_memory(const PlatCrash *x, const Cpu *c) {
    static CrashWin wins[64];   // the crash handler runs once (busy flag), so static storage needs no allocation
    static const char *const gpr_names[16] = PLAT_GPR_NAMES;
    int n = 0;
    char name[8];
    if (x->has_regs)
        for (int i = 0; i < 16; i++) crash_win_add(wins, &n, 64, gpr_names[i], x->gpr[i]);
    if (c) {
        for (int i = 0; i < 31; i++) snprintf(name, sizeof name, "x%d", i), crash_win_add(wins, &n, 64, name, c->x[i]);
        crash_win_add(wins, &n, 64, "sp", c->sp);
    }
    if (!n) return;
    rt_logf("  memory at the register values (8-byte words from value-16, * the value's word):\n");
    for (int i = 0; i < n; i++) {
        rt_logf("  %016llx %-12s", (unsigned long long)wins[i].start, wins[i].names);
        for (int k = 0; k < 8; k++) rt_logf(" %s%016llx", k == 2 ? "*" : "", (unsigned long long)wins[i].w[k]);
        rt_log_raw("\n", 1);
    }
    // The guest stack above sp: the crashing function's spills and locals, and the callers' saved registers
    uint64_t s[16];
    if (c && plat_mem_read(c->sp, s, sizeof s))
        for (int i = 0; i < 16; i += 4)
            rt_logf("  guest stack sp+0x%02x: %016llx %016llx %016llx %016llx\n", i * 8, (unsigned long long)s[i],
                    (unsigned long long)s[i + 1], (unsigned long long)s[i + 2], (unsigned long long)s[i + 3]);
}


static void crash_handler(const PlatCrash *x) {
    static volatile int32_t busy;
    if (plat_xchg32(&busy, 1)) plat_exit(71);
    char d[160];
    uintptr_t rip = x->rip;
    rt_logf("FATAL: %s (%#x) at host %p (", x->what, x->code, (void *)rip);
    if (x->module && !*x->module) rt_logf("outside every module)");
    else rt_logf("%s+%#llx)", x->module ? x->module : "pluckomp", (unsigned long long)x->offset);
    if (x->access >= 0)
        rt_logf(", %s address %#llx", x->access == 1 ? "writing" : x->access == 8 ? "executing" : "reading",
                (unsigned long long)x->addr);
    rt_log_raw("\n", 1);
    if (x->cxx_type)
        rt_logf("  C++ type %s%s%s\n", x->cxx_type, x->cxx_what ? ", what(): " : "", x->cxx_what ? x->cxx_what : "");
    // A driver that runs out of memory faults or throws (std::bad_alloc) instead of returning an error: the commit
    // tells that apart from a driver bug, as rt_fatal's line does for our own failed allocations.
    double proc, used, limit;
    plat_commit_info(&proc, &used, &limit);
    rt_logf("  memory commit: this process %.0f MB, the system %.0f of %.0f MB\n", proc, used, limit);
    uintptr_t fstart = 0;
    uint64_t f = x->module ? 0 : lifted_function_at(rip, &fstart);
    if (f)
        rt_logf("  in lifted function f_%llx = %s, host +%#llx into it\n", (unsigned long long)f,
                ldr_describe(f, d, sizeof d), (unsigned long long)(rip - fstart));
    if (x->has_regs) {
        static const char *const gpr_names[16] = PLAT_GPR_NAMES;
        for (int i = 0; i < 16; i++)
            rt_logf("  %-3s %016llx%s", gpr_names[i], (unsigned long long)x->gpr[i], i % 4 == 3 ? "\n" : "");
        crash_code_bytes(rip);
    }
    // abort and invalid parameter: where the host code called it; a fault in another module: the driver code and the
    // host calls that led into it
    uintptr_t exe = plat_exe_base();
    for (int i = 0; i < x->nframes; i++) {
        const char *m = x->frame_module[i];
        if (m) {
            rt_logf("  host frame %2d: %s+%#llx\n", i, *m ? m : "outside every module",
                    (unsigned long long)x->frame_offset[i]);
            continue;
        }
        uint64_t g = lifted_function(x->frames[i]);
        rt_logf("  host frame %2d: pluckomp+%#llx", i, (unsigned long long)(x->frames[i] - exe));
        if (g) rt_logf(" in lifted function f_%llx", (unsigned long long)g);
        rt_log_raw("\n", 1);
    }
    if (x->frames_left_out) rt_logf("  (%d frames in other modules left out)\n", x->frames_left_out);
    Cpu *c = kern_cpu();
    if (c) {
        for (int i = 0; i < 31; i++)
            rt_logf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "");
        // pc: the guest code running, or the host function a guest call entered (its name tells the host call apart)
        rt_logf("  sp  %016llx\n  pc  %016llx", (unsigned long long)c->sp, (unsigned long long)c->pc);
        if (hostfn_name(c->pc)) rt_logf(" (%s)", ldr_describe(c->pc, d, sizeof d));
        rt_logf("  lr %s\n", ldr_describe(c->x[30], d, sizeof d));
        uint64_t fp = c->x[29];
        for (int n = 0; n < 32 && fp && !(fp & 7) && readable(fp, 16); n++) {
            uint64_t next = LD64(fp), lr = LD64(fp + 8);
            rt_logf("  guest frame %2d: %s\n", n, ldr_describe(lr, d, sizeof d));
            if (next <= fp || next - fp > (64u << 20)) break;
            fp = next;
        }
    } else {
        rt_logf("  not on a guest thread\n");
    }
    crash_memory(x, c);
    kern_free_cores_note();
    fflush(stderr);
    plat_exit(71);
}

// ---- self-test: layout, dispatch table, host-function registry, without game files
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static double now_s(void) { return (double)plat_now_ns() / 1e9; }

static volatile int host_probe_hits;
static void host_probe(Cpu *c) { host_probe_hits++; c->x[0] = 42; }

// The instruction set build_game.py --cpu compiled for: v3 (AVX2/FMA/BMI2, the default) or v2 (SSE4.2/POPCNT).
#ifndef RT_CPU_LEVEL
#define RT_CPU_LEVEL 3
#endif

static void check_cpu(void) {
    __builtin_cpu_init();
#if RT_CPU_LEVEL >= 3
    int ok = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2");
    printf("cpu: x86-64-v3 (AVX2, FMA, BMI2) %s\n", ok ? "present" : "MISSING");
    CHECK(ok, "the lifted code is built for x86-64-v3");
#else
    int ok = __builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt");
    printf("cpu: x86-64-v2 (SSE4.2, POPCNT) %s\n", ok ? "present" : "MISSING");
    CHECK(ok, "the lifted code is built for x86-64-v2");
#endif
}

// Layout: ascending, no overlap, below 2^39, and inside the window mem_init reserved.
static void check_layout(void) {
    const RtLayoutEntry *lay = rt_layout;
    int n = rt_layout_count;
    printf("layout: %d modules\n", n);
    for (int i = 0; i < n; i++) {
        printf("  %-8s base %#llx  size %#10llx  end %#llx\n", lay[i].name, (unsigned long long)lay[i].base,
               (unsigned long long)lay[i].size, (unsigned long long)(lay[i].base + lay[i].size));
        CHECK(lay[i].size > 0, "%s has size 0", lay[i].name);
        CHECK(lay[i].base % 0x10000 == 0, "%s base is not 64 KiB aligned", lay[i].name);
        CHECK(lay[i].base + lay[i].size <= RT_KERNEL_BASE, "%s reaches into the kernel region", lay[i].name);
        if (i) {
            CHECK(lay[i].base > lay[i - 1].base, "%s base is not above %s", lay[i].name, lay[i - 1].name);
            CHECK(lay[i - 1].base + lay[i - 1].size <= lay[i].base, "%s overlaps %s", lay[i - 1].name, lay[i].name);
        }
        uint64_t end;
        int ours = plat_mem_state(lay[i].base, lay[i].base + 1, &end) == PLAT_MEM_RESERVED;
        CHECK(ours, "%s is not inside the reserved module window", lay[i].name);
    }
    const RtGuestLayout *g = mem_layout();
    printf("guest memory: aslr %#llx+%#llx stack %#llx heap %#llx alias %#llx+%#llx kernel %#llx host functions %#llx\n",
           (unsigned long long)g->aslr.base, (unsigned long long)g->aslr.size, (unsigned long long)g->stack.base,
           (unsigned long long)g->heap.base, (unsigned long long)g->alias.base, (unsigned long long)g->alias.size,
           (unsigned long long)g->kernel.base, (unsigned long long)g->hostfn.base);
    CHECK(g->alias.base + g->alias.size <= lay[0].base && g->heap.base + g->heap.size <= g->alias.base,
          "guest regions overlap");
}

// Every function start dispatches to its own function. The word after a start (unless that is a start too), a
// misaligned address and the word below the module dispatch to nothing.
static void check_modules(const RtModule *mods, int n) {
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        const RtModule *m = &mods[i];
        const RtLayoutEntry *l = NULL;
        for (int k = 0; k < rt_layout_count; k++)
            if (!strcmp(rt_layout[k].name, m->name)) l = &rt_layout[k];
        CHECK(l && l->base == m->base && l->size == m->size, "%s: linked base/size differ from the layout", m->name);

        uint64_t bad = 0, bad_neg = 0;
        for (uint64_t k = 0; k < m->count; k++) {
            uint64_t a = m->addrs[k];
            if (rt_dispatch_lookup(a) != m->fns[k] && bad++ < 5)
                printf("  FAIL: %s_fn_addrs[%llu] %#llx does not dispatch to its function\n", m->name,
                       (unsigned long long)k, (unsigned long long)a);
            int next_is_start = k + 1 < m->count && m->addrs[k + 1] == a + 4;
            if ((!next_is_start && rt_dispatch_lookup(a + 4)) || rt_dispatch_lookup(a + 2)) bad_neg++;
        }
        if (rt_dispatch_lookup(m->base - 4)) bad_neg++;
        failures += bad > 0;
        CHECK(!bad_neg, "%s: %llu addresses that are not function starts dispatch to a function", m->name,
              (unsigned long long)bad_neg);
        uint64_t lo = m->count ? m->addrs[0] : 0, hi = m->count ? m->addrs[m->count - 1] : 0;
        printf("  %-8s %8llu functions, first %#llx, last %#llx: %s\n", m->name, (unsigned long long)m->count,
               (unsigned long long)lo, (unsigned long long)hi, bad || bad_neg ? "FAIL" : "ok");
        total += m->count;
    }
    printf("  total    %8llu functions\n", (unsigned long long)total);
}

static void bench(const RtModule *mods, int n) {
    uint64_t total = 0, sink = 0;
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        for (uint64_t k = 0; k < mods[i].count; k++) sink += (uintptr_t)rt_dispatch_lookup(mods[i].addrs[k]);
        total += mods[i].count;
    }
    double dt = now_s() - t0;
    __asm__ volatile("" ::"r"(sink));
    if (total) printf("lookup: %.1f ns per function start (address order)\n", dt * 1e9 / (double)total);
}

// Memory SVCs without modules: heap, physical memory in the alias region, a stack move, QueryMemory, GetInfo.
static void check_memory(void) {
    const RtGuestLayout *g = mem_layout();
    RtMemoryInfo q;
    uint64_t heap = 0, v = 0;
    int before = failures;
    CHECK(mem_set_heap_size(0x400000, &heap) == 0 && heap == g->heap.base, "SetHeapSize 4 MiB");
    memset((void *)(uintptr_t)heap, 0xAB, 0x400000);
    mem_query(heap + 0x1000, &q);
    CHECK(q.base == heap && q.size == 0x400000 && q.state == 5 && q.perm == 3, "QueryMemory on the heap");
    CHECK(mem_set_heap_size(0x300000, &heap) != 0, "SetHeapSize accepts a size that is not a 2 MiB multiple");

    uint64_t a = g->alias.base + 0x100000;
    CHECK(mem_map_physical(a, 0x10000) == 0, "MapPhysicalMemory");
    ST64(a + 0x8000, 0x1234);
    CHECK(mem_map_physical(a - 0x4000, 0x20000) == 0 && LD64(a + 0x8000) == 0x1234,
          "MapPhysicalMemory over a mapped range lost its contents");
    mem_query(a, &q);
    CHECK(q.base == a - 0x4000 && q.size == 0x20000 && q.state == 5, "QueryMemory on mapped alias pages");
    mem_info(7, &v);
    CHECK(v == 0x400000 + 0x20000, "GetInfo 7 (used memory) %#llx", (unsigned long long)v);

    uint64_t stack = g->stack.base + 0x100000;
    ST64(a + 8, 0x99);
    CHECK(mem_map(stack, a, 0x4000) == 0 && LD64(stack + 8) == 0x99, "MapMemory alias -> stack did not copy");
    ST64(stack + 0x10, 0x5678);
    mem_query(stack, &q);
    CHECK(q.base == stack && q.size == 0x4000 && q.state == 0xB, "QueryMemory on a moved stack");
    mem_query(a, &q);
    CHECK(q.base == a && q.state == 5 && q.perm == 0, "QueryMemory on a moved stack's source");
    CHECK(mem_unmap_physical(a, 0x4000) != 0, "UnmapPhysicalMemory under a moved stack");
    CHECK(mem_unmap(stack, a, 0x4000) == 0 && LD64(a + 0x10) == 0x5678, "UnmapMemory did not copy the stack back");

    CHECK(mem_unmap_physical(a - 0x4000, 0x20000) == 0, "UnmapPhysicalMemory");
    mem_query(a, &q);
    CHECK(q.state == 0 && q.base == g->alias.base && q.size == g->alias.size, "QueryMemory on the free alias region");
    CHECK(mem_map_physical(a, 0x1000) == 0 && LD64(a + 0x10) == 0 && mem_unmap_physical(a, 0x1000) == 0,
          "remapped physical memory is not zeroed");
    // past the pool whatever its size (PLUCKOMP_APP_POOL, PLUCKOMP_TEX_HEAP_EXTRA_MB): 4 GiB fits a grown one
    CHECK(mem_map_physical(a, (g->pool + 0x100000) & ~0xfffffull) == 0x10801,
          "MapPhysicalMemory beyond the pool is not LimitReached");
    mem_query(RT_HOSTFN_BASE + 0x40, &q);
    CHECK(q.state == 0x10 && q.base == RT_HOSTFN_BASE, "QueryMemory on the host-function window");
    mem_query(g->alias.base + g->alias.size + 0x1000, &q);
    CHECK(q.state == 3 && q.base == g->alias.base + g->alias.size, "QueryMemory on host memory");
    CHECK(mem_set_heap_size(0, &heap) == 0, "SetHeapSize 0");
    printf("memory: heap, physical memory, stack move, QueryMemory, GetInfo: %s\n", failures > before ? "FAIL" : "ok");
}

static int selftest(void) {
    static RtModule mods[MAX_MODULES];
    int n = rt_linked_modules(mods, MAX_MODULES);

    check_cpu();
    check_layout();
    uint64_t probe = hostfn_register("<selftest probe>", host_probe);

    double t0 = now_s();
    hostfn_seal();
    double dt = now_s() - t0;
    RtDispatchStats s = rt_dispatch_stats();
    printf("dispatch: %d modules linked, table covers %#llx-%#llx: %llu slots, %.1f MiB reserved, built in %.0f ms\n",
           n, (unsigned long long)s.lo, (unsigned long long)s.hi, (unsigned long long)s.slots,
           (double)s.table_bytes / (1 << 20), dt * 1e3);
    check_modules(mods, n);
    CHECK(s.functions > 0, "no functions linked");
    Cpu *c = plat_aligned_alloc(sizeof(Cpu), 64);
    memset(c, 0, sizeof *c);
    rt_fpcr_threshold(c);
    uint64_t r = rt_call(c, probe, 1, (uint64_t)7);
    CHECK(rt_dispatch_lookup(probe) && r == 42 && host_probe_hits == 1 && c->x[30] == 0,
          "a registered host function does not dispatch through rt_call");
    CHECK(!rt_dispatch_lookup(probe + 16), "an unregistered host-function address dispatches");
    CHECK(rt_dispatch_lookup(RT_RET_HOST), "the rt_call return sentinel is not registered");
    plat_aligned_free(c);
    bench(mods, n);

    void *k1 = mem_kernel_alloc(0x200, 16), *k2 = mem_kernel_alloc(0x2000, 0x1000);
    CHECK((uintptr_t)k1 - RT_KERNEL_BASE < RT_KERNEL_SIZE && !((uintptr_t)k2 & 0xFFF) && k1 != k2,
          "kernel-region allocation");
    mem_kernel_free(k1, 0x200);
    CHECK(mem_kernel_alloc(0x200, 16) == k1, "a freed kernel-region block is not reused");
    check_memory();

    float used, total, proc;
    plat_mem_info(&used, &total, &proc);
    if (proc >= 0) printf("process: working set %.0f MiB\n", proc);
    printf("selftest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}

static int usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--exefs DIR] [--romfs DIR] [--profile DIR] "
                    "[--selftest | --probe-loader | --boot | --frames N]\n", argv0);
    return 2;
}

int main(int argc, char **argv) {
    mem_init();   // first: claim the guest address space before anything else can land there
    rt_log("version: Pluckomp %s", rt_port_version);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    plat_crash_hook(crash_handler);
    parse_trace();
    // Timed guest waits (kernel.c block_inner) sleep in whole milliseconds; at the default 15.6 ms tick a 1 ms wait
    // costs a full tick. PLUCKOMP_TIMER_1MS=0 keeps the default for comparison.
    if (cfg_bool("PLUCKOMP_TIMER_1MS")) plat_timer_1ms();
    cfg = (RtConfig){.exefs = rt_default_exefs, .romfs = rt_default_romfs,
                     .profile = cfg_get("PLUCKOMP_PROFILE_DIR") ? cfg_get("PLUCKOMP_PROFILE_DIR") : "build/profile"};
    const char *mode = NULL;
    for (int i = 1; i < argc; i++) {
        const char **path = !strcmp(argv[i], "--exefs") ? &cfg.exefs : !strcmp(argv[i], "--romfs") ? &cfg.romfs
                          : !strcmp(argv[i], "--profile") ? &cfg.profile : NULL;
        if (path) {
            if (++i == argc) return usage(argv[0]);
            *path = argv[i];
        } else if (!mode && !strcmp(argv[i], "--frames")) {
            char *end;
            if (++i == argc) return usage(argv[0]);
            unsigned long long n = strtoull(argv[i], &end, 10);
            if (*end || !n) return usage(argv[0]);
            cfg.frames = n;
            mode = "--frames";
        } else if (!mode && !strncmp(argv[i], "--", 2)) {
            mode = argv[i];
        } else {
            return usage(argv[0]);
        }
    }
    log_open(!mode || !strcmp(mode, "--boot") || !strcmp(mode, "--frames"));
    if (!mode) {
        rt_boot();
        return 0;
    }
    if (!strcmp(mode, "--selftest")) return selftest();
    if (!strcmp(mode, "--probe-loader")) return probe_loader();
    if (!strcmp(mode, "--boot")) { cfg.boot = 1; rt_boot(); return 0; }
    if (!strcmp(mode, "--frames")) { rt_boot(); return 0; }
    return usage(argv[0]);
}
