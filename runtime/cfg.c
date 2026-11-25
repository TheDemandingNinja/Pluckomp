// Settings from PLUCKOMP_* environment variables (cfg.h). Defaults from TotK Recomp runtime/cfg.c's table (MIT).
#include "cfg.h"

#include <stdlib.h>
#include <string.h>

#include "host.h"

static const struct { const char *name, *def; } defaults[] = {
    {"PLUCKOMP_ASYNC_PREEMPT", "1"},     // kernel.c: interrupt a core holder that runs game code too long
    {"PLUCKOMP_FREE_CORES", "0"},        // kernel.c: 1 lifts the virtual core gate (experiments only)
    {"PLUCKOMP_TIMER_1MS", "1"},         // main.c: 1 ms system timer resolution
    {"PLUCKOMP_OPERATION_MODE", "docked"},   // ov_system.c: nn::oe::GetOperationMode
    {"PLUCKOMP_ASSUME_FOCUS", "1"},      // ov_system.c: report focus while the window has none
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

#ifndef _WIN32
extern char **environ;   // Windows: stdlib.h declares it
#endif

void cfg_init(void) {
    for (char **e = environ; e && *e; e++)
        if (!strncmp(*e, "PLUCKOMP_", 9)) rt_log("cfg: %s", *e);
}
