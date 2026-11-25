// Adapted from TotK Recomp runtime/nvn_procs.c and the nvnBootstrapLoader override of runtime/ov_gfx.c @ afae80c (MIT,
// Copyright (c) 2026 Valiantsin Dzerakh); the bootstrap stop takes the place of MacNativeGames' NVN entry
// (games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c @ b75be8cf), no code of it is copied.
// NVN proc table. Every name the game's proc loader asks for gets a synthetic host address through hostfn_register
// before hostfn_seal; nvnBootstrapLoader and nvnDeviceGetProcAddress hand them out.
// nvnp_<name> is the implementation of one proc: the weak definitions here stop with the proc's name, nvn_api_*.c
// and nvn_ext.c define the strong ones. The name list is runtime/nvn_procs.inc (tools/gen_nvn_procs.py).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

// The strong nvnp_nvnDeviceGetProcAddress below replaces the stand-in generated for the same row.
#define nvnp_nvnDeviceGetProcAddress nvnp_stub_nvnDeviceGetProcAddress

#define NVN_PROC(n) void nvnp_##n(Cpu *c);
#include "nvn_procs.inc"
#undef NVN_PROC

#define NVN_PROC(n)                                                                                                  \
    __attribute__((weak)) void nvnp_##n(Cpu *c) {                                                                     \
        (void)c;                                                                                                     \
        rt_fatal("NVN " #n " is not implemented");                                                                   \
    }
#include "nvn_procs.inc"
#undef NVN_PROC
#undef nvnp_nvnDeviceGetProcAddress
void nvnp_nvnDeviceGetProcAddress(Cpu *c);

typedef struct { const char *name; uint64_t addr; } Proc;

#define NVN_PROC(n) +1
enum { PROC_COUNT = 0
#include "nvn_procs.inc"
};
#undef NVN_PROC

static Proc procs[PROC_COUNT];
static int nprocs;

static int by_name(const void *a, const void *b) { return strcmp(((const Proc *)a)->name, ((const Proc *)b)->name); }

void nvn_procs_register(void) {
    if (nprocs) return;
#define NVN_PROC(n) procs[nprocs++] = (Proc){#n, hostfn_register("nvn:" #n, nvnp_##n)};
#include "nvn_procs.inc"
#undef NVN_PROC
    qsort(procs, (size_t)nprocs, sizeof *procs, by_name);
}

uint64_t nvn_proc_address(const char *name) {
    if (!name) return 0;
    const Proc *p = bsearch(&(Proc){name, 0}, procs, (size_t)nprocs, sizeof *procs, by_name);
    if (p) return p->addr;
    // Unknown names get NULL, as on hardware; each distinct one is logged once.
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static char seen[32][96];
    static int nseen;
    plat_mutex_lock(&lock);
    int fresh = 1;
    for (int i = 0; i < nseen; i++)
        if (!strncmp(seen[i], name, sizeof seen[i] - 1)) fresh = 0;
    if (fresh && nseen < 32) {
        strncpy(seen[nseen], name, sizeof seen[0] - 1);
        nseen++;
    } else {
        fresh = 0;
    }
    plat_mutex_unlock(&lock);
    if (fresh) rt_log("nvn: no such proc \"%s\", returning NULL", name);
    return 0;
}

// void *nvnBootstrapLoader(const char *name): the game's first NVN call, the end of --boot (rt_boot_event "nvn")
static void ov_nvn_bootstrap(Cpu *c) {
    const char *name = arg_p(c, 0);
    if (!name) name = "(null)";
    static int32_t first = 1;
    if (plat_xchg32(&first, 0)) rt_boot_event("nvn", c, name);   // exits under --boot
    ret_x(c, nvn_proc_address(name));
}
HOST_OVERRIDE("nvnBootstrapLoader", ov_nvn_bootstrap);

// The procs need their synthetic addresses before hostfn_seal, which rt_boot reaches through ldr_load; registering at
// start-up keeps the order independent of the boot path.
__attribute__((constructor)) static void procs_register_at_start(void) { nvn_procs_register(); }

// PFNNVNGENERICFUNC nvnDeviceGetProcAddress(const NVNdevice *device, const char *name)
void nvnp_nvnDeviceGetProcAddress(Cpu *c) {
    const char *name = arg_p(c, 1);
    ret_x(c, nvn_proc_address(name));
}
