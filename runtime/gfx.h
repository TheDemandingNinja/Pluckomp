// Adapted from TotK Recomp runtime/gfx.h @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// Interface of the graphics layer: the NVN front end (nvn_*.c) and its back end. The null back end (gfx_null.c) is the
// only one for now: no GPU, presents are logged. The window, shader service and Vulkan back end of TotK Recomp are not
// ported.
#pragma once
#include <stdint.h>

#include "host.h"
#include "nvn_obj.h"

// ---- shared with the system overrides
enum { OE_MSG_FOCUS_STATE_CHANGED = 15, OE_MSG_OPERATION_MODE_CHANGED = 30, OE_MSG_PERFORMANCE_MODE_CHANGED = 31 };
void oe_push_message(uint32_t message);                   // ov_system.c
void oe_set_focus(int in_focus);                          // ov_system.c: GetCurrentFocusState; PLUCKOMP_ASSUME_FOCUS pins 1

// ---- GPU address window for CPU-invisible pools
#define RT_GPU_VA_BASE 0x8000000000ull                     // 2^39, never mapped on the host
#define RT_GPU_VA_SIZE (64ull << 30)

// ---- back end. Called by the NVN front end (nvn_api_*.c, nvn_ext.c) on guest threads.
typedef struct {
    const char *name;
    int  (*pool_init)(pool_t *p);                          // 0 fails MemoryPoolInitialize
    void (*pool_fini)(pool_t *p);
    int  (*pool_map_virtual)(pool_t *v, int n, const void *requests);   // NVNmappingRequest[n], 0x28 bytes each
    void (*texture_register)(int id, const tex_t *t, const view_t *v);
    void (*sampler_register)(int id, const samp_par *s);
    void (*texture_release)(const tex_t *t);
    void (*program_release)(const prog_t *p);
    uint64_t (*submit)(const uint64_t *handles, int n);    // replays; returns this submission's fence value
    int  (*fence_wait)(uint64_t value, uint64_t timeout_ns);   // 1 reached; caller has given its core up
    uint64_t (*fence_done)(void);                          // highest value whose side effects are written
    // Queues the present of window image t and returns at once; a back end with a display shows it at at_ns
    // (kern_now_ns clock), at once when that has passed or at_ns is 0. frame counts presents from 1.
    void (*present)(const tex_t *t, const int crop[4], uint64_t at_ns, uint64_t frame);
    void (*finish)(void);
    void (*read_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img);
    void (*write_texels)(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img);
    void (*guest_written)(uint64_t addr, uint64_t size);   // host wrote pool storage
    void (*guest_read)(uint64_t addr, uint64_t size);      // the CPU reads pool storage: GPU-written texels go there first
    void (*cpu_synced)(uint64_t fence);                    // the CPU waited for fence
} GfxBackend;
extern const GfxBackend gfx_null_backend;
const GfxBackend *gfx_backend(void);                       // the null back end

// ---- NVN front end (nvn_procs.c)
void nvn_procs_register(void);                             // hostfn_register every proc; before hostfn_seal
uint64_t nvn_proc_address(const char *name);               // synthetic host address of a proc, 0 for an unknown name

// ---- statistics and frames (nvn_api_*.c, gfx_null.c; boot.c prints them)
typedef struct { uint64_t frames, nvn_calls, nvn_distinct, unimplemented; } GfxStats;
GfxStats gfx_stats(void);
void rt_frame_presented(uint64_t n);                       // boot.c: --frames N ends the run after the Nth present
