/* rt_ring_csv.c - log a frame loop and an audio callback to CSV through
 * ysp/rt.h's event ring.
 *
 * Two producers push into one ring. A deadline worker stands in for an
 * audio callback: every 5.33 ms (256 frames at 48 kHz) it pushes an onset
 * record and a plot point. The main thread runs a 60 Hz frame loop: it
 * times its phases, pushes a flip record in the layout ysp/screen.h will
 * use, marks the frame, and at the end of every frame drains the ring to
 * CSV. The program is built in YRT_TRACE_RING mode, so the zones, the
 * plots and the frame marks go into the same ring, and so do the zones
 * ysp/rt.h puts in its own worker and waits.
 *
 * Nothing here needs hardware. The "flip" is a yrt_sleep_until() to the
 * next 16.67 ms boundary, and the "work" is a spin of a few hundred
 * microseconds per phase.
 *
 * Build (from the repository root):
 *     cc -O2 -pthread -Iinclude -o rt_ring_csv examples/rt/ring_csv.c   # Linux / macOS
 *     cl /O2 /Iinclude examples\rt\ring_csv.c                           # Windows (MSVC)
 *     emcc -O2 -pthread -sPROXY_TO_PTHREAD=1 -sEXIT_RUNTIME=1 -Iinclude \
 *          -o rt_ring_csv.js examples/rt/ring_csv.c && node rt_ring_csv.js
 * or:  cmake -B build && cmake --build build
 *
 * With YRT_NO_THREADS there is no worker, and the frame loop pushes the
 * audio records itself. Under Emscripten, write to stdout: a wasm module
 * sees no host file unless it is linked with -sNODERAWFS=1.
 *
 * Usage: rt_ring_csv [frames] [out.csv]
 *     rt_ring_csv              # 120 frames, CSV on stdout
 *     rt_ring_csv 600 log.csv  # 10 s, to a file
 *
 * Columns: seq,t_ns,tid,source,kind,aux,label,v0,v1,v2. label is the zone or
 * plot name, the message text, "flip", "onset" or "loss". v0..v2 depend on
 * the kind: a zone's duration and value, a plot's value, a flip's target,
 * dropped count and busy time, an onset's frame count and lateness, the
 * records a LOSS row says were lost and the running total.
 *
 * Exit code: 0, also when records were lost; the loss is in the CSV and in
 * the summary on stderr.
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
/* fopen() is C4996 under /W4 /WX, and fopen_s() is not portable. */
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YRT_TRACE_RING
#define YSP_RT_IMPLEMENTATION
#include "ysp/rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This program's own source number and kinds. */
#define SRC_DEMO   YRT_SRC_USER
#define KIND_FLIP  1u
#define KIND_ONSET 2u

#define FRAME_NS   16666667ull
#define AUDIO_NS    5333333ull   /* 256 frames at 48 kHz */

static unsigned char g_mem[YRT_RING_BYTES(4096)];
static yrt_ring g_ring;
static uint64_t g_audio_frames;

/* What a real callback would push: the device frame count at the start of
 * the buffer, and how late the callback ran. */
static void push_onset(int64_t late_ns) {
    yrt_event ev;
    memset(&ev, 0, sizeof(ev));        /* C++-safe; in C a compound literal does */
    ev.source = SRC_DEMO;
    ev.kind = KIND_ONSET;
    ev.u.u64[0] = g_audio_frames;
    ev.u.i64[1] = late_ns;
    (void)yrt_ring_push(&g_ring, &ev);
    YRT_PLOT("audio fill", (double)(g_audio_frames % 1024u) / 1024.0);
    g_audio_frames += 256u;
}

#ifndef YRT_NO_THREADS
typedef struct audio_sim {
    yrt_worker w;
    uint64_t next;
} audio_sim;

/* Re-arms itself: a worker job may submit to its own worker. */
static void audio_cb(void* ctx, const yrt_job_info* info) {
    audio_sim* a = (audio_sim*)ctx;
    if (info->flushed) return;          /* the stop, not a deadline */
    {
        YRT_ZONE(z, "audio callback");
        push_onset(info->late_ns);
        YRT_ZONE_END(z);
    }
    a->next += AUDIO_NS;
    (void)yrt_worker_submit(&a->w, a->next, audio_cb, a);
}
#endif

/* Stand-in for a phase of the frame: spin, and return how long it took. */
static uint32_t phase(uint64_t ns) {
    uint64_t t0 = yrt_now_ns();
    (void)yrt_spin_until(t0 + ns);
    return (uint32_t)(yrt_now_ns() - t0);
}

/* A label in quotes, with quotes doubled, so a message with a comma stays
 * one field. */
static void csv_text(FILE* f, const char* s) {
    fputc('"', f);
    for (; s && *s; s++) {
        if (*s == '"') fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void csv_row(FILE* f, const yrt_event* e) {
    fprintf(f, "%lu,%llu,%lu,%u,%u,%lu,", (unsigned long)e->seq,
            (unsigned long long)e->t_ns, (unsigned long)e->tid,
            (unsigned)e->source, (unsigned)e->kind, (unsigned long)e->aux);
    if (e->source == YRT_SRC_RT) {
        switch (e->kind) {
        case YRT_KIND_ZONE:
            csv_text(f, e->u.zone.loc->name);
            fprintf(f, ",%llu,%llu,\n", (unsigned long long)e->u.zone.dur_ns,
                    (unsigned long long)e->u.zone.value);
            return;
        case YRT_KIND_PLOT:
            csv_text(f, e->u.plot.name);
            fprintf(f, ",%.9g,,\n", e->u.plot.value);
            return;
        case YRT_KIND_FRAME:
            csv_text(f, e->u.plot.name ? e->u.plot.name : "frame");
            fputs(",,,\n", f);
            return;
        case YRT_KIND_MESSAGE:
        case YRT_KIND_THREAD_NAME:
            csv_text(f, e->u.text);
            fputs(",,,\n", f);
            return;
        case YRT_KIND_LOSS:
            fprintf(f, "\"loss\",%llu,%llu,\n", (unsigned long long)e->u.u64[0],
                    (unsigned long long)e->u.u64[1]);
            return;
        default:
            break;
        }
    } else if (e->source == SRC_DEMO && e->kind == KIND_FLIP) {
        uint64_t busy = 0;
        int i;
        for (i = 0; i < 6; i++) busy += e->u.u32[3 + i];
        fprintf(f, "\"flip\",%llu,%u,%llu\n", (unsigned long long)e->u.u64[0],
                (unsigned)e->u.u16[4], (unsigned long long)busy);
        return;
    } else if (e->source == SRC_DEMO && e->kind == KIND_ONSET) {
        fprintf(f, "\"onset\",%llu,%lld,\n", (unsigned long long)e->u.u64[0],
                (long long)e->u.i64[1]);
        return;
    }
    fprintf(f, "\"\",%llu,%llu,%llu\n", (unsigned long long)e->u.u64[0],
            (unsigned long long)e->u.u64[1], (unsigned long long)e->u.u64[2]);
}

int main(int argc, char** argv) {
    int frames = (argc > 1) ? atoi(argv[1]) : 120;
    FILE* out = stdout;
    yrt_ring_desc rd;
    yrt_event ev[64];
    uint64_t target, rows = 0, lost = 0, worst_drain = 0;
    uint16_t dropped = 0;
    int f, n, i;
#ifndef YRT_NO_THREADS
    static audio_sim audio;
    yrt_worker_desc wd;
    int audio_on = 0;
#endif

    if (frames < 1) frames = 1;
    if (argc > 2 && !(out = fopen(argv[2], "w"))) {
        fprintf(stderr, "rt_ring_csv: cannot open %s\n", argv[2]);
        return 1;
    }

    memset(&rd, 0, sizeof(rd));
    rd.memory = g_mem;
    rd.bytes = sizeof(g_mem);
    if (!yrt_ring_open(&g_ring, &rd)) {
        fprintf(stderr, "rt_ring_csv: %s\n", yrt_ring_error(&g_ring));
        return 1;
    }
    yrt_trace_set_ring(&g_ring);
    YRT_THREAD_INIT("frame loop");
    fputs("seq,t_ns,tid,source,kind,aux,label,v0,v1,v2\n", out);

    target = yrt_now_ns() + FRAME_NS;
#ifndef YRT_NO_THREADS
    /* No elevation: an example should not ask for FIFO on a shared machine.
     * A rig's audio thread belongs to the audio library anyway. */
    memset(&wd, 0, sizeof(wd));
    wd.no_elevate = true;
    if (yrt_worker_open(&audio.w, &wd)) {
        audio.next = yrt_now_ns() + AUDIO_NS;
        audio_on = yrt_worker_submit(&audio.w, audio.next, audio_cb, &audio) > 0;
    } else {
        fprintf(stderr, "rt_ring_csv: no audio thread (%s); the frame loop "
                        "pushes the onsets\n", yrt_worker_error(&audio.w));
    }
#endif
    YRT_MESSAGEF("session start, %d frames", frames);

    for (f = 0; f < frames; f++) {
        yrt_event flip;
        uint32_t ph[6];
        uint64_t onset, t0;
        YRT_ZONE(fz, "frame");
        YRT_ZONE_VALUE(fz, f);
        ph[0] = phase(50000);    /* timeline evaluate */
        ph[1] = phase(100000);   /* script callback   */
        {
            YRT_ZONE(dz, "draw");
            ph[2] = phase(400000);
            YRT_ZONE_END(dz);
        }
        ph[3] = phase(30000);    /* texture upload    */
        t0 = yrt_now_ns();
        (void)yrt_sleep_until(target, YRT_DEFAULT_SPIN_NS);   /* "swap" */
        onset = yrt_now_ns();
        ph[4] = (uint32_t)(onset - t0);
        ph[5] = 0;               /* no GPU timer here */
        while (onset > target + FRAME_NS / 2) {   /* a missed vblank */
            target += FRAME_NS;
            dropped++;
        }

        /* The per-flip record: t_ns = estimated onset, aux = display index,
         * payload = target, dropped, mode, six phase durations in ns. */
        memset(&flip, 0, sizeof(flip));
        flip.t_ns = onset;
        flip.source = SRC_DEMO;
        flip.kind = KIND_FLIP;
        flip.aux = 0;
        flip.u.u64[0] = target;
        flip.u.u16[4] = dropped;
        flip.u.u16[5] = 0;
        for (i = 0; i < 6; i++) flip.u.u32[3 + i] = ph[i];
        (void)yrt_ring_push(&g_ring, &flip);
        YRT_FRAME_MARK();
#ifndef YRT_NO_THREADS
        if (!audio_on)
#endif
        {
            push_onset(0);
            push_onset(0);
            push_onset(0);
        }
        YRT_ZONE_END(fz);

        /* The drain: once per frame, on the frame thread, after the flip. */
        t0 = yrt_now_ns();
        while ((n = yrt_ring_drain(&g_ring, ev, 64)) > 0) {
            for (i = 0; i < n; i++) {
                if (ev[i].source == YRT_SRC_RT && ev[i].kind == YRT_KIND_LOSS)
                    lost += ev[i].u.u64[0];
                csv_row(out, &ev[i]);
            }
            rows += (uint64_t)n;
            if (n < 64) break;
        }
        if (yrt_now_ns() - t0 > worst_drain) worst_drain = yrt_now_ns() - t0;
        target += FRAME_NS;
    }

#ifndef YRT_NO_THREADS
    yrt_worker_close(&audio.w);
#endif
    yrt_trace_set_ring(NULL);
    while ((n = yrt_ring_drain(&g_ring, ev, 64)) > 0) {
        for (i = 0; i < n; i++) csv_row(out, &ev[i]);
        rows += (uint64_t)n;
    }
    if (out != stdout) fclose(out);
    fprintf(stderr, "rt_ring_csv: %d frames, %llu rows, %llu records lost, %u dropped "
                    "frames, slowest drain+write %.0f us\n", frames,
            (unsigned long long)rows, (unsigned long long)lost, (unsigned)dropped,
            (double)worst_drain / 1e3);
    return 0;
}
