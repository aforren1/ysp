/* list_cost.c - what the SetupAPI walk costs: yser_list_ports() n times
 * (the walk yser_open() does since v0.6.1 to read its port's driver, plus
 * the other properties). Prints p50, p90, p99, max in microseconds. */
#define YSP_SERIAL_IMPLEMENTATION
#include "ysp/serial.h"
#include <stdio.h>
#include <stdlib.h>

static int cmp(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 100;
    static int64_t v[1000];
    yser_port_info info[16];
    int k = 0;
    if (n > 1000) n = 1000;
    for (int i = 0; i < n; i++) {
        int64_t t0 = yrt_now_ns();
        k = yser_list_ports(info, 16);
        v[i] = yrt_now_ns() - t0;
        yrt_sleep_ns(YRT_MS(10));
    }
    qsort(v, (size_t)n, sizeof v[0], cmp);
    for (int i = 0; i < k && i < 16; i++) printf("%s driver \"%s\" %04x:%04x\n", info[i].name, info[i].driver, info[i].vid, info[i].pid);
    printf("list_ports n %d  p50 %.1f  p90 %.1f  p99 %.1f  max %.1f us\n", n, (double)v[n / 2] / 1e3,
           (double)v[n * 9 / 10] / 1e3, (double)v[n * 99 / 100] / 1e3, (double)v[n - 1] / 1e3);
    return 0;
}
