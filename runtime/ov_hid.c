// nn::hid without input devices: until input arrives (Milestone 5), every call that would reach the hid service
// answers as a Switch with no controller, keyboard, mouse or touch input. State layouts as TotK Recomp
// runtime/ov_input.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh) checks them. The data imports (NpadId::No1 ...,
// VibrationNode::DefaultVibrationSampleInterval) and the SDK's pure library classes (VibrationNode, VibrationMixer,
// VibrationPlayer, ControllerSupportArg) stay lifted.
#include <string.h>

#include "host.h"

#define NPAD_STATE_BYTES 40          // NpadFullKeyState, NpadHandheldState, NpadJoyDualState / Left / Right
#define MOUSE_STATE_BYTES 40
#define KEYBOARD_STATE_BYTES 48
#define TOUCH_STATE_16_BYTES (16 + 16 * 0x28)   // TouchScreenState<16>: sampling, count, 16 touches
#define VIBRATION_INFO_BYTES 8

static volatile int32_t hold_type;   // NpadJoyHoldType: 0 vertical, 1 horizontal

static void zero_out(Cpu *c, int arg, size_t n, const char *what) {
    void *p = arg_p(c, arg);
    if (!p) rt_fatal("nn::hid::%s: null output", what);
    memset(p, 0, n);
}

static void ov_void(Cpu *c) { (void)c; }
static void ov_zero(Cpu *c) { ret_x(c, 0); }

static void ov_set_hold_type(Cpu *c) { __atomic_store_n(&hold_type, (int32_t)arg_x(c, 0), __ATOMIC_RELAXED); }
static void ov_get_hold_type(Cpu *c) { ret_x(c, (uint64_t)__atomic_load_n(&hold_type, __ATOMIC_RELAXED)); }

static void ov_npad_state(Cpu *c) { zero_out(c, 0, NPAD_STATE_BYTES, "GetNpadState"); }
static void ov_keyboard_state(Cpu *c) { zero_out(c, 0, KEYBOARD_STATE_BYTES, "GetKeyboardState"); }
static void ov_mouse_state(Cpu *c) { zero_out(c, 0, MOUSE_STATE_BYTES, "GetMouseState"); }
static void ov_touch_state(Cpu *c) { zero_out(c, 0, TOUCH_STATE_16_BYTES, "GetTouchScreenState"); }
static void ov_vibration_info(Cpu *c) { zero_out(c, 0, VIBRATION_INFO_BYTES, "GetVibrationDeviceInfo"); }

static void ov_controller_support(Cpu *c) {
    char at[128];
    rt_fatal("nn::hid::ShowControllerSupport: the controller applet is not served (from %s)",
             ldr_describe(c->x[30] - 4, at, sizeof at));
}

HOST_OVERRIDE("_ZN2nn3hid14InitializeNpadEv", ov_void);
HOST_OVERRIDE("_ZN2nn3hid18InitializeKeyboardEv", ov_void);
HOST_OVERRIDE("_ZN2nn3hid15InitializeMouseEv", ov_void);
HOST_OVERRIDE("_ZN2nn3hid21InitializeTouchScreenEv", ov_void);
HOST_OVERRIDE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", ov_void);
HOST_OVERRIDE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", ov_void);
HOST_OVERRIDE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", ov_set_hold_type);
HOST_OVERRIDE("_ZN2nn3hid18GetNpadJoyHoldTypeEv", ov_get_hold_type);
HOST_OVERRIDE("_ZN2nn3hid15GetNpadStyleSetERKj", ov_zero);          // no style: nothing connected
HOST_OVERRIDE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", ov_zero);     // dual
HOST_OVERRIDE("_ZN2nn3hid14DisconnectNpadERKj", ov_void);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", ov_npad_state);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", ov_npad_state);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", ov_npad_state);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", ov_npad_state);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", ov_npad_state);
HOST_OVERRIDE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", ov_keyboard_state);
HOST_OVERRIDE("_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE", ov_mouse_state);
HOST_OVERRIDE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", ov_zero);   // no states
HOST_OVERRIDE("_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE", ov_touch_state);
HOST_OVERRIDE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
              ov_controller_support);
// Six-axis sensors and vibration devices: no handle is handed out, so the state and value calls see none
HOST_OVERRIDE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
              ov_zero);
HOST_OVERRIDE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", ov_zero);
HOST_OVERRIDE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", ov_void);
HOST_OVERRIDE("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", ov_void);
HOST_OVERRIDE("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", ov_void);
HOST_OVERRIDE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
              ov_zero);
HOST_OVERRIDE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", ov_void);
HOST_OVERRIDE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", ov_void);
HOST_OVERRIDE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE",
              ov_vibration_info);
