// System service overrides Pikmin 4 calls beyond those TotK Recomp's ov_system.c answers (docs/coverage.md lists
// which are reached). Same rule as there: nothing talks to a Horizon service; queries and initialisation succeed with
// neutral values, online features report that no network or account service is available, and data paths the game
// should not reach on this host stop with a named fatal.
// nn::vi is adapted from TotK Recomp runtime/ov_gfx.c @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh), itself
// from MacNativeGames games/totk/runtime/frontend.m @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames contributors).
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#define VI_R_INVALID 0x272u               // vi: invalid argument (module 114, description 1)
#define NIFM_R_NOT_AVAILABLE 0x3c86eu     // nifm: no network connection (module 110, description 3171)
#define BCAT_R_NOT_AVAILABLE 0x17a7au     // bcat: delivery cache not available (module 122, description 189)
#define ACCOUNT_R_NSA_UNAVAILABLE 0x3cc7cu   // account: network service account unavailable (module 124, description 486)

static PlatMutex lock = PLAT_MUTEX_INIT;

static const char *caller(const Cpu *c, char *buf, int n) { return ldr_describe(c->x[30] - 4, buf, n); }

// A call the game makes on this host only on a path not served yet: named fatal with the caller.
static void unserved(Cpu *c, const char *what) {
    char at[128];
    rt_fatal("%s: not served on this host (from %s)", what, caller(c, at, sizeof at));
}

static void ov_ok(Cpu *c) { ret_x(c, 0); }
static void ov_void(Cpu *c) { (void)c; }
static void ov_false(Cpu *c) { ret_x(c, 0); }

// ---- nn::vi: the identities are small zeroed blocks in the guest-visible kernel region, in case SDK code reads them
static int vi_ready;
static void *display, *layer, *window;
static uint64_t vsync_event;

// The display's vsync at 60 Hz, until the presentation layer takes it over.
static void vsync_thread(void *arg) {
    (void)arg;
    uint64_t next = kern_now_ns();
    for (;;) {
        next += 16666667;
        uint64_t now = kern_now_ns();
        if (next > now) plat_sleep_ns(next - now);
        else next = now;
        kern_signal_system_event(vsync_event);
    }
}

// void nn::vi::Initialize()
static void ov_vi_initialize(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    if (!display) {
        display = mem_kernel_alloc(64, 16);
        layer = mem_kernel_alloc(64, 16);
        window = mem_kernel_alloc(64, 16);
    }
    vi_ready = 1;
    plat_mutex_unlock(&lock);
}

static int vi_up(void) {
    plat_mutex_lock_shared(&lock);
    int r = vi_ready;
    plat_mutex_unlock_shared(&lock);
    return r;
}

// Result nn::vi::OpenDefaultDisplay(Display **out)
static void ov_open_display(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up()) { ret_x(c, VI_R_INVALID); return; }
    *out = display;
    ret_x(c, 0);
}

// Result nn::vi::CreateLayer(Layer **out, Display *display)
static void ov_create_layer(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    *out = layer;
    ret_x(c, 0);
}

// Result nn::vi::GetNativeWindow(void **out, Layer *layer): a placeholder the NVN layer will recognise
static void ov_native_window(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != layer) { ret_x(c, VI_R_INVALID); return; }
    *out = window;
    ret_x(c, 0);
}

// Result nn::vi::GetDisplayVsyncEvent(os::SystemEventType *event, Display *display): auto-clear system event
static void ov_vsync_event(Cpu *c) {
    uint64_t ev = arg_x(c, 0);
    if (!ev || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    kern_bind_system_event(c, ev, 1);
    plat_mutex_lock(&lock);
    int start = !vsync_event;
    vsync_event = ev;
    plat_mutex_unlock(&lock);
    if (start && !plat_thread_new(vsync_thread, NULL, "vsync")) rt_fatal("vi: cannot start the vsync thread");
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn2vi10InitializeEv", ov_vi_initialize);
HOST_OVERRIDE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", ov_open_display);
HOST_OVERRIDE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", ov_create_layer);
HOST_OVERRIDE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", ov_native_window);
HOST_OVERRIDE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", ov_vsync_event);

// ---- nn::aoc: no add-on content (ov_system.c ListAddOnContent agrees)
// void GetAddOnContentListChangedEvent(os::SystemEvent *event): bound, never signalled
static void ov_aoc_changed_event(Cpu *c) { kern_bind_system_event(c, arg_x(c, 0), 0); }

HOST_OVERRIDE("_ZN2nn3aoc17CountAddOnContentEv", ov_false);
HOST_OVERRIDE("_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE", ov_aoc_changed_event);

// ---- nn::oe beyond TotK's set
// void GetDisplayVersion(DisplayVersion *out): char[16], the version the update's control data names
static void ov_display_version(Cpu *c) {
    char *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::oe::GetDisplayVersion: null output");
    memset(out, 0, 16);
    snprintf(out, 16, "2.0.0");
}

// void GetPseudoDeviceId(util::Uuid *out): a fixed id; on hardware it is per device and title
static void ov_pseudo_device_id(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::oe::GetPseudoDeviceId: null output");
    static const uint8_t id[16] = {0x50, 0x4c, 0x55, 0x43, 0x4b, 0x4f, 0x4d, 0x50,
                                   0x40, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(out, id, 16);
}

// uint32_t GetPerformanceConfiguration(PerformanceMode mode): the configuration ids the system applies by default
static void ov_performance_configuration(Cpu *c) {
    int mode = (int)arg_x(c, 0);
    ret_x(c, mode == 1 ? 0x00020003u : 0x00020004u);   // docked: CPU 1020 MHz, GPU 768 MHz; handheld: GPU 384 MHz
}

static void ov_execute_program(Cpu *c) { unserved(c, "nn::oe::ExecuteProgram"); }

HOST_OVERRIDE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", ov_display_version);
HOST_OVERRIDE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", ov_pseudo_device_id);
HOST_OVERRIDE("_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE", ov_performance_configuration);
HOST_OVERRIDE("_ZN2nn2oe16EnableScreenShotEv", ov_void);
HOST_OVERRIDE("_ZN2nn2oe17DisableScreenShotEv", ov_void);
HOST_OVERRIDE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", ov_void);
HOST_OVERRIDE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", ov_void);
HOST_OVERRIDE("_ZN2nn2oe14ExecuteProgramEiPKvm", ov_execute_program);

// ---- nn::nifm: no network. Requests are accepted and never become available.
// A submitted request stays on hold for NIFM_HOLD_NS before it resolves to "no network", as the real service takes a
// while to evaluate it. Pikmin 4 depends on that: its BCAT pre-sync runnable (main+0x206a6d0 constructs it) stores
// status 1 after FRunnableThread::Create returns, while the new thread's Run (main+0x206a830) submits a request,
// polls IsNetworkRequestOnHold every 10 ms and stores status 6 when it resolves without a network. A request that
// resolved at once let Run finish first; status 1 then overwrote the 6 and the boot sequence waited in its
// WaitPreSyncBCAT state forever, on a black screen after the startup logo.
#define NIFM_HOLD_NS 100000000ull
static uint64_t nifm_submitted;   // kern_now_ns of the pending request, 0 when none is on hold

static void ov_nifm_submit(Cpu *c) {
    (void)c;
    __atomic_store_n(&nifm_submitted, kern_now_ns() | 1, __ATOMIC_RELAXED);
}

static void ov_nifm_on_hold(Cpu *c) {
    uint64_t t = __atomic_load_n(&nifm_submitted, __ATOMIC_RELAXED);
    int hold = t && kern_now_ns() - t < NIFM_HOLD_NS;
    if (t && !hold) __atomic_compare_exchange_n(&nifm_submitted, &t, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    ret_x(c, (uint64_t)hold);
}

// SubmitNetworkRequestAndWait returns once the request has resolved; Cancel ends the hold.
static void ov_nifm_resolved(Cpu *c) {
    (void)c;
    __atomic_store_n(&nifm_submitted, 0, __ATOMIC_RELAXED);
}

static void ov_nifm_handle_error(Cpu *c) { ret_x(c, NIFM_R_NOT_AVAILABLE); }
static void ov_nifm_proxy(Cpu *c) { ret_x(c, NIFM_R_NOT_AVAILABLE); }

HOST_OVERRIDE("_ZN2nn4nifm10InitializeEv", ov_ok);
HOST_OVERRIDE("_ZN2nn4nifm18IsNetworkAvailableEv", ov_false);
HOST_OVERRIDE("_ZN2nn4nifm20SubmitNetworkRequestEv", ov_nifm_submit);
HOST_OVERRIDE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", ov_nifm_resolved);
HOST_OVERRIDE("_ZN2nn4nifm22IsNetworkRequestOnHoldEv", ov_nifm_on_hold);
HOST_OVERRIDE("_ZN2nn4nifm20CancelNetworkRequestEv", ov_nifm_resolved);
HOST_OVERRIDE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", ov_nifm_handle_error);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", ov_nifm_proxy);

// ---- nn::bcat: no delivery cache (it is filled over the network)
static void ov_bcat_unavailable(Cpu *c) { ret_x(c, BCAT_R_NOT_AVAILABLE); }

HOST_OVERRIDE("_ZN2nn4bcat10InitializeEv", ov_void);
HOST_OVERRIDE("_ZN2nn4bcat25MountDeliveryCacheStorageEv", ov_bcat_unavailable);
HOST_OVERRIDE("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", ov_void);
HOST_OVERRIDE("_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", ov_bcat_unavailable);
HOST_OVERRIDE("_ZN2nn4bcat30CancelSyncDeliveryCacheRequestEv", ov_void);

// ---- nn::account network service account: none on this host
static void ov_nsa_available(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, 0);
}
static void ov_nsa_unavailable(Cpu *c) { ret_x(c, ACCOUNT_R_NSA_UNAVAILABLE); }

HOST_OVERRIDE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", ov_nsa_available);
HOST_OVERRIDE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", ov_nsa_unavailable);
HOST_OVERRIDE("_ZN2nn7account29LoadCachedNintendoAccountInfoEPNS0_25CachedNintendoAccountInfoERKNS0_10UserHandleEPvm",
              ov_nsa_unavailable);

// ---- nn::ldn: local wireless is not available
HOST_OVERRIDE("_ZN2nn3ldn8GetStateEv", ov_false);   // State_None

// ---- nn::ro: the game registers no module of its own here (NROs would need lifting first)
static void ov_ro_register(Cpu *c) { ret_x(c, 0); }
static void ov_ro_unload(Cpu *c) { unserved(c, "nn::ro::UnloadModule"); }

HOST_OVERRIDE("_ZN2nn2ro10InitializeEv", ov_ok);
HOST_OVERRIDE("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", ov_ro_register);
HOST_OVERRIDE("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", ov_ro_unload);

// ---- nn::prepo: play reports are dropped (ov_system.c PlayReport::Save does the same)
HOST_OVERRIDE("_ZN2nn5prepo10PlayReport4SendERKNS_7account3UidE", ov_ok);

// ---- nn::err, nn::ec: no error viewer or shop applet on this host
HOST_OVERRIDE("_ZN2nn3err21ReportMultimediaErrorENS_6ResultERKN5movie25MultimediaTelemetryReportE", ov_void);
static void ov_shop(Cpu *c) { unserved(c, "nn::ec::ShowShopApplicationInformation"); }
HOST_OVERRIDE("_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdERKNS_7account10UserHandleE", ov_shop);
HOST_OVERRIDE("_ZN2nn2ec30ShowShopApplicationInformationERKNS_7account10UserHandleE", ov_shop);

// ---- nn::socket: a host without network. Initialize succeeds, no socket can be made, and every call on a descriptor
// fails with EBADF, as no descriptor was handed out. Errno values are Horizon's (BSD).
enum { SOCK_EBADF = 9, SOCK_ENETDOWN = 50, SOCK_EAI_FAIL = 4 };
static _Thread_local int sock_errno;

static void sock_fail(Cpu *c, int e) {
    sock_errno = e;
    ret_x(c, (uint64_t)(int64_t)-1);
}
static void ov_sock_socket(Cpu *c) { sock_fail(c, SOCK_ENETDOWN); }
static void ov_sock_badf(Cpu *c) { sock_fail(c, SOCK_EBADF); }
static void ov_sock_errno(Cpu *c) { ret_x(c, (uint64_t)sock_errno); }
static void ov_sock_addrinfo(Cpu *c) {
    void **out = arg_p(c, 3);
    if (out) *out = NULL;
    ret_x(c, SOCK_EAI_FAIL);
}
static void ov_sock_nameinfo(Cpu *c) { ret_x(c, SOCK_EAI_FAIL); }

HOST_OVERRIDE("_ZN2nn6socket10InitializeEPvmmi", ov_ok);
HOST_OVERRIDE("_ZN2nn6socket6SocketEiii", ov_sock_socket);
HOST_OVERRIDE("_ZN2nn6socket12GetLastErrnoEv", ov_sock_errno);
HOST_OVERRIDE("_ZN2nn6socket12GetLastErrorEv", ov_sock_errno);
HOST_OVERRIDE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", ov_sock_addrinfo);
HOST_OVERRIDE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", ov_void);
HOST_OVERRIDE("_ZN2nn6socket11GetNameInfoEPK8sockaddrjPcjS4_ji", ov_sock_nameinfo);
HOST_OVERRIDE("_ZN2nn6socket6AcceptEiP8sockaddrPj", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket4BindEiPK8sockaddrj", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket5CloseEi", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket7ConnectEiPK8sockaddrj", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket5FcntlEiiz", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket10GetSockOptEiiiPvPj", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket5IoctlEijPvm", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket6ListenEii", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket4PollEP6pollfdmi", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket4RecvEiPvmi", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket4SendEiPKvmi", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket10SetSockOptEiiiPKvj", ov_sock_badf);
HOST_OVERRIDE("_ZN2nn6socket8ShutdownEii", ov_sock_badf);

// ---- nv: the game's graphics allocator callbacks and work memory, recorded for the NVN layer (TotK Recomp
// runtime/ov_gfx.c, MIT, from MacNativeGames gfx_bridge.c). Nothing reaches nvdrv.
typedef struct { uint64_t alloc, free, realloc, user; } NvAllocator;
static struct { NvAllocator graphics, devtools; uint64_t memory, memory_size; } nv;

static void set_allocator(Cpu *c, NvAllocator *a) {
    plat_mutex_lock(&lock);
    *a = (NvAllocator){arg_x(c, 0), arg_x(c, 1), arg_x(c, 2), arg_x(c, 3)};
    plat_mutex_unlock(&lock);
}
// void nv::SetGraphicsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)
static void ov_graphics_allocator(Cpu *c) { set_allocator(c, &nv.graphics); }
// void nv::SetGraphicsDevtoolsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)
static void ov_devtools_allocator(Cpu *c) { set_allocator(c, &nv.devtools); }
// void nv::InitializeGraphics(void *memory, size_t size)
static void ov_initialize_graphics(Cpu *c) {
    uint64_t mem = arg_x(c, 0), size = arg_x(c, 1);
    if (!mem || !size) rt_fatal("nv::InitializeGraphics: no graphics work memory (%#llx, %#llx)", (unsigned long long)mem,
                                (unsigned long long)size);
    plat_mutex_lock(&lock);
    nv.memory = mem;
    nv.memory_size = size;
    plat_mutex_unlock(&lock);
    rt_log("nv: graphics work memory %#llx+%#llx recorded", (unsigned long long)mem, (unsigned long long)size);
}

HOST_OVERRIDE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_graphics_allocator);
HOST_OVERRIDE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_devtools_allocator);
HOST_OVERRIDE("_ZN2nv18InitializeGraphicsEPvm", ov_initialize_graphics);
