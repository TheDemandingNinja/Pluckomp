// hwopus: the hardware Opus decoder service (IHardwareOpusDecoderManager, IHardwareOpusDecoder). The SDK's nn::codec
// client runs unchanged; Wwise (CAkOpusDecodingMgr) reaches it through nn::codec::HardwareOpusDecoder. Command ids and
// the packet framing follow the public switchbrew description of hwopus. Until audio has an output, a decode returns
// silence of the packet's own length: the frame count and frame size come from the Opus TOC byte (RFC 6716, 3.1), so
// the game's streams advance as they would with real samples.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "ipc.h"
#include "platform/platform.h"

#define HWOPUS_WORK_BUFFER_BYTES 0x10000u   // the host keeps no state in it
#define HWOPUS_R_INVALID_ARGUMENT 0x60fu    // codec (module 111): invalid argument
#define HWOPUS_PACKET_HEADER 8              // u32 big-endian payload size, u32 big-endian final range

typedef struct { int32_t sample_rate, channels; } OpusParam;   // HardwareOpusDecoderParameterInternal

static int param_ok(const OpusParam *p) {
    int r = p->sample_rate;
    return (r == 8000 || r == 12000 || r == 16000 || r == 24000 || r == 48000) && (p->channels == 1 || p->channels == 2);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Samples per channel at 48 kHz of one Opus packet (RFC 6716 3.1: TOC config gives the frame duration, the code the
// frame count); 0 for a packet too short to say.
static uint32_t packet_samples_48k(const uint8_t *p, uint32_t n) {
    if (!n) return 0;
    unsigned config = p[0] >> 3, code = p[0] & 3, frames;
    static const uint16_t silk[4] = {480, 960, 1920, 2880}, hybrid[2] = {480, 960}, celt[4] = {120, 240, 480, 960};
    uint32_t frame = config < 12 ? silk[config & 3] : config < 16 ? hybrid[config & 1] : celt[config & 3];
    if (code == 0) frames = 1;
    else if (code < 3) frames = 2;
    else if (n >= 2) frames = p[1] & 0x3f;
    else return 0;
    return frame * frames;
}

typedef struct { OpusParam param; uint64_t decodes; } Decoder;

// Decode one framed packet from in into silence in out. Returns the bytes consumed and the samples per channel.
static uint32_t decode(Decoder *d, const IpcRequest *rq, uint32_t *samples) {
    IpcBuffer in = rq->na ? rq->a[0] : (IpcBuffer){0}, out = rq->nb ? rq->b[0] : (IpcBuffer){0};
    *samples = 0;
    if (in.size < HWOPUS_PACKET_HEADER) return 0;
    const uint8_t *p = ipc_guest(in.addr, in.size);
    uint32_t size = be32(p);
    if (size > in.size - HWOPUS_PACKET_HEADER) return 0;
    uint64_t n = (uint64_t)packet_samples_48k(p + HWOPUS_PACKET_HEADER, size) * (uint64_t)d->param.sample_rate / 48000;
    uint64_t bytes = n * (uint64_t)d->param.channels * 2;
    if (bytes > out.size) {
        rt_log("hwopus: a packet of %llu samples does not fit the %llu-byte output", (unsigned long long)n,
               (unsigned long long)out.size);
        return 0;
    }
    if (bytes) memset(ipc_guest(out.addr, out.size), 0, bytes);
    if (!d->decodes++)
        rt_log("hwopus: first decode, %d Hz x%d, %llu samples (silence)", d->param.sample_rate, d->param.channels,
               (unsigned long long)n);
    *samples = (uint32_t)n;
    return HWOPUS_PACKET_HEADER + size;
}

static void decoder_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    Decoder *d = s->state;
    switch (rq->cmd) {
    case 0: case 4: case 6: {   // DecodeInterleavedOld, DecodeInterleavedWithPerfOld, DecodeInterleaved(reset)
        uint32_t samples, consumed = decode(d, rq, &samples);
        if (!consumed) { rs->result = HWOPUS_R_INVALID_ARGUMENT; return; }
        IPC_OUT(rs, consumed);
        IPC_OUT(rs, samples);
        if (rq->cmd != 0) {
            uint64_t perf = 0;   // processing time in microseconds
            IPC_OUT(rs, perf);
        }
        return;
    }
    case 1: return;   // SetContext: the silent decoder keeps no context
    default: ipc_unimplemented(s, rq);
    }
}

static void decoder_close(void *p) { free(p); }

void hwopus_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {   // OpenHardwareOpusDecoder(param, u32 work size; copy tmem) -> IHardwareOpusDecoder
        OpusParam p = IPC_ARG(rq, OpusParam, 0);
        uint32_t work = IPC_ARG(rq, uint32_t, 8);
        IpcTransferMemory *tm = rq->ncopy ? kern_handle_get(rq->copy[0], KERN_TMEM) : NULL;
        if (!param_ok(&p) || !tm || tm->size < work) {
            rt_log("hwopus: OpenHardwareOpusDecoder refused (%d Hz x%d, work %#x)", p.sample_rate, p.channels, work);
            rs->result = HWOPUS_R_INVALID_ARGUMENT;
            return;
        }
        Decoder *d = calloc(1, sizeof *d);
        if (!d) rt_fatal("hwopus: decoder allocation");
        d->param = p;
        rs->move[rs->nmove++] = ipc_session_new("IHardwareOpusDecoder", decoder_handler, d, decoder_close);
        return;
    }
    case 1: {   // GetWorkBufferSize(param) -> u32
        OpusParam p = IPC_ARG(rq, OpusParam, 0);
        if (!param_ok(&p)) { rs->result = HWOPUS_R_INVALID_ARGUMENT; return; }
        uint32_t v = HWOPUS_WORK_BUFFER_BYTES;
        IPC_OUT(rs, v);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
