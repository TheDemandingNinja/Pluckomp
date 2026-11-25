// Adapted from TotK Recomp runtime/boot.c @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh), itself from
// MacNativeGames games/totk/runtime/loader.c and services.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors) and LAS Recomp runtime/main.c @ da35b83 (MIT, Copyright (c) 2026 Peiikai).
// The boot driver: the guest main thread runs the game's own nn::init::Start with three host callbacks, the second of
// which runs DT_INIT of main, subsdk0 and sdk; Start then calls nnMain. UE4's FEngineLoop runs from nnMain.
//
// Checkpoints (logged on every run):
//   module constructors    every module's constructor flag reads 2 after its DT_INIT
//   nnMain                 the game's entry, observed through its export
//   nvnBootstrapLoader     the first NVN call, in UE4's NVN RHI bring-up (nvn_procs.c reports it through
//                          rt_boot_event). --boot ends here with exit code 0.
//   present N              nvnQueuePresentTexture (nvn_api_obj.c, rt_frame_presented). --frames N ends the run after
//                          the Nth present with exit code 0.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

#define SYM_INIT_START "_ZN2nn4init5StartEmmPFvvES2_S2_"
#define CTOR_DONE 2   // a module's constructor flag after its DT_INIT ran the init_array

// main.npdm META of Pikmin 4 (every version so far): main thread priority 44 on core 0 with a 64 KiB stack.
// kernel.c builds the main thread with these values (MAIN_PRIO, MAIN_STACK).
#define NPDM_PRIO 44
#define NPDM_CORE 0
#define NPDM_STACK 0x10000u

static volatile int32_t inits_done;
uint64_t rt_guest_start_ns;

static const RtLoaded *module_named(const char *name) {
    for (int i = 0; i < ldr_count(); i++)
        if (!strcmp(ldr_module(i)->name, name)) return ldr_module(i);
    rt_fatal("boot: module %s is not loaded", name);
}

static uint32_t ctor_state(const RtLoaded *m) { return m->ctor_flag ? LD32(m->ctor_flag) : 0xffffffffu; }

static const char *caller(const Cpu *c, char *buf, int n) { return ldr_describe(c->x[30] - 4, buf, n); }

static void check_npdm(void) {
    size_t n;
    uint8_t *npdm = ldr_read_verified(rt_config()->exefs, "main.npdm", &n);
    if (n < 0x80 || memcmp(npdm, "META", 4)) rt_fatal("boot: main.npdm has no META header");
    uint32_t stack;
    memcpy(&stack, npdm + 0x1C, 4);
    int prio = npdm[0xE], core = npdm[0xF];
    free(npdm);
    if (prio != NPDM_PRIO || core != NPDM_CORE || stack != NPDM_STACK)
        rt_fatal("boot: main.npdm asks for priority %d, core %d, stack %#x; kernel.c builds %d, %d, %#x", prio, core,
                 stack, NPDM_PRIO, NPDM_CORE, NPDM_STACK);
}

static void stats_line(void) {
    RtStats s = rt_stats();
    rt_logf("boot: passed (%llu SVCs, %llu threads)\n", (unsigned long long)s.svcs, (unsigned long long)s.threads);
}

// ---- the three callbacks of nn::init::Start (rtld's job on hardware)
static void cb_loader_ready(Cpu *c) {
    (void)c;
    rt_log("boot: rtld callback: modules linked");
}

// DT_INIT registers the module with musl (__nnmusl_init_dso) and runs its init_array; the constructor flag reads 2
// afterwards. Order main, subsdk0, sdk, as TotK Recomp found the SDK expects without a plugin.
static void cb_module_inits(Cpu *c) {
    if (plat_cas32(&inits_done, 0, 0)) rt_fatal("boot: module constructors requested twice");
    static const char *const order[] = {"main", "subsdk0", "sdk"};
    for (unsigned k = 0; k < sizeof order / sizeof *order; k++) {
        const RtLoaded *m = module_named(order[k]);
        uint64_t entries = m->init_array_size / 8;
        if (!m->init) rt_fatal("boot: %s has no DT_INIT", m->name);
        rt_log("boot: ENTER %s DT_INIT (%llu init_array entries)", m->name, (unsigned long long)entries);
        rt_call(c, m->init, 0);
        uint32_t state = ctor_state(m);
        rt_log("boot: DT_INIT %s done, constructor flag %u", m->name, state);
        if (state != CTOR_DONE)
            rt_fatal("boot: %s DT_INIT left its constructor flag at %u, not %d", m->name, state, CTOR_DONE);
    }
    plat_xchg32(&inits_done, 1);
}

static void cb_module_finis(Cpu *c) {
    char lr[128];
    rt_fatal("boot: the game asked for module finalizers (from %s): the guest exited", caller(c, lr, sizeof lr));
}

// ---- checkpoints
static void run_end(void) {
    stats_line();
    fflush(stdout);
    fflush(stderr);
    fs_save_flush();
    plat_exit(0);   // other guest threads are still running; nothing of theirs needs an orderly end
}

// what "nvn": nvnBootstrapLoader(detail), the first NVN call. Ends --boot; returns otherwise.
void rt_boot_event(const char *what, Cpu *c, const char *detail) {
    char at[128];
    if (strcmp(what, "nvn")) rt_fatal("boot: unknown checkpoint %s", what);
    rt_log("boot: first NVN call nvnBootstrapLoader(\"%s\") from %s", detail, caller(c, at, sizeof at));
    if (rt_config()->boot) run_end();
}

// The nth present of the game's window (n from 1). --frames N ends the run at the Nth.
void rt_frame_presented(uint64_t n) {
    uint64_t want = rt_config()->frames;
    if (!want || n < want) return;
    GfxStats g = gfx_stats();
    rt_logf("boot: present %llu reached (nvn calls %llu, distinct procs %llu)\n", (unsigned long long)n,
            (unsigned long long)g.nvn_calls, (unsigned long long)g.nvn_distinct);
    run_end();
}

// ---- the guest main thread
static uint64_t cb_addr[3];

static void guest_main(Cpu *c) {
    uint32_t handle = kern_main_thread(&c);
    uint64_t start = ldr_symbol(SYM_INIT_START);
    if (!start) rt_fatal("boot: %s is not exported", SYM_INIT_START);
    char d[128];
    rt_log("boot: ENTER nn::init::Start at %s, main thread %#x", ldr_describe(start, d, sizeof d), handle);
    // nn::init::Start(main thread handle, arguments region, loader_ready, module_inits, module_finis)
    rt_call(c, start, 5, (uint64_t)handle, (uint64_t)0, cb_addr[0], cb_addr[1], cb_addr[2]);
    rt_fatal("boot: nn::init::Start returned");
}

void rt_boot(void) {
    check_npdm();
    ldr_load(rt_config()->exefs);
    cb_addr[0] = hostfn_register("<rtld loader_ready>", cb_loader_ready);
    cb_addr[1] = hostfn_register("<rtld module_inits>", cb_module_inits);
    cb_addr[2] = hostfn_register("<rtld module_finis>", cb_module_finis);
    hostfn_seal();
    rt_log("boot: guest starts %llu ms after process start",
           (unsigned long long)(plat_process_uptime_ns() / 1000000));
    rt_guest_start_ns = kern_now_ns();
    kern_run_on_guest_main(guest_main);
    rt_fatal("boot: the guest main thread ended");
}
