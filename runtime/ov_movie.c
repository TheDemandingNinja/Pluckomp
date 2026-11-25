// movie::Decoder on a host without a video decoder. The SDK movie library in subsdk0 (Android MediaCodec over NVIDIA
// NvMM) opens NVDEC through NvRm, whose first NvOsDrvOpen aborts: nvdrv is not served and nv::InitializeGraphics is
// answered on the host (ov_services.c). TotK Recomp met the same abort and replaces the decoder with libvpx
// (runtime/ov_movie.c); until Pikmin 4's movies get a host decoder, Configure answers that the stream is unsupported,
// as a decoder without that codec would, and the library never reaches NvRm. The game's demuxer (movie::Extractor)
// still runs as guest code.
// main constructs decoders through its import of the constructor and calls them through movie::Decoder's vtable in
// subsdk0, whose slots are ABS64 relocations by name, so the HOST_OVERRIDE below replaces the Configure slot.
// Status values as TotK Recomp runtime/ov_movie.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh) names them.
#include <stdint.h>

#include "host.h"

#define MOVIE_ST_UNSUPPORTED 0x2001u

// movie::Decoder::Decoder(DecoderType type, DecoderOutputFormat format, DecoderMode mode, int flags): logged, then the
// library's own constructor runs (it does not reach NvRm).
static void obs_decoder_ctor(Cpu *c) {
    uint64_t self = arg_x(c, 0), type = arg_x(c, 1), format = arg_x(c, 2), mode = arg_x(c, 3);
    char at[128];
    ldr_describe(c->x[30] - 4, at, sizeof at);
    rt_original(c);
    rt_log("movie: decoder %#llx type %llu, output format %llu, mode %llu (from %s)", (unsigned long long)self,
           (unsigned long long)type, (unsigned long long)format, (unsigned long long)mode, at);
}

// Status movie::Decoder::Configure(MediaData *format)
static void ov_decoder_configure(Cpu *c) {
    static uint32_t calls;
    if (__atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED) <= 8)
        rt_log("movie: decoder %#llx: Configure answered unsupported (no host video decoder yet)",
               (unsigned long long)arg_x(c, 0));
    ret_x(c, MOVIE_ST_UNSUPPORTED);
}

HOST_OBSERVER("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", obs_decoder_ctor);
HOST_OVERRIDE("_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", ov_decoder_configure);
