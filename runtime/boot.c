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
#include "nvn.h"
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
    if (!rt_config()->null_gfx) vk_pipeline_save();
    prof_write();   // plat_exit runs no atexit handlers
    plat_exit(0);   // other guest threads are still running; nothing of theirs needs an orderly end
}

// what "nvn": nvnBootstrapLoader(detail), the first NVN call. Ends --boot; returns otherwise.
void rt_boot_event(const char *what, Cpu *c, const char *detail) {
    char at[128];
    if (strcmp(what, "nvn")) rt_fatal("boot: unknown checkpoint %s", what);
    rt_log("boot: first NVN call nvnBootstrapLoader(\"%s\") from %s", detail, caller(c, at, sizeof at));
    if (rt_config()->boot) run_end();
}

// ---- --shot: the presented texture as a PNG
// A PNG with stored (uncompressed) deflate blocks: no compression library, and the shot is only read by tools.
// From TotK Recomp runtime/boot.c rt_write_png.
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t hdr[8] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    memcpy(hdr + 4, type, 4);
    uint32_t crc = crc32_update(crc32_update(~0u, hdr + 4, 4), data, n) ^ ~0u;
    uint8_t tail[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    fwrite(tail, 1, 4, f);
}

void rt_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    uint64_t row = 1 + 4ull * w, raw_n = row * h;
    uint8_t *raw = malloc(raw_n);
    if (!raw) rt_fatal("boot: out of memory for the shot");
    for (uint32_t y = 0; y < h; y++) {
        raw[y * row] = 0;   // filter: none
        memcpy(raw + y * row + 1, rgba + 4ull * w * y, 4ull * w);
    }
    uint64_t blocks = (raw_n + 65534) / 65535;
    uint8_t *z = malloc(2 + raw_n + 5 * blocks + 4);
    if (!z) rt_fatal("boot: out of memory for the shot");
    uint64_t zn = 0;
    z[zn++] = 0x78, z[zn++] = 0x01;
    uint32_t a = 1, b = 0;
    for (uint64_t off = 0; off < raw_n; off += 65535) {
        uint32_t k = raw_n - off > 65535 ? 65535 : (uint32_t)(raw_n - off);
        z[zn++] = off + k >= raw_n;
        z[zn++] = (uint8_t)k, z[zn++] = (uint8_t)(k >> 8), z[zn++] = (uint8_t)~k, z[zn++] = (uint8_t)(~k >> 8);
        memcpy(z + zn, raw + off, k);
        zn += k;
        for (uint32_t i = 0; i < k; i++) a = (a + raw[off + i]) % 65521, b = (b + a) % 65521;
    }
    uint32_t ad = b << 16 | a;
    z[zn++] = (uint8_t)(ad >> 24), z[zn++] = (uint8_t)(ad >> 16), z[zn++] = (uint8_t)(ad >> 8), z[zn++] = (uint8_t)ad;
    FILE *f = fopen(path, "wb");
    if (!f) rt_fatal("boot: cannot write %s", path);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
    fwrite(sig, 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", NULL, 0);
    if (fclose(f)) rt_fatal("boot: cannot write %s", path);
    free(z);
    free(raw);
}

// The presented texture through the back end's ReadTexels (8-bit RGBA or BGRA), as TotK Recomp runtime/boot.c
// save_shot does.
static void save_shot(const tex_t *t, const char *path) {
    int fmt = t->d.format;
    int bgra = fmt == NVN_FORMAT_BGRA8 || fmt == NVN_FORMAT_BGRA8_SRGB;
    if (!bgra && fmt != NVN_FORMAT_RGBA8 && fmt != NVN_FORMAT_RGBA8_SRGB)
        rt_fatal("boot: --shot supports 8-bit RGBA and BGRA targets, the presented texture has NVN format %#x", fmt);
    uint32_t w = t->d.width, h = t->d.height;
    uint8_t *px = malloc(4ull * w * h);
    if (!px) rt_fatal("boot: out of memory for the shot");
    view_t view;
    memset(&view, 0, sizeof view);
    NVNcopyRegion r = {.width = (int)w, .height = (int)h, .depth = 1};
    if (!rt_config()->null_gfx && gfx_replay_async()) gfx_backend()->finish();   // the frame may still be queued
    gfx_backend()->read_texels(t, &view, &r, px, 4ull * w, 4ull * w * h);
    if (bgra)
        for (uint64_t i = 0; i < 4ull * w * h; i += 4) {
            uint8_t x = px[i];
            px[i] = px[i + 2];
            px[i + 2] = x;
        }
    for (uint64_t i = 3; i < 4ull * w * h; i += 4) px[i] = 255;   // the window's alpha is not shown
    rt_write_png(path, px, w, h);
    free(px);
    rt_logf("boot: shot %s (%ux%u)\n", path, w, h);
}

// The nth present of the game's window t (n from 1). --frames N ends the run at the Nth.
void rt_frame_presented(const tex_t *t, uint64_t n) {
    const RtConfig *cfg = rt_config();
    uint64_t want = cfg->frames;
    if (!want || n < want) return;
    if (cfg->shot) save_shot(t, cfg->shot);
    GfxStats g = gfx_stats();
    rt_logf("boot: present %llu reached (nvn calls %llu, distinct procs %llu, unimplemented %llu, validation errors "
            "%llu, shader failures %llu, draws skipped %llu)\n", (unsigned long long)n,
            (unsigned long long)g.nvn_calls, (unsigned long long)g.nvn_distinct, (unsigned long long)g.unimplemented,
            (unsigned long long)g.validation_errors, (unsigned long long)g.shader_failures,
            (unsigned long long)g.draws_skipped);
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

static void prewarm_main(void *arg) {
    (void)arg;
    vk_pipeline_prewarm();
}

void rt_boot(void) {
    check_npdm();
    const RtConfig *cfg = rt_config();
    rt_log("boot: graphics back end %s", gfx_backend()->name);
    PlatThread *warm = NULL;
    if (!cfg->null_gfx) {
        if (!cfg->shot) vk_pipeline_async_enable();   // a --shot image must not depend on build timing
        // Shaders and pipelines recorded in earlier runs are made ready while the modules load; the guest waits.
        warm = plat_thread_new(prewarm_main, NULL, "prewarm");
    }
    ldr_load(cfg->exefs);
    cb_addr[0] = hostfn_register("<rtld loader_ready>", cb_loader_ready);
    cb_addr[1] = hostfn_register("<rtld module_inits>", cb_module_inits);
    cb_addr[2] = hostfn_register("<rtld module_finis>", cb_module_finis);
    hostfn_seal();
    uint64_t held = plat_now_ms();
    if (warm) plat_thread_join(warm);
    else if (!cfg->null_gfx) vk_pipeline_prewarm();
    held = plat_now_ms() - held;
    rt_log("boot: guest starts %llu ms after process start (%llu ms of it waiting for the prewarm)",
           (unsigned long long)(plat_process_uptime_ns() / 1000000), (unsigned long long)held);
    rt_guest_start_ns = kern_now_ns();
    kern_run_on_guest_main(guest_main);
    rt_fatal("boot: the guest main thread ended");
}
