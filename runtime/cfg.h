// Settings: PLUCKOMP_* environment variables. The interface of TotK Recomp runtime/cfg.h (MIT), without its ini file
// and in-game menu: what the copied runtime files call.
#pragma once

// getenv for a setting; NULL when unset or empty. Safe from any thread.
const char *cfg_get(const char *name);
// Logs the PLUCKOMP_* variables the environment sets. Called once at start.
void cfg_init(void);
// The setting's value: the environment's, else the default in cfg.c's table ("" for a key without a row).
const char *cfg_value(const char *name);
// cfg_value parsed: "0" is false, anything else true; an int that does not parse gives 0.
int cfg_bool(const char *name);
int cfg_int(const char *name);
