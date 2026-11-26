// Adapted from TotK Recomp runtime/gfx_wsys.h @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// Between gfx_present.c (swapchain, present, vsync) and the window system's back end (gfx_window_sdl.c). Host threads only.
#pragma once
#include "vk.h"

// ---- what the back end provides
int wsys_open(void);                       // makes the window (and its thread); 0 on failure. Once.
VkSurfaceKHR wsys_surface(VkInstance inst);   // a new Vulkan surface of the window; VK_NULL_HANDLE on failure
void wsys_client_size(int *w, int *h);     // the drawable area in pixels
int wsys_minimized(void);                  // nothing to present into
void wsys_resize(int w, int h);            // gfx_window_resize: the drawable area becomes w x h

// ---- what gfx_present.c provides the back end
void gfx_window_mark_dirty(void);          // the window changed size: the next present rebuilds the swapchain
void gfx_window_focus_changed(int in);     // the window gained (1) or lost (0) the focus
void gfx_window_closed(void);              // the player closed the window: saves, then ends the process
void gfx_window_set_refresh(double hz);    // the refresh rate of the window's monitor, 0 unknown
int gfx_cfg_fullscreen(void);              // PLUCKOMP_FULLSCREEN
void gfx_cfg_window_size(int *w, int *h);  // PLUCKOMP_WINDOW_SIZE, the default when unparsable or out of range
void gfx_window_request_close(void);       // gfx_window_sdl.c: the window thread closes the window (gfx_window_closed)
unsigned gfx_window_dpi(void);             // gfx_window_sdl.c: the display scale as a DPI, 96 at 100%
