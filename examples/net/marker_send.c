/* marker_send.c - an LSL marker outlet on ysp/net.h, and what a mark costs.
 *
 *   net_marker_send [--key lsl:NAME:TYPE:SOURCE_ID] [--int] [--n N]
 *                   [--interval SECONDS] [--wait SECONDS] [--lib PATH]
 *
 *   Makes a marker stream (string by default, int32 with --int; key
 *   lsl:ysp-markers:Markers:ysp-marker-send), waits up to --wait seconds
 *   (10) for an inlet to connect (LabRecorder, net_stream_view, a
 *   LabStreamer), then sends N marks (200) every --interval seconds (0.1),
 *   "mark 1", "mark 2", ... with codes 1, 2, .... It prints the time each
 *   call took on the ysp_rt clock (from just before liblsl's push to just
 *   after it: min, p50, p99, max) and the gap between the LSL stamp and the
 *   outlet's local fit, which is how far liblsl's clock and ysp_rt drift
 *   apart during the run.
 *
 *   With a viewer in a second terminal:
 *       net_stream_view --key lsl:ysp-markers --seconds 30
 *
 * Exit code 0 when every mark was sent; 1 when one failed or no inlet
 * connected; 2 for a usage error or no liblsl.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_NET_IMPLEMENTATION
#include "ysp/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_N 100000

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : (x > y);
}

int main(int argc, char** argv) {
    static ynet_lsl lib;
    static ynet_outlet out;
    static int64_t cost[MAX_N];
    ynet_outlet_desc d;
    ynet_out_info info;
    const char* key = "lsl:ysp-markers:Markers:ysp-marker-send";
    const char* path = NULL;
    double interval = 0.1, wait = 10.0, drift_max = 0;
    int n = 200, as_int = 0, i, failed = 0, rc;
    int64_t next, end;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
        else if (!strcmp(argv[i], "--int")) as_int = 1;
        else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval = atof(argv[++i]);
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait = atof(argv[++i]);
        else if (!strcmp(argv[i], "--lib") && i + 1 < argc) path = argv[++i];
        else {
            fprintf(stderr, "usage: net_marker_send [--key lsl:NAME:TYPE:SOURCE_ID] [--int] [--n N] "
                            "[--interval SECONDS] [--wait SECONDS] [--lib PATH]\n");
            return 2;
        }
    }
    if (n < 1 || n > MAX_N || interval < 0) { fprintf(stderr, "n: 1 to %d; interval >= 0\n", MAX_N); return 2; }
    if ((rc = ynet_lsl_load(&lib, path)) != YNET_OK) { fprintf(stderr, "%s\n", lib.error); return 2; }
    printf("liblsl %d.%d from %s\n", (int)lib.version / 100, (int)lib.version % 100, lib.path);
    memset(&d, 0, sizeof d);
    d.lib = &lib;
    d.role = "marks";
    d.key = key;
    d.device = 1;
    d.format = as_int ? YNET_INT32 : YNET_STRING;
    if (!ynet_outlet_start(&out, &d)) { fprintf(stderr, "%s\n", ynet_outlet_error(&out)); ynet_lsl_unload(&lib); return 2; }
    (void)yrt_thread_elevate(NULL);
    printf("outlet %s (%s); waiting up to %.0f s for an inlet\n", key, as_int ? "int32" : "string", wait);
    end = yrt_now_ns() + (int64_t)(wait * 1e9);
    while (yrt_now_ns() < end && !ynet_outlet_consumers(&out)) (void)yrt_sleep_ns(50000000);
    if (!ynet_outlet_consumers(&out)) {
        fprintf(stderr, "no inlet connected in %.0f s\n", wait);
        ynet_outlet_stop(&out);
        ynet_lsl_unload(&lib);
        return 1;
    }
    next = yrt_now_ns() + 200000000;
    for (i = 0; i < n; i++) {
        char text[32];
        double drift;
        (void)yrt_sleep_until(next, 1000000);
        snprintf(text, sizeof text, "mark %d", i + 1);
        if (ynet_out_mark(&out, (uint32_t)(i + 1), text) != YNET_OK) failed++;
        ynet_out_last(&out, &info);
        cost[i] = info.t_after - info.t_before;
        /* the LSL stamp against the outlet's map of the bracket's middle */
        drift = info.lsl_s - ynet_outlet_to_lsl(&out, (info.t_before + info.t_after) / 2);
        if (drift < 0) drift = -drift;
        if (drift > drift_max) drift_max = drift;
        next += (int64_t)(interval * 1e9);
    }
    qsort(cost, (size_t)n, sizeof cost[0], cmp_i64);
    printf("%d marks, %d failed; a mark's call (push bracket) us: min %.1f p50 %.1f p99 %.1f max %.1f\n", n, failed,
           (double)cost[0] / 1e3, (double)cost[n / 2] / 1e3, (double)cost[n * 99 / 100] / 1e3,
           (double)cost[n - 1] / 1e3);
    printf("LSL stamp against the local fit: max |%.1f| us\n", drift_max * 1e6);
    ynet_outlet_stop(&out);
    ynet_lsl_unload(&lib);
    return failed ? 1 : 0;
}
