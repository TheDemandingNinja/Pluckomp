// Adapted from TotK Recomp runtime/gfx_shader.c @ afae80c (MIT, Copyright (c) 2026 Valiantsin Dzerakh).
// Shader service client: turns an NVN program (control section, code, c1 constants and the
// translator's answers) into SPIR-V plus reflection through `pluckomp_xlate.exe --serve` children, started lazily with
// pipes and kept for the life of the process (a pool: PLUCKOMP_XLATE_PROCS at run time, more for the boot prewarm).
// Results are cached in memory and on disk (shader-cache/<key>.spv and <key>.res.json next to pluckomp.exe), so a shader
// is translated once per machine and translator.
//
// Keys. The code hash is sha256 over stage (u32), the 0x880-byte control section, code_size (u64), the code, the c1
// length (u32) and bytes. Per code hash and translator the cache holds a use mask (<sha256("TKSU", xlate id, code
// hash)>.use): the texture/image slots the translator queried (.res.json tex_queried, img_queried), the vertex
// attributes a VS declares and the colour targets an FS declares (SPIR-V Locations). The input id is sha256 over "TKSI",
// IN_VERSION, the code hash and the answers with everything outside the mask zeroed (fixed little-endian layout), so
// stale bindings in slots the shader never reads do not make new ids; it does not depend on the translator. The cache
// key is sha256 over "TKSK", KEY_VERSION, the translator identity and the input id. A code hash without a known mask is
// translated under a provisional key ("TKSP", full answers); its files are renamed to the cache key and the mask stored.
//
// Translator identity: build/xlate/pluckomp_xlate.id, sha256 hex that tools/build_xlate.py writes over the sources it
// builds from (Program.cs, the csproj, the patches, the vendor tree); without it the sha256 of the exe. PLUCKOMP_XLATE_ID
// (64 hex digits) overrides it. A translator change therefore changes every cache key without a hand-made KEY_VERSION
// bump; KEY_VERSION is for changes on this side (key layout, how the output is used).
//
// Pack: tools/pack_shader_cache.py folds the .spv, .res.json, .use and in/*.in files into <cache>/shader-cache.pack
// (one file instead of four per shader: ~117,000 files after an install) and deletes them; the installer runs it after
// preparing the shaders. The pack is read-only here. Every read looks in the pack first and then for the loose file,
// and new translations are written as loose files as before, until the tool runs again (section "cache pack").
//
// Stored inputs: every translation also writes <cache>/in/<input id>.in (magic "TKSI"; stage, the full answers, the
// control section, the code up to its length, c1), so a pipeline key record (vk_pipeline.c), which names its shaders by
// input id, can be translated again under a new translator without guest memory (gfx_shader_load_input,
// gfx_shader_prewarm).
//
// Reflection: every shader that enters the cache has its .res.json parsed into GfxShaderRes arrays and its SPIR-V
// scanned for interface Locations and the workgroup size (gfx.h accessors at the end of the file).
//
// Provenance: the c2 handle ranges of handle_slot are those of MacNativeGames' totk_xlate Service.cs (MIT).
// The code layout the translator sees (graphics: code[0x30..], compute: code[0..]; c1 = code[c1..length]
// with c1 and length at control+0x700) follows tools/shader_census.py, itself MacNativeGames' totk shader tooling (MIT).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

#define CONTROL_SIZE 0x880
#define CODE_SKIP 0x30                                     // graphics code starts after the first 0x30 bytes of the program
#define BUCKETS 4096
#define LINE_MAX_LEN 4096
#define KEY_VERSION 4                                      // in every cache key; bump when the key layout or the use of the
                                                           // output changes here (4: translator identity, input ids);
                                                           // translator changes are covered by its identity
#define IN_VERSION 1                                       // in every input id and .in file: bump when the answers layout
                                                           // or the program hash changes (drops every stored input)
#define USE_MAGIC 0x4d554b54u                              // "TKUM", .use files
#define USE_VERSION 1
#define IN_MAGIC 0x49534b54u                               // "TKSI", .in files
#define MAX_SVC 32                                         // translator processes

typedef struct {                                           // which answers a shader's translation depends on
    uint32_t slots;                                        // tex_type, img_type, img_format of these c2 slots
    uint32_t attrs;                                        // attr_type (VS input Locations)
    uint32_t fouts;                                        // frag_out_type (FS output Locations)
} UseMask;

typedef struct UseEnt {                                    // code hash -> use mask; known 0 caches a disk miss
    struct UseEnt *next;
    uint8_t code_hash[32];
    int known;
    UseMask m;
} UseEnt;

struct GfxShader {
    struct GfxShader *next;
    uint8_t key[32];
    uint8_t iid[32];                                       // input id (see the keys at the top)
    uint32_t *spirv;
    uint64_t words;
    char *reflection;                                      // the service's .res.json text, parsed into the fields below
    uint64_t reflection_size;
    // Reflection (reflect(), once per shader before it enters the cache)
    int stage;                                             // GFX_STAGE_*
    GfxShaderRes *res[GFX_RES_KINDS];
    int nres[GFX_RES_KINDS];
    uint32_t in_mask, out_mask;                            // user Locations of Input / Output variables
    GfxShaderIo in[32], out[32];
    uint32_t local[3];
};

static PlatMutex cache_lock = PLAT_MUTEX_INIT;                  // memory cache
static GfxShader *buckets[BUCKETS];
static UseEnt *use_buckets[BUCKETS];                       // under cache_lock
static volatile int64_t n_requests, n_hits, n_failures, n_keys, n_xlated;   // n_keys: memory cache entries

typedef struct {                                           // one translator child
    PlatChild *child;                                      // NULL: not started
    int busy;                                              // a request owns it (under S.lock)
    size_t in_len;
    char inbuf[LINE_MAX_LEN * 2];
} Svc;

static struct {
    PlatMutex lock;
    PlatCond freed;
    int limit;                                             // children in use at most (0: svc_runtime_limit)
    Svc s[MAX_SVC];
} S = {PLAT_MUTEX_INIT, PLAT_COND_INIT};

static struct {                                            // translations in flight: cache key, or code hash while the
    PlatMutex lock;                                          // use mask is unknown; one translation per id at a time
    PlatCond done;
    uint8_t id[2 * MAX_SVC][32];
    int n;
} F = {PLAT_MUTEX_INIT, PLAT_COND_INIT};

#define PATH_CAP 1024                                  // bytes of a UTF-8 path
static char cache_dir[PATH_CAP];
static char xlate_exe[PATH_CAP];
static uint8_t xlate_id[32];                               // translator identity (see the keys at the top)
static int host_f64 = 1, host_zero_locals, host_set;                         // gfx_shader_set_host
static uint32_t host_sbuf, host_sg[3] = {32, 32, 32}, host_gather;
static PlatOnce paths_once = PLAT_ONCE_INIT;

// ---- small helpers

#define file_exists plat_file_exists
#define mkdir_p plat_file_mkdir_p
#define read_file plat_file_read
#define write_file plat_file_write

// ---- cache pack (<cache>/shader-cache.pack, written by tools/pack_shader_cache.py)
// Little-endian: "TKSPACK1", u32 count, u32 0, then count entries sorted by (kind, name), then the data. name is the
// 32 bytes the loose file's 64 hex digits spell. The index is read once (48 bytes a file: ~5.6 MB after an install);
// the data is read per lookup with positioned reads on one handle, safe from every thread. A pack that fails a check is
// ignored as a whole and the loose files are used.
enum { PACK_SPV = 1, PACK_RES = 2, PACK_USE = 3, PACK_IN = 4 };
typedef struct { uint8_t name[32]; uint32_t kind, size; uint64_t offset; } PackEnt;
_Static_assert(sizeof(PackEnt) == 48, "shader-cache.pack entry layout");
static PlatFile *pack_file;
static PackEnt *pack_ix;
static uint32_t pack_n;

static int pack_cmp(const PackEnt *a, uint32_t kind, const uint8_t name[32]) {
    if (a->kind != kind) return a->kind < kind ? -1 : 1;
    return memcmp(a->name, name, 32);
}

static int pack_read_at(uint64_t offset, void *out, uint32_t n) {
    return plat_file_pread(pack_file, out, n, offset) == (int64_t)n;
}

// Opens <cache>/shader-cache.pack and reads its index (init_paths). Nothing happens when there is none.
static void pack_open(void) {
    char p[PATH_CAP + 32];
    snprintf(p, sizeof p, "%s/shader-cache.pack", cache_dir);
    PlatFile *h = plat_file_open(p);
    if (!h) return;
    PlatFileId id;
    uint8_t head[16];
    uint32_t n = 0;
    const char *why = NULL;
    PackEnt *ix = NULL;
    pack_file = h;
    if (!plat_file_id(h, &id) || !pack_read_at(0, head, sizeof head) || memcmp(head, "TKSPACK1", 8)) why = "header";
    if (!why) {
        memcpy(&n, head + 8, 4);
        if (n > (1u << 24) || (uint64_t)n * sizeof *ix + sizeof head > id.size ||
            !(ix = malloc((size_t)n * sizeof *ix + 1)))
            why = "index size";
        else if (!pack_read_at(sizeof head, ix, n * (uint32_t)sizeof *ix)) why = "index read";
    }
    for (uint32_t i = 0; !why && i < n; i++) {
        if (ix[i].offset + ix[i].size > id.size) why = "entry out of bounds";
        else if (i && pack_cmp(&ix[i - 1], ix[i].kind, ix[i].name) >= 0) why = "index order";
    }
    if (why) {
        rt_log("gfx_shader: %s is not a usable cache pack (%s); reading the loose files", p, why);
        free(ix);
        plat_file_close(h);
        pack_file = NULL;
        return;
    }
    pack_ix = ix, pack_n = n;
    rt_log("gfx_shader: cache pack %s: %u files", p, n);
}

static const PackEnt *pack_find(uint32_t kind, const uint8_t name[32]) {
    size_t lo = 0, hi = pack_n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = pack_cmp(&pack_ix[mid], kind, name);
        if (!c) return &pack_ix[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

// A packed file as plat_file_read returns a loose one: malloc'd, NUL after the last byte; NULL when absent or empty.
static uint8_t *pack_read(uint32_t kind, const uint8_t name[32], uint64_t *size) {
    const PackEnt *e = pack_find(kind, name);
    uint8_t *buf = e && e->size ? malloc((size_t)e->size + 1) : NULL;
    if (!buf) return NULL;
    if (!pack_read_at(e->offset, buf, e->size)) {
        free(buf);
        return NULL;
    }
    buf[e->size] = 0;
    *size = e->size;
    return buf;
}

// Appends a UTF-8 string as a JSON string body: ASCII as is, quotes and backslashes escaped, the rest as \uXXXX
// (UTF-16 units), so the service's stdin encoding does not matter.
static void json_esc(char **o, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++;
        if (c >= 0x80) {
            int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            c &= 0x3f >> more;
            while (more-- && (*p & 0xc0) == 0x80) c = c << 6 | (*p++ & 0x3f);
        }
        if (c == '"' || c == '\\') *(*o)++ = '\\', *(*o)++ = (char)c;
        else if (c >= 0x20 && c < 0x7f) *(*o)++ = (char)c;
        else if (c >= 0x10000) {
            c -= 0x10000;
            *o += sprintf(*o, "\\u%04x\\u%04x", 0xd800 + (c >> 10), 0xdc00 + (c & 0x3ff));
        } else *o += sprintf(*o, "\\u%04x", c);
    }
}

// ---- paths and the translator identity

static int hex32(const char *h, uint8_t out[32]) {        // 64 hex digits, then nothing but white space
    for (int i = 0; i < 64; i++) {
        char c = h[i] >= 'A' && h[i] <= 'F' ? (char)(h[i] | 32) : h[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return 0;
        out[i / 2] = (uint8_t)(i & 1 ? out[i / 2] | v : v << 4);
    }
    for (const char *p = h + 64; *p; p++)
        if (*p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') return 0;
    return 1;
}

// PLUCKOMP_XLATE_ID (64 hex digits, or any other text, which is hashed), else <translator>.id next to the exe
// (tools/build_xlate.py), else the sha256 of the exe.
static void xlate_identity(void) {
    char text[256] = "";
    const char *how = "PLUCKOMP_XLATE_ID";
    const char *id_env = cfg_get("PLUCKOMP_XLATE_ID");
    if (id_env && *id_env && strlen(id_env) < sizeof text) {
        strcpy(text, id_env);
        if (!hex32(text, xlate_id)) rt_sha256(text, strlen(text), xlate_id);
    } else {
        char p[PATH_CAP + 8];
        strcpy(p, xlate_exe);
        char *dot = strrchr(p, '.'), *sep = plat_last_sep(p);
        if (dot && dot > sep) *dot = 0;
        strcat(p, ".id");
        uint64_t size = 0;
        char *f = (char *)read_file(p, &size);
        how = "pluckomp_xlate.id";
        if (!f || !hex32(f, xlate_id)) {
            uint8_t *exe = read_file(xlate_exe, &size);
            how = exe ? "sha256 of the exe; tools/build_xlate.py writes pluckomp_xlate.id" : "none: no translator";
            if (exe) rt_sha256(exe, (size_t)size, xlate_id);
            free(exe);
        }
        free(f);
    }
    rt_log("gfx_shader: translator identity %02x%02x%02x%02x%02x%02x%02x%02x (%s)", xlate_id[0], xlate_id[1], xlate_id[2],
           xlate_id[3], xlate_id[4], xlate_id[5], xlate_id[6], xlate_id[7], how);
}

// The repository root is the nearest parent of pluckomp.exe that holds build/xlate/pluckomp_xlate.exe. The shader cache lives
// next to pluckomp.exe, so every build dir has its own (as the pipeline cache, vk_pipeline.c cache_path_init).
static void use_preload_start(void);

static void init_paths(void *param) {
    (void)param;
    char exe[PATH_CAP], root[PATH_CAP] = "", exe_dir[PATH_CAP] = "";
    if (plat_exe_path(exe, sizeof exe)) {
        char *last = plat_last_sep(exe);
        if (last) snprintf(exe_dir, sizeof exe_dir, "%.*s", (int)(last - exe), exe);
        for (char *cut = plat_last_sep(exe); cut; cut = plat_last_sep(exe)) {
            *cut = 0;
            char probe[PATH_CAP + 32];
            snprintf(probe, sizeof probe, "%s/build/xlate/pluckomp_xlate" PLAT_EXE_SUFFIX, exe);
            if (file_exists(probe)) {
                strcpy(root, exe);
                break;
            }
        }
    }
    const char *xlate_cfg = cfg_get("PLUCKOMP_XLATE"), *cache_cfg = cfg_get("PLUCKOMP_SHADER_CACHE");
    char beside[PATH_CAP + 32];   // a packaged folder (tools/package_build.py) keeps the translator next to pluckomp.exe
    if (exe_dir[0]) snprintf(beside, sizeof beside, "%s/pluckomp_xlate" PLAT_EXE_SUFFIX, exe_dir);
    if (xlate_cfg && *xlate_cfg && strlen(xlate_cfg) < sizeof xlate_exe) strcpy(xlate_exe, xlate_cfg);
    else if (exe_dir[0] && file_exists(beside)) strcpy(xlate_exe, beside);
    else if (root[0]) snprintf(xlate_exe, sizeof xlate_exe, "%s/build/xlate/pluckomp_xlate" PLAT_EXE_SUFFIX, root);
    if (cache_cfg && *cache_cfg && strlen(cache_cfg) < sizeof cache_dir) strcpy(cache_dir, cache_cfg);
    else if (exe_dir[0]) snprintf(cache_dir, sizeof cache_dir, "%s/shader-cache", exe_dir);
    xlate_identity();
    if (cache_dir[0]) pack_open();
    use_preload_start();
    return;
}

static void paths(void) { plat_once(&paths_once, init_paths, NULL); }

// Host answers (vk_caps.c, at device creation, before any translation). Answers other than the defaults (Float64,
// exact storage offsets, 32 lanes, no gather bias, undefined locals) are mixed into the translator identity, so they
// get their own cache keys.
void gfx_shader_set_host(int float64, uint32_t sbuf_align, const uint32_t subgroup[3], uint32_t gather_bits, int zero_locals) {
    paths();
    if (host_set++) return;
    host_f64 = float64, host_sbuf = sbuf_align, host_gather = gather_bits, host_zero_locals = zero_locals;
    memcpy(host_sg, subgroup, sizeof host_sg);
    if (float64 && !sbuf_align && subgroup[0] == 32 && subgroup[1] == 32 && subgroup[2] == 32 && !gather_bits && !zero_locals) return;
    uint8_t buf[32 + 96];
    int n = snprintf((char *)buf + 32, 96, "TKHO f64=%d sbuf=%u sg=%u,%u,%u", float64, sbuf_align, subgroup[0], subgroup[1],
                     subgroup[2]);
    if (gather_bits) n += snprintf((char *)buf + 32 + n, (size_t)(96 - n), " gather=%u", gather_bits);
    if (zero_locals) n += snprintf((char *)buf + 32 + n, (size_t)(96 - n), " zero_locals");
    memcpy(buf, xlate_id, 32);
    rt_sha256(buf, 32 + (size_t)n, xlate_id);
    rt_log("gfx_shader: host options %s: own shader cache keys", (char *)buf + 37);
}

// ---- the translator children

static void child_stop(Svc *v) {
    plat_child_stop(v->child);
    v->child = NULL;
    v->in_len = 0;
}

static int child_start(Svc *v) {
    paths();
    if (!xlate_exe[0] || !file_exists(xlate_exe)) {
        rt_log("gfx_shader: translator not found (%s); set PLUCKOMP_XLATE", xlate_exe[0] ? xlate_exe : "<none>");
        return 0;
    }
    uint32_t err = 0;
    v->child = plat_child_start(xlate_exe, "--serve", &err);
    if (!v->child) {
        rt_log("gfx_shader: cannot start %s (error %lu)", xlate_exe, (unsigned long)err);
        return 0;
    }
    v->in_len = 0;
    return 1;
}

// One line out, one line in (no newline in the reply). 0 when the pipe broke.
static int child_exchange_raw(Svc *v, const char *line, char *reply, size_t reply_size) {
    if (!v->child && !child_start(v)) return 0;
    plat_inc64(&n_requests);
    size_t n = strlen(line);
    if (!plat_child_write(v->child, line, n)) return 0;
    if (!plat_child_write(v->child, "\n", 1)) return 0;
    for (;;) {
        char *nl = memchr(v->inbuf, '\n', v->in_len);
        if (nl) {
            size_t len = (size_t)(nl - v->inbuf), keep = len < reply_size - 1 ? len : reply_size - 1;
            memcpy(reply, v->inbuf, keep);
            while (keep && reply[keep - 1] == '\r') keep--;
            reply[keep] = 0;
            memmove(v->inbuf, nl + 1, v->in_len - len - 1);
            v->in_len -= len + 1;
            return 1;
        }
        if (v->in_len == sizeof v->inbuf) v->in_len = 0;  // an overlong line: drop it, wait for its end
        long got = plat_child_read(v->child, v->inbuf + v->in_len, sizeof v->inbuf - v->in_len);
        if (got <= 0) return 0;
        v->in_len += (size_t)got;
    }
}

static int child_exchange(Svc *v, const char *line, char *reply, size_t reply_size) {
    uint64_t p0 = perf_begin();   // PLUCKOMP_TRACE=perf: render-thread time spent translating
    int ok = child_exchange_raw(v, line, reply, reply_size);
    perf_end(PERF_XLATE, p0);
    return ok;
}

// Children allowed at run time: PLUCKOMP_XLATE_PROCS, default 2.
static int svc_runtime_limit(void) {
    static volatile int32_t n;
    if (!n) {
        const char *e = cfg_get("PLUCKOMP_XLATE_PROCS");
        int v = e && *e ? atoi(e) : 2;
        plat_xchg32(&n, v < 1 ? 1 : v > MAX_SVC ? MAX_SVC : v);
    }
    return (int)n;
}

static int svc_used(void) {                                // caller holds S.lock
    int used = 0;
    for (int i = 0; i < MAX_SVC; i++) used += S.s[i].busy || S.s[i].child;
    return used;
}

// A child for one request: an idle running one, else a new slot while fewer than S.limit are in use, else waits.
static Svc *svc_get(void) {
    plat_mutex_lock(&S.lock);
    if (!S.limit) S.limit = svc_runtime_limit();
    Svc *v;
    for (;;) {
        Svc *empty = NULL;
        v = NULL;
        for (int i = 0; i < MAX_SVC && !v; i++) {
            Svc *c = &S.s[i];
            if (!c->busy && c->child) v = c;
            else if (!c->busy && !empty) empty = c;
        }
        if (!v && empty && svc_used() < S.limit) v = empty;
        if (v) break;
        plat_cond_wait(&S.freed, &S.lock);
    }
    v->busy = 1;
    plat_mutex_unlock(&S.lock);
    return v;
}

// Returns a child; one over the limit (svc_set_limit lowered it) is stopped.
static void svc_put(Svc *v) {
    plat_mutex_lock(&S.lock);
    int stop = v->child && svc_used() > S.limit;
    if (!stop) v->busy = 0;
    plat_mutex_unlock(&S.lock);
    if (stop) {
        child_stop(v);
        plat_mutex_lock(&S.lock);
        v->busy = 0;
        plat_mutex_unlock(&S.lock);
    }
    plat_cond_broadcast(&S.freed);
}

// Sets the number of children; idle ones over it are stopped now, busy ones when they are returned.
static void svc_set_limit(int n) {
    Svc *stop[MAX_SVC];
    int k = 0;
    plat_mutex_lock(&S.lock);
    S.limit = n < 1 ? 1 : n > MAX_SVC ? MAX_SVC : n;
    for (int i = 0, used = svc_used(); i < MAX_SVC && used > S.limit; i++)
        if (!S.s[i].busy && S.s[i].child) S.s[i].busy = 1, stop[k++] = &S.s[i], used--;
    plat_mutex_unlock(&S.lock);
    for (int i = 0; i < k; i++) child_stop(stop[i]);
    plat_mutex_lock(&S.lock);
    for (int i = 0; i < k; i++) stop[i]->busy = 0;
    plat_mutex_unlock(&S.lock);
    plat_cond_broadcast(&S.freed);
}

// One request on a child; a dead child is restarted once. 0 when the translator is unavailable. The caller has given
// up its virtual core (kern_core_away): the wait for a child and the translation are host time.
static int svc_request(const char *line, char *reply, size_t reply_size) {
    Svc *v = svc_get();
    int ok = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++)
        if (!(ok = child_exchange(v, line, reply, reply_size))) child_stop(v);
    svc_put(v);
    return ok;
}

// One translation per id at a time (F); F.id full also waits.
static void flight_enter(const uint8_t id[32]) {
    plat_mutex_lock(&F.lock);
    for (;;) {
        int busy = F.n == 2 * MAX_SVC;
        for (int i = 0; i < F.n && !busy; i++) busy = !memcmp(F.id[i], id, 32);
        if (!busy) break;
        plat_cond_wait(&F.done, &F.lock);
    }
    memcpy(F.id[F.n++], id, 32);
    plat_mutex_unlock(&F.lock);
}

static void flight_leave(const uint8_t id[32]) {
    plat_mutex_lock(&F.lock);
    for (int i = 0; i < F.n; i++)
        if (!memcmp(F.id[i], id, 32)) {
            memcpy(F.id[i], F.id[--F.n], 32);
            break;
        }
    plat_mutex_unlock(&F.lock);
    plat_cond_broadcast(&F.done);
}

// ---- cache

static GfxShader *mem_find(const uint8_t key[32]) {
    plat_mutex_lock_shared(&cache_lock);
    GfxShader *s = buckets[(key[0] | key[1] << 8) % BUCKETS];
    while (s && memcmp(s->key, key, 32)) s = s->next;
    plat_mutex_unlock_shared(&cache_lock);
    return s;
}

static void shader_free(GfxShader *s) {
    for (int k = 0; k < GFX_RES_KINDS; k++) free(s->res[k]);
    free(s->spirv), free(s->reflection), free(s);
}

static void reflect(GfxShader *s);

// Inserts s, or frees it and returns the entry another thread inserted first.
static GfxShader *mem_insert(GfxShader *s) {
    plat_mutex_lock(&cache_lock);
    GfxShader **head = &buckets[(s->key[0] | s->key[1] << 8) % BUCKETS], *r = *head;
    while (r && memcmp(r->key, s->key, 32)) r = r->next;
    if (r) {
        shader_free(s);
        s = r;
    } else {
        s->next = *head;
        *head = s;
        n_keys++;
    }
    plat_mutex_unlock(&cache_lock);
    return s;
}

static void key_hex(const uint8_t key[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[i * 2] = hex[key[i] >> 4], out[i * 2 + 1] = hex[key[i] & 15];
    out[64] = 0;
}

// A .res.json the translator wrote: one JSON object that names its stage, no NUL inside. A file an interrupted write
// left zero-filled (3647 of 21924 in one cache were) fails this; reflect() would take it for a vertex shader with
// no resources, and its pipeline layout would lack the shader's buffers and textures.
static int reflection_valid(const char *res, uint64_t size) {
    if (strlen(res) != size) return 0;
    const char *b = res, *e = res + size;
    while (b < e && isspace((unsigned char)*b)) b++;
    while (e > b && isspace((unsigned char)e[-1])) e--;
    if (e - b < 2 || *b != '{' || e[-1] != '}') return 0;
    const char *st = strstr(b, "\"stage\"");
    if (!st) return 0;
    st = strchr(st + 7, '"');
    return st && (!strncmp(st, "\"vs\"", 4) || !strncmp(st, "\"ps\"", 4) || !strncmp(st, "\"cs\"", 4));
}

// Every instruction after the header has a word count and the last one ends at the module's end: a module with
// zero-filled clusters fails.
static int spirv_complete(const uint32_t *w, uint64_t n) {
    uint64_t i = 5;
    while (i < n) {
        uint32_t len = w[i] >> 16;
        if (!len) return 0;
        i += len;
    }
    return i == n;
}

// Reads <key>.spv and <key>.res.json from the pack (packed 1) or the loose files; NULL if either is missing or
// malformed. iid: the input id, NULL when the caller sets it.
static GfxShader *disk_load_from(const uint8_t key[32], const uint8_t *iid, int packed) {
    static volatile int32_t n_bad;
    char hex[65], p[PATH_CAP + 80];
    key_hex(key, hex);
    uint64_t spv_size = 0, res_size = 0;
    snprintf(p, PATH_CAP + 80, "%s/%s.res.json", cache_dir, hex);
    uint8_t *res = packed ? pack_read(PACK_RES, key, &res_size) : read_file(p, &res_size);
    if (!res) return NULL;
    if (!reflection_valid((const char *)res, res_size)) {
        int32_t n = plat_inc32(&n_bad);
        if (n <= 3 || !(n & (n - 1))) rt_log("gfx_shader: %s.res.json is malformed, translating again (%d so far)", hex, n);
        free(res);
        return NULL;
    }
    snprintf(p, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
    uint8_t *spv = packed ? pack_read(PACK_SPV, key, &spv_size) : read_file(p, &spv_size);
    uint32_t magic = 0;
    if (spv && spv_size >= 20) memcpy(&magic, spv, 4);
    if (!spv || spv_size % 4 || magic != 0x07230203 || !spirv_complete((const uint32_t *)spv, spv_size / 4)) {
        free(res), free(spv);
        return NULL;
    }
    GfxShader *s = calloc(1, sizeof *s);
    memcpy(s->key, key, 32);
    if (iid) memcpy(s->iid, iid, 32);
    s->spirv = (uint32_t *)spv;
    s->words = spv_size / 4;
    s->reflection = (char *)res;
    s->reflection_size = res_size;
    reflect(s);
    return s;
}

const GfxShader *gfx_shader_own(const uint32_t *spirv, uint64_t words, const char *res_json) {
    if (words < 5 || spirv[0] != 0x07230203 || !spirv_complete(spirv, words) ||
        !reflection_valid(res_json, strlen(res_json)))
        return NULL;
    GfxShader *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    size_t rn = strlen(res_json);
    uint8_t *buf = malloc(words * 4 + rn);
    s->spirv = malloc(words * 4), s->reflection = malloc(rn + 1);
    if (!buf || !s->spirv || !s->reflection) rt_fatal("gfx_shader: out of memory");
    memcpy(buf, spirv, words * 4), memcpy(buf + words * 4, res_json, rn);
    rt_sha256(buf, words * 4 + rn, s->key);
    free(buf);
    memcpy(s->spirv, spirv, words * 4), memcpy(s->reflection, res_json, rn + 1);
    s->words = words, s->reflection_size = rn;
    reflect(s);
    return s;
}

// The pack, then the loose files (a newer translation of a key the pack holds a bad copy of); NULL when neither has a
// valid pair (the caller translates again and writes loose files).
static GfxShader *disk_load(const uint8_t key[32], const uint8_t *iid) {
    GfxShader *s = pack_n ? disk_load_from(key, iid, 1) : NULL;
    return s ? s : disk_load_from(key, iid, 0);
}

// ---- request building

static const char *const tex_names[] = {NULL, "1d", "1darray", "buffer", "2d", "2darray", "2dms", "2dmsarray", "3d",
                                        "cube", "cubearray"};   // GfxShaderAnswers tex_type / img_type codes
static const char *const fmt_names[] = {   // img_format codes: Ryujinx TextureFormat enum order
    NULL, "R8Unorm", "R8Snorm", "R8Uint", "R8Sint", "R16Float", "R16Unorm", "R16Snorm", "R16Uint", "R16Sint", "R32Float",
    "R32Uint", "R32Sint", "R8G8Unorm", "R8G8Snorm", "R8G8Uint", "R8G8Sint", "R16G16Float", "R16G16Unorm", "R16G16Snorm",
    "R16G16Uint", "R16G16Sint", "R32G32Float", "R32G32Uint", "R32G32Sint", "R8G8B8A8Unorm", "R8G8B8A8Snorm",
    "R8G8B8A8Uint", "R8G8B8A8Sint", "R16G16B16A16Float", "R16G16B16A16Unorm", "R16G16B16A16Snorm", "R16G16B16A16Uint",
    "R16G16B16A16Sint", "R32G32B32A32Float", "R32G32B32A32Uint", "R32G32B32A32Sint", "R10G10B10A2Unorm",
    "R10G10B10A2Uint", "R11G11B10Float"};
#define COUNT(a) ((int)(sizeof a / sizeof *a))

static const char *attr_name(uint8_t t) { return t == 1 ? "sint" : t == 2 ? "uint" : "float"; }

// ---- reflection: the .res.json the translator writes (tools/xlate/Program.cs Translate) and the SPIR-V itself

// `"name":` inside [o, e): the first byte after the colon and its spaces, NULL when absent.
static const char *json_key(const char *o, const char *e, const char *name) {
    char pat[40];
    int n = snprintf(pat, sizeof pat, "\"%s\":", name);
    for (const char *p = o; p + n <= e; p++)
        if (*p == '"' && !memcmp(p, pat, (size_t)n)) {
            p += n;
            while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
            return p;
        }
    return NULL;
}

static long json_int(const char *o, const char *e, const char *name, long def) {
    const char *p = json_key(o, e, name);
    return p ? strtol(p, NULL, 10) : def;
}

// Copies a string value into buf (empty when absent or not a string).
static void json_str(const char *o, const char *e, const char *name, char *buf, size_t n) {
    const char *p = json_key(o, e, name);
    size_t k = 0;
    if (p && *p == '"')
        for (p++; p < e && *p != '"' && k + 1 < n; p++) buf[k++] = *p;
    buf[k] = 0;
}

// The objects of the flat array `"name": [ {...}, ... ]`: calls fn for each object's [start, end). Returns the count.
static int json_objects(const char *js, const char *name, void (*fn)(GfxShader *, int, const char *, const char *),
                        GfxShader *s, int kind) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    if (!p || *p != '[') return 0;
    int n = 0;
    for (p++; p < end && *p != ']'; p++) {
        if (*p != '{') continue;
        const char *q = memchr(p, '}', (size_t)(end - p));
        if (!q) break;
        if (fn) fn(s, kind, p, q);
        n++;
        p = q;
    }
    return n;
}

static int fmt_code(const char *name) {
    for (int i = 1; i < COUNT(fmt_names); i++)
        if (!strcmp(fmt_names[i], name)) return i;
    return 0;
}

// c2 slot of a texture or image handle (Service.cs of MNG's pluckomp_xlate: combined handles at 0x20, 32 slots; images at
// 0x120; separate textures at 0x168), -1 outside those ranges or for a handle in another constant buffer.
static int handle_slot(int kind, int cbuf_slot, int handle, int separate) {
    if (cbuf_slot >= 0 && cbuf_slot != 2) return -1;
    int bytes = (handle & 0x3fff) * 4;
    if (kind == GFX_RES_IMG) return bytes >= 0x120 && bytes < 0x168 ? (bytes - 0x120) / 8 : -1;
    if (separate) return bytes >= 0x168 && bytes < 0x568 ? (bytes - 0x168) / 8 : -1;
    return bytes >= 0x20 && bytes < 0x120 ? (bytes - 0x20) / 8 : -1;
}

static void add_res(GfxShader *s, int kind, const char *o, const char *e) {
    GfxShaderRes *r = &s->res[kind][s->nres[kind]++];
    r->set = 0;
    r->binding = (uint16_t)json_int(o, e, "binding", 0);
    if (kind == GFX_RES_UBO || kind == GFX_RES_SSBO) {
        r->slot = (int16_t)json_int(o, e, "slot", -1);
        r->count = 1;
        if (kind == GFX_RES_SSBO) {
            r->cbuf_slot = (int8_t)json_int(o, e, "cb_slot", -1);
            r->handle = (uint16_t)json_int(o, e, "cb_offset", 0);
            r->flags = (uint8_t)json_int(o, e, "flags", 0);
        }
        return;
    }
    char buf[32];
    json_str(o, e, "format", buf, sizeof buf);
    const char *sep = json_key(o, e, "separate");
    r->type = (uint16_t)json_int(o, e, "type", 0);
    r->format = (uint8_t)fmt_code(buf);
    r->flags = (uint8_t)json_int(o, e, "flags", 0);
    r->cbuf_slot = (int8_t)json_int(o, e, "cbuf_slot", -1);
    int handle = (int)json_int(o, e, "handle", 0);
    r->handle = (uint16_t)handle;
    r->count = (uint16_t)json_int(o, e, "array", 1);
    r->separate = sep && *sep == 't';
    r->slot = (int16_t)handle_slot(kind, r->cbuf_slot, handle, r->separate);
}

// SPIR-V opcodes and enumerants used below (SPIR-V 1.6 specification, section 3).
enum { OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
       OP_TYPE_ARRAY = 28, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71,
       DEC_BUILTIN = 11, DEC_LOCATION = 30, DEC_COMPONENT = 31, SC_INPUT = 1, SC_OUTPUT = 3, EM_LOCAL_SIZE = 17 };

// Interface Locations and the compute workgroup size from the module. Ids beyond 2^20 are ignored (Ryujinx modules stay
// far below).
static void reflect_spirv(GfxShader *s) {
    const uint32_t *w = s->spirv;
    uint64_t n = s->words;
    s->local[0] = s->local[1] = s->local[2] = 1;
    if (n < 5) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    // per id: location + 1 (0 = none), component flag, scalar/vector description of types, pointee of pointers
    int32_t *loc = calloc(bound, sizeof *loc);
    uint8_t *packed = calloc(bound, 1), *ttype = calloc(bound, 1), *tcomps = calloc(bound, 1);
    uint32_t *pointee = calloc(bound, sizeof *pointee);
    if (!loc || !packed || !ttype || !tcomps || !pointee) goto out;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *a = w + i + 1;
        uint32_t id = len > 1 ? a[0] : 0;
        if (op == OP_EXECUTION_MODE && len >= 6 && a[1] == EM_LOCAL_SIZE) {
            s->local[0] = a[2], s->local[1] = a[3], s->local[2] = a[4];
        } else if (op == OP_DECORATE && len >= 4 && id < bound) {
            if (a[1] == DEC_LOCATION) loc[id] = (int32_t)a[2] + 1;
            else if (a[1] == DEC_COMPONENT) packed[id] = 1;
        } else if (op == OP_TYPE_INT && len >= 4 && id < bound) {
            ttype[id] = a[2] ? GFX_TYPE_SINT : GFX_TYPE_UINT, tcomps[id] = 1;
        } else if (op == OP_TYPE_FLOAT && len >= 3 && id < bound) {
            ttype[id] = GFX_TYPE_FLOAT, tcomps[id] = 1;
        } else if (op == OP_TYPE_VECTOR && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = (uint8_t)a[2];
        } else if (op == OP_TYPE_ARRAY && len >= 4 && id < bound && a[1] < bound) {
            ttype[id] = ttype[a[1]], tcomps[id] = tcomps[a[1]];   // element type (arrayed interface blocks)
        } else if (op == OP_TYPE_POINTER && len >= 4 && id < bound) {
            pointee[id] = a[2];
        } else if (op == OP_VARIABLE && len >= 4) {
            uint32_t type = a[0], var = a[1], sc = a[2];
            if ((sc == SC_INPUT || sc == SC_OUTPUT) && var < bound && type < bound && loc[var] > 0 && loc[var] <= 32) {
                int l = loc[var] - 1;
                uint32_t t = pointee[type] < bound ? pointee[type] : 0;
                GfxShaderIo io = {ttype[t], tcomps[t] ? tcomps[t] : 4, packed[var]};
                if (sc == SC_INPUT) s->in_mask |= 1u << l, s->in[l] = io;
                else s->out_mask |= 1u << l, s->out[l] = io;
            }
        }
        i += len;
    }
out:
    free(loc), free(packed), free(ttype), free(tcomps), free(pointee);
}

// Every gradient sample (OpImageSampleExplicitLod with Grad) whose two gradients are each built from two components gets
// them transposed, dx = (a, c) and dy = (b, d) from dx = (a, b) and dy = (c, d), by two new OpCompositeConstruct before
// it. TotK's shaders fill a TXD's gradient registers per coordinate (du/dx, du/dy, dv/dx, dv/dy), and the translator
// (Ryujinx, InstEmitTexture EmitTxd) builds dx = (first two registers), dy = (last two) = (du/dx, du/dy) and
// (dv/dx, dv/dy): a mip level chosen from the wrong derivatives, too sharp on terrain faces turned across the screen,
// which crawled in patches that followed the faces (1.4.3, the Depths). PLUCKOMP_TXD_TRANSPOSE=0 turns it off.
// Applied to the SPIR-V after it is loaded, so the shader cache is untouched.
enum { OP_COMPOSITE_CONSTRUCT = 80, OP_SAMPLE_EXPLICIT = 88, IO_GRAD = 4 };

static void patch_txd_transpose(GfxShader *s) {
    static int on = -1;
    static volatile int32_t n_logged;
    if (on < 0) {
        const char *e = cfg_get("PLUCKOMP_TXD_TRANSPOSE");
        on = !(e && *e == '0');
    }
    uint32_t *w = s->spirv;
    uint64_t n = s->words;
    if (!on || n < 5) return;
    uint32_t bound = w[3] < (1u << 20) ? w[3] : (1u << 20);
    uint64_t *cc = calloc(bound, sizeof *cc);   // word index of a 2-constituent OpCompositeConstruct defining the id
    int found = 0;
    if (!cc) return;
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) goto out;
        if (op == OP_COMPOSITE_CONSTRUCT && len == 5 && w[i + 2] < bound) cc[w[i + 2]] = i;
        if (op == OP_SAMPLE_EXPLICIT && len >= 8 && w[i + 5] == IO_GRAD && w[i + 6] < bound && w[i + 7] < bound &&
            cc[w[i + 6]] && cc[w[i + 7]] && w[cc[w[i + 6]] + 1] == w[cc[w[i + 7]] + 1])
            found++;
        i += len;
    }
    if (!found) goto out;
    uint32_t *o = malloc((n + (uint64_t)found * 10) * sizeof *o);
    if (!o) goto out;
    uint64_t k = 0;
    uint32_t next = w[3];
    for (uint64_t i = 0; i < 5; i++) o[k++] = w[i];
    for (uint64_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (op == OP_SAMPLE_EXPLICIT && len >= 8 && w[i + 5] == IO_GRAD && w[i + 6] < bound && w[i + 7] < bound &&
            cc[w[i + 6]] && cc[w[i + 7]] && w[cc[w[i + 6]] + 1] == w[cc[w[i + 7]] + 1]) {
            const uint32_t *dx = w + cc[w[i + 6]], *dy = w + cc[w[i + 7]];   // [hdr, type, id, a, b], [hdr, type, id, c, d]
            uint32_t ndx = next++, ndy = next++;
            o[k++] = 5u << 16 | OP_COMPOSITE_CONSTRUCT, o[k++] = dx[1], o[k++] = ndx, o[k++] = dx[3], o[k++] = dy[3];
            o[k++] = 5u << 16 | OP_COMPOSITE_CONSTRUCT, o[k++] = dx[1], o[k++] = ndy, o[k++] = dx[4], o[k++] = dy[4];
            for (uint32_t j = 0; j < len; j++) o[k++] = w[i + j];
            o[k - len + 6] = ndx, o[k - len + 7] = ndy;
        } else {
            for (uint32_t j = 0; j < len; j++) o[k++] = w[i + j];
        }
        i += len;
    }
    o[3] = next;
    free(s->spirv);
    s->spirv = o, s->words = k;
    if (plat_inc32(&n_logged) <= 4) rt_log("gfx_shader: %d gradient samples now read their derivatives per coordinate", found);
out:
    free(cc);
}

static void reflect(GfxShader *s) {
    static const char *const kinds[GFX_RES_KINDS] = {"cbufs", "sbufs", "textures", "images"};
    const char *js = s->reflection;
    char stage[8];
    json_str(js, js + strlen(js), "stage", stage, sizeof stage);
    s->stage = !strcmp(stage, "cs") ? GFX_STAGE_CS : !strcmp(stage, "ps") ? GFX_STAGE_FS : GFX_STAGE_VS;
    for (int k = 0; k < GFX_RES_KINDS; k++) {
        int count = json_objects(js, kinds[k], NULL, s, k);
        s->res[k] = calloc(count ? count : 1, sizeof *s->res[k]);
        json_objects(js, kinds[k], add_res, s, k);
    }
    reflect_spirv(s);
    patch_txd_transpose(s);
}

// ---- use masks (see the cache key at the top)

// The slots of the int array `"name": [...]` below 32 as a mask.
static uint32_t json_slot_mask(const char *js, const char *name) {
    const char *end = js + strlen(js), *p = json_key(js, end, name);
    uint32_t m = 0;
    if (!p || *p != '[') return 0;
    for (p++; p < end && *p != ']';) {
        char *q;
        long v = strtol(p, &q, 10);
        if (q == p) {
            p++;
            continue;
        }
        if (v >= 0 && v < 32) m |= 1u << v;
        p = q;
    }
    return m;
}

static UseMask use_of(const GfxShader *s) {
    UseMask m = {json_slot_mask(s->reflection, "tex_queried") | json_slot_mask(s->reflection, "img_queried"), 0, 0};
    if (s->stage == GFX_STAGE_VS) m.attrs = s->in_mask;
    else if (s->stage == GFX_STAGE_FS) m.fouts = s->out_mask & 0xff;
    return m;
}

// <sha256("TKSU", translator identity, code hash)>.use: masks are per translator.
static void use_name(const uint8_t h[32], uint8_t name[32]) {
    uint8_t buf[4 + 32 + 32];
    memcpy(buf, "TKSU", 4), memcpy(buf + 4, xlate_id, 32), memcpy(buf + 36, h, 32);
    rt_sha256(buf, sizeof buf, name);
}

static void use_path_of(const uint8_t name[32], char *p) {
    char hex[65];
    key_hex(name, hex);
    snprintf(p, PATH_CAP + 80, "%s/%s.use", cache_dir, hex);
}

static void use_path(const uint8_t h[32], char *p) {
    uint8_t name[32];
    use_name(h, name);
    use_path_of(name, p);
}

// Every .use file of the cache, read by a background thread when the paths are set (use_preload), by file name: a
// lookup on the render thread then opens no file. Opening one took 0.1 ms alone but 11-40 ms in play, behind the
// translator's and the pipeline cache's writes and flushes on the same volume, and each new shader of a draw opened one.
// The table is complete and read-only once use_files_ready is 1; masks found later are in use_buckets.
#define USE_FILE_BUCKETS 65536
typedef struct UseFile { struct UseFile *next; uint8_t name[32]; UseMask m; } UseFile;
static UseFile *use_files[USE_FILE_BUCKETS];
static volatile int32_t use_files_ready;

static UseFile **use_file_slot(const uint8_t name[32]) {
    UseFile **e = &use_files[(name[0] | name[1] << 8) % USE_FILE_BUCKETS];
    while (*e && memcmp((*e)->name, name, 32)) e = &(*e)->next;
    return e;
}

static int hex_byte(const char *c) {
    int v = 0;
    for (int i = 0; i < 2; i++) {
        int d = isdigit((unsigned char)c[i]) ? c[i] - '0' : c[i] >= 'a' && c[i] <= 'f' ? c[i] - 'a' + 10 : -1;
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}

typedef struct { int n, bad; } UsePreload;

// One <64 hex digits>.use file of the cache directory into the table.
static int use_preload_file(const char *name, void *ctx) {
    UsePreload *st = ctx;
    char p[PATH_CAP + 80];
    UseFile e = {0};
    int ok = strlen(name) == 68;
    for (int i = 0; ok && i < 32; i++) {
        int v = hex_byte(name + 2 * i);
        ok = v >= 0, e.name[i] = (uint8_t)v;
    }
    uint64_t len = 0;
    uint32_t *u = NULL;
    if (ok) {
        snprintf(p, sizeof p, "%s/%s", cache_dir, name);
        u = (uint32_t *)read_file(p, &len);
        ok = u && len == 20 && u[0] == USE_MAGIC && u[1] == USE_VERSION;
    }
    UseFile **slot = ok ? use_file_slot(e.name) : NULL, *x;
    if (slot && !*slot && (x = malloc(sizeof *x))) {
        e.m.slots = u[2], e.m.attrs = u[3], e.m.fouts = u[4];
        *x = e, *slot = x, st->n++;
    } else if (!ok) {
        st->bad++;
    }
    free(u);
    return 1;
}

static void use_preload(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    uint64_t t0 = plat_now_ns();
    int n = 0, bad = 0;
    for (uint32_t i = 0; i < pack_n; i++) {   // the packed masks; the loose files below add the masks found since
        uint32_t u[5];
        UseFile **slot, *x;
        if (pack_ix[i].kind != PACK_USE) continue;
        if (pack_ix[i].size != sizeof u || !pack_read_at(pack_ix[i].offset, u, sizeof u) || u[0] != USE_MAGIC ||
            u[1] != USE_VERSION) {
            bad++;
            continue;
        }
        if (!*(slot = use_file_slot(pack_ix[i].name)) && (x = malloc(sizeof *x))) {
            memcpy(x->name, pack_ix[i].name, 32);
            x->next = NULL, x->m.slots = u[2], x->m.attrs = u[3], x->m.fouts = u[4];
            *slot = x, n++;
        }
    }
    UsePreload st = {0};
    plat_file_list(cache_dir, ".use", use_preload_file, &st);
    n += st.n, bad += st.bad;
    plat_xchg32(&use_files_ready, 1);
    rt_log("gfx_shader: %d use masks read in %.0f ms on a background thread (%d unreadable)", n,
           (plat_now_ns() - t0) / 1e6, bad);
    return;
}

static void use_preload_start(void) {
    if (!cache_dir[0]) return;
    plat_thread_spawn(use_preload, NULL, "use preload", 0);
}

static UseEnt **use_slot(const uint8_t h[32]) {         // caller holds cache_lock
    UseEnt **e = &use_buckets[(h[0] | h[1] << 8) % BUCKETS];
    while (*e && memcmp((*e)->code_hash, h, 32)) e = &(*e)->next;
    return e;
}

// Sets or adds the entry for h. Caller holds cache_lock exclusively.
static void use_put(const uint8_t h[32], int known, const UseMask *m) {
    UseEnt **e = use_slot(h);
    if (!*e && !(*e = calloc(1, sizeof **e))) return;
    memcpy((*e)->code_hash, h, 32);
    if (known) (*e)->known = 1, (*e)->m = *m;
}

// 1 with *m when the use mask of h is known (memory, then <hash>.use; a disk miss is remembered).
static int use_find(const uint8_t h[32], UseMask *m) {
    plat_mutex_lock_shared(&cache_lock);
    UseEnt *e = *use_slot(h);
    int seen = e != NULL, known = e && e->known;
    if (known) *m = e->m;
    plat_mutex_unlock_shared(&cache_lock);
    if (seen) return known;
    uint8_t name[32];
    use_name(h, name);
    if (plat_cas32(&use_files_ready, 0, 0)) {   // complete: a name not in it has no file
        UseFile *u = *use_file_slot(name);
        if ((known = u != NULL)) *m = u->m;
    } else {
        char p[PATH_CAP + 80];
        use_path_of(name, p);
        uint64_t n = 0;
        uint32_t *f = (uint32_t *)pack_read(PACK_USE, name, &n);
        if (!f) f = (uint32_t *)read_file(p, &n);
        known = f && n == 20 && f[0] == USE_MAGIC && f[1] == USE_VERSION;
        if (known) m->slots = f[2], m->attrs = f[3], m->fouts = f[4];
        free(f);
    }
    plat_mutex_lock(&cache_lock);
    use_put(h, known, m);
    plat_mutex_unlock(&cache_lock);
    return known;
}

static void use_store(const uint8_t h[32], const UseMask *m) {
    char p[PATH_CAP + 80];
    use_path(h, p);
    uint32_t f[5] = {USE_MAGIC, USE_VERSION, m->slots, m->attrs, m->fouts};
    plat_file_write_atomic(p, f, sizeof f);
    plat_mutex_lock(&cache_lock);
    use_put(h, 1, m);
    plat_mutex_unlock(&cache_lock);
}

static void put_u32(uint8_t **p, uint32_t v) { memcpy(*p, &v, 4), *p += 4; }

// Fixed-layout answers for hashing; returns the byte count (at most 256).
static size_t pack_answers(uint8_t *o, const GfxShaderAnswers *a) {
    uint8_t *p = o;
    GfxShaderAnswers zero = {0};
    if (!a) a = &zero;
    memcpy(p, a->tex_type, 32), p += 32;
    memcpy(p, a->img_type, 32), p += 32;
    memcpy(p, a->img_format, 32), p += 32;
    memcpy(p, a->attr_type, 32), p += 32;
    memcpy(p, a->frag_out_type, 8), p += 8;
    for (int i = 0; i < 3; i++) *p++ = (uint8_t)a->local[i], *p++ = (uint8_t)(a->local[i] >> 8);
    put_u32(&p, a->shared_bytes);
    *p++ = a->depth_minus_w;
    return (size_t)(p - o);
}

static int build_code_hash(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                           const uint8_t *c1, uint32_t c1_len, uint8_t hash[32]) {
    size_t total = 4 + CONTROL_SIZE + 8 + (size_t)code_size + 4 + c1_len;
    uint8_t *buf = malloc(total), *p = buf;
    if (!buf) return 0;
    put_u32(&p, (uint32_t)stage);
    memcpy(p, control, CONTROL_SIZE), p += CONTROL_SIZE;
    memcpy(p, &code_size, 8), p += 8;
    memcpy(p, code, (size_t)code_size), p += code_size;
    put_u32(&p, c1_len);
    if (c1_len) memcpy(p, c1, c1_len);
    rt_sha256(buf, total, hash);
    free(buf);
    return 1;
}

// The answers with everything outside the mask zeroed; the translator ignores depth_minus_w (Program.cs).
static void mask_answers(const GfxShaderAnswers *a, const UseMask *m, GfxShaderAnswers *o) {
    memset(o, 0, sizeof *o);
    if (!a) return;
    for (int i = 0; i < 32; i++) {
        if (m->slots >> i & 1) o->tex_type[i] = a->tex_type[i], o->img_type[i] = a->img_type[i], o->img_format[i] = a->img_format[i];
        if (m->attrs >> i & 1) o->attr_type[i] = a->attr_type[i];
    }
    for (int i = 0; i < 8; i++)
        if (m->fouts >> i & 1) o->frag_out_type[i] = a->frag_out_type[i];
    memcpy(o->local, a->local, sizeof o->local);
    o->shared_bytes = a->shared_bytes;
}

// The cache key of an input id under the current translator.
static void key_of_input(const uint8_t iid[32], uint8_t key[32]) {
    uint8_t buf[8 + 32 + 32], *p = buf;
    memcpy(p, "TKSK", 4), p += 4;
    put_u32(&p, KEY_VERSION);
    memcpy(p, xlate_id, 32), p += 32;
    memcpy(p, iid, 32), p += 32;
    rt_sha256(buf, (size_t)(p - buf), key);
}

// The input id and cache key (m given), or the provisional key (m NULL: full answers; iid untouched).
static void build_key(const uint8_t code_hash[32], const GfxShaderAnswers *a, const UseMask *m, uint8_t key[32],
                      uint8_t iid[32]) {
    GfxShaderAnswers masked;
    uint8_t buf[8 + 32 + 32 + 256], *p = buf;
    if (m) {
        memcpy(p, "TKSI", 4), p += 4;
        put_u32(&p, IN_VERSION);
        memcpy(p, code_hash, 32), p += 32;
        mask_answers(a, m, &masked);
        p += pack_answers(p, &masked);
        rt_sha256(buf, (size_t)(p - buf), iid);
        key_of_input(iid, key);
        return;
    }
    memcpy(p, "TKSP", 4), p += 4;
    put_u32(&p, KEY_VERSION);
    memcpy(p, xlate_id, 32), p += 32;
    memcpy(p, code_hash, 32), p += 32;
    p += pack_answers(p, a);
    rt_sha256(buf, (size_t)(p - buf), key);
}

static size_t build_request(char *o0, const char *hex, const char *stage, const char *code_path, const uint8_t *c1,
                            uint32_t c1_len, const GfxShaderAnswers *a, const uint8_t *control) {
    char *o = o0;
    o += sprintf(o, "{\"job\":{\"key\":\"");
    json_esc(&o, hex);
    o += sprintf(o, "\",\"stage\":\"%s\",\"debug\":false,\"code\":\"", stage);
    json_esc(&o, code_path);
    o += sprintf(o, "\",\"c1\":\"");
    for (uint32_t i = 0; i < c1_len; i++) o += sprintf(o, "%02x", c1[i]);
    o += sprintf(o, "\"");
    if (a) {
        const char *sep = "";
        o += sprintf(o, ",\"tex\":{");
        for (int i = 0; i < 32; i++)
            if (a->tex_type[i] && a->tex_type[i] < COUNT(tex_names)) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, tex_names[a->tex_type[i]]), sep = ",";
        sep = "";
        o += sprintf(o, "},\"img\":{");
        for (int i = 0; i < 32; i++)
            if (a->img_type[i] && a->img_type[i] < COUNT(tex_names)) {
                const char *f = a->img_format[i] && a->img_format[i] < COUNT(fmt_names) ? fmt_names[a->img_format[i]] : "R8G8B8A8Unorm";
                o += sprintf(o, "%s\"%d\":{\"type\":\"%s\",\"fmt\":\"%s\"}", sep, i, tex_names[a->img_type[i]], f), sep = ",";
            }
        sep = "";
        o += sprintf(o, "},\"attr\":{");
        for (int i = 0; i < 32; i++)
            if (a->attr_type[i]) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, attr_name(a->attr_type[i])), sep = ",";
        sep = "";
        o += sprintf(o, "},\"fout\":{");
        for (int i = 0; i < 8; i++)
            if (a->frag_out_type[i]) o += sprintf(o, "%s\"%d\":\"%s\"", sep, i, attr_name(a->frag_out_type[i])), sep = ",";
        o += sprintf(o, "},\"shared\":%u", a->shared_bytes);
    }
    if (!strcmp(stage, "cs")) {
        uint32_t l[3] = {1, 1, 1};
        if (a && (a->local[0] | a->local[1] | a->local[2])) l[0] = a->local[0], l[1] = a->local[1], l[2] = a->local[2];
        else memcpy(l, control + 0x748, 12);
        o += sprintf(o, ",\"local\":[%u,%u,%u]", l[0], l[1], l[2]);
    }
    /* Control byte +0x718 of a fragment shader: early fragment tests (the NVN driver's early-Z-force, MacNativeGames
       ae8f4f4). Without it a shader that stores to memory is depth-tested late, and the G-buffer draws' EQUAL test
       after the depth prepass fails along contour lines. Already part of the code hash (the whole control section). */
    if (!strcmp(stage, "ps") && (control[0x718] & 1)) o += sprintf(o, ",\"early_fragment_tests\":true");
    int si = !strcmp(stage, "vs") ? 0 : !strcmp(stage, "cs") ? 2 : 1;   // gfx_shader_set_host answers
    if (!host_f64) o += sprintf(o, ",\"softfloat64\":true");
    if (host_sbuf) o += sprintf(o, ",\"sbuf_align\":%u", host_sbuf);
    if (host_sg[si] != 32) o += sprintf(o, ",\"subgroup\":%u", host_sg[si]);
    if (host_gather) o += sprintf(o, ",\"gather_bias\":%u", host_gather);
    if (host_zero_locals) o += sprintf(o, ",\"zero_locals\":true");
    o += sprintf(o, "},\"out\":\"");
    json_esc(&o, cache_dir);
    o += sprintf(o, "\"}");
    return (size_t)(o - o0);
}

// ---- public

// Raw line exchange for the probe: one line to the service, its reply in `reply`. Starts the service if needed.
int gfx_shader_raw_request(const char *line, char *reply, size_t reply_size) {
    int away = kern_core_away();
    Svc *v = svc_get();
    int ok = child_exchange(v, line, reply, reply_size);
    if (!ok) child_stop(v);
    svc_put(v);
    kern_core_back(&away);
    return ok;
}

uint64_t gfx_shader_failures(void) { return (uint64_t)n_failures; }
uint64_t gfx_shader_requests(void) { return (uint64_t)n_requests; }   // lines sent to the service
uint64_t gfx_shader_cache_hits(void) { return (uint64_t)n_hits; }     // gets answered from memory or disk

// Drops the memory cache (the probe uses it to prove the disk cache). Pointers handed out before are invalid.
void gfx_shader_drop_memory(void) {
    plat_mutex_lock(&cache_lock);
    for (int i = 0; i < BUCKETS; i++) {
        for (GfxShader *s = buckets[i], *n; s; s = n) {
            n = s->next;
            shader_free(s);
        }
        buckets[i] = NULL;
        n_keys = 0;
        for (UseEnt *e = use_buckets[i], *n; e; e = n) n = e->next, free(e);
        use_buckets[i] = NULL;
    }
    // the preloaded .use table misses the masks this run wrote, which only use_buckets held: back to the disk
    plat_xchg32(&use_files_ready, 0);
    for (int i = 0; i < USE_FILE_BUCKETS; i++) {
        for (UseFile *e = use_files[i], *n; e; e = n) n = e->next, free(e);
        use_files[i] = NULL;
    }
    plat_mutex_unlock(&cache_lock);
}

static const GfxShader *fail(const char *why, const char *hex) {
    plat_inc64(&n_failures);
    rt_log("gfx_shader: translation failed (%s): %s", hex ? hex : "-", why);
    return NULL;
}

// A validated request: the program's code length, its c1 constants and the cache key. 0 (failure counted) when invalid.
typedef struct {
    const char *stage_name;
    uint32_t length, c1_len;
    const uint8_t *c1;
    uint8_t code_hash[32];
    uint8_t key[32];                                       // cache key, or the provisional one with final 0
    uint8_t iid[32];                                       // input id with final 1
    int final;
} Req;

static int req_init(Req *r, int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size, const uint8_t *c1,
                    const GfxShaderAnswers *a) {
    r->stage_name = stage == GFX_STAGE_VS ? "vs" : stage == GFX_STAGE_FS ? "ps" : stage == GFX_STAGE_CS ? "cs" : NULL;
    if (!r->stage_name) return fail("stage is not translated", NULL), 0;
    uint32_t c1_off;
    memcpy(&c1_off, control + 0x700, 4), memcpy(&r->length, control + 0x704, 4);
    uint64_t skip = stage == GFX_STAGE_CS ? 0 : CODE_SKIP;
    if (!code || r->length > code_size || c1_off > r->length || skip >= r->length) return fail("bad control section", NULL), 0;
    r->c1_len = r->length - c1_off;
    r->c1 = c1 ? c1 : code + c1_off;
    if (!build_code_hash(stage, control, code, r->length, r->c1, r->c1_len, r->code_hash))
        return fail("out of memory", NULL), 0;
    paths();
    UseMask m;
    r->final = use_find(r->code_hash, &m);
    build_key(r->code_hash, a, r->final ? &m : NULL, r->key, r->iid);
    return 1;
}

static const GfxShader *translate(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                  const GfxShaderAnswers *a);

static const GfxShader *get_async_one(Req r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a, int *pending);

static const GfxShader *get_one(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                const GfxShaderAnswers *a) {
    GfxShader *s = mem_find(r->key);
    if (s) return plat_inc64(&n_hits), s;
    paths();
    if (!cache_dir[0]) return fail("no cache directory", NULL);
    if (r->final && (s = disk_load(r->key, r->iid))) return plat_inc64(&n_hits), mem_insert(s);
    return translate(r, stage, control, code, a);
}

const GfxShader *gfx_shader_get(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                const uint8_t *c1, const GfxShaderAnswers *a) {
    Req r;
    if (!req_init(&r, stage, control, code, code_size, c1, a)) return NULL;
    const GfxShader *s = get_one(&r, stage, control, code, a);
    return s;
}

// ---- stored inputs (<cache>/in/<input id>.in, see the top)

typedef struct {
    uint32_t magic, version, stage, length, c1_len, answers_size;
} InHeader;                                                // then GfxShaderAnswers, control, code[length], c1[c1_len]

typedef struct {
    uint8_t *buf;                                          // the file; the pointers below point into it
    int stage;
    uint32_t length, c1_len;
    const uint8_t *control, *code, *c1;
    GfxShaderAnswers a;
} Inputs;

static void in_path(const uint8_t iid[32], char *p) {
    char hex[65];
    key_hex(iid, hex);
    snprintf(p, PATH_CAP + 80, "%s/in/%s.in", cache_dir, hex);
}

// Stores the inputs of a translation unless they are stored already; the file appears whole (written, then renamed).
static void in_store(const uint8_t iid[32], int stage, const uint8_t *control, const uint8_t *code, const Req *r,
                     const GfxShaderAnswers *a) {
    char p[PATH_CAP + 80], tmp[PATH_CAP + 120];
    in_path(iid, p);
    if (pack_find(PACK_IN, iid) || file_exists(p)) return;
    GfxShaderAnswers zero = {0};
    InHeader h = {IN_MAGIC, IN_VERSION, (uint32_t)stage, r->length, r->c1_len, sizeof(GfxShaderAnswers)};
    size_t n = sizeof h + sizeof zero + CONTROL_SIZE + r->length + r->c1_len;
    uint8_t *buf = malloc(n), *o = buf;
    if (!buf) return;
    memcpy(o, &h, sizeof h), o += sizeof h;
    memcpy(o, a ? a : &zero, sizeof zero), o += sizeof zero;
    memcpy(o, control, CONTROL_SIZE), o += CONTROL_SIZE;
    memcpy(o, code, r->length), o += r->length;
    if (r->c1_len) memcpy(o, r->c1, r->c1_len);
    snprintf(tmp, PATH_CAP + 120, "%s/in", cache_dir);
    mkdir_p(tmp);
    plat_file_write_atomic(p, buf, n);
    free(buf);
}

// Reads stored inputs into *in (free in->buf); 0 when absent or malformed.
static int in_load(const uint8_t iid[32], Inputs *in) {
    char p[PATH_CAP + 80];
    in_path(iid, p);
    uint64_t n = 0;
    uint8_t *f = pack_read(PACK_IN, iid, &n);
    if (!f) f = read_file(p, &n);
    InHeader h;
    if (!f || n < sizeof h) return free(f), 0;
    memcpy(&h, f, sizeof h);
    if (h.magic != IN_MAGIC || h.version != IN_VERSION || h.answers_size != sizeof(GfxShaderAnswers) ||
        n != sizeof h + sizeof(GfxShaderAnswers) + CONTROL_SIZE + (uint64_t)h.length + h.c1_len)
        return free(f), 0;
    const uint8_t *o = f + sizeof h;
    memcpy(&in->a, o, sizeof in->a), o += sizeof in->a;
    in->control = o, o += CONTROL_SIZE;
    in->code = o, o += h.length;
    in->c1 = o;
    in->buf = f, in->stage = (int)h.stage, in->length = h.length, in->c1_len = h.c1_len;
    return 1;
}

// ---- translation

static const GfxShader *translate_one(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a) {
    const uint8_t *key = r->key;
    GfxShader *s;
    UseMask m;
    uint8_t mk[32], miid[32];
    // Another thread may have translated it while this one waited in flight_enter.
    const uint8_t *look = r->final ? key : NULL, *look_iid = r->iid;
    if (!r->final && use_find(r->code_hash, &m)) build_key(r->code_hash, a, &m, mk, miid), look = mk, look_iid = miid;
    if (look && (s = mem_find(look))) return plat_inc64(&n_hits), s;
    if (look && (s = disk_load(look, look_iid))) return plat_inc64(&n_hits), mem_insert(s);

    uint64_t skip = stage == GFX_STAGE_CS ? 0 : CODE_SKIP;
    char hex[65], code_path[PATH_CAP + 80], spv_path[PATH_CAP + 80], res_path[PATH_CAP + 80];
    key_hex(key, hex);
    mkdir_p(cache_dir);
    snprintf(code_path, PATH_CAP + 80, "%s/%s.code", cache_dir, hex);
    snprintf(spv_path, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
    snprintf(res_path, PATH_CAP + 80, "%s/%s.res.json", cache_dir, hex);
    if (!write_file(code_path, code + skip, (size_t)(r->length - skip))) return fail("cannot write the code file", hex);
    size_t cap = 2 * PATH_CAP * 6 + 2 * r->c1_len + 8192;
    char *req = malloc(cap), reply[LINE_MAX_LEN];
    if (!req) return plat_file_delete(code_path), fail("out of memory", hex);
    build_request(req, hex, r->stage_name, code_path, r->c1, r->c1_len, a, control);
    int ok = svc_request(req, reply, sizeof reply);
    free(req);
    plat_file_delete(code_path);
    if (!ok) return fail("translator unavailable", hex);
    if (strncmp(reply, "ok ", 3)) {
        plat_file_delete(spv_path), plat_file_delete(res_path);
        return fail(reply, hex);
    }
    plat_file_flush(spv_path), plat_file_flush(res_path);   // the translator wrote them; on disk before use or rename
    if (!(s = disk_load(key, r->iid))) return fail("translator output missing or malformed", hex);
    if (!r->final) {                                       // name the files after the cache key, remember the mask
        m = use_of(s);
        build_key(r->code_hash, a, &m, mk, miid);
        char mhex[65], to[PATH_CAP + 80];
        key_hex(mk, mhex);
        snprintf(to, PATH_CAP + 80, "%s/%s.spv", cache_dir, mhex);
        int moved = plat_file_replace(spv_path, to);
        snprintf(to, PATH_CAP + 80, "%s/%s.res.json", cache_dir, mhex);
        moved = moved && plat_file_replace(res_path, to);
        if (moved) {
            use_store(r->code_hash, &m);
        } else {                                           // keep the mask for this run only
            plat_file_delete(spv_path), plat_file_delete(res_path), rt_log("gfx_shader: cannot rename %s", hex);
            plat_mutex_lock(&cache_lock);
            use_put(r->code_hash, 1, &m);
            plat_mutex_unlock(&cache_lock);
        }
        memcpy(s->key, mk, 32), memcpy(s->iid, miid, 32);
    }
    in_store(s->iid, stage, control, code, r, a);
    plat_inc64(&n_xlated);
    return mem_insert(s);
}

// The service round trip for a request that is in neither cache. One translation per cache key (per code hash while
// the use mask is unknown) runs at a time; different shaders run in parallel on the children.
static const GfxShader *translate(const Req *r, int stage, const uint8_t *control, const uint8_t *code,
                                  const GfxShaderAnswers *a) {
    int away = kern_core_away();
    const uint8_t *id = r->final ? r->key : r->code_hash;
    flight_enter(id);
    const GfxShader *s = translate_one(r, stage, control, code, a);
    flight_leave(id);
    kern_core_back(&away);
    return s;
}

// ---- background translation (gfx_shader_get_async)
// svc_runtime_limit() workers, one per translator child. A job owns copies of the control section and the code, so the
// guest may free or rewrite its program while the job waits. A key stays in X.waits while its job is queued or running,
// and for good when the translation failed, so a failing shader is not sent again on every draw.
typedef struct XJob {
    struct XJob *next;
    int stage;
    Req r;                                                 // r.c1 points into code
    uint8_t *control, *code;
    GfxShaderAnswers a;
} XJob;

typedef struct XWait {
    struct XWait *next;
    uint8_t key[32];
    int failed;
} XWait;

static struct {
    PlatMutex lock;
    PlatCond work, done;                         // done: a job finished (gfx_shader_await)
    XJob *head, *tail;
    XWait *waits[256];
    int started;                                           // 1 running, -1 no thread could be created
} X = {PLAT_MUTEX_INIT, PLAT_COND_INIT, PLAT_COND_INIT};

static volatile int64_t n_async;

// Caller holds X.lock.
static XWait **wait_slot(const uint8_t key[32]) {
    XWait **w = &X.waits[key[0]];
    while (*w && memcmp((*w)->key, key, 32)) w = &(*w)->next;
    return w;
}

static void xlate_worker(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&X.lock);
        while (!X.head) plat_cond_wait(&X.work, &X.lock);
        XJob *j = X.head;
        if (!(X.head = j->next)) X.tail = NULL;
        plat_mutex_unlock(&X.lock);
        const GfxShader *s = translate(&j->r, j->stage, j->control, j->code, &j->a);
        plat_mutex_lock(&X.lock);
        XWait **w = wait_slot(j->r.key), *e = *w;
        if (e && s) *w = e->next, free(e);
        else if (e) e->failed = 1;
        plat_mutex_unlock(&X.lock);
        plat_cond_broadcast(&X.done);
        if (s) plat_inc64(&n_async);
        free(j->control), free(j->code), free(j);
    }
    return;
}
const GfxShader *gfx_shader_get_async(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                      const GfxShaderAnswers *a, int *pending) {
    *pending = 0;
    Req r;
    if (!req_init(&r, stage, control, code, code_size, NULL, a)) return NULL;
    const GfxShader *s = get_async_one(r, stage, control, code, a, pending);
    return s;
}

static const GfxShader *get_async_one(Req r, int stage, const uint8_t *control, const uint8_t *code,
                                      const GfxShaderAnswers *a, int *pending) {
    GfxShader *s = mem_find(r.key);
    if (s) return plat_inc64(&n_hits), s;
    plat_mutex_lock_shared(&X.lock);
    XWait *w = *wait_slot(r.key);
    int failed = w && w->failed;
    plat_mutex_unlock_shared(&X.lock);
    if (w) return *pending = !failed, NULL;
    paths();
    if (!cache_dir[0]) return fail("no cache directory", NULL);
    if (r.final && (s = disk_load(r.key, r.iid))) return plat_inc64(&n_hits), mem_insert(s);

    XJob *j = calloc(1, sizeof *j);
    XWait *e = calloc(1, sizeof *e);
    uint8_t *ctl = malloc(CONTROL_SIZE), *cd = malloc(r.length);
    plat_mutex_lock(&X.lock);
    if (!X.started) {
        int n = 0;
        for (int i = 0; i < svc_runtime_limit(); i++) {
            if (plat_thread_spawn(xlate_worker, NULL, "xlate", 0)) n++;
        }
        X.started = n ? 1 : -1;
        if (n) rt_log("gfx_shader: graphics shaders are translated by %d background workers (PLUCKOMP_XLATE_PROCS)", n);
        else rt_log("gfx_shader: cannot start the translation workers; translating synchronously");
    }
    XWait **slot = wait_slot(r.key);
    int raced = *slot != NULL;                             // another thread queued it between the checks
    int queued = !raced && X.started == 1 && j && e && ctl && cd;
    if (queued) {
        memcpy(ctl, control, CONTROL_SIZE), memcpy(cd, code, r.length);
        *j = (XJob){.stage = stage, .r = r, .control = ctl, .code = cd, .a = *a};
        j->r.c1 = cd + (r.length - r.c1_len);
        memcpy(e->key, r.key, 32);
        *slot = e;
        if (X.tail) X.tail->next = j;
        else X.head = j;
        X.tail = j;
        plat_cond_signal(&X.work);
    }
    plat_mutex_unlock(&X.lock);
    if (queued) return *pending = 1, NULL;
    free(j), free(e), free(ctl), free(cd);
    if (raced) return *pending = 1, NULL;
    return translate(&r, stage, control, code, a);         // no worker or out of memory
}

const GfxShader *gfx_shader_await(int stage, const uint8_t *control, const uint8_t *code, uint64_t code_size,
                                  const GfxShaderAnswers *a) {
    int pending;
    const GfxShader *s = gfx_shader_get_async(stage, control, code, code_size, a, &pending);
    Req r;
    if (!pending || !req_init(&r, stage, control, code, code_size, NULL, a)) return s;
    uint64_t p0 = perf_begin();   // the wait counts as the render thread's translation time
    plat_mutex_lock(&X.lock);
    for (XJob *j = X.head, *prev = NULL; j; prev = j, j = j->next)
        if (!memcmp(j->r.key, r.key, 32)) {                // queued: to the front, the worker takes it next
            if (prev) {
                prev->next = j->next;
                if (X.tail == j) X.tail = prev;
                j->next = X.head, X.head = j;
            }
            break;
        }
    XWait *w;
    while ((w = *wait_slot(r.key)) && !w->failed) plat_cond_wait(&X.done, &X.lock);
    plat_mutex_unlock(&X.lock);
    perf_end(PERF_XLATE, p0);
    if (w) return NULL;                                    // failed: logged by the worker
    s = gfx_shader_get_async(stage, control, code, code_size, a, &pending);
    return pending ? NULL : s;
}

uint64_t gfx_shader_async_done(void) { return (uint64_t)n_async; }

void gfx_shader_counts(uint64_t *keys, uint64_t *translated) { *keys = (uint64_t)n_keys, *translated = (uint64_t)n_xlated; }

const uint8_t *gfx_shader_key(const GfxShader *s) { return s->key; }

const uint8_t *gfx_shader_input_id(const GfxShader *s) { return s->iid; }

// By input id: memory, then the disk cache under the current translator, then (xlate 1) a translation from the stored
// inputs. NULL when none has it; a failed translation is counted as a failure.
const GfxShader *gfx_shader_load_input(const uint8_t iid[32], int xlate) {
    paths();
    if (!cache_dir[0]) return NULL;
    uint8_t key[32];
    key_of_input(iid, key);
    GfxShader *s = mem_find(key);
    if (s) return s;
    if ((s = disk_load(key, iid))) return mem_insert(s);
    Inputs in;
    if (!xlate || !in_load(iid, &in)) return NULL;
    const GfxShader *t = gfx_shader_get(in.stage, in.control, in.code, in.length, in.c1, &in.a);
    free(in.buf);
    return t;
}

// From the disk cache only, as a shader the caller owns and releases with gfx_shader_release_private (the boot library
// prebuild warms the driver's pipeline cache with it without keeping 40 KB per shader in memory). When the memory
// cache has it, that entry is returned with *owned 0 and must not be released. NULL when not cached.
const GfxShader *gfx_shader_load_private(const uint8_t iid[32], int *owned) {
    *owned = 0;
    paths();
    if (!cache_dir[0]) return NULL;
    uint8_t key[32];
    key_of_input(iid, key);
    GfxShader *s = mem_find(key);
    if (s) return s;
    if ((s = disk_load(key, iid))) *owned = 1;
    return s;
}

void gfx_shader_release_private(const GfxShader *s) {
    if (s) shader_free((GfxShader *)s);
}

// ---- boot prewarm (gfx_shader_prewarm)

typedef struct {
    const uint8_t (*named)[32];                            // loaded into memory, results in out
    size_t n_named;
    const GfxShader **out;
    uint8_t (*other)[32];                                  // other stored inputs: translated when not cached, not loaded
    size_t n_other;
    volatile int64_t next, done;
    volatile int32_t missing, other_cached;
} Warm;

static void warm_worker(void *arg) {
    Warm *w = arg;
    for (;;) {
        size_t i = (size_t)plat_inc64(&w->next) - 1;
        if (i >= w->n_named + w->n_other) return;
        if (i < w->n_named) {
            if (!(w->out[i] = gfx_shader_load_input(w->named[i], 1))) plat_inc32(&w->missing);
        } else {
            const uint8_t *iid = w->other[i - w->n_named];
            uint8_t key[32];
            char hex[65], p[PATH_CAP + 80];
            key_of_input(iid, key);
            key_hex(key, hex);
            snprintf(p, PATH_CAP + 80, "%s/%s.spv", cache_dir, hex);
            if (pack_find(PACK_SPV, key) || file_exists(p)) plat_inc32(&w->other_cached);
            else gfx_shader_load_input(iid, 1);
        }
        plat_inc64(&w->done);
    }
}

static int iid_cmp(const void *x, const void *y) { return memcmp(x, y, 32); }

typedef struct {
    const uint8_t (*named)[32];
    size_t n_named, n, cap;
    uint8_t (*v)[32];
} InList;

static int in_collect(const char *file, void *ctx) {
    InList *l = ctx;
    char name[80];
    uint8_t iid[32];
    if (strlen(file) != 67) return 1;                      // 64 hex digits + ".in"
    memcpy(name, file, 64);
    name[64] = 0;
    if (!hex32(name, iid) || (l->n_named && bsearch(iid, l->named, l->n_named, 32, iid_cmp))) return 1;
    if (l->n == l->cap) {
        size_t c = l->cap ? l->cap * 2 : 1024;
        uint8_t (*t)[32] = realloc(l->v, c * 32);
        if (!t) return 0;
        l->v = t, l->cap = c;
    }
    memcpy(l->v[l->n++], iid, 32);
    return 1;
}

// The stored inputs not in named (sorted): ids parsed from the in/ file names.
static size_t other_inputs(const uint8_t (*named)[32], size_t n_named, uint8_t (**out)[32]) {
    char dir[PATH_CAP + 80];
    snprintf(dir, sizeof dir, "%s/in", cache_dir);
    InList l = {.named = named, .n_named = n_named};
    char name[68];
    for (uint32_t i = 0; i < pack_n; i++) {   // the packed inputs, under the name their loose file would have
        if (pack_ix[i].kind != PACK_IN) continue;
        key_hex(pack_ix[i].name, name);
        memcpy(name + 64, ".in", 4);
        if (!in_collect(name, &l)) break;
    }
    plat_file_list(dir, ".in", in_collect, &l);   // in_store writes no loose file for a packed input: no duplicates
    return *out = l.v, l.n;
}

void gfx_shader_prewarm(const uint8_t (*iids)[32], size_t n, const GfxShader **out, int threads, int procs) {
    paths();
    double t0 = (double)plat_now_ms();
    memset(out, 0, n * sizeof *out);
    if (!cache_dir[0]) return;
    uint64_t xl0 = (uint64_t)n_xlated, fail0 = (uint64_t)n_failures;
    uint8_t (*sorted)[32] = n ? malloc(n * 32) : NULL;
    if (sorted) memcpy(sorted, iids, n * 32), qsort(sorted, n, 32, iid_cmp);
    Warm w = {.named = iids, .n_named = n, .out = out};
    w.n_other = other_inputs(sorted, sorted ? n : 0, &w.other);
    free(sorted);
    size_t total = w.n_named + w.n_other;
    threads = threads < 1 ? 1 : threads > 64 ? 64 : threads;
    if ((size_t)threads > total) threads = (int)(total ? total : 1);
    svc_set_limit(procs);
    PlatThread *t[64];
    int k = 0;
    for (int i = 0; i < threads; i++)
        if ((t[k] = plat_thread_new(warm_worker, &w, "shader prewarm"))) k++;
    if (!k) warm_worker(&w);
    for (int m = 0; m < k; m++)
        while (!plat_thread_join_ns(t[m], 2000000000u))
            rt_log("gfx_shader: prewarm: %lld of %zu shaders, %llu translated, %.0f ms", (long long)w.done, total,
                   (unsigned long long)((uint64_t)n_xlated - xl0), (double)plat_now_ms() - t0);
    svc_set_limit(svc_runtime_limit());
    rt_log("gfx_shader: prewarm: %zu shaders named by pipeline keys (%ld not available), %zu other stored inputs (%ld "
           "cached); %llu translated, %llu failed, with up to %d translators and %d threads, in %.0f ms", w.n_named,
           (long)w.missing, w.n_other, (long)w.other_cached, (unsigned long long)((uint64_t)n_xlated - xl0),
           (unsigned long long)((uint64_t)n_failures - fail0), procs, k, (double)plat_now_ms() - t0);
    free(w.other);
}
const uint32_t *gfx_shader_spirv(const GfxShader *s, uint64_t *words) {
    if (words) *words = s->words;
    return s->spirv;
}

int gfx_shader_stage(const GfxShader *s) { return s->stage; }

int gfx_shader_resources(const GfxShader *s, int kind, const GfxShaderRes **res) {
    if (kind < 0 || kind >= GFX_RES_KINDS) return 0;
    if (res) *res = s->res[kind];
    return s->nres[kind];
}

// The slots of the use mask: answers outside it select the same translation (mask_answers), so a caller may leave
// their texture and image types zero. Parses the reflection: callers keep the result.
uint32_t gfx_shader_slot_mask(const GfxShader *s) { return use_of(s).slots; }

uint32_t gfx_shader_inputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->in, sizeof s->in);
    return s->in_mask;
}


uint32_t gfx_shader_outputs(const GfxShader *s, GfxShaderIo io[32]) {
    if (io) memcpy(io, s->out, sizeof s->out);
    return s->out_mask;
}

void gfx_shader_local_size(const GfxShader *s, uint32_t local[3]) { memcpy(local, s->local, sizeof s->local); }