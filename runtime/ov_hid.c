// nn::hid with one connected controller and no input yet: player 1's Pro Controller (handheld with
// PLUCKOMP_OPERATION_MODE=handheld), every button up and the sticks centred, and no keyboard, mouse or touch input.
// The game waits in the controller applet until a controller is connected, so it needs one to reach its title screen;
// Milestone 5 feeds real input into the same states. The connected-controller logic (style set, supported ids and
// styles, the applet's answer) is TotK Recomp runtime/ov_input.c @ afae80c (MIT, Copyright (c) 2026 Valiantsin
// Dzerakh) without its input polling; the state layouts as it checks them. The data imports (NpadId::No1 ...,
// VibrationNode::DefaultVibrationSampleInterval) and the SDK's pure library classes (VibrationNode, VibrationMixer,
// VibrationPlayer, ControllerSupportArg) stay lifted.
#include <string.h>

#include "host.h"
#include "ov.h"
#include "platform/platform.h"

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

// ---- the connected controller
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } NpadState;
typedef struct { int8_t players; uint8_t pad[3]; uint32_t selected; } ControllerSupportResult;
_Static_assert(sizeof(NpadState) == NPAD_STATE_BYTES, "npad state ABI");
_Static_assert(sizeof(ControllerSupportResult) == 8, "controller support result ABI");
enum { STYLE_FULL_KEY = 1, STYLE_HANDHELD = 2, ATTR_CONNECTED = 1 };

static PlatMutex lock = PLAT_MUTEX_INIT;
static uint32_t styles = STYLE_FULL_KEY;   // SetSupportedNpadStyleSet
static int enabled, allowed = 1;           // InitializeNpad; the game's supported ids name ours and it is connected
static int64_t sample;

static uint32_t pad_id(void) { return ov_handheld() ? 0x20u : 0u; }   // NpadId::Handheld or NpadId::No1
static uint32_t pad_style(void) { return ov_handheld() ? STYLE_HANDHELD : STYLE_FULL_KEY; }

static const uint32_t *need_id(Cpu *c, int i, const char *fn) {
    const uint32_t *id = arg_p(c, i);
    if (!id) rt_fatal("nn::hid::%s: null controller id", fn);
    return id;
}

// void InitializeNpad()
static void ov_initialize_npad(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    enabled = 1;
    plat_mutex_unlock(&lock);
    rt_log("hid: controllers initialized (one connected %s controller, no input yet)",
           ov_handheld() ? "handheld" : "full-key");
}

// void SetSupportedNpadIdType(const NpadIdType *ids, size_t count)
static void ov_supported_ids(Cpu *c) {
    const uint32_t *ids = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    if (n > 10 || (!ids && n)) rt_fatal("nn::hid::SetSupportedNpadIdType: invalid id list (%llu ids)", (unsigned long long)n);
    int ok = 0;
    for (uint64_t i = 0; i < n; i++) ok |= ids[i] == pad_id();
    plat_mutex_lock(&lock);
    allowed = ok;
    plat_mutex_unlock(&lock);
}

// void SetSupportedNpadStyleSet(NpadStyleSet styles)
static void ov_supported_styles(Cpu *c) {
    plat_mutex_lock(&lock);
    styles = (uint32_t)arg_x(c, 0);
    plat_mutex_unlock(&lock);
}

// NpadStyleSet GetNpadStyleSet(const NpadIdType &id)
static void ov_style_set(Cpu *c) {
    uint32_t id = *need_id(c, 0, "GetNpadStyleSet");
    plat_mutex_lock(&lock);
    uint32_t v = enabled && allowed && id == pad_id() ? styles & pad_style() : 0;
    plat_mutex_unlock(&lock);
    ret_x(c, v);
}

// void GetNpadState(<State> *out, const NpadIdType &id): the newest state; connected, nothing pressed, for the
// controller's own style. want: the style of the overload, 0 for the Joy-Con overloads (never connected).
static void npad_state(Cpu *c, uint32_t want) {
    NpadState *out = arg_p(c, 0);
    uint32_t id = *need_id(c, 1, "GetNpadState");
    if (!out) rt_fatal("nn::hid::GetNpadState: null output");
    plat_mutex_lock(&lock);
    NpadState s = {.sampling = ++sample};
    int live = want && enabled && allowed && id == pad_id() && want == pad_style() && (styles & want);
    plat_mutex_unlock(&lock);
    if (live) s.attributes = ATTR_CONNECTED;
    *out = s;
}
static void ov_state_full_key(Cpu *c) { npad_state(c, STYLE_FULL_KEY); }
static void ov_state_handheld(Cpu *c) { npad_state(c, STYLE_HANDHELD); }
static void ov_state_joycon(Cpu *c) { npad_state(c, 0); }

// void DisconnectNpad(const NpadIdType &id): only the controller in use
static void ov_disconnect(Cpu *c) {
    uint32_t id = *need_id(c, 0, "DisconnectNpad");
    if (id != pad_id()) return;
    plat_mutex_lock(&lock);
    allowed = 0;
    plat_mutex_unlock(&lock);
}
static void ov_keyboard_state(Cpu *c) { zero_out(c, 0, KEYBOARD_STATE_BYTES, "GetKeyboardState"); }
static void ov_mouse_state(Cpu *c) { zero_out(c, 0, MOUSE_STATE_BYTES, "GetMouseState"); }
static void ov_touch_state(Cpu *c) { zero_out(c, 0, TOUCH_STATE_16_BYTES, "GetTouchScreenState"); }
static void ov_vibration_info(Cpu *c) { zero_out(c, 0, VIBRATION_INFO_BYTES, "GetVibrationDeviceInfo"); }

// Result ShowControllerSupport(ControllerSupportResultInfo *out, const ControllerSupportArg &arg): the one controller
// is already connected, so the applet returns at once with one player on it. The first calls and every 100th are
// logged, so a game that asks again and again shows in the log.
static void ov_controller_support(Cpu *c) {
    static uint32_t calls;
    uint32_t n = __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (n <= 3 || n % 100 == 0) {
        char at[128];
        rt_log("hid: ShowControllerSupport call %u from %s: one player", n, ldr_describe(c->x[30] - 4, at, sizeof at));
    }
    ControllerSupportResult *out = arg_p(c, 0);
    if (out) *out = (ControllerSupportResult){.players = 1, .selected = pad_id()};
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn3hid14InitializeNpadEv", ov_initialize_npad);
HOST_OVERRIDE("_ZN2nn3hid18InitializeKeyboardEv", ov_void);
HOST_OVERRIDE("_ZN2nn3hid15InitializeMouseEv", ov_void);
HOST_OVERRIDE("_ZN2nn3hid21InitializeTouchScreenEv", ov_void);
HOST_OVERRIDE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", ov_supported_ids);
HOST_OVERRIDE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", ov_supported_styles);
HOST_OVERRIDE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", ov_set_hold_type);
HOST_OVERRIDE("_ZN2nn3hid18GetNpadJoyHoldTypeEv", ov_get_hold_type);
HOST_OVERRIDE("_ZN2nn3hid15GetNpadStyleSetERKj", ov_style_set);
HOST_OVERRIDE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", ov_zero);     // dual
HOST_OVERRIDE("_ZN2nn3hid14DisconnectNpadERKj", ov_disconnect);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", ov_state_full_key);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", ov_state_handheld);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", ov_state_joycon);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", ov_state_joycon);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", ov_state_joycon);
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
