/* audio_tone.c - play one quiet tone at a time and print its record.
 *
 *     audio_tone [--null] [--device NAME] [--exclusive] [--count N]
 *                [--db DB] [--silent]
 *
 *   --null        miniaudio's null device: no sound hardware (CI runs this)
 *   --device NAME the playback device whose name contains NAME; default the
 *                 OS default device
 *   --exclusive   WASAPI exclusive mode (every other program goes silent)
 *   --count N     tones, 300 ms apart (default 3)
 *   --db DB       peak level in dBFS (default -40; values above -30 are
 *                 refused, because this is a demo, not a stimulus)
 *   --silent      the same schedule with silent buffers
 *
 * Each tone is 50 ms of 1 kHz with 5 ms raised-cosine ramps, scheduled 200
 * ms ahead on the ysp_rt clock. The program prints the describe line, then
 * one line per tone: residual, start frame, the lead the path used, tier
 * and flags.
 *
 * Exit: 0 when every tone has a record, 1 when the device did not open,
 * 2 usage, 3 a record is missing.
 */
#define YSP_AUDIO_IMPLEMENTATION
#include "ysp/audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static yau_audio au;

int main(int argc, char** argv) {
    yau_desc d;
    yau_tone_desc td;
    yau_buf tone;
    char line[512];
    int i, count = 3, silent = 0, missing = 0;
    double db = -40;
    yau_id ids[64];
    memset(&d, 0, sizeof d);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--null")) d.backend = YAU_BACKEND_NULL;
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) d.device = argv[++i];
        else if (!strcmp(argv[i], "--exclusive")) d.exclusive = true;
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--db") && i + 1 < argc) db = atof(argv[++i]);
        else if (!strcmp(argv[i], "--silent")) silent = 1;
        else { fprintf(stderr, "usage: audio_tone [--null] [--device NAME] [--exclusive] [--count N] [--db DB] [--silent]\n"); return 2; }
    }
    if (count < 1 || count > 64 || db > -30) { fprintf(stderr, "audio_tone: --count 1..64, --db at most -30\n"); return 2; }
    d.arena_bytes = 1 << 20;
    if (!yau_open(&au, &d)) { fprintf(stderr, "%s\n", yau_error(&au)); return 1; }
    yau_describe(&au, line, sizeof line);
    puts(line);
    memset(&td, 0, sizeof td);
    td.hz = 1000;
    td.dur = YAU_MS(50);
    td.peak = yau_db((float)db);
    td.ramp = YAU_MS(5);
    tone = yau_tone(&au, &td);
    if (!tone.frames) { fprintf(stderr, "%s\n", yau_error(&au)); yau_close(&au); return 1; }
    if (silent) memset(tone.frames, 0, (size_t)tone.n * sizeof(float));
    for (i = 0; i < count; i++) {
        int64_t t = yrt_now_ns() + YAU_MS(200);
        ids[i] = yau_play_at(&au, tone, t);
        yrt_sleep_until((int64_t)(t + YAU_MS(100)), YRT_DEFAULT_SPIN_NS);
    }
    for (i = 0; i < count; i++) {
        yau_onset r;
        int rc;
        memset(&r, 0, sizeof r);
        rc = ids[i] > 0 ? yau_wait(&au, ids[i], YAU_S(1), &r) : (int)ids[i];
        if (rc != YAU_OK) { printf("tone %d: %s\n", i, yau_strerror(rc)); missing++; continue; }
        printf("tone %d: residual %+8.1f us  frame %lld  lead %.2f ms  tier %d  flags 0x%03x\n", i,
               (double)r.residual / 1e3, (long long)r.start_frame,
               (double)(r.onset - r.rendered_at) / 1e6, r.tier, (unsigned)r.flags);
    }
    yau_describe(&au, line, sizeof line);
    puts(line);
    yau_close(&au);
    return missing ? 3 : 0;
}
