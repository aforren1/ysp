/* box_test.c - self-checking test for ysp/box.h. No framework: it returns
 * 0 when every check passed and 1 after printing each failure.
 *
 * Each family decodes a generated stream whose frames are known: whole, a
 * byte at a time and in random chunks; with garbage between frames that
 * cannot start one (counted exactly), with broken frames whose rest holds
 * a real frame (found again), with output arrays too small for a read, and
 * as random bytes (no crash, progress, counts that add up). The XID bytes
 * follow pyxid2's struct formats ('<cBI' frames, '<ccBcIB' StimTracker 2
 * frames, '<cccI' timer answers); no byte from a Cedrus device was
 * available. The encoders' bytes are checked against the sources the
 * header cites.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -o box_test tests/adapt/box_test.c
 */
#define YSP_BOX_IMPLEMENTATION
#include "ysp/box.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { \
    fprintf(stderr, "box_test: FAIL at line %d: %s\n", __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); g_checks++; \
    if (g_ != w_) { fprintf(stderr, "box_test: FAIL at line %d: %s (got %lld, want %lld)\n", \
                            __LINE__, #got, g_, w_); g_failures++; } } while (0)

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(uint32_t n) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return n ? (uint32_t)(g_rng % n) : 0;
}

/* --- expected frames ------------------------------------------------------- */

typedef struct expect {
    int      is_event;
    uint8_t  type;
    uint32_t control, code;
    float    value;
    uint64_t ticks;
    uint32_t probe;
    int      is_out;         /* LINE "O": has_out with this code and ticks */
    uint32_t out_code;
} expect;

#define MAXF 4096
static expect   g_exp[MAXF];
static int      g_nexp;
static uint8_t  g_bytes[1 << 20];
static size_t   g_nbytes;

static void put(const void* b, size_t n) { memcpy(g_bytes + g_nbytes, b, n); g_nbytes += n; }
static void put_le32(uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put(b, 4);
}

/* Bytes that start no frame in the family: garbage the decoder must skip
 * one by one. */
static uint8_t garbage_byte(int family) {
    for (;;) {
        uint8_t b = (uint8_t)rnd(256);
        if (family == YBOX_XID && (b == 'k' || b == '_' || b == 'o')) continue;
        if (family == YBOX_PHOTO && (b == 0xA5 || b == 0x5A)) continue;
        if (family == YBOX_LINE && b == '\n') continue;
        return b;
    }
}

static void gen_xid(int frames, int garbage) {
    int i;
    for (i = 0; i < frames; i++) {
        expect* e = &g_exp[g_nexp++];
        int k;
        memset(e, 0, sizeof *e);
        e->ticks = rnd(0xFFFFFFFFu);
        for (k = 0; k < garbage; k++) { uint8_t b = garbage_byte(YBOX_XID); put(&b, 1); }
        if (rnd(4) == 0) {
            put("_e5", 3);
            put_le32((uint32_t)e->ticks);
            e->probe = 1;
        } else if (rnd(3) == 0) {
            /* StimTracker 2: 'o', port, key (0 = 8), '1' or '0', ms, 0 */
            uint32_t key = rnd(9), port = rnd(256), pressed = rnd(2);
            uint8_t hdr[4], z = 0;
            hdr[0] = 'o';
            hdr[1] = (uint8_t)port;
            hdr[2] = (uint8_t)key;
            hdr[3] = pressed ? '1' : '0';
            put(hdr, 4);
            put_le32((uint32_t)e->ticks);
            put(&z, 1);
            e->is_event = 1;
            e->type = pressed ? YIN_PRESS : YIN_RELEASE;
            e->control = key == 0 ? 8 : key;
            e->code = port;
            e->value = (float)pressed;
        } else {
            uint32_t key = 1 + rnd(8), port = rnd(4), pressed = rnd(2);
            uint8_t hdr[2];
            hdr[0] = 'k';
            hdr[1] = (uint8_t)(((key & 7u) << 5) | (pressed << 4) | port);   /* key 8 is 0 */
            put(hdr, 2);
            put_le32((uint32_t)e->ticks);
            e->is_event = 1;
            e->type = pressed ? YIN_PRESS : YIN_RELEASE;
            e->control = key;
            e->code = port;
            e->value = (float)pressed;
        }
    }
}

static void gen_line(int frames, int garbage) {
    int i;
    for (i = 0; i < frames; i++) {
        expect* e = &g_exp[g_nexp++];
        char s[128];
        int n = 0, k, what = (int)rnd(6);
        memset(e, 0, sizeof *e);
        e->ticks = ((uint64_t)rnd(0xFFFFFFFFu) << (rnd(2) ? 20 : 0)) + rnd(1000);
        if (garbage) {
            /* a garbage line: text that is no frame */
            for (k = 0; k < garbage; k++) s[k] = (char)('a' + rnd(26));
            s[garbage] = '\n';
            put(s, (size_t)garbage + 1);
        }
        switch (what) {
        case 0:
            n = snprintf(s, sizeof s, "S %llu\n", (unsigned long long)e->ticks);
            break;
        case 1:
            e->probe = 1 + rnd(0xFFFFFFFEu);
            n = snprintf(s, sizeof s, "Q %lu %llu\r\n", (unsigned long)e->probe, (unsigned long long)e->ticks);
            break;
        case 5:
            e->is_out = 1;
            e->out_code = rnd(256);
            n = snprintf(s, sizeof s, "O %llu %lu\n", (unsigned long long)e->ticks, (unsigned long)e->out_code);
            break;
        case 2: case 3: {
            uint32_t ch = 1 + rnd(255), lvl = rnd(2);
            n = snprintf(s, sizeof s, "E %llu %lu %lu\n", (unsigned long long)e->ticks, (unsigned long)ch,
                         (unsigned long)lvl);
            e->is_event = 1;
            e->type = lvl ? YIN_PRESS : YIN_RELEASE;
            e->control = ch;
            e->value = (float)lvl;
            break;
        }
        default: {
            uint32_t ch = 1 + rnd(255);
            double v = ((double)rnd(2000001) - 1000000.0) / 1000.0;
            n = snprintf(s, sizeof s, "A %llu %lu %.3f\n", (unsigned long long)e->ticks, (unsigned long)ch, v);
            e->is_event = 1;
            e->type = YIN_SAMPLE;
            e->control = ch;
            e->value = (float)v;
            break;
        }
        }
        put(s, (size_t)n);
    }
}

static void gen_photo(int frames, int garbage) {
    int i;
    for (i = 0; i < frames; i++) {
        expect* e = &g_exp[g_nexp++];
        uint8_t hdr[2];
        int k;
        memset(e, 0, sizeof *e);
        e->ticks = rnd(0xFFFFFFFFu);
        for (k = 0; k < garbage; k++) { uint8_t b = garbage_byte(YBOX_PHOTO); put(&b, 1); }
        if (rnd(3) == 0) {
            hdr[0] = 0x5A; hdr[1] = 0;
        } else {
            uint8_t lvl = (uint8_t)rnd(2);
            hdr[0] = 0xA5; hdr[1] = lvl;
            e->is_event = 1;
            e->type = lvl ? YIN_PRESS : YIN_RELEASE;
            e->control = 1;
            e->value = lvl;
        }
        put(hdr, 2);
        put_le32((uint32_t)e->ticks);
    }
}

/* --- decoding --------------------------------------------------------------- */

static yin_event g_ev[MAXF * 2];
static ybox_pair g_pair[MAXF * 2];
static int g_nev, g_npair;
static uint64_t g_garbage;
static uint32_t g_out_code[MAXF];
static uint64_t g_out_ticks[MAXF];
static int g_nout;

/* chunk 0: whole; 1: a byte at a time; 2: random sizes. cap: the output
 * arrays' size per call; the decoder must never write past it. */
static void decode_all(int family, int chunk, int cap) {
    ybox_decoder d;
    yin_event ev[64];
    ybox_pair pr[64];
    ybox_out o;
    size_t at = 0;
    g_nev = g_npair = g_nout = 0;
    g_garbage = 0;
    CHECK_I(ybox_init(&d, family, YIN_KIND_BOX, 7), 0);
    memset(&o, 0, sizeof o);
    o.ev = ev; o.ev_cap = cap; o.pair = pr; o.pair_cap = cap;
    while (at < g_nbytes) {
        size_t n = chunk == 0 ? g_nbytes - at : chunk == 1 ? 1 : 1 + rnd(40);
        size_t used = 0;
        if (n > g_nbytes - at) n = g_nbytes - at;
        while (used < n) {
            size_t u;
            int i;
            ybox_out_clear(&o);
            u = ybox_decode(&d, g_bytes + at + used, n - used, 1000 + (int64_t)at, &o);
            if (o.n_ev > cap || o.n_pair > cap) { CHECK(o.n_ev <= cap && o.n_pair <= cap); return; }
            if (u == 0) { CHECK(u > 0); return; }
            used += u;
            for (i = 0; i < o.n_ev; i++) g_ev[g_nev++] = ev[i];
            for (i = 0; i < o.n_pair; i++) g_pair[g_npair++] = pr[i];
            if (o.has_out && g_nout < MAXF) {
                g_out_code[g_nout] = o.out_code;
                g_out_ticks[g_nout++] = o.out_ticks;
            }
            g_garbage += o.garbage;
        }
        at += n;
    }
    CHECK_I(d.garbage, g_garbage);
}

/* Every expected frame in order, every field. */
static void compare(int family, const char* what) {
    int i, ie = 0, ip = 0, io = 0, bad = 0;
    for (i = 0; i < g_nexp; i++) {
        const expect* e = &g_exp[i];
        if (ip >= g_npair || g_pair[ip].ticks != e->ticks || g_pair[ip].probe != e->probe) bad++;
        ip++;
        if (e->is_out) {
            if (io >= g_nout || g_out_code[io] != e->out_code || g_out_ticks[io] != e->ticks) bad++;
            io++;
        }
        if (!e->is_event) continue;
        if (ie >= g_nev) { bad++; continue; }
        {
            const yin_event* v = &g_ev[ie++];
            if (v->type != e->type || v->control != e->control || v->kind != YIN_KIND_BOX || v->device != 7 ||
                v->ticks != (uint32_t)e->ticks || !(v->flags & YIN_DEVTICKS) || fabsf(v->value - e->value) > 1e-3f ||
                (family == YBOX_XID && v->code != e->code))
                bad++;
        }
    }
    if (ie != g_nev || ip != g_npair || io != g_nout) bad++;
    if (bad) fprintf(stderr, "box_test: %s %s: %d frames wrong (%d events, %d pairs decoded)\n",
                     ybox_family_name(family), what, bad, g_nev, g_npair);
    CHECK_I(bad, 0);
}

typedef void (*gen_fn)(int, int);

static void test_family(int family, gen_fn gen) {
    int chunk;
    /* clean stream, three ways of splitting it, two output sizes */
    for (chunk = 0; chunk < 3; chunk++) {
        g_nexp = 0; g_nbytes = 0;
        gen(1000, 0);
        decode_all(family, chunk, 64);
        compare(family, chunk == 0 ? "whole" : chunk == 1 ? "byte by byte" : "random chunks");
        CHECK_I(g_garbage, 0);
        decode_all(family, chunk, 1);
        compare(family, "one at a time");
    }
    /* garbage between frames, counted exactly for the byte families */
    g_nexp = 0; g_nbytes = 0;
    gen(1000, 3);
    decode_all(family, 2, 64);
    compare(family, "with garbage");
    if (family == YBOX_LINE) CHECK_I(g_garbage, 1000 * 4);
    else CHECK_I(g_garbage, 1000 * 3);
    printf("  %s: 1000 frames whole, byte by byte, in chunks, 1 per call, with garbage: ok\n",
           ybox_family_name(family));
}

/* A frame that breaks after its start, with a real frame inside its bytes. */
static void test_resync(void) {
    static const uint8_t xid[] = { 'k', 0x04,   /* port bit 2: no frame */
                                   'k', 0x30, 0x10, 0x00, 0x00, 0x00,   /* key 1, pressed, 16 ms */
                                   '_', 'e', 'x',                       /* not a timer answer */
                                   '_', 'e', '5', 0x01, 0x02, 0x00, 0x00 };
    static const uint8_t photo[] = { 0xA5, 0x07,   /* level 7: no frame */
                                     0x5A, 0x00, 0x10, 0x00, 0x00, 0x00,
                                     0x5A, 0x01,   /* sync with level 1: no frame */
                                     0xA5, 0x01, 0x20, 0x00, 0x00, 0x00 };
    g_nbytes = 0; put(xid, sizeof xid);
    decode_all(YBOX_XID, 1, 64);
    CHECK_I(g_nev, 1);
    CHECK_I(g_npair, 2);
    CHECK(g_nev == 1 && g_ev[0].control == 1 && g_ev[0].type == YIN_PRESS && g_ev[0].ticks == 16);
    CHECK(g_npair == 2 && g_pair[1].ticks == 0x0201 && g_pair[1].probe == 1);
    CHECK_I(g_garbage, 5);   /* 'k' 0x04, then '_' 'e' 'x' as three */
    g_nbytes = 0; put(xid, sizeof xid);
    decode_all(YBOX_XID, 0, 64);
    CHECK(g_nev == 1 && g_npair == 2);
    g_nbytes = 0; put(photo, sizeof photo);
    decode_all(YBOX_PHOTO, 1, 64);
    CHECK(g_nev == 1 && g_ev[0].type == YIN_PRESS && g_ev[0].ticks == 0x20);
    CHECK(g_npair == 2 && g_pair[0].ticks == 0x10);
    CHECK_I(g_garbage, 4);
    /* a StimTracker 2 frame broken at its null byte, a key frame inside */
    {
        static const uint8_t st2[] = { 'o', 'k', 0x30, '1', 0x05, 0x00, 0x00, 0x00, 0x09,   /* last byte not 0 */
                                       'o', 0x02, 0x03, '0', 0x40, 0x00, 0x00, 0x00, 0x00 };
        g_nbytes = 0; put(st2, sizeof st2);
        decode_all(YBOX_XID, 1, 1);
        CHECK_I(g_nev, 2);
        CHECK(g_nev == 2 && g_ev[0].control == 1 && g_ev[0].type == YIN_PRESS && g_ev[0].ticks == 0x0531);   /* bytes 2 to 7 */
        CHECK(g_nev == 2 && g_ev[1].control == 3 && g_ev[1].code == 2 && g_ev[1].type == YIN_RELEASE &&
              g_ev[1].ticks == 0x40);
        CHECK_I(g_garbage, 3);    /* the 'o', and the 0x00 and 0x09 after the key frame */
    }
    /* key 8 is sent as 0 */
    {
        static const uint8_t k8[] = { 'k', 0x03, 1, 0, 0, 0 };
        g_nbytes = 0; put(k8, sizeof k8);
        decode_all(YBOX_XID, 0, 64);
        CHECK(g_nev == 1 && g_ev[0].control == 8 && g_ev[0].type == YIN_RELEASE && g_ev[0].code == 3);
    }
    printf("  resync after broken frames: ok\n");
}

static void test_line_rules(void) {
    static const char good[] =
        "# a comment\n"
        "\n"
        "I ysp-line 1 teensy41 photodiode\r\n"
        "A 5 2 -1.5e-3\n"
        "E 18446744073709551615 255 0\n";
    static const char bad[] =
        "Q 0 5\n"                         /* seq 0 */
        "Q 4294967296 5\n"                /* seq past 32 bits */
        "E 5 0 1\n"                       /* channel 0 */
        "E 5 256 1\n"                     /* channel 256 */
        "E 5 1 2\n"                       /* level 2 */
        "E 5 1\n"                         /* missing field */
        "E 5 1 1 9\n"                     /* extra field */
        "S 18446744073709551616\n"        /* past 64 bits */
        "S -5\n"
        "A 5 1 x\n"
        "A 5 1\n"
        "X 5\n"
        "S5\n";
    ybox_decoder d;
    yin_event ev[16];
    ybox_pair pr[16];
    ybox_out o;
    char longline[300];
    memset(&o, 0, sizeof o);
    o.ev = ev; o.ev_cap = 16; o.pair = pr; o.pair_cap = 16;
    ybox_init(&d, YBOX_LINE, YIN_KIND_SYNC, 3);
    ybox_out_clear(&o);
    CHECK_I(ybox_decode(&d, (const uint8_t*)good, sizeof good - 1, 9, &o), sizeof good - 1);
    CHECK(o.has_text && strcmp(o.text, "ysp-line 1 teensy41 photodiode") == 0);
    CHECK(o.n_ev == 2 && ev[0].type == YIN_SAMPLE && fabsf(ev[0].value + 1.5e-3f) < 1e-7f && ev[0].control == 2);
    CHECK(o.n_ev == 2 && ev[1].ticks == 0xFFFFFFFFu && ev[1].control == 255 && ev[1].kind == YIN_KIND_SYNC);
    CHECK(o.n_pair == 2 && pr[1].ticks == 18446744073709551615ull);
    CHECK_I(o.garbage, 0);
    ybox_out_clear(&o);
    CHECK_I(ybox_decode(&d, (const uint8_t*)bad, sizeof bad - 1, 9, &o), sizeof bad - 1);
    CHECK_I(o.n_ev, 0);
    CHECK_I(o.n_pair, 0);
    CHECK_I(o.garbage, sizeof bad - 1);
    /* an overlong line and a line with a control byte are dropped whole */
    memset(longline, 'S', sizeof longline);
    longline[sizeof longline - 1] = '\n';
    ybox_out_clear(&o);
    (void)ybox_decode(&d, (const uint8_t*)longline, sizeof longline, 9, &o);
    (void)ybox_decode(&d, (const uint8_t*)"S 1\x01" "2\nS 7\n", 10, 9, &o);
    CHECK(o.n_pair == 1 && pr[0].ticks == 7);
    CHECK_I(o.garbage, sizeof longline + 6);   /* each dropped line with its newline */
    /* the tail of an overlong line is not a line of its own */
    memset(longline, 'x', 96);
    memcpy(longline + 96, "S 5\n", 4);
    ybox_out_clear(&o);
    (void)ybox_decode(&d, (const uint8_t*)longline, 100, 9, &o);
    CHECK_I(o.n_pair, 0);
    printf("  line protocol rules: ok\n");
}

static void test_queries(void) {
    uint8_t b[32];
    CHECK_I(ybox_probe(YBOX_XID, 0, b, 32), 3);
    CHECK(memcmp(b, "_e5", 3) == 0);
    CHECK_I(ybox_probe(YBOX_XID, 0, b, 2), 0);
    CHECK_I(ybox_probe(YBOX_LINE, 42, b, 32), 5);
    CHECK(memcmp(b, "q 42\n", 5) == 0);
    CHECK_I(ybox_probe(YBOX_LINE, 0, b, 32), 0);
    CHECK_I(ybox_probe(YBOX_LINE, 4294967295u, b, 32), 13);
    CHECK_I(ybox_probe(YBOX_LINE, 4294967295u, b, 12), 0);
    CHECK_I(ybox_probe(YBOX_PHOTO, 1, b, 32), 0);
    CHECK_I(ybox_identify(YBOX_XID, b, 32), 3);
    CHECK(memcmp(b, "_c1", 3) == 0);
    CHECK_I(ybox_identify(YBOX_LINE, b, 32), 2);
    CHECK(memcmp(b, "i\n", 2) == 0);
    CHECK_I(ybox_identify(YBOX_PHOTO, b, 32), 0);
    CHECK(ybox_ns_per_tick(YBOX_XID) == 1e6 && ybox_ns_per_tick(YBOX_LINE) == 1e3);
    CHECK(ybox_slow_write(YBOX_XID) && !ybox_slow_write(YBOX_LINE));
    /* an init: 0, or YBOX_ERR_ARG (v0.3.0; it was a bool) */
    CHECK_I(ybox_init(NULL, YBOX_XID, 0, 0), YBOX_ERR_ARG);
    {
        ybox_decoder d;
        CHECK_I(ybox_init(&d, 0, 0, 0), YBOX_ERR_ARG);
        CHECK_I(ybox_init(&d, 9, 0, 0), YBOX_ERR_ARG);
        CHECK_I(ybox_init(&d, YBOX_PARALLEL, 0, 0), 0);
    }
    CHECK(strcmp(ybox_strerror(YBOX_ERR_ARG), "bad argument") == 0 && strcmp(ybox_strerror(3), "ok") == 0);
    CHECK(strcmp(ybox_family_name(YBOX_LINE), "line") == 0);
    CHECK(strcmp(ybox_version(), YBOX_VERSION_STRING) == 0);
    printf("  queries and names: ok\n");
}

/* Random bytes: no crash, every call progresses while there is room, and
 * the counts add up (each frame gives one pair; events never outnumber
 * pairs). */
/* Bytes for each output op, against the sources ysp/box.h cites. */
static void test_encoders(void) {
    uint8_t b[64];
    static const uint8_t mh[] = { 'm', 'h', 0x34, 0x12 };
    static const uint8_t mp_mh[] = { 'm', 'p', 0x05, 0x00, 0x00, 0x00, 'm', 'h', 0x01, 0x00 };
    static const uint8_t mp0[] = { 'm', 'p', 0, 0, 0, 0 };
    int f;
    /* XID: mh + low, high (pyxid2 'mh'+chr(lo)+chr(hi)); mp + uint32 LE ms
     * (pack('<ccI')); 0 ms means SETs hold (Cedrus) */
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_SET, 0x1234, 0, b, 64), 4);
    CHECK(memcmp(b, mh, 4) == 0);
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_PULSE, 1, YRT_US(4600), b, 64), 10);   /* 4.6 ms rounds to 5 */
    CHECK(memcmp(b, mp_mh, 10) == 0);
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_PULSE, 1, YRT_US(100), b, 64), 10);    /* never 0 ms: that holds */
    CHECK(b[2] == 1 && b[3] == 0);
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_WIDTH, 0, 0, b, 64), 6);
    CHECK(memcmp(b, mp0, 6) == 0);
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_WIDTH, 0, YRT_S(4000), b, 64), 6);
    CHECK(b[2] == 0x00 && b[3] == 0x09 && b[4] == 0x3D && b[5] == 0x00);  /* 4,000,000 ms */
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_SET, 0x10000, 0, b, 64), -1);
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_PULSE, 1, YRT_MS(5), b, 9), -1);
    /* widths in ns (v0.3.0): negative, or past the 32-bit wire field, is an
     * error, not a clamp; SET ignores the width */
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_PULSE, 1, -1, b, 64), YBOX_ERR_ARG);
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_WIDTH, 0, (int64_t)0xFFFFFFFF * 1000 + 1, b, 64), YBOX_ERR_ARG);
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_WIDTH, 0, (int64_t)0xFFFFFFFF * 1000, b, 64), 6);
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_SET, 1, -1, b, 64), 4);
    CHECK_I(ybox_encode(YBOX_XID, YBOX_OP_CLOSE, 0, 0, b, 64), 0);
    /* LINE */
    CHECK_I(ybox_encode(YBOX_LINE, YBOX_OP_SET, 5, 0, b, 64), 4);
    CHECK(memcmp(b, "o 5\n", 4) == 0);
    CHECK_I(ybox_encode(YBOX_LINE, YBOX_OP_PULSE, 255, YRT_MS(2), b, 64), 11);
    CHECK(memcmp(b, "p 255 2000\n", 11) == 0);
    CHECK_I(ybox_encode(YBOX_LINE, YBOX_OP_PULSE, 255, 1999501, b, 64), 11);   /* rounds to the us */
    CHECK(memcmp(b, "p 255 2000\n", 11) == 0);
    CHECK_I(ybox_encode(YBOX_LINE, YBOX_OP_PULSE, 1, 0, b, 64), -1);
    CHECK_I(ybox_encode(YBOX_LINE, YBOX_OP_PULSE, 1, 499, b, 64), -1);          /* 0 us */
    CHECK_I(ybox_encode(YBOX_LINE, YBOX_OP_SET, 256, 0, b, 64), -1);
    CHECK_I(ybox_encode(YBOX_LINE, YBOX_OP_PULSE, 1, YRT_MS(2), b, 8), -1);
    CHECK_I(ybox_encode(YBOX_LINE, YBOX_OP_WIDTH, 1, YRT_MS(2), b, 64), 0);
    /* one byte: TriggerBox, BioSemi, MMBT-S, parallel; no device-timed pulse */
    for (f = YBOX_TRIGGERBOX; f <= YBOX_PARALLEL; f++) {
        if (f == YBOX_LINES) continue;
        CHECK_I(ybox_encode(f, YBOX_OP_SET, 0xA5, 0, b, 64), 1);
        CHECK_I(b[0], 0xA5);
        CHECK_I(ybox_encode(f, YBOX_OP_SET, 256, 0, b, 64), -1);
        CHECK_I(ybox_encode(f, YBOX_OP_PULSE, 1, YRT_MS(2), b, 64), 0);
        CHECK_I(ybox_encode(f, YBOX_OP_SET, 1, 0, b, 0), -1);
        CHECK_I(ybox_code_max(f), 255);
    }
    CHECK_I(ybox_encode(YBOX_TRIGGERBOX, YBOX_OP_CLOSE, 0, 0, b, 64), 1);
    CHECK_I(b[0], 0xFF);   /* "reset to their default levels by writing a 0xFF" */
    CHECK_I(ybox_encode(YBOX_BIOSEMI, YBOX_OP_CLOSE, 0, 0, b, 64), 0);
    /* DTR and RTS: no bytes, a 2-bit code */
    CHECK_I(ybox_encode(YBOX_LINES, YBOX_OP_SET, 3, 0, b, 64), 0);
    CHECK_I(ybox_encode(YBOX_LINES, YBOX_OP_SET, 4, 0, b, 64), -1);
    CHECK_I(ybox_code_max(YBOX_LINES), 3);
    /* no outputs */
    CHECK_I(ybox_encode(YBOX_PHOTO, YBOX_OP_SET, 1, 0, b, 64), -1);
    CHECK_I(ybox_encode(9, YBOX_OP_SET, 1, 0, b, 64), -1);
    CHECK_I(ybox_encode(YBOX_LINE, 7, 1, 0, b, 64), -1);
    CHECK_I(ybox_code_max(YBOX_PHOTO), 0);
    CHECK_I(ybox_code_max(YBOX_XID), 65535);
    /* caps, widths, rates */
    CHECK(ybox_caps(YBOX_XID) & YBOX_CAP_PULSE);
    CHECK(ybox_caps(YBOX_LINE) & YBOX_CAP_PULSE);
    CHECK(!(ybox_caps(YBOX_TRIGGERBOX) & (YBOX_CAP_PULSE | YBOX_CAP_FIXED | YBOX_CAP_IN)));
    CHECK(ybox_caps(YBOX_LINES) & YBOX_CAP_LINES);
    CHECK_I(ybox_fixed_pulse_ns(YBOX_BIOSEMI), 8000000);
    CHECK_I(ybox_fixed_pulse_ns(YBOX_MMBTS), 8000000);
    CHECK_I(ybox_fixed_pulse_ns(YBOX_TRIGGERBOX), 0);
    CHECK_I(ybox_default_baud(YBOX_MMBTS), 9600);
    CHECK_I(ybox_default_baud(YBOX_BIOSEMI), 115200);
    CHECK_I(ybox_caps(0), 0);
    CHECK(strcmp(ybox_family_name(YBOX_MMBTS), "mmbts") == 0 && strcmp(ybox_family_name(YBOX_LINES), "lines") == 0);
    printf("  encoders: XID mh and mp, line o and p, single bytes, TriggerBox close, caps: ok\n");
}

/* LINE "O": one per call, then the rest. */
static void test_line_out(void) {
    static const char two[] = "O 100 3\nO 200 0\nS 300\n";
    ybox_decoder d;
    yin_event ev[4];
    ybox_pair pr[4];
    ybox_out o;
    size_t u;
    memset(&o, 0, sizeof o);
    o.ev = ev; o.ev_cap = 4; o.pair = pr; o.pair_cap = 4;
    ybox_init(&d, YBOX_LINE, YIN_KIND_BOX, 1);
    ybox_out_clear(&o);
    u = ybox_decode(&d, (const uint8_t*)two, sizeof two - 1, 0, &o);
    CHECK_I(u, 8);
    CHECK(o.has_out && o.out_code == 3 && o.out_ticks == 100 && o.n_pair == 1 && pr[0].ticks == 100);
    ybox_out_clear(&o);
    u += ybox_decode(&d, (const uint8_t*)two + u, sizeof two - 1 - u, 0, &o);
    CHECK(o.has_out && o.out_code == 0 && o.out_ticks == 200);
    ybox_out_clear(&o);
    u += ybox_decode(&d, (const uint8_t*)two + u, sizeof two - 1 - u, 0, &o);
    CHECK(!o.has_out && o.n_pair == 1 && pr[0].ticks == 300 && u == sizeof two - 1);
    ybox_out_clear(&o);
    CHECK_I(ybox_decode(&d, (const uint8_t*)"O 1 4294967296\nO 1\n", 19, 0, &o), 19);
    CHECK(!o.has_out && o.garbage == 19);
    printf("  line O reports: ok\n");
}

static void test_fuzz(void) {
    int family;
    for (family = YBOX_XID; family <= YBOX_FAMILY_LAST; family++) {
        size_t i;
        g_nbytes = 0;
        for (i = 0; i < 200000; i++) {
            uint8_t b = (uint8_t)rnd(256);
            if (family == YBOX_LINE && rnd(4) == 0) b = (uint8_t)" SEQAIO0123456789\n"[rnd(18)];
            if (family == YBOX_XID && rnd(8) == 0) b = rnd(2) ? 'k' : 'o';
            put(&b, 1);
        }
        decode_all(family, 2, 1 + (int)rnd(8));
        CHECK(g_nev <= g_npair);
        if (!(ybox_caps(family) & YBOX_CAP_IN)) CHECK(g_npair == 0 && g_garbage == 200000);
        printf("  fuzz %s: 200000 bytes, %d events, %d pairs, %llu garbage\n", ybox_family_name(family), g_nev,
               g_npair, (unsigned long long)g_garbage);
    }
}

int main(void) {
    printf("box_test: ysp/box.h %s\n", ybox_version());
    test_family(YBOX_XID, gen_xid);
    test_family(YBOX_LINE, gen_line);
    test_family(YBOX_PHOTO, gen_photo);
    test_resync();
    test_line_rules();
    test_queries();
    test_encoders();
    test_line_out();
    test_fuzz();
    if (g_failures) {
        fprintf(stderr, "box_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("box_test: all %d checks passed\n", g_checks);
    return 0;
}
