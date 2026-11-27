// Adapted from TotK Recomp runtime/ov_movie.c @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh), itself adapted
// from MacNativeGames games/totk/runtime/movie_host.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames contributors).
// movie::Decoder for VP9 on the host with libvpx (BSD-3-Clause, linked statically).
// Pikmin 4's movies (MOV_*.webm in the pak: the title background, cut-scenes, guides) play through UE4's WebMMedia
// plugin, whose demuxer is guest code, into the SDK movie library in subsdk0: Android's MediaCodec over NVIDIA NvMM. Its
// decoder opens NVDEC through NvRm, whose first NvOsDrvOpen aborts ("Use of nv::InitializeGraphics is required."):
// nvdrv is not served and nv::InitializeGraphics is answered on the host (ov_services.c). So the video decoder is
// replaced; the demuxer and the audio decoder stay guest code.
// main imports the constructor and calls the decoder through the vtable for movie::Decoder, whose slots are ABS64
// relocations by name: the HOST_OVERRIDE entries below replace them. A VP9 decoder (type 3) is a host object, +0 the
// library's vtable, +8 the host Movie (where the library keeps its DecoderImpl); any other decoder runs the original code.
// Behaviour follows subsdk0's DecoderImpl:
// - DecoderImpl::IsOutputFormatSupported: mode 1 (buffer) takes output format 4 (packed NV12: Y plane, then
//   interleaved UV; stride = width) or 2 (ARGB) for video; mode 2 (native texture) takes format 3 only. Pikmin 4 asks
//   for mode 2, format 3. Buffer mode with NV12 is kept from TotK Recomp; ARGB is refused as unsupported.
// - Mode 2: GetOutputBuffer(index, Buffer) gets the client's NVNtexture * as the Buffer's base (+0x28), its NVNdevice *
//   at +0x30 and an NVNsync * at +0x38 (Buffer::SetNvnDev, SetNvnSync). DecoderImpl::CopyOutputBufferToClientTextureAnd-
//   Release releases the codec's buffer (renderOutputBufferAndRelease) and NnCodecConsumer::acquireBuffer blits the
//   picture into the texture on the GPU with SfNvnUtil::Blit. Here the worker converts each frame to RGBA (BT.709 limited
//   range), GetOutputBuffer writes it into the texture with the back end's WriteTexels and fences the sync at the newest
//   submit, as a QueueFenceSync after the blit would, and the output index is released by that call.
// - Input and output buffers are indices handed out FIFO, as MediaCodec does; input flag 4 / output flag 4 = end of
//   stream. Client events {input available, output available, format changed, error} (DecoderEvents, 4 EventType
//   pointers) are signalled with nn::os::SignalEvent on a guest thread.
// Decoding runs on one host worker thread per decoder (libvpx adds its own tile threads), never on the game's threads:
// SendInputBufferForDecode queues the input and returns. Events the worker owes (input free, format changed, error) are
// signalled at the next entry call or present, whichever comes first.
// Pacing: decoded frames are handed over when their presentation time is due, counted from the first frame (movie_tick,
// at every present on the presenting guest thread), at most one per present. When the game falls behind, a frame more
// than 1.5 periods late is dropped if the next one is due too. PLUCKOMP_MOVIE_PACE=0 hands every decoded frame over at
// the next present.
// Status values as TotK Recomp names them.
#include <emmintrin.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_obj.h"
#include "platform/platform.h"

enum { IN_BUFS = 4, IN_CAP = 2 << 20, OUT_BUFS = 8, MAX_MOVIES = 16, VPX_THREADS = 4 };
enum { ST_OK = 0, ST_ERROR = 0x1000, ST_NULL = 0x100b, ST_NO_OUTPUT = 0x100e, ST_NO_INPUT = 0x1011,
       ST_UNSUPPORTED = 0x2001, ST_NOT_CONFIGURED = 0x2002 };

// Entries in the order of movie_symbols[]: the imported constructor, then the slots of the vtable for movie::Decoder.
enum { MOVIE_CTOR, MOVIE_D2, MOVIE_D0, MOVIE_CONFIGURE, MOVIE_REGISTER_EVENTS, MOVIE_GET_OUTPUT_FORMAT, MOVIE_START,
       MOVIE_STOP, MOVIE_RESET, MOVIE_FLUSH, MOVIE_ACQUIRE_INPUT, MOVIE_GET_INPUT, MOVIE_SEND_INPUT, MOVIE_ACQUIRE_OUTPUT,
       MOVIE_GET_OUTPUT, MOVIE_GET_OUTPUT_SIZE, MOVIE_RELEASE_OUTPUT, MOVIE_GET_LAST_ERROR, MOVIE_GET_OUTPUT_MD,
       MOVIE_GET_OUTPUT_BUFFER_FORMAT, MOVIE_ENTRIES };
static const char *const movie_symbols[MOVIE_ENTRIES] = {
    "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", "_ZN5movie7DecoderD2Ev",
    "_ZN5movie7DecoderD0Ev", "_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE",
    "_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", "_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE",
    "_ZN5movie7Decoder5StartEv", "_ZN5movie7Decoder4StopEv", "_ZN5movie7Decoder5ResetEv", "_ZN5movie7Decoder5FlushEv",
    "_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", "_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE",
    "_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", "_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_",
    "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", "_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_",
    "_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", "_ZN5movie7Decoder12GetLastErrorEv",
    "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE",
    "_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE"};

enum { MOVIE_EV_INPUT, MOVIE_EV_OUTPUT, MOVIE_EV_FORMAT, MOVIE_EV_ERROR };   // DecoderEvents: EventType * per event
enum { MOVIE_TYPE_VP9 = 3, MOVIE_MODE_BUFFER = 1, MOVIE_MODE_TEXTURE = 2, MOVIE_FORMAT_TEXTURE = 3,
       MOVIE_FORMAT_NV12 = 4, MOVIE_FLAG_EOS = 4 };
enum { BUF_BASE = 0x28, BUF_NVN_DEV = 0x30, BUF_NVN_SYNC = 0x38 };   // movie::Buffer fields (Buffer::Base, SetNvnDev, SetNvnSync)

// Guest addresses of the functions the decoder calls with rt_call, the vtable address the constructor stores
// (vtable + 0x10), and the original code of every entry for decoders left to the library.
typedef struct {
    uint64_t signal_event, buf_set_range, buf_set_data, buf_base, buf_capacity;
    uint64_t md_find_int32, md_find_string, md_set_int32, md_set_rect, md_set_string;
    uint64_t vptr;
    uint64_t orig[MOVIE_ENTRIES];
} MovieGuest;

typedef struct { uint64_t frames, decode_ns, max_ns, errors, dropped, handed, uploads; } MovieStats;
typedef struct { int index; unsigned flags; int64_t time; } Ready;
typedef struct { int index, offset, size; int64_t time; unsigned flags; } Pending;
typedef struct {
    uint64_t guest;                          // the movie::Decoder object
    int mode, format, configured, started, w, h, eos_in, format_sent, last_error;
    uint64_t events;                         // guest DecoderEvents *: EventType * per MOVIE_EV_*
    vpx_codec_ctx_t vpx;
    int vpx_live;
    uint8_t *in[IN_BUFS];                    // kernel region: the game writes the compressed frames here
    int in_free[IN_BUFS], nin_free;
    Pending pending[IN_BUFS];                // sent, not decoded yet (the worker takes them in order)
    int npending;
    uint8_t *out[OUT_BUFS];                  // NV12 frames (buffer mode) or RGBA frames (texture mode)
    int out_bytes[OUT_BUFS], out_held[OUT_BUFS];
    Ready decoded[OUT_BUFS];                 // decoded, not due yet
    int ndecoded;
    Ready ready[OUT_BUFS];                   // handed over: AcquireOutputBufferIndex returns these
    int nready;
    unsigned owed;                           // events to signal on the next guest thread (bit per MOVIE_EV_*)
    uint64_t t0;                             // host time of the first frame handed over (pacing)
    int64_t pts0;                            // its presentation time (us)
    MovieStats st;
    PlatMutex mu;
    PlatCond work, idle;
    PlatThread *thread;
    int busy, quit;
    uint64_t gen;                            // bumped by Flush, Stop, Reset, destruction: a decode in flight is dropped
} Movie;

static MovieGuest G;
static volatile int32_t g_ready;
static PlatMutex g_init = PLAT_MUTEX_INIT;
static Movie *g_movies[MAX_MOVIES];
static volatile int32_t g_count;                // live host decoders: movie_tick returns at once when 0
static PlatMutex g_lock = PLAT_MUTEX_INIT;        // lock order: g_lock, then Movie.mu

static int pace_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = cfg_get("PLUCKOMP_MOVIE_PACE");
        v = !(e && !strcmp(e, "0"));
    }
    return v;
}

static uint64_t need(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("movie: guest symbol %s is not loaded", name);
    return a;
}

// The guest functions, resolved the first time any entry runs (the modules are loaded and relocated by then).
static void guest_init(void) {
    if (g_ready) return;
    plat_mutex_lock(&g_init);
    if (!g_ready) {
        G.signal_event = need("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
        G.buf_set_range = need("_ZN5movie6Buffer8SetRangeEii");
        G.buf_set_data = need("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
        G.buf_base = need("_ZNK5movie6Buffer4BaseEv");
        G.buf_capacity = need("_ZNK5movie6Buffer8CapacityEv");
        G.md_find_int32 = need("_ZNK5movie9MediaData9FindInt32EPKcPi");
        G.md_find_string = need("_ZNK5movie9MediaData10FindStringEPKcPS2_");
        G.md_set_int32 = need("_ZN5movie9MediaData8SetInt32EPKci");
        G.md_set_rect = need("_ZN5movie9MediaData7SetRectEPKciiii");
        G.md_set_string = need("_ZN5movie9MediaData9SetStringEPKcS2_");
        G.vptr = need("_ZTVN5movie7DecoderE") + 0x10;
        for (int i = 0; i < MOVIE_ENTRIES; i++) G.orig[i] = need(movie_symbols[i]);
        plat_xchg32(&g_ready, 1);
    }
    plat_mutex_unlock(&g_init);
}

#define U(x) ((uint64_t)(uintptr_t)(x))
static void buf_set_range(Cpu *c, uint64_t b, int off, int n) { rt_call(c, G.buf_set_range, 3, b, (uint64_t)off, (uint64_t)n); }
static void buf_set_data(Cpu *c, uint64_t b, uint64_t p, uint64_t n) { rt_call(c, G.buf_set_data, 3, b, p, n); }
static int md_find_int32(Cpu *c, uint64_t md, const char *k, int *v) {
    return (uint8_t)rt_call(c, G.md_find_int32, 3, md, U(k), U(v));
}
static void md_set_int32(Cpu *c, uint64_t md, const char *k, int v) { rt_call(c, G.md_set_int32, 3, md, U(k), (uint64_t)(uint32_t)v); }

// The original code of an entry with the caller's arguments; its result stays in x0.
static void pass(Cpu *c, int which) {
    uint64_t a[8];
    memcpy(a, c->x, sizeof a);
    rt_call(c, G.orig[which], 8, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

static Movie *find(uint64_t guest) {
    Movie *m = NULL;
    plat_mutex_lock_shared(&g_lock);
    for (int i = 0; i < MAX_MOVIES && !m; i++)
        if (g_movies[i] && g_movies[i]->guest == guest) m = g_movies[i];
    plat_mutex_unlock_shared(&g_lock);
    return m;
}

// Events are signalled after the locks are released: the woken game thread calls straight back in.
typedef struct { uint64_t ev[4 * MAX_MOVIES]; int n; } Signals;
static void collect(Movie *m, Signals *s) {   // m->mu held
    for (int k = 0; k < 4; k++)
        if (m->owed & (1u << k) && m->events && s->n < (int)(sizeof s->ev / sizeof *s->ev)) {
            uint64_t ev = LD64(m->events + 8 * (uint64_t)k);
            if (ev) s->ev[s->n++] = ev;
        }
    m->owed = 0;
}
static void fire(Cpu *c, const Signals *s) {
    for (int i = 0; i < s->n; i++) rt_call(c, G.signal_event, 1, s->ev[i]);
}

static int frame_bytes(const Movie *m) {
    return m->mode == MOVIE_MODE_TEXTURE ? m->w * m->h * 4 : m->w * m->h + (m->w / 2) * (m->h / 2) * 2;
}

// I420 (libvpx) -> packed NV12: Y rows, then rows of interleaved U, V.
static void to_nv12(const vpx_image_t *img, uint8_t *dst, int w, int h) {
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)y * w, img->planes[VPX_PLANE_Y] + (size_t)y * img->stride[VPX_PLANE_Y], (size_t)w);
    uint8_t *uv = dst + (size_t)w * h;
    int cw = w / 2;
    for (int y = 0; y < h / 2; y++) {
        const uint8_t *u = img->planes[VPX_PLANE_U] + (size_t)y * img->stride[VPX_PLANE_U];
        const uint8_t *v = img->planes[VPX_PLANE_V] + (size_t)y * img->stride[VPX_PLANE_V];
        uint8_t *o = uv + (size_t)y * w;
        int x = 0;
        for (; x + 16 <= cw; x += 16) {
            __m128i a = _mm_loadu_si128((const __m128i *)(u + x)), b = _mm_loadu_si128((const __m128i *)(v + x));
            _mm_storeu_si128((__m128i *)(o + 2 * x), _mm_unpacklo_epi8(a, b));
            _mm_storeu_si128((__m128i *)(o + 2 * x + 16), _mm_unpackhi_epi8(a, b));
        }
        for (; x < cw; x++) {
            o[2 * x] = u[x];
            o[2 * x + 1] = v[x];
        }
    }
}

// I420 -> RGBA8, BT.709 limited range (BT.601 below 720 lines), opaque alpha. Fixed point in 1/1024; four pixels of
// a 2x2 block share their chroma.
static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void to_rgba(const vpx_image_t *img, uint8_t *dst, int w, int h) {
    int hd = h >= 720;
    // R = Y' + rv V, G = Y' - gu U - gv V, B = Y' + bu U, Y' = 1.164 (Y - 16), U and V centred on 128
    const int ky = 1192, rv = hd ? 1836 : 1634, gu = hd ? 218 : 401, gv = hd ? 546 : 832, bu = hd ? 2163 : 2066;
    for (int y = 0; y < h; y++) {
        const uint8_t *py = img->planes[VPX_PLANE_Y] + (size_t)y * img->stride[VPX_PLANE_Y];
        const uint8_t *pu = img->planes[VPX_PLANE_U] + (size_t)(y / 2) * img->stride[VPX_PLANE_U];
        const uint8_t *pv = img->planes[VPX_PLANE_V] + (size_t)(y / 2) * img->stride[VPX_PLANE_V];
        uint8_t *o = dst + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            int yy = (py[x] - 16) * ky, u = pu[x / 2] - 128, v = pv[x / 2] - 128;
            o[4 * x + 0] = clamp8((yy + rv * v + 512) >> 10);
            o[4 * x + 1] = clamp8((yy - gu * u - gv * v + 512) >> 10);
            o[4 * x + 2] = clamp8((yy + bu * u + 512) >> 10);
            o[4 * x + 3] = 255;
        }
    }
}

static int free_out(const Movie *m) {
    for (int i = 0; i < OUT_BUFS; i++)
        if (!m->out_held[i]) return i;
    return -1;
}
static int free_outs(const Movie *m) {
    int n = 0;
    for (int i = 0; i < OUT_BUFS; i++) n += !m->out_held[i];
    return n;
}
static int can_take(const Movie *m, const Pending *p) {
    int want = (p->size > 0) + ((p->flags & MOVIE_FLAG_EOS) && !m->eos_in);
    return free_outs(m) >= want;
}
static void push_decoded(Movie *m, int slot, unsigned flags, int64_t time, int bytes) {
    m->out_held[slot] = 1;
    m->out_bytes[slot] = bytes;
    m->decoded[m->ndecoded++] = (Ready){slot, flags, time};
}

static int vpx_open(Movie *m) {
    vpx_codec_dec_cfg_t cfg = {.threads = VPX_THREADS, .w = (unsigned)m->w, .h = (unsigned)m->h};
    if (vpx_codec_dec_init(&m->vpx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) return 0;
    vpx_codec_control(&m->vpx, VP9D_SET_ROW_MT, 1);
    m->vpx_live = 1;
    return 1;
}

// The worker: takes inputs in order, decodes them outside the lock into a reserved output slot, publishes the frame.
static void worker(void *arg) {
    Movie *m = arg;
    plat_mutex_lock(&m->mu);
    for (;;) {
        while (!m->quit && !(m->started && m->vpx_live && m->npending && can_take(m, &m->pending[0])))
            plat_cond_wait(&m->work, &m->mu);
        if (m->quit) break;
        Pending p = m->pending[0];
        memmove(m->pending, m->pending + 1, (size_t)--m->npending * sizeof *m->pending);
        int slot = -1;
        if (p.size > 0) {
            slot = free_out(m);
            m->out_held[slot] = 1;   // reserved: in no queue, so the game cannot release it
        }
        uint64_t gen = m->gen, dt = 0;
        int got = 0, w = m->w, h = m->h, rgba = m->mode == MOVIE_MODE_TEXTURE;
        uint8_t *dst = slot >= 0 ? m->out[slot] : NULL;
        vpx_codec_err_t e = VPX_CODEC_OK;
        const char *bad = NULL;
        m->busy = 1;
        plat_mutex_unlock(&m->mu);
        if (p.size > 0) {
            uint64_t t0 = kern_now_ns();
            e = vpx_codec_decode(&m->vpx, m->in[p.index] + p.offset, (unsigned)p.size, NULL, 0);
            if (e == VPX_CODEC_OK) {
                vpx_codec_iter_t it = NULL;
                const vpx_image_t *img;
                while ((img = vpx_codec_get_frame(&m->vpx, &it)) != NULL) {
                    if (img->fmt != VPX_IMG_FMT_I420 || (int)img->d_w != w || (int)img->d_h != h) bad = "layout";
                    else if (!got) {
                        if (rgba) to_rgba(img, dst, w, h);
                        else to_nv12(img, dst, w, h);
                    }
                    got++;
                }
            }
            dt = kern_now_ns() - t0;
        }
        plat_mutex_lock(&m->mu);
        m->busy = 0;
        plat_cond_broadcast(&m->idle);
        if (m->gen != gen) continue;   // flushed, stopped or reset meanwhile: the queues were reset after this
        m->in_free[m->nin_free++] = p.index;
        m->owed |= 1u << MOVIE_EV_INPUT;
        if (p.size > 0) {
            m->st.decode_ns += dt;
            if (dt > m->st.max_ns) m->st.max_ns = dt;
            if (bad) rt_fatal("movie: VP9 frame layout differs from the configured %dx%d I420", w, h);
            if (e != VPX_CODEC_OK || !got) {
                m->out_held[slot] = 0;
                if (e != VPX_CODEC_OK) {
                    m->st.errors++;
                    m->last_error = ST_ERROR;
                    m->owed |= 1u << MOVIE_EV_ERROR;
                    if (m->st.errors <= 4) {
                        const char *d = vpx_codec_error_detail(&m->vpx);
                        rt_log("movie: VP9 decode error on a %d-byte frame at %lld us: %s", p.size, (long long)p.time,
                               d ? d : vpx_codec_err_to_string(e));
                    }
                }
            } else {
                if (got > 1) rt_log("movie: %d frames from one input at %lld us, the first kept", got, (long long)p.time);
                if (!m->format_sent) {
                    m->format_sent = 1;
                    m->owed |= 1u << MOVIE_EV_FORMAT;
                }
                push_decoded(m, slot, 0, p.time, frame_bytes(m));
                m->st.frames++;
                if (m->st.frames % 600 == 0)
                    rt_log("movie: %llu frames, decode %.2f ms per frame (max %.2f)", (unsigned long long)m->st.frames,
                           m->st.decode_ns / 1e6 / (double)m->st.frames, m->st.max_ns / 1e6);
            }
        }
        if (p.flags & MOVIE_FLAG_EOS && !m->eos_in) {
            m->eos_in = 1;
            push_decoded(m, free_out(m), MOVIE_FLAG_EOS, p.time, 0);
            rt_log("movie: end of stream after %llu frames (decode %.2f ms per frame, max %.2f, %llu errors)",
                   (unsigned long long)m->st.frames, m->st.frames ? m->st.decode_ns / 1e6 / (double)m->st.frames : 0.0,
                   m->st.max_ns / 1e6, (unsigned long long)m->st.errors);
        }
    }
    plat_mutex_unlock(&m->mu);
}

// With m->mu held: invalidate a decode in flight and wait for the worker to leave it. Returns whether the calling guest
// thread gave its core up; the caller calls kern_core_back after releasing m->mu.
static int quiesce(Movie *m) {
    int away = 0;
    m->gen++;
    while (m->busy) {
        if (!away) away = kern_core_away();
        plat_cond_wait(&m->idle, &m->mu);
    }
    return away;
}
static void unlock_back(Movie *m, int away) {
    plat_mutex_unlock(&m->mu);
    kern_core_back(&away);
}

static void reset_queues(Movie *m) {
    m->nin_free = 0;
    for (int i = 0; i < IN_BUFS; i++) m->in_free[m->nin_free++] = i;
    memset(m->out_held, 0, sizeof m->out_held);
    m->npending = m->nready = m->ndecoded = 0;
    m->eos_in = 0;
    m->t0 = 0;
}
static void release_codec(Movie *m) {   // m->mu held, worker quiesced
    if (m->vpx_live) {
        vpx_codec_destroy(&m->vpx);
        m->vpx_live = 0;
    }
    for (int i = 0; i < IN_BUFS; i++) {
        if (m->in[i]) mem_kernel_free(m->in[i], IN_CAP);
        m->in[i] = NULL;
    }
    for (int i = 0; i < OUT_BUFS; i++) {
        free(m->out[i]);
        m->out[i] = NULL;
    }
    m->configured = m->started = 0;
    reset_queues(m);
}

// Host time a decoded frame is due (ns); end of stream is due as soon as it is first in line.
static uint64_t due_at(const Movie *m, const Ready *d) {
    int64_t ahead = d->time - m->pts0;
    if (ahead < 0) ahead = 0;
    return m->t0 + (uint64_t)ahead * 1000;
}
// One present: drop leading frames more than 1.5 periods late while the next one is due too, then hand over at most one
// due frame (end of stream right after the last frame).
static void release_due(Movie *m, uint64_t now) {
    if (!m->ndecoded) return;
    if (!pace_on()) {
        for (int i = 0; i < m->ndecoded; i++) m->ready[m->nready++] = m->decoded[i];
        m->st.handed += m->ndecoded;
        m->ndecoded = 0;
        m->owed |= 1u << MOVIE_EV_OUTPUT;
        return;
    }
    while (m->ndecoded > 1 && m->t0 && !(m->decoded[0].flags & MOVIE_FLAG_EOS) &&
           !(m->decoded[1].flags & MOVIE_FLAG_EOS)) {
        uint64_t due = due_at(m, &m->decoded[0]), next = due_at(m, &m->decoded[1]);
        if (now < next || now - due <= (next - due) * 3 / 2) break;
        m->out_held[m->decoded[0].index] = 0;
        m->st.dropped++;
        memmove(m->decoded, m->decoded + 1, (size_t)--m->ndecoded * sizeof *m->decoded);
        plat_cond_signal(&m->work);
    }
    Ready d = m->decoded[0];
    if (!(d.flags & MOVIE_FLAG_EOS)) {
        if (!m->t0) {
            m->t0 = now;
            m->pts0 = d.time;
        }
        if (now < due_at(m, &d)) return;
    }
    memmove(m->decoded, m->decoded + 1, (size_t)--m->ndecoded * sizeof *m->decoded);
    m->ready[m->nready++] = d;
    m->st.handed++;
    m->owed |= 1u << MOVIE_EV_OUTPUT;
}

// At every present, on the presenting guest thread (nvn_api_obj.c nvnQueuePresentTexture).
void movie_tick(Cpu *c) {
    if (!g_count) return;
    Signals s = {0};
    plat_mutex_lock_shared(&g_lock);
    for (int i = 0; i < MAX_MOVIES; i++) {
        Movie *m = g_movies[i];
        if (!m) continue;
        plat_mutex_lock(&m->mu);
        if (m->started) release_due(m, kern_now_ns());
        collect(m, &s);
        plat_mutex_unlock(&m->mu);
    }
    plat_mutex_unlock_shared(&g_lock);
    fire(c, &s);
}

// ---- entries
// Decoder(DecoderType type, DecoderOutputFormat format, DecoderMode mode, int priority)
static void ov_ctor(Cpu *c) {
    uint64_t obj = arg_x(c, 0);
    int type = (int)arg_x(c, 1), format = (int)arg_x(c, 2), mode = (int)arg_x(c, 3);
    guest_init();
    char at[128];
    ldr_describe(c->x[30] - 4, at, sizeof at);
    if (type != MOVIE_TYPE_VP9 || (mode != MOVIE_MODE_BUFFER && mode != MOVIE_MODE_TEXTURE)) {
        rt_log("movie: decoder %#llx type %d, output format %d, mode %d left to the library (from %s)",
               (unsigned long long)obj, type, format, mode, at);
        pass(c, MOVIE_CTOR);
        return;
    }
    Movie *m = calloc(1, sizeof *m);
    if (!m) rt_fatal("movie: decoder allocation");
    m->guest = obj;
    m->mode = mode;
    m->format = format;
    plat_mutex_init(&m->mu);
    plat_cond_init(&m->work);
    plat_cond_init(&m->idle);
    m->thread = plat_thread_new(worker, m, "movie");
    if (!m->thread) rt_fatal("movie: cannot start the worker thread");
    plat_mutex_lock(&g_lock);
    int i = 0;
    while (i < MAX_MOVIES && g_movies[i] && g_movies[i]->guest != obj) i++;
    if (i == MAX_MOVIES) rt_fatal("movie: more than %d live decoders", MAX_MOVIES);
    if (g_movies[i]) rt_fatal("movie: decoder object %#llx constructed twice", (unsigned long long)obj);
    g_movies[i] = m;
    plat_inc32(&g_count);
    plat_mutex_unlock(&g_lock);
    ST64(obj, G.vptr);
    ST64(obj + 8, U(m));
    rt_log("movie: VP9 decoder %#llx on the host (libvpx %s), output format %d, mode %d (from %s)",
           (unsigned long long)obj, vpx_codec_version_str(), format, mode, at);
}

static void destroy(uint64_t obj) {
    Movie *m = NULL;
    plat_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_MOVIES; i++)
        if (g_movies[i] && g_movies[i]->guest == obj) {
            m = g_movies[i];
            g_movies[i] = NULL;
            plat_dec32(&g_count);
        }
    plat_mutex_unlock(&g_lock);
    if (!m) return;
    plat_mutex_lock(&m->mu);
    m->quit = 1;
    m->gen++;
    plat_cond_broadcast(&m->work);
    plat_mutex_unlock(&m->mu);
    int away = kern_core_away();
    plat_thread_join(m->thread);
    kern_core_back(&away);
    rt_log("movie: decoder %#llx destroyed after %llu frames (%llu dropped to keep time)", (unsigned long long)obj,
           (unsigned long long)m->st.frames, (unsigned long long)m->st.dropped);
    release_codec(m);
    free(m);
    ST64(obj, G.vptr);   // what the library's destructor leaves (no DecoderImpl)
    ST64(obj + 8, 0);
}

static void ov_d2(Cpu *c) {
    uint64_t obj = arg_x(c, 0);
    if (!find(obj)) { pass(c, MOVIE_D2); return; }
    destroy(obj);
}
// Deleting destructor: free the host state, then the library's deleting destructor frees the object (impl now 0).
static void ov_d0(Cpu *c) {
    uint64_t obj = arg_x(c, 0);
    if (find(obj)) destroy(obj);
    pass(c, MOVIE_D0);
}

// Status Configure(MediaData *format)
static void ov_configure(Cpu *c) {
    uint64_t obj = arg_x(c, 0), md = arg_x(c, 1);
    Movie *m = find(obj);
    if (!m) { pass(c, MOVIE_CONFIGURE); return; }
    if (!md) { ret_x(c, ST_NULL); return; }
    int fmt_ok = m->mode == MOVIE_MODE_TEXTURE ? m->format == MOVIE_FORMAT_TEXTURE : m->format == MOVIE_FORMAT_NV12;
    if (!fmt_ok) {
        rt_log("movie: output format %d in mode %d not implemented on the host (NV12 = 4 in buffer mode, texture = 3 "
               "in texture mode)", m->format, m->mode);
        ret_x(c, ST_UNSUPPORTED);
        return;
    }
    uint64_t mime_p = 0;
    int w = 0, h = 0;
    rt_call(c, G.md_find_string, 3, md, U("mime"), U(&mime_p));
    md_find_int32(c, md, "width", &w);
    md_find_int32(c, md, "height", &h);
    const char *mime = (const char *)(uintptr_t)mime_p;
    if (!mime || strcmp(mime, "video/x-vnd.on2.vp9") || w <= 0 || h <= 0 || w > 8192 || h > 8192 || (w | h) & 1) {
        rt_log("movie: configure: mime %s %dx%d not supported", mime ? mime : "(none)", w, h);
        ret_x(c, ST_UNSUPPORTED);
        return;
    }
    plat_mutex_lock(&m->mu);
    int away = quiesce(m);
    release_codec(m);
    m->w = w;
    m->h = h;
    if (!vpx_open(m)) rt_fatal("movie: libvpx VP9 decoder initialisation");
    for (int i = 0; i < IN_BUFS; i++) m->in[i] = mem_kernel_alloc(IN_CAP, 0x1000);
    for (int i = 0; i < OUT_BUFS; i++)
        if (!(m->out[i] = malloc((size_t)frame_bytes(m)))) rt_fatal("movie: output buffers");
    reset_queues(m);
    m->format_sent = 0;
    m->configured = 1;
    m->last_error = 0;
    unlock_back(m, away);
    rt_log("movie: configured %dx%d VP9 (%s), %d input buffers of %d KiB, %d output frames of %d bytes", w, h,
           m->mode == MOVIE_MODE_TEXTURE ? "into the client's texture" : "NV12 buffers", IN_BUFS, IN_CAP >> 10,
           OUT_BUFS, frame_bytes(m));
    ret_x(c, ST_OK);
}

// Status RegisterClientEvents(DecoderEvents *events)
static void ov_register_events(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_REGISTER_EVENTS); return; }
    plat_mutex_lock(&m->mu);
    m->events = arg_x(c, 1);
    plat_mutex_unlock(&m->mu);
    ret_x(c, ST_OK);
}

static void set_format(Cpu *c, int w, int h, uint64_t md) {
    rt_call(c, G.md_set_string, 3, md, U("mime"), U("video/raw"));
    md_set_int32(c, md, "width", w);
    md_set_int32(c, md, "height", h);
    md_set_int32(c, md, "stride", w);
    md_set_int32(c, md, "slice-height", h);
    md_set_int32(c, md, "color-format", 21);   // COLOR_FormatYUV420SemiPlanar
    rt_call(c, G.md_set_rect, 6, md, U("crop"), (uint64_t)0, (uint64_t)0, (uint64_t)(uint32_t)(w - 1),
            (uint64_t)(uint32_t)(h - 1));
    md_set_int32(c, md, "crop-width", w);
    md_set_int32(c, md, "crop-height", h);
    md_set_int32(c, md, "nv12-y-stride", w);
    md_set_int32(c, md, "nv12-uv-stride", w);
    md_set_int32(c, md, "nv12-y-offset", 0);
    md_set_int32(c, md, "nv12-uv-offset", w * h);
}

// Status GetOutputFormat(MediaData *format)
static void ov_get_output_format(Cpu *c) {
    uint64_t md = arg_x(c, 1);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_OUTPUT_FORMAT); return; }
    if (!md) { ret_x(c, ST_NULL); return; }
    plat_mutex_lock_shared(&m->mu);
    int ok = m->configured, w = m->w, h = m->h;
    plat_mutex_unlock_shared(&m->mu);
    if (!ok) { ret_x(c, ST_ERROR); return; }
    set_format(c, w, h, md);
    ret_x(c, ST_OK);
}

static void ov_start(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_START); return; }
    Signals s = {0};
    plat_mutex_lock(&m->mu);
    int st = ST_NOT_CONFIGURED;
    if (m->configured) {
        m->started = 1;
        m->owed |= 1u << MOVIE_EV_INPUT;
        plat_cond_signal(&m->work);
        st = ST_OK;
    }
    collect(m, &s);
    plat_mutex_unlock(&m->mu);
    fire(c, &s);
    ret_x(c, (uint32_t)st);
}

static void ov_stop(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_STOP); return; }
    plat_mutex_lock(&m->mu);
    int away = quiesce(m);
    rt_log("movie: stop after %llu frames (%llu dropped to keep time, %llu uploaded; decode %.2f ms per frame, max %.2f)",
           (unsigned long long)m->st.frames, (unsigned long long)m->st.dropped, (unsigned long long)m->st.uploads,
           m->st.frames ? m->st.decode_ns / 1e6 / (double)m->st.frames : 0.0, m->st.max_ns / 1e6);
    release_codec(m);
    unlock_back(m, away);
    ret_x(c, ST_OK);
}

static void ov_reset(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_RESET); return; }
    plat_mutex_lock(&m->mu);
    int away = quiesce(m);
    release_codec(m);
    m->last_error = 0;
    unlock_back(m, away);
    ret_x(c, ST_OK);
}

// Drop queued work; the next input starts a new decode (after a seek: a key frame).
static void ov_flush(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_FLUSH); return; }
    plat_mutex_lock(&m->mu);
    int away = quiesce(m);
    if (m->configured) {
        vpx_codec_destroy(&m->vpx);
        m->vpx_live = 0;
        if (!vpx_open(m)) rt_fatal("movie: libvpx VP9 decoder re-initialisation");
        reset_queues(m);
    }
    unlock_back(m, away);
    ret_x(c, ST_OK);
}

// Status AcquireInputBufferIndex(int *index, int *remaining)
static void ov_acquire_input(Cpu *c) {
    int *index = arg_p(c, 1), *remaining = arg_p(c, 2);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_ACQUIRE_INPUT); return; }
    if (!index || !remaining) { ret_x(c, ST_NULL); return; }
    Signals s = {0};
    plat_mutex_lock(&m->mu);
    *index = -1;
    *remaining = 0;
    int st = ST_NO_INPUT;
    if (m->started && m->nin_free > 0) {
        *index = m->in_free[0];
        memmove(m->in_free, m->in_free + 1, (size_t)--m->nin_free * sizeof *m->in_free);
        *remaining = m->nin_free;
        st = ST_OK;
    }
    collect(m, &s);
    plat_mutex_unlock(&m->mu);
    fire(c, &s);
    ret_x(c, (uint32_t)st);
}

// Status GetInputBuffer(int index, Buffer *buffer)
static void ov_get_input(Cpu *c) {
    int index = (int)arg_x(c, 1);
    uint64_t buf = arg_x(c, 2);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_INPUT); return; }
    if (!buf) { ret_x(c, ST_NULL); return; }
    plat_mutex_lock_shared(&m->mu);
    uint8_t *p = index >= 0 && index < IN_BUFS ? m->in[index] : NULL;
    plat_mutex_unlock_shared(&m->mu);
    buf_set_range(c, buf, 0, p ? IN_CAP : 0);
    buf_set_data(c, buf, U(p), p ? IN_CAP : 0);
    ret_x(c, p ? ST_OK : ST_ERROR);
}

// Status SendInputBufferForDecode(int index, int offset, int size, long time, unsigned flags)
static void ov_send_input(Cpu *c) {
    int index = (int)arg_x(c, 1), offset = (int)arg_x(c, 2), size = (int)arg_x(c, 3);
    int64_t time = (int64_t)arg_x(c, 4);
    unsigned flags = (unsigned)arg_x(c, 5);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_SEND_INPUT); return; }
    if (index < 0 || index >= IN_BUFS || offset < 0 || size < 0 || (int64_t)offset + size > IN_CAP) {
        ret_x(c, ST_ERROR);
        return;
    }
    Signals s = {0};
    plat_mutex_lock(&m->mu);
    int st = ST_OK;
    if (!m->started || m->npending == IN_BUFS) st = ST_ERROR;
    else if (m->eos_in) {   // after end of stream: hand the buffer back
        m->in_free[m->nin_free++] = index;
        m->owed |= 1u << MOVIE_EV_INPUT;
    } else {
        m->pending[m->npending++] = (Pending){index, offset, size, time, flags};
        plat_cond_signal(&m->work);
    }
    collect(m, &s);
    plat_mutex_unlock(&m->mu);
    fire(c, &s);
    ret_x(c, (uint32_t)st);
}

// Status AcquireOutputBufferIndex(int *index, long *time, unsigned *flags, int *remaining)
static void ov_acquire_output(Cpu *c) {
    int *index = arg_p(c, 1), *remaining = arg_p(c, 4);
    int64_t *time = arg_p(c, 2);
    unsigned *flags = arg_p(c, 3);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_ACQUIRE_OUTPUT); return; }
    if (!index || !time || !flags || !remaining) { ret_x(c, ST_NULL); return; }
    Signals s = {0};
    plat_mutex_lock(&m->mu);   // paced frames become ready only at presents (movie_tick)
    *index = -1;
    *time = 0;
    *flags = 0;
    *remaining = 0;
    int st = ST_NO_OUTPUT;
    if (m->nready > 0) {
        Ready r = m->ready[0];
        memmove(m->ready, m->ready + 1, (size_t)--m->nready * sizeof *m->ready);
        *index = r.index;
        *time = r.time;
        *flags = r.flags;
        *remaining = m->nready;
        st = ST_OK;
    }
    collect(m, &s);
    plat_mutex_unlock(&m->mu);
    fire(c, &s);
    ret_x(c, (uint32_t)st);
}

// Whether output index i is held by the game: acquired, not queued any more.
static int held_by_game(const Movie *m, int index) {   // m->mu held
    if (index < 0 || index >= OUT_BUFS || !m->out_held[index]) return 0;
    for (int i = 0; i < m->nready; i++)
        if (m->ready[i].index == index) return 0;
    for (int i = 0; i < m->ndecoded; i++)
        if (m->decoded[i].index == index) return 0;
    return 1;
}

// Texture mode: the RGBA frame into the client's texture (Buffer base), the client's sync fenced, the index released.
// The texture is written with the back end's WriteTexels, as nvnTextureWriteTexels would, so the replay orders it
// before every later submit; the sync is fenced at the newest submit, which the frame's upload precedes.
static int upload_output(Cpu *c, Movie *m, int index, uint64_t buf) {
    const tex_t *t = (const tex_t *)(uintptr_t)LD64(buf + BUF_BASE);
    sync_t *sync = (sync_t *)(uintptr_t)LD64(buf + BUF_NVN_SYNC);
    if (!t) return ST_NULL;
    int fmt = t->d.format, bgra = fmt == NVN_FORMAT_BGRA8 || fmt == NVN_FORMAT_BGRA8_SRGB;
    if (!bgra && fmt != NVN_FORMAT_RGBA8 && fmt != NVN_FORMAT_RGBA8_SRGB && fmt != NVN_FORMAT_RGBX8_SRGB)
        rt_fatal("movie: the client's texture %p has NVN format %#x; only 8-bit RGBA and BGRA are written",
                 (const void *)t, fmt);
    plat_mutex_lock(&m->mu);
    int ok = held_by_game(m, index) && m->out_bytes[index], w = m->w, h = m->h;
    uint8_t *frame = ok ? m->out[index] : NULL;
    plat_mutex_unlock(&m->mu);
    if (!ok) return ST_ERROR;   // the game holds the index, so the frame stays until the release below
    static volatile int32_t logged;
    if (!plat_xchg32(&logged, 1))
        rt_log("movie: first frame into texture %p (%dx%d, NVN format %#x), sync %p", (const void *)t, t->d.width,
               t->d.height, fmt, (void *)sync);
    int cw = (int)t->d.width < w ? (int)t->d.width : w, ch = (int)t->d.height < h ? (int)t->d.height : h;
    if (bgra)   // the frame is the game's alone until released, so it is swizzled in place
        for (size_t i = 0; i < (size_t)w * h * 4; i += 4) {
            uint8_t x = frame[i];
            frame[i] = frame[i + 2];
            frame[i + 2] = x;
        }
    view_t view;
    memset(&view, 0, sizeof view);
    NVNcopyRegion r = {.width = cw, .height = ch, .depth = 1};
    {
        int away = kern_core_away();
        gfx_backend()->write_texels(t, &view, &r, frame, (uint64_t)w * 4, (uint64_t)w * 4 * h);
        kern_core_back(&away);
    }
    if (sync) {
        sync->fenced = 1;
        sync->fence = nvn_fence_last();
    }
    Signals s = {0};
    plat_mutex_lock(&m->mu);
    if (held_by_game(m, index)) m->out_held[index] = 0;
    m->st.uploads++;
    plat_cond_signal(&m->work);
    collect(m, &s);
    plat_mutex_unlock(&m->mu);
    fire(c, &s);
    return ST_OK;
}

static int get_output(Cpu *c, Movie *m, int index, uint64_t buf) {
    if (m->mode == MOVIE_MODE_TEXTURE) return upload_output(c, m, index, buf);
    buf_set_range(c, buf, 0, 0);
    uint8_t *dst = (uint8_t *)(uintptr_t)rt_call(c, G.buf_base, 1, buf);
    uint64_t cap = rt_call(c, G.buf_capacity, 1, buf);
    int st = ST_ERROR, bytes = 0;
    plat_mutex_lock_shared(&m->mu);
    if (index >= 0 && index < OUT_BUFS && m->out_held[index]) {
        bytes = m->out_bytes[index];
        if (!bytes) st = ST_OK;
        else if (dst && cap >= (uint64_t)bytes) {
            memcpy(dst, m->out[index], (size_t)bytes);
            st = ST_OK;
        } else {
            rt_log("movie: output buffer of %llu bytes for a %d-byte frame", (unsigned long long)cap, bytes);
            bytes = 0;
        }
    }
    plat_mutex_unlock_shared(&m->mu);
    if (st == ST_OK && bytes) buf_set_range(c, buf, 0, bytes);
    return st;
}

// Status GetOutputBuffer(int index, Buffer *buffer)
static void ov_get_output(Cpu *c) {
    int index = (int)arg_x(c, 1);
    uint64_t buf = arg_x(c, 2);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_OUTPUT); return; }
    ret_x(c, buf ? (uint32_t)get_output(c, m, index, buf) : ST_NULL);
}

// Status GetOutputBufferSize(int *size, int *count)
static void ov_get_output_size(Cpu *c) {
    int *size = arg_p(c, 1), *count = arg_p(c, 2);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_OUTPUT_SIZE); return; }
    if (!size || !count) { ret_x(c, ST_NULL); return; }
    plat_mutex_lock_shared(&m->mu);
    *count = 1;
    *size = m->configured ? frame_bytes(m) : 0;
    plat_mutex_unlock_shared(&m->mu);
    ret_x(c, ST_OK);
}

// Status ReleaseOutputBufferIndex(int index). In texture mode GetOutputBuffer has released the index already, as
// DecoderImpl::CopyOutputBufferToClientTextureAndRelease does; a release of an index the game no longer holds is then
// answered OK.
static void ov_release_output(Cpu *c) {
    int index = (int)arg_x(c, 1);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_RELEASE_OUTPUT); return; }
    Signals s = {0};
    plat_mutex_lock(&m->mu);
    int st = ST_ERROR;
    if (held_by_game(m, index)) {
        m->out_held[index] = 0;
        plat_cond_signal(&m->work);
        st = ST_OK;
    } else if (m->mode == MOVIE_MODE_TEXTURE && index >= 0 && index < OUT_BUFS) st = ST_OK;
    collect(m, &s);
    plat_mutex_unlock(&m->mu);
    fire(c, &s);
    ret_x(c, (uint32_t)st);
}

static void ov_get_last_error(Cpu *c) {
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_LAST_ERROR); return; }
    plat_mutex_lock_shared(&m->mu);
    int e = m->last_error;
    plat_mutex_unlock_shared(&m->mu);
    ret_x(c, (uint32_t)e);
}

static int output_format(Cpu *c, Movie *m, int index, uint64_t md) {
    plat_mutex_lock_shared(&m->mu);
    int ok = index >= 0 && index < OUT_BUFS && m->out_held[index], w = m->w, h = m->h;
    plat_mutex_unlock_shared(&m->mu);
    if (!ok) return ST_ERROR;
    set_format(c, w, h, md);
    return ST_OK;
}

// Status GetOutputBufferFormat(int index, MediaData *format)
static void ov_get_output_buffer_format(Cpu *c) {
    int index = (int)arg_x(c, 1);
    uint64_t md = arg_x(c, 2);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_OUTPUT_BUFFER_FORMAT); return; }
    ret_x(c, md ? (uint32_t)output_format(c, m, index, md) : ST_NULL);
}

// Status GetOutputBuffer(int index, Buffer *buffer, MediaData *format)
static void ov_get_output_md(Cpu *c) {
    int index = (int)arg_x(c, 1);
    uint64_t buf = arg_x(c, 2), md = arg_x(c, 3);
    Movie *m = find(arg_x(c, 0));
    if (!m) { pass(c, MOVIE_GET_OUTPUT_MD); return; }
    if (!buf && !md) { ret_x(c, ST_NULL); return; }
    int st = md ? output_format(c, m, index, md) : ST_OK;
    if (st == ST_OK && buf) st = get_output(c, m, index, buf);
    ret_x(c, (uint32_t)st);
}

HOST_OVERRIDE("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", ov_ctor);
HOST_OVERRIDE("_ZN5movie7DecoderD2Ev", ov_d2);
// subsdk0 exports the complete-object destructor at the same address as D2 (no virtual bases).
HOST_OVERRIDE("_ZN5movie7DecoderD1Ev", ov_d2);
HOST_OVERRIDE("_ZN5movie7DecoderD0Ev", ov_d0);
HOST_OVERRIDE("_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", ov_configure);
HOST_OVERRIDE("_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", ov_register_events);
HOST_OVERRIDE("_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", ov_get_output_format);
HOST_OVERRIDE("_ZN5movie7Decoder5StartEv", ov_start);
HOST_OVERRIDE("_ZN5movie7Decoder4StopEv", ov_stop);
HOST_OVERRIDE("_ZN5movie7Decoder5ResetEv", ov_reset);
HOST_OVERRIDE("_ZN5movie7Decoder5FlushEv", ov_flush);
HOST_OVERRIDE("_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", ov_acquire_input);
HOST_OVERRIDE("_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", ov_get_input);
HOST_OVERRIDE("_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", ov_send_input);
HOST_OVERRIDE("_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_", ov_acquire_output);
HOST_OVERRIDE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", ov_get_output);
HOST_OVERRIDE("_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", ov_get_output_size);
HOST_OVERRIDE("_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", ov_release_output);
HOST_OVERRIDE("_ZN5movie7Decoder12GetLastErrorEv", ov_get_last_error);
HOST_OVERRIDE("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", ov_get_output_md);
HOST_OVERRIDE("_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE", ov_get_output_buffer_format);
