// audout:u: the audio output service (IAudioOutManager, IAudioOut). The SDK's nn::audio::AudioOut client runs
// unchanged; Wwise opens the default output through it (CAkSink::Init). Command ids and the AudioOutBuffer layout follow
// the public switchbrew description of audout. Until audio has an output device the service plays silently: appended
// buffers are given back once their samples would have played at the output rate, and the buffer event is signalled,
// so the game's feeder runs at the pace it would on hardware.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "ipc.h"
#include "platform/platform.h"

#define AUDOUT_RATE 48000
#define AUDOUT_QUEUE 32
#define AUDOUT_R_BUFFER_FULL 0x1099u   // audio (module 153): too many buffers queued
#define AUDOUT_R_INVALID 0x0c99u       // audio: invalid argument
enum { STATE_STARTED, STATE_STOPPED };
enum { FORMAT_PCM16 = 2 };

static const char device_name[] = "DeviceOut";

typedef struct { uint64_t next, buffer, capacity, data_size, data_offset; } AudioOutBuffer;   // guest layout
_Static_assert(sizeof(AudioOutBuffer) == 0x28, "AudioOutBuffer");

typedef struct {
    PlatMutex lock;
    PlatThread *thread;
    volatile int32_t running;
    uint32_t state, channels, event;               // event: writable handle
    float volume;
    uint64_t queued[AUDOUT_QUEUE], queued_samples[AUDOUT_QUEUE];
    unsigned nqueued;
    uint64_t released[AUDOUT_QUEUE];
    unsigned nreleased;
    uint64_t head_end_ns;                          // when the head buffer has played; 0: not started
    uint64_t played;                               // samples per channel
} AudioOut;

// Gives the head buffers back as their play time passes, while started.
static void play_thread(void *p) {
    AudioOut *o = p;
    while (plat_cas32(&o->running, 0, 0)) {
        int signal = 0;
        uint64_t now = kern_now_ns();
        plat_mutex_lock(&o->lock);
        while (o->state == STATE_STARTED && o->nqueued) {
            if (!o->head_end_ns) o->head_end_ns = now + o->queued_samples[0] * 1000000000ull / AUDOUT_RATE;
            if (now < o->head_end_ns) break;
            if (o->nreleased < AUDOUT_QUEUE) o->released[o->nreleased++] = o->queued[0];
            o->played += o->queued_samples[0];
            o->nqueued--;
            memmove(o->queued, o->queued + 1, o->nqueued * sizeof *o->queued);
            memmove(o->queued_samples, o->queued_samples + 1, o->nqueued * sizeof *o->queued_samples);
            // the next buffer follows without a gap, unless the queue ran dry
            o->head_end_ns = o->nqueued ? o->head_end_ns + o->queued_samples[0] * 1000000000ull / AUDOUT_RATE : 0;
            signal = 1;
        }
        plat_mutex_unlock(&o->lock);
        if (signal) kern_event_signal(o->event);
        plat_sleep_ns(1000000);
    }
}

static IpcBuffer in_buffer(const IpcRequest *rq) {
    return rq->na && rq->a[0].size ? rq->a[0] : rq->nx ? rq->x[0] : (IpcBuffer){0};
}
static IpcBuffer out_buffer(const IpcRequest *rq, int *pointer) {
    *pointer = !(rq->nb && rq->b[0].size) && rq->nc && rq->c[0].size;
    return *pointer ? rq->c[0] : rq->nb ? rq->b[0] : (IpcBuffer){0};
}

static void audio_out_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *o = s->state;
    int pointer;
    switch (rq->cmd) {
    case 0: { uint32_t v = o->state; IPC_OUT(rs, v); return; }   // GetAudioOutState
    case 1: case 2:                                               // StartAudioOut, StopAudioOut
        plat_mutex_lock(&o->lock);
        o->state = rq->cmd == 1 ? STATE_STARTED : STATE_STOPPED;
        o->head_end_ns = 0;
        plat_mutex_unlock(&o->lock);
        return;
    case 3: case 7: {   // AppendAudioOutBuffer(Auto)(u64 tag, AudioOutBuffer)
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        IpcBuffer b = in_buffer(rq);
        if (b.size < sizeof(AudioOutBuffer)) { rs->result = AUDOUT_R_INVALID; return; }
        AudioOutBuffer ab;
        memcpy(&ab, ipc_guest(b.addr, sizeof ab), sizeof ab);
        plat_mutex_lock(&o->lock);
        uint32_t r = 0;
        if (o->nqueued == AUDOUT_QUEUE) r = AUDOUT_R_BUFFER_FULL;
        else {
            o->queued[o->nqueued] = tag;
            o->queued_samples[o->nqueued++] = ab.data_size / (2ull * o->channels);
        }
        plat_mutex_unlock(&o->lock);
        rs->result = r;
        return;
    }
    case 4: rs->copy[rs->ncopy++] = kern_event_readable(o->event); return;   // RegisterBufferEvent
    case 5: case 8: {   // GetReleasedAudioOutBuffers(Auto) -> u32 count; tags into the output buffer
        IpcBuffer b = out_buffer(rq, &pointer);
        uint64_t *tags = b.size ? ipc_guest(b.addr, b.size) : NULL;
        unsigned room = (unsigned)(b.size / 8);
        plat_mutex_lock(&o->lock);
        uint32_t n = o->nreleased < room ? o->nreleased : room;
        if (tags) memset(tags, 0, room * 8ull);
        for (uint32_t i = 0; i < n; i++) tags[i] = o->released[i];
        o->nreleased -= n;
        memmove(o->released, o->released + n, o->nreleased * sizeof *o->released);
        plat_mutex_unlock(&o->lock);
        if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b.addr, b.size, b.index};
        IPC_OUT(rs, n);
        return;
    }
    case 6: {   // ContainsAudioOutBuffer(u64 tag) -> bool
        uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        uint8_t v = 0;
        plat_mutex_lock(&o->lock);
        for (unsigned i = 0; i < o->nqueued; i++) v |= o->queued[i] == tag;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, v);
        return;
    }
    case 9: {   // GetAudioOutBufferCount -> u32
        plat_mutex_lock(&o->lock);
        uint32_t v = o->nqueued;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, v);
        return;
    }
    case 10: {   // GetAudioOutPlayedSampleCount -> u64
        plat_mutex_lock(&o->lock);
        uint64_t v = o->played;
        plat_mutex_unlock(&o->lock);
        IPC_OUT(rs, v);
        return;
    }
    case 11: {   // FlushAudioOutBuffers -> bool: the queued buffers count as played
        plat_mutex_lock(&o->lock);
        uint8_t v = o->nqueued != 0;
        for (unsigned i = 0; i < o->nqueued && o->nreleased < AUDOUT_QUEUE; i++) o->released[o->nreleased++] = o->queued[i];
        o->nqueued = 0;
        o->head_end_ns = 0;
        plat_mutex_unlock(&o->lock);
        if (v) kern_event_signal(o->event);
        IPC_OUT(rs, v);
        return;
    }
    case 12: o->volume = IPC_ARG(rq, float, 0); return;      // SetAudioOutVolume
    case 13: { float v = o->volume; IPC_OUT(rs, v); return; }   // GetAudioOutVolume
    default: ipc_unimplemented(s, rq);
    }
}

static void audio_out_close(void *p) {
    AudioOut *o = p;
    plat_xchg32(&o->running, 0);
    int away = kern_core_away();
    plat_thread_join(o->thread);
    kern_core_back(&away);
    kern_handle_close(o->event);
    rt_log("audout: closed after %llu samples", (unsigned long long)o->played);
    free(o);
}

static void put_name(const IpcBuffer *b, IpcResponse *rs, int pointer) {
    if (!b->size) return;
    char *dst = ipc_guest(b->addr, b->size);
    size_t n = b->size < sizeof device_name ? b->size : sizeof device_name;
    memset(dst, 0, b->size);
    memcpy(dst, device_name, n);
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, b->size, b->index};
}

void audout_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    int pointer;
    switch (rq->cmd) {
    case 0: case 2: {   // ListAudioOuts(Auto) -> u32 count; names into the output buffer
        IpcBuffer b = out_buffer(rq, &pointer);
        uint32_t n = b.size ? 1 : 0;
        put_name(&b, rs, pointer);
        IPC_OUT(rs, n);
        return;
    }
    case 1: case 3: {   // OpenAudioOut(Auto)(AudioOutParameter {u32 rate, u16 channels}, u64 aruid; name in; name out)
        uint32_t rate = IPC_ARG(rq, uint32_t, 0);
        uint16_t channels = IPC_ARG(rq, uint16_t, 4);
        if (rate && rate != AUDOUT_RATE) {
            rt_log("audout: OpenAudioOut at %u Hz refused (the output runs at %d Hz)", rate, AUDOUT_RATE);
            rs->result = AUDOUT_R_INVALID;
            return;
        }
        AudioOut *o = calloc(1, sizeof *o);
        if (!o) rt_fatal("audout: allocation");
        plat_mutex_init(&o->lock);
        o->state = STATE_STOPPED;
        o->channels = channels == 6 ? 6 : 2;
        o->volume = 1.0f;
        o->event = kern_event_new();
        o->running = 1;
        o->thread = plat_thread_new(play_thread, o, "audout");
        if (!o->thread) rt_fatal("audout: cannot start the play thread");
        IpcBuffer b = out_buffer(rq, &pointer);
        put_name(&b, rs, pointer);
        uint32_t out[4] = {AUDOUT_RATE, o->channels, FORMAT_PCM16, STATE_STOPPED};
        IPC_OUT(rs, out);
        rt_log("audout: %s open %d Hz x%u (silent)", device_name, AUDOUT_RATE, o->channels);
        rs->move[rs->nmove++] = ipc_session_new("IAudioOut", audio_out_handler, o, audio_out_close);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
