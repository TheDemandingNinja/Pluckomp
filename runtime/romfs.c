// The read-only RomFS: nn::fs::MountRom and the RomFS half of the nn::fs file calls (fs.h), served from the RomFS
// directory tools/extract.py writes (build/extract/<title>/v<version>/romfs). The interface is TotK Recomp's
// runtime/romfs.c (MIT); TotK reads a packed romfs.bin with an index, Pikmin 4 reads the extracted tree in place.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "romfs_path.h"

#define MAX_HANDLES 4096

static PlatMutex lock;
static PlatOnce init_once;
static char root[PLAT_PATH_MAX];
static char mount_name[32];
static PlatFile *handles[MAX_HANDLES];
static uint64_t handle_size[MAX_HANDLES];

static void init_root(void *arg) {
    (void)arg;
    const char *dir = rt_config()->romfs;
    if (!dir || !plat_dir_exists(dir)) rt_fatal("RomFS: directory %s not found (--romfs DIR)", dir ? dir : "(none)");
    snprintf(root, sizeof root, "%s", dir);
    rt_log("RomFS: %s", root);
}

void fs_init(void) { plat_once(&init_once, init_root, NULL); }

void fs_trace(const char *what, const char *path) {
    if (rt_trace & RT_TRACE_FS) rt_log("fs: %s %s", what, path);
}

int romfs_owns(const char *path) {
    size_t n = strlen(mount_name);
    return n && path && !strncmp(path, mount_name, n) && path[n] == ':';
}

int romfs_handle(uint64_t h) {
    return h >= ROMFS_HANDLE_BASE && h < ROMFS_HANDLE_BASE + MAX_HANDLES && handles[h - ROMFS_HANDLE_BASE];
}

uint32_t romfs_mount(const char *name) {
    if (!name || !*name || strlen(name) >= sizeof mount_name) return FS_R_INVALID_ARGUMENT;
    fs_init();
    plat_mutex_lock(&lock);
    if (*mount_name && strcmp(mount_name, name)) {
        plat_mutex_unlock(&lock);
        rt_fatal("RomFS: mounted as %s already, then as %s", mount_name, name);
    }
    snprintf(mount_name, sizeof mount_name, "%s", name);
    plat_mutex_unlock(&lock);
    rt_log("RomFS: mounted as %s:", name);
    return 0;
}

int romfs_host_path(const char *path, char *out, size_t n) {
    if (!romfs_owns(path)) return 0;
    const char *p = path + strlen(mount_name) + 1;
    while (*p == '/') p++;
    if (*p && !romfs_path_valid_relative(p)) return 0;
    return snprintf(out, n, "%s%s%s", root, *p ? "/" : "", p) < (int)n;
}

uint32_t romfs_open(uint64_t *out, const char *path, int mode) {
    if (!out) return FS_R_INVALID;
    if (mode != FS_OPEN_READ) return FS_R_UNSUPPORTED_OPEN;
    char p[PLAT_PATH_MAX];
    if (!romfs_host_path(path, p, sizeof p) || !plat_file_exists(p)) {
        fs_trace("missing", path);
        return FS_R_PATH_NOT_FOUND;
    }
    PlatFile *f = plat_file_open(p);
    if (!f) return FS_R_PATH_NOT_FOUND;
    int64_t size = plat_file_size(p);
    fs_trace("read", path);
    plat_mutex_lock(&lock);
    for (int i = 0; i < MAX_HANDLES; i++)
        if (!handles[i]) {
            handles[i] = f;
            handle_size[i] = size < 0 ? 0 : (uint64_t)size;
            plat_mutex_unlock(&lock);
            *out = ROMFS_HANDLE_BASE + (uint64_t)i;
            return 0;
        }
    plat_mutex_unlock(&lock);
    rt_fatal("RomFS: file handle table full (%d)", MAX_HANDLES);
}

uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    if (!romfs_handle(h) || offset < 0 || (!buffer && size)) return FS_R_INVALID;
    uint64_t j = h - ROMFS_HANDLE_BASE, total = handle_size[j];
    if ((uint64_t)offset > total) return FS_R_INVALID;
    if (size > total - (uint64_t)offset) size = total - (uint64_t)offset;
    uint64_t done = 0;
    while (done < size) {
        uint64_t chunk = size - done > 0x40000000u ? 0x40000000u : size - done;
        int64_t got = plat_file_pread(handles[j], (uint8_t *)buffer + done, (uint32_t)chunk, (uint64_t)offset + done);
        if (got <= 0) rt_fatal("RomFS: read of handle %#llx at %#llx failed", (unsigned long long)h,
                               (unsigned long long)(offset + done));
        done += (uint64_t)got;
    }
    if (out) *out = size;
    return 0;
}

uint32_t romfs_size(int64_t *out, uint64_t h) {
    if (!out || !romfs_handle(h)) return FS_R_INVALID;
    *out = (int64_t)handle_size[h - ROMFS_HANDLE_BASE];
    return 0;
}

void romfs_close(uint64_t h) {
    if (!romfs_handle(h)) rt_fatal("RomFS: close of invalid file handle %#llx", (unsigned long long)h);
    plat_mutex_lock(&lock);
    PlatFile *f = handles[h - ROMFS_HANDLE_BASE];
    handles[h - ROMFS_HANDLE_BASE] = NULL;
    plat_mutex_unlock(&lock);
    plat_file_close(f);
}

uint32_t romfs_entry_type(int *out, const char *path) {
    if (!out) return FS_R_INVALID;
    char p[PLAT_PATH_MAX];
    if (!romfs_host_path(path, p, sizeof p)) return FS_R_PATH_NOT_FOUND;
    if (plat_file_exists(p)) *out = FS_ENTRY_FILE;
    else if (plat_dir_exists(p)) *out = FS_ENTRY_DIRECTORY;
    else return FS_R_PATH_NOT_FOUND;
    return 0;
}

// ---- overrides
// nn::Result nn::fs::MountRom(const char *name, void *cache, size_t cache_size)
static void ov_mount_rom(Cpu *c) { ret_x(c, romfs_mount(arg_p(c, 0))); }
// nn::Result nn::fs::QueryMountRomCacheSize(size_t *out): the host reads the files directly, but a zero size makes
// some SDK versions pass NULL as the cache, which MountRom accepts here
static void ov_query_cache(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (out) *out = 0x1000;
    ret_x(c, out ? 0 : FS_R_INVALID);
}
// void nn::fs::InitializeForApplication(): SDK-internal; the host needs no fs service session
static void ov_initialize(Cpu *c) {
    (void)c;
    fs_init();
}

HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKcPvm", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", ov_query_cache);
HOST_OVERRIDE("_ZN2nn2fs24InitializeForApplicationEv", ov_initialize);
