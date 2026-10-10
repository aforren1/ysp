/* make_c_trial.c - writes tests/c_trial.bin: a trial run by the C header
 * alone, as a rig logs it, for test_rdk.py to replay through the binding.
 *
 *     cc -O2 -I../../../../include -o make_c_trial make_c_trial.c -lm && ./make_c_trial c_trial.bin
 *     cl /O2 /I..\..\..\..\include make_c_trial.c && make_c_trial c_trial.bin
 *
 * The field is LL in a 8 x 6 rect with a 0.3 s lifetime, 60 dots shown;
 * coherence, direction and speed change on every update, the onsets jitter
 * and one frame is dropped. The file, little-endian:
 *   "RDKT", u32 1, u64 seed, i64 t0, u32 steps, u32 n
 *   steps x (i64 t, f32 coherence, f32 direction, f32 speed, i32 set):
 *       rdk.last after each update
 *   f32 xy[steps][n][2], f32 direction[steps][n], u8 signal[steps][n]:
 *       yrdk_write() after each update
 *   u64 digest at the end
 * test_rdk.py holds the same desc. Run it again only when the header's
 * rules change (YRDK_VERSION_STRING), and commit the new file.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_RDK_IMPLEMENTATION
#include "ysp/rdk.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define STEPS 150
#define N 60

static float xy[STEPS][N][2], dirs[STEPS][N];
static uint8_t sig[STEPS][N];
static yrdk_step steps[STEPS];

static void put(FILE* fp, const void* p, size_t n) { fwrite(p, 1, n, fp); }   /* x86 and arm64: little-endian */

int main(int argc, char** argv) {
    static yrdk_field f;
    yrdk_desc d;
    const uint64_t seed = 0x5eed0000c0ffee01ull;
    const int64_t t0 = 5000000000LL;
    int64_t t = t0;
    uint32_t u;
    int k;
    FILE* fp;
    uint64_t dg;
    if (argc != 2) { fprintf(stderr, "usage: make_c_trial OUT.bin\n"); return 2; }
    memset(&d, 0, sizeof d);
    d.algorithm = YRDK_LL; d.aperture = YRDK_RECT; d.w = 8; d.h = 6; d.count = N;
    /* 0.3 s as v0.1 rounded the float 0.3f, so c_trial.bin is unchanged */
    d.lifetime_ns = (int64_t)llround((double)0.3f * 1e9);
    d.coherence = 0.5f; d.direction = 0; d.speed = 4;
    if (!yrdk_open(&f, &d)) { fprintf(stderr, "%s\n", yrdk_error(&f)); return 1; }
    yrdk_start(&f, seed, t0);
    for (k = 0; k < STEPS; k++) {
        yrdk_out o;
        t += 16666667 + (int64_t)(((k * 7919) % 101) - 50) * 1000 + (k == 70 ? 16666667 : 0);
        f.coherence = (float)(k % 9) / 8.0f;
        f.direction = (float)(k * 7) + 0.25f;
        f.speed = (k / 30) % 2 ? -3.5f : 4.0f;
        yrdk_update(&f, t);
        steps[k] = f.last;
        memset(&o, 0, sizeof o);
        o.xy = &xy[k][0][0]; o.dir = dirs[k]; o.signal = sig[k];
        yrdk_write(&f, &o);
    }
    dg = yrdk_digest(&f);
    fp = fopen(argv[1], "wb");
    if (!fp) return 1;
    put(fp, "RDKT", 4);
    u = 1; put(fp, &u, 4);
    put(fp, &seed, 8); put(fp, &t0, 8);
    u = STEPS; put(fp, &u, 4);
    u = N; put(fp, &u, 4);
    for (k = 0; k < STEPS; k++) {
        put(fp, &steps[k].t, 8); put(fp, &steps[k].coherence, 4); put(fp, &steps[k].direction, 4);
        put(fp, &steps[k].speed, 4); put(fp, &steps[k].set, 4);
    }
    put(fp, xy, sizeof xy); put(fp, dirs, sizeof dirs); put(fp, sig, sizeof sig);
    put(fp, &dg, 8);
    fclose(fp);
    printf("wrote %s: %d steps of %d dots, digest %016llx\n", argv[1], STEPS, N, (unsigned long long)dg);
    yrdk_close(&f);
    return 0;
}
