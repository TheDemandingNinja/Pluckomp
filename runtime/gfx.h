// Interface of the graphics layer. For now only what the system overrides share with it (TotK Recomp runtime/gfx.h,
// MIT, "moved from ov.h"); the NVN front end and its back ends come with the null renderer.
#pragma once
#include <stdint.h>

#include "host.h"

enum { OE_MSG_FOCUS_STATE_CHANGED = 15, OE_MSG_OPERATION_MODE_CHANGED = 30, OE_MSG_PERFORMANCE_MODE_CHANGED = 31 };
void oe_push_message(uint32_t message);                   // ov_system.c
void oe_set_focus(int in_focus);                          // ov_system.c: GetCurrentFocusState; PLUCKOMP_ASSUME_FOCUS pins 1
