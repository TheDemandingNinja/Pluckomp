// Adapted from TotK Recomp runtime/audio_mixer.c @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh), itself from
// MacNativeGames games/totk/runtime/audio_mixer.c @ b75be8c (MIT, Copyright (c) 2026 MacNativeGames contributors):
// audio_voice_consumed and the IAudioDevice service. The mixing stage and the SDL3 output are not ported.
// The silent mixer behind audren:u (audren.c): every mixer call returns at once, nothing is decoded or played. Voices
// still consume their wave buffers in real time (audren.c advance_voice with audio_voice_consumed), so the game's
// streaming sees its buffers come back as on hardware. IAudioDevice reports one stereo output.
#include <math.h>
#include <string.h>

#include "audren.h"
#include "host.h"
#include "ipc.h"

void audio_mixer_open(AudioRenderer *r) {
    (void)r;
    rt_log("audio: silent mixer (no output device)");
}
void audio_mixer_set_running(AudioRenderer *r, int running) { (void)r, (void)running; }
void audio_mixer_close(AudioRenderer *r) { (void)r; }
void audio_mixer_update_routing(AudioRenderer *r, const uint8_t *splitters, uint32_t splitter_bytes,
                                const uint8_t *mixes, uint32_t mix_bytes, int mix_count) {
    (void)r, (void)splitters, (void)splitter_bytes, (void)mixes, (void)mix_bytes, (void)mix_count;
}
void audio_mixer_update_effects(AudioRenderer *r) { (void)r; }
// The effect's result block (REV9+): zeros, as an effect that processed silence reports.
void audio_mixer_effect_result(AudioRenderer *r, int id, void *output, unsigned size) {
    (void)r, (void)id;
    memset(output, 0, size);
}
// Circular sinks: nothing written yet.
uint32_t audio_mixer_sink_written(AudioRenderer *r, int id) { (void)r, (void)id; return 0; }
void audio_mixer_render(AudioRenderer *r, double seconds) { (void)r, (void)seconds; }
// No device: the render thread's timer paces the frames.
int64_t audio_mixer_pace_ns(AudioRenderer *r, uint64_t now) { (void)r, (void)now; return -1; }

static double voice_step(const AudioRenderer *r, const ServerVoice *v) {
    return v->in.sample_rate * (v->in.pitch > 0 ? v->in.pitch : 1.0) / r->param.sample_rate;
}
// Source samples the voice consumes over `frames` output samples, accumulated one output sample at a time as a mixer
// would step; leaves the fractional position in v->fraction.
uint64_t audio_voice_consumed(const AudioRenderer *r, ServerVoice *v, int frames) {
    const double step = voice_step(r, v);
    double frac = v->fraction;
    uint64_t consumed = 0;
    for (int k = 0; k < frames; k++) {
        frac += step;
        while (frac >= 1.0) { frac -= 1.0; consumed++; }
    }
    v->fraction = frac;
    return consumed;
}

// ---- IAudioDevice: one stereo output device
static const char device_name[] = "AudioTvOutput";
static float device_volume = 1.0f;

static void put_name(const IpcBuffer *b, IpcResponse *rs, int pointer) {
    if (!b->size) return;
    char *dst = ipc_guest(b->addr, b->size);
    size_t n = b->size < sizeof device_name ? b->size : sizeof device_name;
    memset(dst, 0, b->size);
    memcpy(dst, device_name, n);
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, b->size, b->index};
}

static IpcBuffer out_buffer(const IpcRequest *rq, int *pointer) {
    *pointer = !(rq->nb && rq->b[0].size) && rq->nc && rq->c[0].size;
    return *pointer ? rq->c[0] : rq->nb ? rq->b[0] : (IpcBuffer){0};
}

void audio_device_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    static uint32_t change_event;   // device change events: never signalled, the device does not change
    int pointer;
    switch (rq->cmd) {
    case 0: case 6: case 14: {   // ListAudioDeviceName(Auto), ListAudioOutputDeviceName [13.0.0+]
        IpcBuffer b = out_buffer(rq, &pointer);
        int32_t n = b.size >= 0x100 ? 1 : 0;
        if (n) put_name(&b, rs, pointer);
        IPC_OUT(rs, n);
        return;
    }
    case 1: case 7: {   // SetAudioDeviceOutputVolume(Auto)
        float v = IPC_ARG(rq, float, 0);
        __atomic_store_n(&device_volume, isfinite(v) ? v : 0.0f, __ATOMIC_RELAXED);
        return;
    }
    case 2: case 8: { float v = __atomic_load_n(&device_volume, __ATOMIC_RELAXED); IPC_OUT(rs, v); return; }
    case 3: case 10: case 13: {   // GetActiveAudioDeviceName(Auto), GetActiveAudioOutputDeviceName [13.0.0+]
        IpcBuffer b = out_buffer(rq, &pointer);
        put_name(&b, rs, pointer);
        return;
    }
    case 5: { int32_t n = 2; IPC_OUT(rs, n); return; }   // GetActiveChannelCount
    case 4: case 11: case 12: {   // QueryAudioDeviceSystemEvent, ...InputEvent, ...OutputEvent
        uint32_t ev = __atomic_load_n(&change_event, __ATOMIC_ACQUIRE);
        if (!ev) {
            uint32_t mine = kern_event_new(), expected = 0;
            if (__atomic_compare_exchange_n(&change_event, &expected, mine, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                ev = mine;
            else
                ev = expected;   // the loser's event stays unused (no host-side close yet)
        }
        rs->copy[rs->ncopy++] = kern_event_readable(ev);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
