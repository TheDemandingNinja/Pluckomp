// Adapted from TotK Recomp runtime/ov.h @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// Shared by the system overrides (ov_system.c, ov_services.c, ov_hid.c). oe_push_message and oe_set_focus are in gfx.h,
// where a window back end will post focus messages.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

// ---- ov_system.c
int ov_handheld(void);                     // PLUCKOMP_OPERATION_MODE=handheld (default docked); fatal on another value
const char *ov_language(void);             // the game's language, a Switch language code ("en-US", "zh-Hans", ...)
const char *ov_shared_font_dir(char *buf, size_t n);   // PLUCKOMP_SHARED_FONTS, else <romfs>/../../firmware/shared-fonts
uint64_t ov_bfttf_size(const uint8_t *p, uint64_t len); // decoded font size of a BFTTF image, 0 if it is not one
