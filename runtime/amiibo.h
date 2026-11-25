// The amiibo reader of TotK Recomp runtime/amiibo.h (MIT), absent here: nn::nfp reports no NFC device until
// Milestone 5 adds one. Only what ov_system.c calls.
#pragma once
#include <stdbool.h>

static inline bool amiibo_is_device_present(void) { return false; }
static inline void amiibo_start_detection(void) {}
static inline void amiibo_stop_detection(void) {}
static inline bool amiibo_mount(void) { return false; }
static inline void amiibo_unmount(void) {}
static inline bool amiibo_is_mounted(void) { return false; }
static inline bool amiibo_has_tag(void) { return false; }
static inline int amiibo_get_device_state(void) { return 0; }
static inline void amiibo_get_tag_info(void *out) { (void)out; }
static inline void amiibo_get_model_info(void *out) { (void)out; }
