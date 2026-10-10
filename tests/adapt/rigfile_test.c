/* rigfile_test.c - self-checking test for ysp/rigfile.h. No framework: it returns
 * 0 when every check passed and 1 after printing each failure.
 *
 *     gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude \
 *         -o rigfile_test tests/adapt/rigfile_test.c -lm -lpthread     (Linux)
 *     ... -lsetupapi                                          (MinGW)
 *
 * It writes under ./rig_test_work (the working folder) and points the
 * user config folder there (APPDATA on Windows, XDG_CONFIG_HOME on POSIX)
 * for the user-folder checks, so it never touches the real rig profile.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define getcwd _getcwd
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------- harness */

static int g_failures = 0;
static int g_checks = 0;

static void fail_s(int line, const char* what, const char* got, const char* want) {
    fprintf(stderr, "rigfile_test: FAIL at line %d: %s\n  got:  [%s]\n  want: [%s]\n", line, what, got ? got : "(null)", want ? want : "(null)");
    g_failures++;
}

#define CHECK(cond) do { g_checks++; if (!(cond)) { \
    fprintf(stderr, "rigfile_test: FAIL at line %d: %s\n", __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_I(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); g_checks++; \
    if (g_ != w_) { fprintf(stderr, "rigfile_test: FAIL at line %d: %s (got %lld, want %lld)\n", \
                            __LINE__, #got, g_, w_); g_failures++; } } while (0)
#define CHECK_S(got, want) do { const char* g_ = (got); const char* w_ = (want); g_checks++; \
    if (!g_ || !w_ || strcmp(g_, w_) != 0) fail_s(__LINE__, #got, g_, w_); } while (0)
#define CHECK_HAS(str, sub) do { const char* s_ = (str); g_checks++; \
    if (!s_ || !strstr(s_, (sub))) fail_s(__LINE__, "message lacks a substring", s_, (sub)); } while (0)

static uint64_t g_rng = 0x2545F4914F6CDD1DULL;
static uint64_t rnd64(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}
static int rnd(int n) { return (int)(rnd64() % (uint64_t)n); }

static yrig_profile g_p, g_q;
static char g_buf[1 << 16];
static char g_err[512];

#define A64 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define B64 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define C64 "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"

/* The canonical bytes of build_golden()'s profile. */
static const char GOLDEN[] =
    "{\n"
    "  \"display\": {\n"
    "    \"calibration\": {\n"
    "      \"file\": \"display.yspcal\",\n"
    "      \"sha256\": \"" A64 "\"\n"
    "    },\n"
    "    \"onset_offset_s\": 0.0123\n"
    "  },\n"
    "  \"format\": \"ysp-rig 1\",\n"
    "  \"rig\": \"booth-2 \xC3\xA9\",\n"
    "  \"roles\": {\n"
    "    \"mm\": {\n"
    "      \"family\": \"mmbts\",\n"
    "      \"key\": \"serial:2341:0043::\",\n"
    "      \"options\": {\n"
    "        \"latched\": true\n"
    "      }\n"
    "    },\n"
    "    \"resp\": {\n"
    "      \"bounds\": {\n"
    "        \"date\": \"2026-10-08T09:00:00Z\",\n"
    "        \"hi_s\": 0.0011,\n"
    "        \"lo_s\": -0.0003,\n"
    "        \"n\": 200,\n"
    "        \"sha256\": \"" B64 "\"\n"
    "      },\n"
    "      \"family\": \"xid\",\n"
    "      \"key\": \"serial:0403:6001:FT4ABC12:\",\n"
    "      \"options\": {\n"
    "        \"baud\": 115200,\n"
    "        \"buttons\": {\n"
    "          \"1\": \"left\",\n"
    "          \"2\": \"right\"\n"
    "        },\n"
    "        \"ftdi_latency_s\": 0.001\n"
    "      }\n"
    "    },\n"
    "    \"trig\": {\n"
    "      \"family\": \"triggerbox\",\n"
    "      \"key\": \"serial:0403:6001:TB0123:\",\n"
    "      \"latency\": {\n"
    "        \"date\": \"2026-10-09T13:58:12Z\",\n"
    "        \"max_s\": 0.0021,\n"
    "        \"median_s\": 0.000577,\n"
    "        \"n\": 200,\n"
    "        \"p5_s\": 0.000201,\n"
    "        \"p95_s\": 0.000983,\n"
    "        \"sha256\": \"" C64 "\"\n"
    "      },\n"
    "      \"pulse_s\": 2\n"
    "    }\n"
    "  },\n"
    "  \"written\": \"2026-10-09T14:03:00Z\"\n"
    "}\n";

static void build_golden(yrig_profile* p) {
    yrig_role* r;
    yrig_init(p, "booth-2 \xC3\xA9");
    strcpy(p->written, "2026-10-09T14:03:00Z");
    CHECK_I(yrig_bind(p, "trig", YBOX_TRIGGERBOX, "serial:0403:6001:TB0123:"), YRIG_OK);
    r = yrig_find(p, "trig");
    r->pulse_ns = 2000000000;
    r->latency.set = true;
    r->latency.n = 200;
    r->latency.p5_ns = 201000;
    r->latency.median_ns = 577000;
    r->latency.p95_ns = 983000;
    r->latency.max_ns = 2100000;
    strcpy(r->latency.date, "2026-10-09T13:58:12Z");
    strcpy(r->latency.sha256, C64);
    CHECK_I(yrig_bind(p, "resp", YBOX_XID, "serial:0403:6001:FT4ABC12:"), YRIG_OK);
    r = yrig_find(p, "resp");
    r->baud = 115200;
    r->ftdi_latency_ns = 1000000;
    r->n_buttons = 2;
    strcpy(r->buttons[0].code, "2");
    strcpy(r->buttons[0].name, "right");
    strcpy(r->buttons[1].code, "1");
    strcpy(r->buttons[1].name, "left");
    r->bounds.set = true;
    r->bounds.n = 200;
    r->bounds.lo_ns = -300000;
    r->bounds.hi_ns = 1100000;
    strcpy(r->bounds.date, "2026-10-08T09:00:00Z");
    strcpy(r->bounds.sha256, B64);
    CHECK_I(yrig_bind(p, "mm", YBOX_MMBTS, "serial:2341:0043::"), YRIG_OK);
    yrig_find(p, "mm")->latched = true;
    p->display.set = true;
    p->display.onset_offset_ns = 12300000;
    strcpy(p->display.calibration, "display.yspcal");
    strcpy(p->display.calibration_sha256, A64);
}

static void sha_hex(const char* s, size_t n, char out[65]) {
    uint8_t d[32];
    int i;
    yrig_sha256(s, n, d);
    for (i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* ------------------------------------------------------------ the format */

static void check_sha(void) {
    char h[65];
    char* big = (char*)malloc(1000000);
    sha_hex("", 0, h);
    CHECK_S(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    sha_hex("abc", 3, h);
    CHECK_S(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    CHECK_S(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    memset(big, 'a', 1000000);
    sha_hex(big, 1000000, h);
    CHECK_S(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(big);
}

static void check_round_trip(void) {
    size_t n;
    char h1[65], h2[65], want[65];
    build_golden(&g_p);
    n = yrig_write(&g_p, g_buf, sizeof g_buf);
    CHECK_I(n, sizeof GOLDEN - 1);
    CHECK_S(g_buf, GOLDEN);
    CHECK_I(yrig_write(&g_p, NULL, 0), n);
    CHECK_I(yrig_hash(&g_p, h1), YRIG_OK);
    sha_hex(GOLDEN, sizeof GOLDEN - 1, want);
    CHECK_S(h1, want);
    /* parse, write: the same bytes, the same hash */
    CHECK_I(yrig_parse(&g_q, GOLDEN, sizeof GOLDEN - 1, NULL, g_err, sizeof g_err), YRIG_OK);
    CHECK_I(yrig_write(&g_q, g_buf, sizeof g_buf), n);
    CHECK_S(g_buf, GOLDEN);
    CHECK_I(yrig_hash(&g_q, h2), YRIG_OK);
    CHECK_S(h2, h1);
    CHECK_I(g_q.n_roles, 3);
    CHECK_S(g_q.role[0].name, "mm");
    CHECK(g_q.role[0].latched && g_q.role[0].family_id == YBOX_MMBTS);
    CHECK_I(g_q.role[1].baud, 115200);
    CHECK_I(g_q.role[1].ftdi_latency_ns, 1000000);
    CHECK_I(g_q.role[1].n_buttons, 2);
    CHECK_S(g_q.role[1].buttons[0].code, "1");
    CHECK_S(g_q.role[1].buttons[1].name, "right");
    CHECK_I(g_q.role[1].bounds.lo_ns, -300000);
    CHECK_I(g_q.role[1].bounds.check, YRIG_CHECK_NOT_RUN);
    CHECK_I(g_q.role[2].pulse_ns, 2000000000);
    CHECK_I(g_q.role[2].latency.median_ns, 577000);
    CHECK_I(g_q.role[2].latency.check, YRIG_CHECK_NOT_RUN);
    CHECK_I(g_q.role[0].latency.check, YRIG_CHECK_NONE);
    CHECK_I(g_q.display.onset_offset_ns, 12300000);
    CHECK_I(g_q.display.check, YRIG_CHECK_NOT_RUN);
    CHECK_I(g_q.n_notes, 0);
    /* a hand edit in another spelling: the same canonical bytes and hash */
    {
        static const char hand[] =
            "\xEF\xBB\xBF{ \"written\":\"2026-10-09T14:03:00Z\", \"rig\":\"booth-2 \\u00e9\", \"format\":\"ysp-rig 1\",\r\n"
            "  \"roles\": { \"trig\": { \"pulse_s\": 2.000, \"latency\": { \"sha256\": \"" C64 "\", \"p95_s\": 983e-6,"
            " \"p5_s\": 2.01E-4, \"n\": 200, \"median_s\": 0.0005770, \"max_s\": 0.00210, \"date\": \"2026-10-09T13:58:12Z\" },"
            " \"key\": \"serial:0403:6001:TB0123:\", \"family\": \"triggerbox\", \"options\": {} },\n"
            "  \"mm\": {\"family\": \"mmbts\", \"key\": \"serial:2341:0043::\", \"options\": {\"latched\": true}},\n"
            "  \"resp\": {\"family\": \"xid\", \"key\": \"serial:0403:6001:FT4ABC12:\", \"options\": {\"latched\": false, \"baud\": 115200,"
            " \"ftdi_latency_s\": 1e-3, \"buttons\": {\"2\": \"right\", \"1\": \"left\"}}, \"bounds\": {\"n\": 200, \"lo_s\": -3e-4,"
            " \"hi_s\": 0.0011, \"date\": \"2026-10-08T09:00:00Z\", \"sha256\": \"" B64 "\"}}},\n"
            "  \"display\": {\"onset_offset_s\": 0.012300000, \"calibration\": {\"sha256\": \"" A64 "\", \"file\": \"display.yspcal\"}}}";
        CHECK_I(yrig_parse(&g_q, hand, sizeof hand - 1, NULL, g_err, sizeof g_err), YRIG_OK);
        CHECK_I(yrig_write(&g_q, g_buf, sizeof g_buf), n);
        CHECK_S(g_buf, GOLDEN);
        CHECK_I(yrig_hash(&g_q, h2), YRIG_OK);
        CHECK_S(h2, h1);
    }
    /* seconds to the nanosecond, both ways */
    {
        static const char t[] = "{\"format\":\"ysp-rig 1\",\"rig\":\"r\",\"written\":\"2026-01-01T00:00:00Z\",\"roles\":{\"x\":{\"family\":\"line\","
                                "\"key\":\"k\",\"pulse_s\":15e-7}}}";
        CHECK_I(yrig_parse(&g_q, t, sizeof t - 1, NULL, g_err, sizeof g_err), YRIG_OK);
        CHECK_I(g_q.role[0].pulse_ns, 1500);
        yrig_write(&g_q, g_buf, sizeof g_buf);
        CHECK_HAS(g_buf, "\"pulse_s\": 0.0000015\n");
    }
    /* a minimal profile; and the canonical bytes of an empty one */
    {
        static const char minimal[] = "{\"format\":\"ysp-rig 1\",\"rig\":\"r\",\"written\":\"2026-01-01T00:00:00Z\",\"roles\":{}}";
        CHECK_I(yrig_parse(&g_q, minimal, sizeof minimal - 1, NULL, g_err, sizeof g_err), YRIG_OK);
        yrig_write(&g_q, g_buf, sizeof g_buf);
        CHECK_S(g_buf, "{\n  \"format\": \"ysp-rig 1\",\n  \"rig\": \"r\",\n  \"roles\": {},\n  \"written\": \"2026-01-01T00:00:00Z\"\n}\n");
    }
    /* a truncated buffer still ends in NUL; the length is the whole */
    {
        char small[10];
        CHECK_I(yrig_write(&g_p, small, sizeof small), n);
        CHECK(small[9] == '\0' && memcmp(small, "{\n  \"disp", 9) == 0);
    }
}

/* ----------------------------------------------------------- refusals */

/* GOLDEN with one edit; the result and a substring of the message. */
static void refuse(int line, const char* find, const char* repl, int code, const char* says) {
    static char t[1 << 16];
    const char* at = strstr(GOLDEN, find);
    size_t fl = strlen(find), rl = strlen(repl), pre;
    int rc;
    g_checks++;
    if (!at) { fprintf(stderr, "rigfile_test: FAIL at line %d: '%s' is not in GOLDEN\n", line, find); g_failures++; return; }
    pre = (size_t)(at - GOLDEN);
    memcpy(t, GOLDEN, pre);
    memcpy(t + pre, repl, rl);
    strcpy(t + pre + rl, at + fl);
    g_err[0] = '\0';
    rc = yrig_parse(&g_q, t, strlen(t), NULL, g_err, sizeof g_err);
    if (rc != code || !strstr(g_err, says) || g_q.n_roles != 0) {
        fprintf(stderr, "rigfile_test: FAIL at line %d: %d '%s' (want %d '%s')\n", line, rc, g_err, code, says);
        g_failures++;
    }
}
#define REFUSE(f, r, code, says) refuse(__LINE__, f, r, code, says)

static void check_refusals(void) {
    char* big;
    /* not JSON: the line and column */
    REFUSE("\"rig\": \"booth", "\"rig\" \"booth", YRIG_ERR_JSON, "line 10, column 9: ':' is expected");
    REFUSE("\"n\": 200,\n        \"sha256\": \"" B64, "\"n\": 200,,\n        \"sha256\": \"" B64, YRIG_ERR_JSON, "line 24, column 18");
    REFUSE("  \"written\"", "  // a comment\n  \"written\"", YRIG_ERR_JSON, "line 53, column 3");
    REFUSE("\"mm\": {", "\"trig\": {}, \"mm\": {", YRIG_ERR_JSON, "duplicate key \"trig\"");
    REFUSE("0.0123", "0.0123,", YRIG_ERR_JSON, "line 8");
    /* the format */
    REFUSE("\"ysp-rig 1\"", "\"ysp-rig 2\"", YRIG_ERR_FORMAT, "format: \"ysp-rig 2\" is not \"ysp-rig 1\"");
    REFUSE("\"format\": \"ysp-rig 1\",", "", YRIG_ERR_FORMAT, "format: is missing");
    REFUSE("\"ysp-rig 1\"", "1", YRIG_ERR_FORMAT, "format: must be a string");
    REFUSE("\"format\"", "\"Format\"", YRIG_ERR_FORMAT, "unknown key \"Format\"");
    /* the top level */
    REFUSE("\"rig\": \"booth-2 \xC3\xA9\",", "", YRIG_ERR_FORMAT, "rig: is missing");
    REFUSE("\"rig\": \"booth-2 \xC3\xA9\"", "\"rig\": \"\"", YRIG_ERR_FORMAT, "rig: must be 1 to 63 bytes");
    REFUSE("\"rig\": \"booth-2 \xC3\xA9\"", "\"rig\": \"a\\tb\"", YRIG_ERR_FORMAT, "rig: has a control character");
    REFUSE("\"rig\": \"booth-2 \xC3\xA9\"", "\"rig\": [\"booth\"]", YRIG_ERR_FORMAT, "rig: must be a string (line 10, column 10)");
    REFUSE("\"written\": \"2026-10-09T14:03:00Z\"", "\"written\": \"2026-10-09 14:03:00\"", YRIG_ERR_FORMAT, "written: must be a UTC date");
    REFUSE("\"written\": \"2026-10-09T14:03:00Z\"", "\"written\": \"2026-13-09T14:03:00Z\"", YRIG_ERR_FORMAT, "written: must be a UTC date");
    REFUSE(",\n  \"written\": \"2026-10-09T14:03:00Z\"", "", YRIG_ERR_FORMAT, "written: is missing");
    REFUSE("\"roles\": {", "\"roles\": [], \"x\": {", YRIG_ERR_FORMAT, "unknown key \"x\"");
    REFUSE("\"roles\": {\n    \"mm\"", "\"roles\": 5, \"r2\": {\n    \"mm\"", YRIG_ERR_FORMAT, "unknown key \"r2\"");
    REFUSE("\"display\": {", "\"display\": [", YRIG_ERR_JSON, "line");
    REFUSE("  \"display\": {\n    \"calibration\": {\n      \"file\": \"display.yspcal\",\n      \"sha256\": \"" A64 "\"\n    },\n    \"onset_offset_s\": 0.0123\n  },\n", "  \"display\": 0,\n", YRIG_ERR_FORMAT, "display: must be an object");
    /* a role */
    REFUSE("\"mm\": {", "\"m m\": {", YRIG_ERR_FORMAT, "roles: \"m m\" is not a role name");
    REFUSE("\"mm\": {", "\"abcdefghijabcdefghijabcdefghijab\": {", YRIG_ERR_FORMAT, "is not a role name");
    REFUSE("\"mm\": {\n      \"family\": \"mmbts\",", "\"mm\": {\n      \"family\": \"mmbtz\",", YRIG_ERR_FORMAT,
           "roles.mm.family: \"mmbtz\" is not a ysp/box.h family");
    REFUSE("\"family\": \"mmbts\",\n", "", YRIG_ERR_FORMAT, "roles.mm.family: is missing");
    REFUSE("\"key\": \"serial:2341:0043::\",\n", "", YRIG_ERR_FORMAT, "roles.mm.key: is missing");
    REFUSE("\"key\": \"serial:2341:0043::\"", "\"key\": 2341", YRIG_ERR_FORMAT, "roles.mm.key: must be a string");
    REFUSE("\"family\": \"xid\",", "\"family\": \"xid\", \"colour\": 1,", YRIG_ERR_FORMAT, "roles.resp: unknown key \"colour\"");
    REFUSE("\"latched\": true", "\"latched\": 1", YRIG_ERR_FORMAT, "roles.mm.options.latched: must be true or false");
    REFUSE("\"family\": \"mmbts\"", "\"family\": \"biosemi\"", YRIG_ERR_FORMAT, "roles.mm.options.latched: only an mmbts can be latched");
    REFUSE("\"baud\": 115200", "\"baud\": 0", YRIG_ERR_FORMAT, "roles.resp.options.baud: 0 is out of range");
    REFUSE("\"baud\": 115200", "\"baud\": 115200.5", YRIG_ERR_FORMAT, "roles.resp.options.baud: must be an integer");
    REFUSE("\"baud\": 115200", "\"baud\": \"115200\"", YRIG_ERR_FORMAT, "roles.resp.options.baud: must be an integer");
    REFUSE("\"ftdi_latency_s\": 0.001", "\"ftdi_latency_s\": 1", YRIG_ERR_FORMAT, "roles.resp.options.ftdi_latency_s: 1 is out of range");
    REFUSE("\"ftdi_latency_s\": 0.001", "\"ftdi_latency_s\": \"0.001\"", YRIG_ERR_FORMAT, "must be a number (seconds)");
    REFUSE("\"buttons\": {", "\"buttons\": [], \"x\": {", YRIG_ERR_FORMAT, "roles.resp.options: unknown key \"x\"");
    REFUSE("\"1\": \"left\"", "\"1\": 7", YRIG_ERR_FORMAT, "roles.resp.options.buttons.1: must be a string");
    REFUSE("\"1\": \"left\"", "\"1\": \"\"", YRIG_ERR_FORMAT, "roles.resp.options.buttons.1: must be 1 to 31 bytes");
    REFUSE("\"1\": \"left\"", "\"1234567890123456\": \"left\"", YRIG_ERR_FORMAT, "a button code is 1 to 15 bytes");
    REFUSE("\"options\": {\n        \"latched\": true\n      }", "\"options\": {\"latched\": true, \"buttons\": {\"a\":\"1\",\"b\":\"1\",\"c\":\"1\","
           "\"d\":\"1\",\"e\":\"1\",\"f\":\"1\",\"g\":\"1\",\"h\":\"1\",\"i\":\"1\",\"j\":\"1\",\"k\":\"1\",\"l\":\"1\",\"m\":\"1\",\"n\":\"1\",\"o\":\"1\","
           "\"p\":\"1\",\"q\":\"1\"}}", YRIG_ERR_FORMAT, "more than 16 buttons");
    REFUSE("\"pulse_s\": 2", "\"pulse_s\": 0", YRIG_ERR_FORMAT, "roles.trig.pulse_s: 0 is out of range");
    REFUSE("\"pulse_s\": 2", "\"pulse_s\": 0.0000000004", YRIG_ERR_FORMAT, "roles.trig.pulse_s: 0.0000000004 is out of range");
    REFUSE("\"pulse_s\": 2", "\"pulse_s\": 11", YRIG_ERR_FORMAT, "roles.trig.pulse_s: 11 is out of range");
    REFUSE("\"pulse_s\": 2", "\"pulse_s\": null", YRIG_ERR_FORMAT, "roles.trig.pulse_s: must be a number (seconds)");
    /* latency and bounds */
    REFUSE("\"max_s\": 0.0021,\n", "", YRIG_ERR_FORMAT, "roles.trig.latency.max_s: is missing");
    REFUSE("\"median_s\": 0.000577", "\"median_s\": 0.00099", YRIG_ERR_FORMAT, "roles.trig.latency: p5_s <= median_s <= p95_s <= max_s");
    REFUSE("\"p5_s\": 0.000201,\n", "\"p5_s\": 0.000201,\n        \"lo_s\": 0,\n", YRIG_ERR_FORMAT, "roles.trig.latency: unknown key \"lo_s\"");
    REFUSE("\"n\": 200,\n        \"p5_s\"", "\"n\": 0,\n        \"p5_s\"", YRIG_ERR_FORMAT, "roles.trig.latency.n: 0 is out of range");
    REFUSE("\"n\": 200,\n        \"p5_s\"", "\"n\": -1,\n        \"p5_s\"", YRIG_ERR_FORMAT, "roles.trig.latency.n: -1 is out of range");
    REFUSE("\"sha256\": \"" C64 "\"", "\"sha256\": \"" "C" C64 "\"", YRIG_ERR_FORMAT, "roles.trig.latency.sha256: must be 64 lowercase hex digits");
    REFUSE("\"sha256\": \"" C64 "\"", "\"sha256\": \"CCcccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"", YRIG_ERR_FORMAT,
           "roles.trig.latency.sha256: must be 64 lowercase hex digits");
    REFUSE("\"date\": \"2026-10-09T13:58:12Z\",\n", "", YRIG_ERR_FORMAT, "roles.trig.latency.date: is missing");
    REFUSE("\"date\": \"2026-10-09T13:58:12Z\"", "\"date\": \"2026-10-09T25:58:12Z\"", YRIG_ERR_FORMAT, "roles.trig.latency.date: must be a UTC date");
    REFUSE("\"lo_s\": -0.0003", "\"lo_s\": 0.0012", YRIG_ERR_FORMAT, "roles.resp.bounds: lo_s <= hi_s does not hold");
    REFUSE("\"hi_s\": 0.0011,\n", "", YRIG_ERR_FORMAT, "roles.resp.bounds.hi_s: is missing");
    REFUSE("\"lo_s\": -0.0003", "\"lo_s\": -1e400", YRIG_ERR_FORMAT, "roles.resp.bounds.lo_s: -1e400 is out of range");
    REFUSE("\"bounds\": {", "\"bounds\": [], \"b2\": {", YRIG_ERR_FORMAT, "unknown key \"b2\"");
    REFUSE("\"bounds\": {\n        \"date\"", "\"bounds\": {\"x\": 1,\n        \"date\"", YRIG_ERR_FORMAT, "roles.resp.bounds: unknown key \"x\"");
    /* display */
    REFUSE("\"onset_offset_s\": 0.0123", "\"onset_offset_s\": 2", YRIG_ERR_FORMAT, "display.onset_offset_s: 2 is out of range");
    REFUSE("    \"onset_offset_s\": 0.0123\n", "    \"x\": 0\n", YRIG_ERR_FORMAT, "display: unknown key \"x\"");
    REFUSE("},\n    \"onset_offset_s\": 0.0123\n", "}\n", YRIG_ERR_FORMAT, "display.onset_offset_s: is missing");
    REFUSE("\"file\": \"display.yspcal\"", "\"file\": \"../display.yspcal\"", YRIG_ERR_FORMAT, "display.calibration.file: must be a file name");
    REFUSE("\"file\": \"display.yspcal\"", "\"file\": \"sub/display.yspcal\"", YRIG_ERR_FORMAT, "display.calibration.file: must be a file name");
    REFUSE("\"file\": \"display.yspcal\"", "\"file\": \"c:display.yspcal\"", YRIG_ERR_FORMAT, "display.calibration.file: must be a file name");
    REFUSE("\"file\": \"display.yspcal\",\n", "", YRIG_ERR_FORMAT, "display.calibration.file: is missing");
    REFUSE("\"sha256\": \"" A64 "\"", "\"sha256\": \"" A64 "\", \"z\": 1", YRIG_ERR_FORMAT, "display.calibration: unknown key \"z\"");
    REFUSE("\"sha256\": \"" A64 "\"", "\"sha256\": 5", YRIG_ERR_FORMAT, "display.calibration.sha256: must be 64 lowercase hex digits");
    /* the root is not an object; too deep; too large; too many roles */
    CHECK_I(yrig_parse(&g_q, "[]", 2, NULL, g_err, sizeof g_err), YRIG_ERR_FORMAT);
    CHECK_HAS(g_err, "the profile: must be an object");
    { static const char deep[] = "{\"format\":[[[[[[[[1]]]]]]]]}"; CHECK_I(yrig_parse(&g_q, deep, sizeof deep - 1, NULL, g_err, sizeof g_err), YRIG_ERR_JSON); }
    CHECK_HAS(g_err, "nested deeper");
    big = (char*)malloc(YRIG_MAX_BYTES + 2);
    memset(big, ' ', YRIG_MAX_BYTES + 1);
    big[0] = '{';
    big[YRIG_MAX_BYTES] = '}';
    CHECK_I(yrig_parse(&g_q, big, YRIG_MAX_BYTES + 1, NULL, g_err, sizeof g_err), YRIG_ERR_FORMAT);
    CHECK_HAS(g_err, "larger than");
    free(big);
    {
        char t[8192];
        int k, p = 0;
        p += sprintf(t + p, "{\"format\":\"ysp-rig 1\",\"rig\":\"r\",\"written\":\"2026-01-01T00:00:00Z\",\"roles\":{");
        for (k = 0; k < 33; k++) p += sprintf(t + p, "%s\"r%d\":{\"family\":\"line\",\"key\":\"k\"}", k ? "," : "", k);
        p += sprintf(t + p, "}}");
        CHECK_I(yrig_parse(&g_q, t, (size_t)p, NULL, g_err, sizeof g_err), YRIG_ERR_FORMAT);
        CHECK_HAS(g_err, "roles: more than 32 roles");
        /* 32 is fine */
        p = 0;
        p += sprintf(t + p, "{\"format\":\"ysp-rig 1\",\"rig\":\"r\",\"written\":\"2026-01-01T00:00:00Z\",\"roles\":{");
        for (k = 0; k < 32; k++) p += sprintf(t + p, "%s\"r%d\":{\"family\":\"line\",\"key\":\"k\"}", k ? "," : "", k);
        p += sprintf(t + p, "}}");
        CHECK_I(yrig_parse(&g_q, t, (size_t)p, NULL, g_err, sizeof g_err), YRIG_OK);
        CHECK_I(g_q.n_roles, 32);
    }
    CHECK_I(yrig_parse(NULL, "{}", 2, NULL, g_err, sizeof g_err), YRIG_ERR_ARG);
    CHECK_I(yrig_parse(&g_q, NULL, 2, NULL, g_err, sizeof g_err), YRIG_ERR_ARG);
    CHECK_I(yrig_parse(&g_q, "", 0, NULL, NULL, 0), YRIG_ERR_JSON);   /* no message buffer: fine */
    /* a profile in memory that its format cannot hold is not hashed */
    {
        char h[65];
        build_golden(&g_q);
        strcpy(g_q.role[0].name, "bad name");
        CHECK_I(yrig_hash(&g_q, h), YRIG_ERR_FORMAT);
        CHECK_S(h, "");
        build_golden(&g_q);
        strcpy(g_q.rig, "\xFF");
        CHECK_I(yrig_write(&g_q, g_buf, sizeof g_buf), 0);
        CHECK_I(yrig_hash(&g_q, h), YRIG_ERR_FORMAT);
    }
}

/* ------------------------------------------------- the profile in memory */

static void check_edit(void) {
    int k;
    yrig_role* r;
    char name[16];
    yrig_init(&g_q, NULL);
    CHECK_S(g_q.rig, "rig");
    CHECK(yrig_add(&g_q, "bad name") == NULL);
    CHECK(yrig_add(&g_q, "") == NULL);
    for (k = 0; k < YRIG_MAX_ROLES; k++) {
        sprintf(name, "r%d", k);
        CHECK(yrig_add(&g_q, name) != NULL);
    }
    CHECK(yrig_add(&g_q, "r5") == &g_q.role[5]);        /* found, not added */
    CHECK(yrig_add(&g_q, "one-more") == NULL);
    CHECK_I(yrig_bind(&g_q, "one-more", YBOX_LINE, "k"), YRIG_ERR_FULL);
    CHECK_I(yrig_bind(&g_q, "r1", YBOX_LINE, "k"), YRIG_OK);
    CHECK(yrig_remove(&g_q, "r0"));
    CHECK(!yrig_remove(&g_q, "r0"));
    CHECK_I(g_q.n_roles, 31);
    CHECK_S(g_q.role[0].name, "r1");
    CHECK_S(g_q.role[0].family, "line");
    CHECK_I(yrig_bind(&g_q, "r1", 0, "k"), YRIG_ERR_ARG);
    CHECK_I(yrig_bind(&g_q, "r1", YBOX_FAMILY_LAST + 1, "k"), YRIG_ERR_ARG);
    CHECK_I(yrig_bind(&g_q, "r1", YBOX_LINE, ""), YRIG_ERR_ARG);
    CHECK_I(yrig_bind(&g_q, "bad name", YBOX_LINE, "k"), YRIG_ERR_ARG);
    /* a binding clears what was measured on the old device */
    build_golden(&g_q);
    r = yrig_find(&g_q, "mm");
    CHECK(r && r->latched);
    CHECK_I(yrig_bind(&g_q, "mm", YBOX_LINES, "serial:0403:6001:AD1:"), YRIG_OK);
    CHECK(!r->latched);
    r = yrig_find(&g_q, "trig");
    CHECK(r->latency.set);
    CHECK_I(yrig_bind(&g_q, "trig", YBOX_TRIGGERBOX, "serial:0403:6001:TB9999:"), YRIG_OK);
    CHECK(!r->latency.set && r->pulse_ns == 2000000000);
    CHECK(yrig_find(&g_q, NULL) == NULL && yrig_find(NULL, "x") == NULL);
    CHECK_S(yrig_check_name(YRIG_CHECK_DIFFERS), "differs");
    CHECK_S(yrig_check_name(99), "?");
}

/* -------------------------------------------------------- files and checks */

static char g_cwd[700];
static char g_dir[800];

static bool file_exists(const char* p) {
    FILE* f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static bool write_text(const char* path, const char* s) {
    FILE* f = fopen(path, "wb");
    bool ok = f && fwrite(s, 1, strlen(s), f) == strlen(s);
    if (f) fclose(f);
    return ok;
}

/* An output latency file as examples/device/out_latency.c writes it. */
static const char LATFILE[] =
    "# ysp output latency: edge minus the time before the write (docs/devices_spec.md 12)\n"
    "format ysp-out-latency 1\n"
    "date 2026-10-09T13:58:12Z\n"
    "ysp_device 0.2.0\n"
    "out_family lines (simulated)\n"
    "n 200\n"
    "missed 0\n"
    "min_s 0.0001500\n"
    "p5_s 0.0002010\n"
    "median_s 0.0005770\n"
    "p95_s 0.0009830\n"
    "max_s 0.0021000\n"
    "mean_s 0.0006000\n"
    "samples_s 0.0001500 0.0021000\n";

static void check_files(void) {
    char path[900], sha[65], bsha[65], csha[65], lp[1000];
    yrig_loopback lb;
    yrig_role* r;
    int k;
    CHECK(getcwd(g_cwd, sizeof g_cwd) != NULL);
    snprintf(g_dir, sizeof g_dir, "%s" YRIG__SEP "rig_test_work" YRIG__SEP "rig", g_cwd);
    /* the output latency file's summary */
    CHECK_I(yrig_read_latency(LATFILE, strlen(LATFILE), &lb, g_err, sizeof g_err), YRIG_OK);
    CHECK(lb.n == 200 && lb.p5_ns == 201000 && lb.median_ns == 577000 && lb.p95_ns == 983000 && lb.max_ns == 2100000);
    CHECK_S(lb.date, "2026-10-09T13:58:12Z");
    CHECK_I(yrig_read_latency("format other 1\n", 15, &lb, g_err, sizeof g_err), YRIG_ERR_FORMAT);
    CHECK_HAS(g_err, "not an output latency file");
    CHECK_I(yrig_read_latency("format ysp-out-latency 1\nn 0\nmissed 9\n", 37, &lb, g_err, sizeof g_err), YRIG_ERR_FORMAT);
    CHECK_HAS(g_err, "without its summary");
    CHECK_I(yrig_read_latency("n 5\n", 4, &lb, g_err, sizeof g_err), YRIG_ERR_FORMAT);
    {
        /* every field but the format line: not this file */
        const char* f = strstr(LATFILE, "format ysp-out-latency 1\n");
        char t[1024];
        size_t pre = (size_t)(f - LATFILE);
        memcpy(t, LATFILE, pre);
        strcpy(t + pre, f + 25);
        CHECK_I(yrig_read_latency(t, strlen(t), &lb, g_err, sizeof g_err), YRIG_ERR_FORMAT);
        CHECK_HAS(g_err, "no format line");
    }
    CHECK_I(yrig_read_latency("format ysp-out-latency 1\r\nn x\r\n", 31, &lb, g_err, sizeof g_err), YRIG_ERR_FORMAT);
    CHECK_HAS(g_err, "bad n");
    /* store it; the profile names it */
    CHECK_I(yrig_store_loopback(g_dir, LATFILE, strlen(LATFILE), sha, g_err, sizeof g_err), YRIG_OK);
    CHECK_S(sha, lb.sha256);
    snprintf(lp, sizeof lp, "%s" YRIG__SEP "loopback" YRIG__SEP "%s.txt", g_dir, sha);
    CHECK(file_exists(lp));
    CHECK_I(yrig_store_loopback(g_dir, LATFILE, strlen(LATFILE), sha, g_err, sizeof g_err), YRIG_OK);   /* again: same file */
    CHECK_I(yrig_store_loopback(g_dir, "resp bounds\n", 12, bsha, g_err, sizeof g_err), YRIG_OK);
    snprintf(path, sizeof path, "%s" YRIG__SEP "display.yspcal", g_dir);
    CHECK(write_text(path, "a calibration"));
    sha_hex("a calibration", 13, csha);
    build_golden(&g_p);
    r = yrig_find(&g_p, "trig");
    r->latency = lb;
    r = yrig_find(&g_p, "resp");
    strcpy(r->bounds.sha256, bsha);
    strcpy(g_p.display.calibration_sha256, csha);
    snprintf(path, sizeof path, "%s" YRIG__SEP "profile.json", g_dir);
    CHECK_I(yrig_save_file(&g_p, path, g_err, sizeof g_err), YRIG_OK);
    CHECK(strcmp(g_p.written, "2026-10-09T14:03:00Z") != 0 && g_p.written[19] == 'Z');
    CHECK_I(yrig_load_file(&g_q, path, g_err, sizeof g_err), YRIG_OK);
    CHECK_S(g_q.dir, g_dir);
    CHECK_I(g_q.n_notes, 0);
    CHECK_I(yrig_find(&g_q, "trig")->latency.check, YRIG_CHECK_OK);
    CHECK_I(yrig_find(&g_q, "resp")->bounds.check, YRIG_CHECK_OK);
    CHECK_I(yrig_find(&g_q, "mm")->latency.check, YRIG_CHECK_NONE);
    CHECK_I(g_q.display.check, YRIG_CHECK_OK);
    /* the file holds the canonical bytes */
    {
        char* text;
        size_t n;
        char h1[65], h2[65];
        CHECK_I(yrig__read(path, YRIG_MAX_BYTES, &text, &n), YRIG_OK);
        CHECK_I(yrig_write(&g_q, g_buf, sizeof g_buf), n);
        CHECK(text && memcmp(text, g_buf, n) == 0);
        sha_hex(text, n, h1);
        CHECK_I(yrig_hash(&g_q, h2), YRIG_OK);
        CHECK_S(h2, h1);
        free(text);
    }
    /* the summary edited by hand: DIFFERS */
    yrig_find(&g_p, "trig")->latency.median_ns = 578000;
    CHECK_I(yrig_save_file(&g_p, path, g_err, sizeof g_err), YRIG_OK);
    CHECK_I(yrig_load_file(&g_q, path, g_err, sizeof g_err), YRIG_OK);
    CHECK_I(yrig_find(&g_q, "trig")->latency.check, YRIG_CHECK_DIFFERS);
    CHECK_I(g_q.n_notes, 1);
    CHECK_HAS(g_q.notes[0], "roles.trig.latency");
    CHECK_HAS(g_q.notes[0], "does not match the profile's summary: the role's tier is UNKNOWN");
    yrig_find(&g_p, "trig")->latency.median_ns = 577000;
    CHECK_I(yrig_save_file(&g_p, path, g_err, sizeof g_err), YRIG_OK);
    /* a loopback file changed: CHANGED */
    CHECK(write_text(lp, "format ysp-out-latency 1\nn 1\n"));
    CHECK_I(yrig_load_file(&g_q, path, g_err, sizeof g_err), YRIG_OK);
    CHECK_I(yrig_find(&g_q, "trig")->latency.check, YRIG_CHECK_CHANGED);
    CHECK_HAS(g_q.notes[0], "has changed");
    /* removed: MISSING; the calibration too */
    CHECK(remove(lp) == 0);
    snprintf(path, sizeof path, "%s" YRIG__SEP "display.yspcal", g_dir);
    CHECK(write_text(path, "another calibration"));
    snprintf(path, sizeof path, "%s" YRIG__SEP "profile.json", g_dir);
    CHECK_I(yrig_load_file(&g_q, path, g_err, sizeof g_err), YRIG_OK);
    CHECK_I(yrig_find(&g_q, "trig")->latency.check, YRIG_CHECK_MISSING);
    CHECK_I(yrig_find(&g_q, "resp")->bounds.check, YRIG_CHECK_OK);
    CHECK_I(g_q.display.check, YRIG_CHECK_CHANGED);
    CHECK_I(g_q.n_notes, 2);
    CHECK_HAS(g_q.notes[0], "is missing");
    CHECK_HAS(g_q.notes[1], "display.calibration: display.yspcal has changed");
    /* the same text without a folder: NOT_RUN, no notes */
    {
        char* text;
        size_t n;
        CHECK_I(yrig__read(path, YRIG_MAX_BYTES, &text, &n), YRIG_OK);
        CHECK_I(yrig_parse(&g_q, text, n, NULL, g_err, sizeof g_err), YRIG_OK);
        CHECK_I(yrig_find(&g_q, "trig")->latency.check, YRIG_CHECK_NOT_RUN);
        CHECK_I(g_q.n_notes, 0);
        CHECK_S(g_q.dir, "");
        free(text);
    }
    /* many failing checks: the notes stop at YRIG_MAX_NOTES */
    {
        yrig_init(&g_p, "many");
        for (k = 0; k < 20; k++) {
            char name[8];
            sprintf(name, "r%d", k);
            CHECK_I(yrig_bind(&g_p, name, YBOX_XID, "k"), YRIG_OK);
            r = yrig_find(&g_p, name);
            r->bounds.set = true;
            r->bounds.n = 1;
            strcpy(r->bounds.date, "2026-10-09T00:00:00Z");
            strcpy(r->bounds.sha256, C64);
        }
        snprintf(path, sizeof path, "%s" YRIG__SEP "many.json", g_dir);
        CHECK_I(yrig_save_file(&g_p, path, g_err, sizeof g_err), YRIG_OK);
        CHECK_I(yrig_load_file(&g_q, path, g_err, sizeof g_err), YRIG_OK);
        CHECK_I(g_q.n_notes, YRIG_MAX_NOTES);
    }
    /* files that are not there, or not profiles */
    snprintf(path, sizeof path, "%s" YRIG__SEP "nothing.json", g_dir);
    CHECK_I(yrig_load_file(&g_q, path, g_err, sizeof g_err), YRIG_ERR_MISSING);
    CHECK_HAS(g_err, "no rig profile at");
    snprintf(path, sizeof path, "%s" YRIG__SEP "display.yspcal", g_dir);
    CHECK_I(yrig_load_file(&g_q, path, g_err, sizeof g_err), YRIG_ERR_JSON);
    CHECK_HAS(g_err, "display.yspcal: line 1, column 1");
    CHECK_I(yrig_load_file(&g_q, "", g_err, sizeof g_err), YRIG_ERR_ARG);
    CHECK_I(yrig_store_loopback("", "x", 1, sha, g_err, sizeof g_err), YRIG_ERR_ARG);
    /* a profile file beside the program: its folder is "." */
    CHECK_I(yrig_save_file(&g_p, "rig_test_here.json", g_err, sizeof g_err), YRIG_OK);
    CHECK_I(yrig_load_file(&g_q, "rig_test_here.json", g_err, sizeof g_err), YRIG_OK);
    CHECK_S(g_q.dir, ".");
    remove("rig_test_here.json");
    printf("  files and checks: ok\n");
}

/* --------------------------------------------------------- the user folder */

static void set_config_home(const char* dir) {
#if defined(_WIN32)
    wchar_t w[800];
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, 800);
    SetEnvironmentVariableW(L"APPDATA", w);
#elif defined(__APPLE__)
    setenv("HOME", dir, 1);               /* $HOME/Library/Application Support */
#else
    setenv("XDG_CONFIG_HOME", dir, 1);
#endif
}

/* The rig folder yrt_user_dir gives for a config base. */
static void rig_dir_of(const char* base, char* out, size_t cap) {
#if defined(__APPLE__)
    snprintf(out, cap, "%s/Library/Application Support/ysp/rig", base);
#else
    snprintf(out, cap, "%s" YRIG__SEP "ysp" YRIG__SEP "rig", base);
#endif
}

static void check_user_folder(void) {
    char base[800], dir[800], want[900], path[900], home[700];
    snprintf(home, sizeof home, "%s", getenv("HOME") ? getenv("HOME") : "");
    snprintf(base, sizeof base, "%s" YRIG__SEP "rig_test_work" YRIG__SEP "cfg", g_cwd);
    set_config_home(base);
#if !defined(_WIN32)
    if (yrig_dir(dir, sizeof dir) != YRIG_OK && home[0]) {
        /* The working folder is under one that all users can write (/tmp),
         * which yrt_user_dir rightly refuses: use one under HOME. */
        snprintf(base, sizeof base, "%s/.cache/ysp_rigfile_test", home);
        CHECK(yrig__mkdirs(base));
        set_config_home(base);
        printf("  user folder: the working folder is under a folder all users can write; using %s\n", base);
    }
#endif
    CHECK_I(yrig_dir(dir, sizeof dir), YRIG_OK);
    rig_dir_of(base, want, sizeof want);
    CHECK_S(dir, want);
    { char tiny[4]; CHECK_I(yrig_dir(tiny, sizeof tiny), YRIG_ERR_FOLDER); }
    CHECK_I(yrig_dir(NULL, 4), YRIG_ERR_ARG);
    CHECK_I(yrig_path(dir, NULL, path, sizeof path), YRIG_OK);
    snprintf(want, sizeof want, "%s" YRIG__SEP "profile.json", dir);
    CHECK_S(path, want);
    CHECK_I(yrig_path(dir, "profile", path, sizeof path), YRIG_OK);
    CHECK_S(path, want);
    CHECK_I(yrig_path(dir, "booth-2_b", path, sizeof path), YRIG_OK);
    CHECK_I(yrig_path(dir, "../x", path, sizeof path), YRIG_ERR_ARG);
    CHECK_I(yrig_path(dir, "a.json", path, sizeof path), YRIG_ERR_ARG);
    CHECK_I(yrig_path(dir, "a b", path, sizeof path), YRIG_ERR_ARG);
    CHECK_I(yrig_path(dir, "a/b", path, sizeof path), YRIG_ERR_ARG);
    CHECK_I(yrig_path(dir, "a", path, 8), YRIG_ERR_ARG);
    CHECK_S(path, "");
    CHECK_I(yrig_path("", "a", path, sizeof path), YRIG_ERR_ARG);
    /* nothing there yet (a run before this one left its files) */
    remove(want);
    {
        char b2[900];
        CHECK_I(yrig_path(dir, "booth2", b2, sizeof b2), YRIG_OK);
        remove(b2);
    }
    CHECK_I(yrig_load(&g_q, NULL, g_err, sizeof g_err), YRIG_ERR_MISSING);
    /* save makes the folders */
    build_golden(&g_p);
    CHECK_I(yrig_save(&g_p, NULL, g_err, sizeof g_err), YRIG_OK);
    CHECK(file_exists(want));
    CHECK_I(yrig_load(&g_q, NULL, g_err, sizeof g_err), YRIG_OK);
    CHECK_S(g_q.dir, dir);
    CHECK_I(g_q.n_roles, 3);
    CHECK_I(g_q.n_notes, 3);       /* the golden profile's files are not there */
    strcpy(g_p.rig, "second rig");
    CHECK_I(yrig_save(&g_p, "booth2", g_err, sizeof g_err), YRIG_OK);
    CHECK_I(yrig_load(&g_q, "booth2", g_err, sizeof g_err), YRIG_OK);
    CHECK_S(g_q.rig, "second rig");
    CHECK_I(yrig_load(&g_q, "booth3", g_err, sizeof g_err), YRIG_ERR_MISSING);
    CHECK_I(yrig_load(&g_q, "../booth2", g_err, sizeof g_err), YRIG_ERR_ARG);
    CHECK_HAS(g_err, "is not a profile name");
    CHECK_I(yrig_save(&g_p, "a.b", g_err, sizeof g_err), YRIG_ERR_ARG);
    /* no config folder: a relative variable is none, and nothing else is tried */
    set_config_home("relative" YRIG__SEP "cfg");
#if defined(_WIN32)
    CHECK_I(yrig_load(&g_q, NULL, g_err, sizeof g_err), YRIG_ERR_FOLDER);
    CHECK_HAS(g_err, "no rig folder");
    CHECK_I(yrig_save(&g_p, NULL, g_err, sizeof g_err), YRIG_ERR_FOLDER);
#else
    /* XDG_CONFIG_HOME relative is ignored: HOME's folder; leave HOME alone */
#endif
#if !defined(_WIN32)
    {
        /* a planted folder: all users can write it */
        char planted[900];
        snprintf(planted, sizeof planted, "%s/rig_test_work/planted", g_cwd);
        mkdir(planted, 0777);
        CHECK(chmod(planted, 0777) == 0);
        set_config_home(planted);
        CHECK_I(yrig_dir(dir, sizeof dir), YRIG_ERR_FOLDER);
        CHECK_I(yrig_load(&g_q, NULL, g_err, sizeof g_err), YRIG_ERR_FOLDER);
        CHECK_HAS(g_err, "another user can write it");
        CHECK_I(yrig_save(&g_p, NULL, g_err, sizeof g_err), YRIG_ERR_FOLDER);
        CHECK(!file_exists("rig_test_work/planted/ysp/rig/profile.json"));
        /* a good base with a planted rig folder under it */
        set_config_home(base);
        rig_dir_of(base, planted, sizeof planted);
        CHECK(chmod(planted, 0777) == 0);
        CHECK_I(yrig_load(&g_q, NULL, g_err, sizeof g_err), YRIG_ERR_FOLDER);
        CHECK(chmod(planted, 0700) == 0);
        CHECK_I(yrig_load(&g_q, NULL, g_err, sizeof g_err), YRIG_OK);
        printf("  user folder: planted folders refused\n");
    }
#else
    printf("  user folder: the planted-folder check is POSIX only (yrt_user_dir reads no ACL on Windows)\n");
#endif
#if defined(__APPLE__)
    setenv("HOME", home, 1);
#else
    set_config_home(base);
#endif
}

/* ------------------------------------------------- binding through a fake */

typedef struct fakebox {
    char     key[160];
    uint8_t  bytes[256];
    int      n, opens;
} fakebox;
static fakebox g_fb;
static int64_t g_now = 1000000000;
static int64_t now_fn(void* ctx) { (void)ctx; return g_now; }

static void* f_open(void* user, const char* key, char* found, size_t cap) {
    fakebox* f = (fakebox*)user;
    snprintf(f->key, sizeof f->key, "%s", key);
    f->opens++;
    snprintf(found, cap, "fake");
    return f;
}
static int f_read(void* conn, uint8_t* buf, int cap, int64_t timeout_ns) {
    (void)conn; (void)buf; (void)cap;
    g_now += timeout_ns;
    return 0;
}
static int f_write(void* conn, const uint8_t* buf, int n) {
    fakebox* f = (fakebox*)conn;
    if (f->n + n <= (int)sizeof f->bytes) { memcpy(f->bytes + f->n, buf, (size_t)n); f->n += n; }
    return n;
}
static int f_lines(void* conn, uint32_t code, uint32_t changed) { (void)conn; (void)code; (void)changed; return 0; }
static void f_close(void* conn) { (void)conn; }

static void check_binding(void) {
    static ydev_device dev;
    static ydev_roles roles;
    ydev_desc base, d;
    yin_source s;
    build_golden(&g_p);
    memset(&base, 0, sizeof base);
    base.transport.open = f_open;
    base.transport.read = f_read;
    base.transport.write = f_write;
    base.transport.close = f_close;
    base.transport.lines = f_lines;
    base.transport.user = &g_fb;
    base.manual = true;
    base.now = now_fn;
    base.probe_ns = -1;
    base.no_identify = true;
    base.roles = &roles;
    base.pulse_ns = 5000000;
    /* the desc of each role */
    d = base;
    CHECK_I(yrig_desc(&g_p, "trig", &d, g_err, sizeof g_err), YRIG_OK);
    CHECK_S(d.role, "trig");
    CHECK_I(d.family, YBOX_TRIGGERBOX);
    CHECK_S(d.key, "serial:0403:6001:TB0123:");
    CHECK_I(d.pulse_ns, 2000000000);
    CHECK(d.transport.open == f_open && d.roles == &roles);
    d = base;
    CHECK_I(yrig_desc(&g_p, "mm", &d, g_err, sizeof g_err), YRIG_OK);
    CHECK(d.latched && d.family == YBOX_MMBTS && d.pulse_ns == 5000000);
    d = base;
    CHECK_I(yrig_desc(&g_p, "resp", &d, g_err, sizeof g_err), YRIG_OK);
    CHECK(d.baud == 115200 && d.family == YBOX_XID && !d.latched);
    CHECK_I(yrig_desc(&g_p, "eye", &d, g_err, sizeof g_err), YRIG_ERR_MISSING);
    CHECK_HAS(g_err, "has no role \"eye\"");
    CHECK(yrig_add(&g_p, "unbound") != NULL);
    CHECK_I(yrig_desc(&g_p, "unbound", &d, g_err, sizeof g_err), YRIG_ERR_FORMAT);
    CHECK_HAS(g_err, "roles.unbound: no device is bound");
    CHECK_I(yrig_desc(NULL, "trig", &d, g_err, sizeof g_err), YRIG_ERR_ARG);
    /* start the output from the profile: the key reaches the transport,
     * the role its index, and the box gets its bytes */
    g_p.role[yrig_find(&g_p, "trig") - g_p.role].pulse_ns = 2000000;
    memset(&g_fb, 0, sizeof g_fb);
    CHECK(yrig_open_device(&dev, &g_p, "trig", &base));
    CHECK_I(ydev_wait_running(&dev, YRT_MS(5)), YDEV_RUNNING);
    CHECK_S(g_fb.key, "serial:0403:6001:TB0123:");
    CHECK_I(ydev_role_index(&roles, "trig"), 1);
    CHECK(ydev_role_device(&roles, 1) == &dev);
    CHECK_I(dev.d.pulse_ns, 2000000);
    CHECK_I(ydev_out_set(&dev, 5), YDEV_OK);
    CHECK_I(ydev_out_mark(&dev, 7, "m"), YDEV_OK);      /* desc.pulse_ns from the profile: 2 ms */
    {
        int64_t t0 = g_now;
        while (g_now < t0 + 3000000) (void)ydev_poll(&dev, YRT_MS(1));
    }
    CHECK(g_fb.n == 4 && g_fb.bytes[0] == 0 && g_fb.bytes[1] == 5 && g_fb.bytes[2] == 7 && g_fb.bytes[3] == 0);
    s = yrig_source(&g_p, &dev);
    CHECK_I(s.tier, YIN_TIER_UNKNOWN);                  /* an output: no input bounds */
    {
        /* bounds on an output family, checked: still no tier (tier 1 is a
         * device clock's, and a trigger box has none) */
        yrig_role* tr = yrig_find(&g_p, "trig");
        tr->bounds.set = true;
        tr->bounds.check = YRIG_CHECK_OK;
        CHECK_I(yrig_source(&g_p, &dev).tier, YIN_TIER_UNKNOWN);
        tr->bounds.set = false;
        tr->bounds.check = YRIG_CHECK_NONE;
    }
    ydev_close(&dev);
    /* an input role whose bounds were checked: tier 1 with its bounds */
    CHECK(yrig_open_device(&dev, &g_p, "resp", &base));
    s = yrig_source(&g_p, &dev);
    CHECK_I(s.tier, YIN_TIER_UNKNOWN);                  /* NOT_RUN: no folder */
    yrig_find(&g_p, "resp")->bounds.check = YRIG_CHECK_OK;
    s = yrig_source(&g_p, &dev);
    CHECK_I(s.tier, YIN_TIER_1);
    CHECK_I(s.lo_us, -300);
    CHECK_I(s.hi_us, 1100);
    CHECK_I(s.device, (int)dev.d.device);
    CHECK_HAS(s.note, "checked by SHA-256");
    yrig_find(&g_p, "resp")->bounds.check = YRIG_CHECK_CHANGED;
    s = yrig_source(&g_p, &dev);
    CHECK(s.tier == YIN_TIER_UNKNOWN && s.lo_us == 0 && s.hi_us == 0);
    yrig_find(&g_p, "resp")->bounds.check = YRIG_CHECK_OK;
    CHECK_I(yrig_source(&g_q, &dev).tier, YIN_TIER_UNKNOWN);   /* another profile without the role */
    ydev_close(&dev);
    /* a start that the device layer refuses names the role */
    d = base;
    d.device = 0;
    d.roles = NULL;
    CHECK(!yrig_open_device(&dev, &g_p, "trig", &d));
    CHECK_HAS(ydev_error(&dev), "roles.trig: ysp_device: desc.device");
    ydev_close(&dev);                                   /* after a failed open */
    /* refused by the profile before the open: the message is in the
     * device's place too (v0.3.0) */
    CHECK(!yrig_open_device(&dev, &g_p, "eye", &base));
    CHECK_HAS(ydev_error(&dev), "has no role \"eye\"");
    CHECK(!yrig_open_device(&dev, &g_p, "unbound", &base));
    CHECK_HAS(ydev_error(&dev), "roles.unbound: no device is bound");
    ydev_close(&dev);
    ydev_close(&dev);                                   /* twice */
    CHECK(!yrig_open_device(NULL, &g_p, "trig", &base));
    {
        /* the device keeps its own copy of the role and the key: the
         * profile may go (v0.3.0, ysp/device.h copies them) */
        static yrig_profile tmp;
        tmp = g_p;
        CHECK(yrig_open_device(&dev, &tmp, "trig", &base));
        memset(&tmp, 0, sizeof tmp);
        CHECK_S(dev.d.key, "serial:0403:6001:TB0123:");
        CHECK_S(dev.d.role, "trig");
        ydev_close(&dev);
    }
    /* a binder rebinds a role and saves it */
    CHECK_I(yrig_bind(&g_p, "trig", YBOX_LINES, "serial:0403:6001:AD1:"), YRIG_OK);
    d = base;
    CHECK_I(yrig_desc(&g_p, "trig", &d, g_err, sizeof g_err), YRIG_OK);
    CHECK(d.family == YBOX_LINES && strcmp(d.key, "serial:0403:6001:AD1:") == 0);
    printf("  binding through a fake transport: ok\n");
}

/* ---------------------------------------------- random damage, round trips */

static void check_damage(void) {
    static char t[8192], t1[8192], t2[8192];
    int k, ok = 0, bad = 0;
    size_t gn = sizeof GOLDEN - 1;
    for (k = 0; k < 20000; k++) {
        size_t n = gn, j;
        int edits = 1 + rnd(3), m;
        memcpy(t, GOLDEN, gn);
        for (m = 0; m < edits; m++) {
            j = (size_t)rnd((int)n);
            switch (rnd(3)) {
            case 0: {
                static const char tok[] = "{}[]\",:0123456789.e-abtfn \n";
                t[j] = tok[rnd((int)sizeof tok - 1)];
            } break;
            case 1: if (n > 1) { memmove(t + j, t + j + 1, n - j - 1); n--; } break;
            default: if (n < sizeof t - 1) { memmove(t + j + 1, t + j, n - j); t[j] = (char)(32 + rnd(95)); n++; } break;
            }
        }
        g_err[0] = 0;
        if (yrig_parse(&g_q, t, n, NULL, g_err, sizeof g_err) == YRIG_OK) {
            size_t n1 = yrig_write(&g_q, t1, sizeof t1), n2;
            if (!n1 || yrig_parse(&g_q, t1, n1, NULL, g_err, sizeof g_err) != YRIG_OK) { bad++; continue; }
            n2 = yrig_write(&g_q, t2, sizeof t2);
            if (n1 != n2 || memcmp(t1, t2, n1) != 0) bad++;
            ok++;
        } else if (!g_err[0] || g_q.n_roles != 0) {
            bad++;
        }
    }
    CHECK_I(bad, 0);
    printf("  damage: 20000 edits of the golden profile, %d still valid, each canonical and stable\n", ok);
}

/* ------------------------------------------------- lsl roles (v0.2.0) */

static void check_lsl(void) {
    static const char T[] =
        "{\n"
        "  \"format\": \"ysp-rig 1\",\n"
        "  \"rig\": \"r\",\n"
        "  \"roles\": {\n"
        "    \"lat\": {\n"
        "      \"family\": \"lsl\",\n"
        "      \"key\": \"lsl:Latencies:::\"\n"
        "    },\n"
        "    \"ref\": {\n"
        "      \"bounds\": {\n"
        "        \"date\": \"2026-10-09T09:00:00Z\",\n"
        "        \"hi_s\": 0.0002,\n"
        "        \"lo_s\": -0.0001,\n"
        "        \"n\": 300,\n"
        "        \"sha256\": \"" B64 "\"\n"
        "      },\n"
        "      \"family\": \"lsl\",\n"
        "      \"key\": \"lsl:Data::LabStreamer:\",\n"
        "      \"options\": {\n"
        "        \"buttons\": {\n"
        "          \"1\": \"photodiode\"\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "  },\n"
        "  \"written\": \"2026-10-09T14:03:00Z\"\n"
        "}\n";
    static char t[4096];
    ydev_desc d;
    yin_source s;
    size_t n;
    /* read, written back to the same bytes */
    CHECK_I(yrig_parse(&g_q, T, strlen(T), NULL, g_err, sizeof g_err), YRIG_OK);
    CHECK_I(g_q.n_roles, 2);
    CHECK_I(yrig_find(&g_q, "ref")->family_id, YRIG_FAMILY_LSL);
    CHECK_S(yrig_find(&g_q, "ref")->family, "lsl");
    n = yrig_write(&g_q, g_buf, sizeof g_buf);
    CHECK(n == strlen(T) && memcmp(g_buf, T, n) == 0);
    /* the binding: refused for ysp/device.h, with the header to use */
    memset(&d, 0, sizeof d);
    CHECK_I(yrig_desc(&g_q, "ref", &d, g_err, sizeof g_err), YRIG_ERR_FORMAT);
    CHECK_HAS(g_err, "roles.ref is an LSL stream: start it with ysp/net.h");
    CHECK(d.key == NULL);
    /* the tier by role name: 1 only when the bounds checked */
    memset(&s, 0, sizeof s);
    s.kind = YIN_KIND_SYNC;
    s.tier = YIN_TIER_2;
    s.note = "the inlet's";
    CHECK_I(yrig_source_role(&g_q, "ref", s).tier, YIN_TIER_UNKNOWN);   /* NOT_RUN */
    yrig_find(&g_q, "ref")->bounds.check = YRIG_CHECK_OK;
    s = yrig_source_role(&g_q, "ref", s);
    CHECK(s.tier == YIN_TIER_1 && s.lo_us == -100 && s.hi_us == 200 && s.kind == YIN_KIND_SYNC);
    CHECK_I(yrig_source_role(&g_q, "lat", s).tier, YIN_TIER_UNKNOWN);   /* no bounds */
    CHECK_I(yrig_source_role(&g_q, "nobody", s).tier, YIN_TIER_UNKNOWN);
    CHECK_I(yrig_source_role(NULL, "ref", s).tier, YIN_TIER_UNKNOWN);
    /* refusals */
#define LSL_REFUSE(find, repl, says) do { \
        const char* at_ = strstr(T, find); \
        size_t pre_ = at_ ? (size_t)(at_ - T) : 0; \
        g_checks++; \
        if (!at_) { fprintf(stderr, "rigfile_test: FAIL at line %d: not in T\n", __LINE__); g_failures++; break; } \
        memcpy(t, T, pre_); strcpy(t + pre_, repl); strcat(t, at_ + strlen(find)); \
        g_err[0] = '\0'; \
        if (yrig_parse(&g_q, t, strlen(t), NULL, g_err, sizeof g_err) != YRIG_ERR_FORMAT || !strstr(g_err, says)) { \
            fprintf(stderr, "rigfile_test: FAIL at line %d: '%s' (want '%s')\n", __LINE__, g_err, says); g_failures++; } \
    } while (0)
    LSL_REFUSE("\"key\": \"lsl:Latencies:::\"", "\"key\": \"serial:0403:6001::\"", "roles.lat.key: an lsl key is lsl:");
    LSL_REFUSE("\"key\": \"lsl:Latencies:::\"", "\"key\": \"lsl:o'brien\"", "roles.lat.key: an lsl key is lsl:");
    LSL_REFUSE("\"family\": \"lsl\",\n      \"key\": \"lsl:Latencies", "\"family\": \"line\",\n      \"key\": \"lsl:Latencies",
               "roles.lat.key: an lsl key needs the family lsl");
    LSL_REFUSE("\"key\": \"lsl:Latencies:::\"", "\"key\": \"lsl:Latencies:::\", \"pulse_s\": 0.002", "roles.lat.pulse_s: an lsl role has no pulse");
    LSL_REFUSE("\"buttons\": {", "\"baud\": 9600, \"buttons\": {", "roles.ref.options: an lsl role has no serial options");
    LSL_REFUSE("\"buttons\": {", "\"latched\": false, \"buttons\": {", "roles.ref.options: an lsl role has no serial options");
    LSL_REFUSE("\"family\": \"lsl\",\n      \"key\": \"lsl:Lat", "\"family\": \"lsll\",\n      \"key\": \"lsl:Lat",
               "\"lsll\" is not a ysp/box.h family or lsl");
#undef LSL_REFUSE
    /* bind: family and key must agree; a new lsl binding drops serial options */
    yrig_init(&g_q, "r");
    CHECK_I(yrig_bind(&g_q, "ref", YRIG_FAMILY_LSL, "lsl:Data::"), YRIG_OK);
    CHECK_S(yrig_find(&g_q, "ref")->family, "lsl");
    CHECK_I(yrig_bind(&g_q, "ref", YRIG_FAMILY_LSL, "serial:0403:6001::"), YRIG_ERR_ARG);
    CHECK_I(yrig_bind(&g_q, "ref", YBOX_LINE, "lsl:Data::"), YRIG_ERR_ARG);
    CHECK_I(yrig_bind(&g_q, "ref", YRIG_FAMILY_LSL, "lsl:it's"), YRIG_ERR_ARG);
    CHECK_I(yrig_bind(&g_q, "ref", 63, "lsl:Data::"), YRIG_ERR_ARG);
    CHECK_I(yrig_bind(&g_q, "x", YBOX_XID, "serial:0403:6001:FT1:"), YRIG_OK);
    yrig_find(&g_q, "x")->baud = 115200;
    yrig_find(&g_q, "x")->pulse_ns = 2000000;
    CHECK_I(yrig_bind(&g_q, "x", YRIG_FAMILY_LSL, "lsl:Markers"), YRIG_OK);
    CHECK(yrig_find(&g_q, "x")->baud == 0 && yrig_find(&g_q, "x")->pulse_ns == -1);
    n = yrig_write(&g_q, g_buf, sizeof g_buf);
    CHECK(n > 0 && yrig_parse(&g_p, g_buf, n, NULL, g_err, sizeof g_err) == YRIG_OK);
}

int main(void) {
    printf("ysp_rig %s (ysp_json %s, ysp_device %s)\n", yrig_version(), yjs_version(), ydev_version());
    CHECK_S(yrig_version(), YRIG_VERSION_STRING);
    check_sha();
    check_round_trip();
    check_refusals();
    check_edit();
    check_files();
    check_user_folder();
    check_binding();
    check_lsl();
    check_damage();
    if (g_failures) {
        fprintf(stderr, "rigfile_test: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("rigfile_test: all %d checks passed\n", g_checks);
    return 0;
}
