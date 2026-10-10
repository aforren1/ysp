/* parallel_trigger.c - minimal ysp_parallel demo / trigger sender
 *
 * ysp/parallel.h needs its sibling ysp/rt.h beside it; both are in
 * include/ysp/, so -Iinclude / /Iinclude finds the pair.
 *
 * Build (from the repository root):
 *     cc -O2 -pthread -Iinclude -o parallel_trigger examples/parallel/trigger.c   # Linux
 *     cl /O2 /Iinclude examples\parallel\trigger.c                                # Windows (MSVC)
 * or:  cmake -B build && cmake --build build
 *
 * Run (Linux ppdev): load the ppdev driver and give yourself device access:
 *     sudo modprobe ppdev                       # creates /dev/parport0
 *     sudo usermod -aG lp $USER && newgrp lp    # one-time: grant access
 *     ./parallel_trigger
 *
 * Usage: parallel_trigger [trigger_byte]
 *
 * Exit codes: 0 success, 1 no usable port (or a write failed).
 */
#define YSP_PARALLEL_IMPLEMENTATION
#include "ysp/parallel.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <time.h>
#endif

/* Coarse sleep, only used to keep this demo alive while an async pulse is in
 * flight. Do not time triggers with it. */
static void demo_sleep_ms(unsigned ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

int main(int argc, char** argv) {
    unsigned long code = (argc > 1) ? strtoul(argv[1], NULL, 0) : 0x01;

    ypar_port pp = { 0 };
    /* All defaults: ppdev "/dev/parport0" on Linux, inpout @ LPT1 on Windows.
     * For raw x86 port I/O instead (root):
     *     ypar_desc desc = { .backend = YPAR_BACKEND_DIRECT, .base_addr = YPAR_LPT1 };
     */
    ypar_desc desc = {0};
    if (!ypar_open(&pp, &desc)) {
        fprintf(stderr, "ypar_open failed: %s\n", ypar_error(&pp));
        return 1;
    }

    /* async_policy is the ysp/rt.h scheduling rung the trailing-edge worker
     * obtained. Record it next to any jitter numbers you measure: the same
     * histogram means different things at different rungs. */
    printf("port open (backend %d, base 0x%X, async policy %s)\n",
           pp.backend, pp.base_addr, yrt_policy_name(pp.async_policy));

    /* Send a 2 ms trigger pulse carrying `code`. */
    if (ypar_pulse(&pp, (uint8_t)code, YRT_MS(2)) < 0) {
        fprintf(stderr, "pulse failed: %s\n", ypar_error(&pp));
        ypar_close(&pp);
        return 1;
    }
    printf("sent trigger 0x%02lX (2 ms blocking pulse)\n", code & 0xFF);

    /* Non-blocking pulse: returns immediately, a real-time worker thread
     * drops the line after the requested width. The onset is written here on
     * this thread, so it is not delayed by the worker waking up. */
    if (ypar_pulse_async(&pp, (uint8_t)code, YRT_MS(2)) < 0) {
        fprintf(stderr, "async pulse failed: %s\n", ypar_error(&pp));
    } else {
        printf("queued async trigger 0x%02lX (returned without blocking)\n",
               code & 0xFF);
        /* Outlast the 2 ms width. ypar_close() stops the worker and flushes
         * any pending trailing edge AT ONCE, so closing (or exiting) while a
         * pulse is in flight would cut it short instead of honoring it. Real
         * experiment code spends this time on the next trial, which is the
         * point of the async pulse. */
        demo_sleep_ms(5);
        /* The worker cannot report a failed trailing-edge write through
         * ypar_error(); it sets this sticky flag instead. */
        if (pp.async_write_failed)
            fprintf(stderr, "async trailing edge failed; lines may be held\n");
    }

    /* Read back the input lines. */
    int status = ypar_read_status(&pp);
    if (status < 0) {
        fprintf(stderr, "status read failed: %s\n", ypar_error(&pp));
        ypar_close(&pp);
        return 1;
    }
    printf("status = 0x%02X  busy=%d ack=%d paper=%d select=%d error=%d\n",
           status,
           (status & YPAR_STATUS_BUSY)   ? 1 : 0,
           (status & YPAR_STATUS_ACK)    ? 1 : 0,
           (status & YPAR_STATUS_PAPER)  ? 1 : 0,
           (status & YPAR_STATUS_SELECT) ? 1 : 0,
           (status & YPAR_STATUS_ERROR)  ? 1 : 0);

    ypar_close(&pp);
    return 0;
}
