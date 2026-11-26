// Settings from PLUCKOMP_* environment variables (cfg.h). Defaults from TotK Recomp runtime/cfg.c's metas table @ afae80c (MIT).
#include "cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

static const struct { const char *name, *def; } defaults[] = {
    {"PLUCKOMP_ASYNC_PREEMPT", "1"},     // kernel.c: interrupt a core holder that runs game code too long
    {"PLUCKOMP_FREE_CORES", "0"},        // kernel.c: 1 lifts the virtual core gate (experiments only)
    {"PLUCKOMP_TIMER_1MS", "1"},         // main.c: 1 ms system timer resolution
    {"PLUCKOMP_OPERATION_MODE", "docked"},   // ov_system.c: nn::oe::GetOperationMode
    {"PLUCKOMP_ASSUME_FOCUS", "1"},      // ov_system.c: report focus while the window has none
    // Window and present (gfx_present.c, gfx_window_sdl.c)
    {"PLUCKOMP_FULLSCREEN", "0"}, {"PLUCKOMP_WINDOW_SIZE", "1920x1080"}, {"PLUCKOMP_VK_PRESENT", "mailbox"},
    {"PLUCKOMP_ASPECT", "fit"},
    // Vulkan device and memory (vk_device.c, vk_caps.c, vk_memory.c)
    {"PLUCKOMP_VK_VALIDATION", "0"}, {"PLUCKOMP_VK_SUBGROUP_PIN", "1"}, {"PLUCKOMP_MEMORY", "auto"},
    {"PLUCKOMP_VMA_BLOCK_MIB", "64"}, {"PLUCKOMP_TEX_BLOCK_MIB", "64"}, {"PLUCKOMP_FIX_ZERO", "1"},
    {"PLUCKOMP_WB_EXACT", "1"}, {"PLUCKOMP_WB_MERGE", "1"},
    {"PLUCKOMP_WB_FENCE_LIMIT", "65536"}, {"PLUCKOMP_MEMX", "1"},
    // Textures (vk_texture.c, vk_astc.c)
    {"PLUCKOMP_ASTC", "auto"}, {"PLUCKOMP_TEX_ALIAS", "1"}, {"PLUCKOMP_TEX_MEMO", "1"}, {"PLUCKOMP_ANISO", "game"},
    {"PLUCKOMP_SHADOW_SCALE", "1"},
    // Replay (vk_exec*.c, vk_sync.c, vk_backend.c)
    {"PLUCKOMP_REPLAY_ASYNC", "0"}, {"PLUCKOMP_REPLAY_STATE_CHECK", "0"}, {"PLUCKOMP_REPLAY_CHUNKS", "0"},
    {"PLUCKOMP_REPLAY_CHUNK_DRAWS", "128"}, {"PLUCKOMP_REPLAY_SKEW_US", "0"}, {"PLUCKOMP_REPLAY_WORKERS", "0"},
    {"PLUCKOMP_SPLIT_DRAWS", "64"}, {"PLUCKOMP_SPLIT_US", "1000"}, {"PLUCKOMP_ADDR_REPORT", "0"},
    {"PLUCKOMP_BARRIER_MERGE", "0"}, {"PLUCKOMP_FIX_C0", "1"}, {"PLUCKOMP_PUSH_SKIP", "1"}, {"PLUCKOMP_VB_USED", "1"},
    {"PLUCKOMP_BIND_INC", "0"}, {"PLUCKOMP_BIND_SKIP", "1"}, {"PLUCKOMP_SHADER_MEMO", "1"},
    // Pipelines (vk_pipeline.c)
    {"PLUCKOMP_DESC_TEMPLATE", "1"}, {"PLUCKOMP_PIPE_MEMO", "1"}, {"PLUCKOMP_PIPE_PREBUILD", "1"},
    {"PLUCKOMP_PIPE_SKIP", "fullscreen"}, {"PLUCKOMP_PIPE_WAIT", "default"},
};

const char *cfg_get(const char *name) {
    const char *v = getenv(name);
    return v && *v ? v : NULL;
}

const char *cfg_value(const char *name) {
    const char *v = cfg_get(name);
    if (v) return v;
    for (unsigned i = 0; i < sizeof defaults / sizeof *defaults; i++)
        if (!strcmp(defaults[i].name, name)) return defaults[i].def;
    return "";
}

int cfg_bool(const char *name) { return strcmp(cfg_value(name), "0") != 0 && *cfg_value(name); }
int cfg_int(const char *name) { return atoi(cfg_value(name)); }

int rt_knob(const char *name, const char *def) {
    char key[64];
    snprintf(key, sizeof key, "PLUCKOMP_%s", name);
    const char *e = cfg_get(key);
    if (!e) e = def;
    return strcmp(e, "0") != 0;
}

#ifndef _WIN32
extern char **environ;   // Windows: stdlib.h declares it
#endif

void cfg_init(void) {
    for (char **e = environ; e && *e; e++)
        if (!strncmp(*e, "PLUCKOMP_", 9)) rt_log("cfg: %s", *e);
}
