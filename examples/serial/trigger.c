/* serial_trigger.c - minimal ysp_serial demo: list ports, send a trigger
 * byte, then echo whatever the device sends back for a short while.
 *
 * Build (from the repository root):
 *     cc -O2 -Iinclude -o serial_trigger examples/serial/trigger.c   # Linux / macOS
 *     cl /O2 /Iinclude examples\serial\trigger.c                     # Windows (MSVC)
 * or:  cmake -B build && cmake --build build
 *
 * Usage: serial_trigger <device> [trigger_byte] [baud]
 *     serial_trigger COM3 0x2A
 *     serial_trigger /dev/ttyUSB0 42 115200
 * With no arguments it lists the ports it can see and exits.
 *
 * Exit codes: 0 success, 1 the port could not be opened or used, 2 no
 * arguments (it then lists the ports it can see).
 */
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void list_ports(void) {
    yser_port_info ports[32];
    int n = yser_list_ports(ports, 32);
    if (n < 0) { fprintf(stderr, "list_ports: %s\n", yser_strerror(n)); return; }
    printf("%d serial port(s) detected\n", n);
    for (int i = 0; i < n && i < 32; i++) {
        printf("  %-24s %s", ports[i].name, ports[i].description);
        if (ports[i].vid) printf("  [%04X:%04X %s @ %s]", ports[i].vid, ports[i].pid,
                                 ports[i].serial_number, ports[i].location);
        printf("\n");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        list_ports();
        fprintf(stderr, "usage: %s <device> [trigger_byte] [baud]\n", argv[0]);
        return 2;
    }
    unsigned long code = (argc > 2) ? strtoul(argv[2], NULL, 0) : 0x01;

    /* Zeroed, as the header requires: yser_open() refuses a handle whose
     * is_open flag is set, and an automatic variable's is set to whatever was
     * on the stack. */
    yser_port sp;
    memset(&sp, 0, sizeof sp);
    yser_desc desc = {
        .device      = argv[1],
        .baud        = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0,  /* 0 = 115200 */
        .low_latency = true,   /* FTDI: 16 ms -> 1 ms RX latency where the OS allows */
    };
    /* To find a box by USB id instead of naming the port:
     *     yser_port_filter f = { .vid = 0x0403, .pid = 0x6001 };
     *     yser_port_info info;
     *     if (yser_find_ports(&f, &info, 1) == 1) desc.device = info.name;
     */

    if (!yser_open(&sp, &desc)) {
        fprintf(stderr, "yser_open failed: %s\n", yser_error(&sp));
        return 1;
    }
    printf("port open: %s @ %u baud, %u%c%d, low_latency=%d\n",
           sp.device, sp.baud, sp.data_bits,
           sp.parity == YSER_PARITY_NONE ? 'N' : sp.parity == YSER_PARITY_EVEN ? 'E' : 'O',
           sp.stop_bits == YSER_STOP_BITS_2 ? 2 : 1, sp.low_latency ? 1 : 0);

    /* Devices that reset on DTR (Arduino-style) need a moment before they
     * listen; drop anything they printed while booting. Everything but
     * yser_open() reports through its return value, never yser_error(). */
    int rc = yser_purge(&sp, YSER_PURGE_RX);
    if (rc < 0) fprintf(stderr, "purge: %s (oserr %d)\n", yser_strerror(rc), sp.rd_oserr);

    /* 2 ms trigger pulse: `code`, then 0x00. Bracket it with the library's
     * clock so the log has both bounds of the onset. */
    int64_t t0 = yrt_now_ns();
    int r = yser_pulse(&sp, (uint8_t)code, 0x00, YRT_MS(2));
    int64_t t1 = yrt_now_ns();
    if (r < 0) {
        fprintf(stderr, "pulse failed: %s (oserr %d)\n", yser_strerror(r), sp.wr_oserr);
        yser_close(&sp);
        return 1;
    }
    printf("sent trigger 0x%02lX (2 ms blocking pulse), accepted between +0 and +%lld us\n",
           code & 0xFF, (long long)((t1 - t0) / 1000));

    /* Echo incoming bytes for ~2 s. YSER_READ_ANY returns as soon as anything
     * arrives, so the 100 ms timeout only bounds the idle wait. */
    printf("listening for 2 s...\n");
    uint8_t buf[64];
    for (int i = 0; i < 20; i++) {
        int n = yser_read(&sp, buf, (int)sizeof(buf), YRT_MS(100), YSER_READ_ANY);
        if (n < 0) { fprintf(stderr, "read failed: %s (oserr %d)\n", yser_strerror(n), sp.rd_oserr); break; }
        for (int k = 0; k < n; k++) printf("%02X ", buf[k]);
        if (n) { printf("\n"); fflush(stdout); }
    }

    yser_close(&sp);
    return 0;
}
