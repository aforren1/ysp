/* trials_pins_run.h - the scripted sessions whose hashes pin the v0.1
 * behavior of ysp/trials.h. Included by trials_pin_gen.c, which prints
 * the constants of trials_pins.h when built against a header, and by
 * trials_test.c, which checks a newer header against those constants.
 * It uses the v0.1 API only, so it builds against v0.1.1 and every later
 * version.
 *
 * Each design runs under 20 seeds through one fixed script: next(), a
 * re-queue or an update with a computed outcome and record, marked breaks
 * with and without a pending trial, a snapshot in the middle. Everything
 * the session exposes is folded into one FNV-1a 64 hash: the open() error
 * text when open() fails, the schedule at open, every trial_info, the
 * history, the records, the tallies, the format lines (the meta line
 * without its version token) and both snapshots. A change to any draw,
 * any order, any byte of a snapshot or any formatted field changes it. */
#ifndef YSP_TRIALS_PINS_RUN_H
#define YSP_TRIALS_PINS_RUN_H

#include <string.h>

#define PIN_N_DESIGNS 30
#define PIN_N_SEEDS   20

typedef struct pin_track {
    int n;      /* trials the track has been given */
    int stop;   /* done once n reaches it          */
} pin_track;

static bool pin_track_done(void* ctx) {
    const pin_track* p = (const pin_track*)ctx;
    return p->n >= p->stop;
}

static uint64_t pin_fnv(uint64_t h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static uint64_t pin_int(uint64_t h, long v) {
    unsigned char b[8];
    int i;
    unsigned long long u = (unsigned long long)(long long)v;
    for (i = 0; i < 8; i++) b[i] = (unsigned char)(u >> (8 * i));
    return pin_fnv(h, b, 8);
}

static uint64_t pin_str(uint64_t h, const char* s) {
    return pin_fnv(h, s, strlen(s) + 1);
}

/* Large state lives in statics: the handles are about 90 KB. */
static ytr_trials pin_t;
static unsigned char pin_records[8 * YTR_MAX_TRIALS];
static unsigned char pin_snap[1 << 18];
static char pin_line[4096];

/* The pins predate the psy-era magic "PSTR" becoming "YTRL" (v0.5.1): the
 * snapshot is hashed under the old magic, so the pins still prove that no
 * other byte changed. */
static uint64_t pin_snap_fnv(uint64_t h, size_t sz) {
    if (sz >= 4 && memcmp(pin_snap, "YTRL", 4) == 0) memcpy(pin_snap, "PSTR", 4);
    return pin_fnv(h, pin_snap, sz);
}

static const int pin_cond_reps_a[6] = { 1, 4, 0, 2, 3, 2 };
static const int pin_cond_reps_b[4] = { 6, 2, 2, 6 };
static const int pin_warm_list[3] = { 0, 2, 3 };

/* Fill `d` for design k. Returns false past the last design. */
static bool pin_design(int k, ytr_desc* d, pin_track* tr, uint64_t* seed) {
    memset(d, 0, sizeof(*d));
    d->rng = ytr_splitmix;
    d->rng_ctx = seed;
    switch (k) {
    case 0: d->n_conditions = 5; d->reps = 4; d->order = YTR_ORDER_SEQUENTIAL; break;
    case 1: d->n_conditions = 5; d->reps = 4; d->order = YTR_ORDER_RANDOM; break;
    case 2: d->n_conditions = 5; d->reps = 4; d->order = YTR_ORDER_FULL_RANDOM; break;
    case 3:
        d->factors[0] = ytr_factor("ori", 2); d->factors[1] = ytr_factor("con", 3);
        d->n_factors = 2; d->reps = 5; d->order = YTR_ORDER_FULL_RANDOM; break;
    case 4:
        d->factors[0] = ytr_factor("orientation", 2); d->factors[1] = ytr_factor("contrast", 5);
        d->n_factors = 2; d->reps = 20; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 3); d->n_constraints = 1; break;
    case 5:
        d->factors[0] = ytr_factor("a", 3); d->factors[1] = ytr_factor("b", 3);
        d->n_factors = 2; d->reps = 6; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_max_in_window(1, 2, 6, 2); d->n_constraints = 1; break;
    case 6:
        d->n_conditions = 4; d->reps = 8; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_min_gap(YTR_CONDITION, 0, 2); d->n_constraints = 1; break;
    case 7:
        d->factors[0] = ytr_factor("x", 3); d->factors[1] = ytr_factor("y,\"q\"", 2);
        d->n_factors = 2; d->reps = 6; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_no_transition(0, 0, 1);
        d->constraints[1] = ytr_first_not(0, 1); d->n_constraints = 2; break;
    case 8:
        d->factors[0] = ytr_factor("p", 2); d->factors[1] = ytr_factor("q", 3);
        d->n_factors = 2; d->reps = 8; d->order = YTR_ORDER_CONSTRAINED; d->block_size = 12;
        d->constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 2);
        d->constraints[1] = ytr_min_gap(1, 2, 1);
        d->constraints[2] = ytr_max_in_window(YTR_CONDITION, 5, 7, 2); d->n_constraints = 3; break;
    case 9:
        d->factors[0] = ytr_factor("p", 2); d->factors[1] = ytr_factor("q", 3);
        d->n_factors = 2; d->reps = 8; d->order = YTR_ORDER_CONSTRAINED; d->block_size = 12;
        d->constraints_span_blocks = true;
        d->constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 2); d->n_constraints = 1; break;
    case 10: d->n_conditions = 6; d->cond_reps = pin_cond_reps_a; d->order = YTR_ORDER_SEQUENTIAL; break;
    case 11: d->n_conditions = 6; d->cond_reps = pin_cond_reps_a; d->order = YTR_ORDER_FULL_RANDOM; break;
    case 12: d->n_conditions = 6; d->cond_reps = pin_cond_reps_a; d->order = YTR_ORDER_RANDOM; break;
    case 13:
        d->n_conditions = 4; d->reps = 10; d->order = YTR_ORDER_RANDOM; d->n_practice = 5;
        d->n_warmup = 2; d->block_size = 10; d->warmup_conditions = pin_warm_list;
        d->n_warmup_conditions = 3; break;
    case 14:
        d->n_conditions = 4; d->reps = 3; d->order = YTR_ORDER_SEQUENTIAL; d->n_practice = 7;
        d->warmup_conditions = pin_warm_list; d->n_warmup_conditions = 3;
        d->n_warmup = 2; d->block_size = 5; break;
    case 15:
        tr[0].stop = 12; tr[1].stop = 20; tr[2].stop = 7;
        d->tracks[0] = ytr_track(&tr[0], pin_track_done);
        d->tracks[1] = ytr_track(&tr[1], pin_track_done);
        d->tracks[2] = ytr_track(&tr[2], pin_track_done);
        d->tracks[1].weight = 3.0; d->n_tracks = 3; break;
    case 16:
        tr[0].stop = 9; tr[1].stop = 4; tr[2].stop = 11;
        d->tracks[0] = ytr_track(&tr[0], pin_track_done);
        d->tracks[1] = ytr_track(&tr[1], pin_track_done);
        d->tracks[2] = ytr_track(&tr[2], pin_track_done);
        d->n_tracks = 3; d->interleave = YTR_INTERLEAVE_ROUND_ROBIN; break;
    case 17:
        tr[0].stop = 30; tr[1].stop = 25; tr[2].stop = 40;
        d->n_conditions = 1; d->reps = 24; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_min_gap(YTR_CONDITION, 0, 1); d->n_constraints = 1;
        d->tracks[0] = ytr_track(&tr[0], pin_track_done);
        d->tracks[1] = ytr_track(&tr[1], pin_track_done);
        d->tracks[2] = ytr_track(&tr[2], pin_track_done);
        d->n_tracks = 3; d->track_rate = 0.9; d->record_size = 8; d->records = pin_records; break;
    case 18:
        tr[0].stop = 6; tr[1].stop = 8;
        d->n_conditions = 3; d->reps = 4; d->order = YTR_ORDER_FULL_RANDOM;
        d->tracks[0] = ytr_track(&tr[0], pin_track_done);
        d->tracks[1] = ytr_track(&tr[1], pin_track_done);
        d->n_tracks = 2; d->track_rate = 1.0; break;
    case 19:
        d->n_conditions = 5; d->reps = 6; d->order = YTR_ORDER_FULL_RANDOM; d->requeue_gap = 3; break;
    case 20:
        d->n_conditions = 3; d->reps = 10; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 1); d->n_constraints = 1; break;
    case 21:
        d->n_conditions = 2; d->cond_reps = pin_cond_reps_b; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 1); d->n_constraints = 1; break;
    case 22:
        d->n_conditions = 3; d->reps = 30; d->order = YTR_ORDER_CONSTRAINED; d->max_swaps = 3;
        d->constraints[0] = ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 1); d->n_constraints = 1; break;
    case 23:
        d->n_conditions = 4; d->reps = 5; d->order = YTR_ORDER_RANDOM;
        d->record_size = 8; d->records = pin_records; d->requeue_gap = 2; break;
    case 24:
        d->factors[0] = ytr_factor("a", 2); d->factors[1] = ytr_factor("b", 2);
        d->factors[2] = ytr_factor("c", 2); d->factors[3] = ytr_factor("d", 2);
        d->n_factors = 4; d->reps = 12; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 2);
        d->constraints[1] = ytr_max_run(1, YTR_ANY_LEVEL, 2);
        d->constraints[2] = ytr_max_run(2, YTR_ANY_LEVEL, 2);
        d->constraints[3] = ytr_max_run(3, YTR_ANY_LEVEL, 3); d->n_constraints = 4; break;
    case 25:
        tr[0].stop = 15;
        d->n_conditions = 3; d->reps = 6; d->order = YTR_ORDER_FULL_RANDOM;
        d->tracks[0] = ytr_track(&tr[0], pin_track_done); d->n_tracks = 1; d->track_rate = 0.5;
        d->n_warmup = 2; d->block_size = 8; d->n_practice = 2; break;
    case 26:
        d->n_conditions = 4; d->reps = 9; d->order = YTR_ORDER_CONSTRAINED; d->block_size = 9;
        d->constraints[0] = ytr_max_run(YTR_CONDITION, YTR_ANY_LEVEL, 2);
        d->constraints[1] = ytr_max_in_window(YTR_CONDITION, 1, 5, 2); d->n_constraints = 2;
        d->requeue_gap = 1; break;
    case 27:
        d->factors[0] = ytr_factor("w", 3); d->n_factors = 1; d->reps = 12;
        d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_max_in_window(0, YTR_ANY_LEVEL, 5, 2); d->n_constraints = 1; break;
    case 28:
        d->n_conditions = 5; d->reps = 6; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_min_gap(YTR_CONDITION, YTR_ANY_LEVEL, 2); d->n_constraints = 1; break;
    case 29:
        tr[0].stop = 10; tr[1].stop = 14;
        d->n_conditions = 4; d->reps = 5; d->order = YTR_ORDER_RANDOM; d->n_practice = 3;
        d->tracks[0] = ytr_track(&tr[0], pin_track_done);
        d->tracks[1] = ytr_track(&tr[1], pin_track_done);
        d->n_tracks = 2; d->interleave = YTR_INTERLEAVE_ROUND_ROBIN; d->track_rate = 0.3; break;
    default: return false;
    }
    return true;
}

static uint64_t pin_info(uint64_t h, const ytr_trial_info* ti) {
    h = pin_int(h, ti->index); h = pin_int(h, ti->condition); h = pin_int(h, ti->track);
    h = pin_int(h, ti->rep); h = pin_int(h, ti->block);
    h = pin_int(h, (ti->first_in_block ? 1 : 0) | (ti->after_break ? 2 : 0) | (ti->practice ? 4 : 0) |
                   (ti->warmup ? 8 : 0) | (ti->requeued ? 16 : 0));
    return h;
}

typedef bool (*pin_design_fn)(int k, ytr_desc* d, pin_track* tr, uint64_t* seed);

/* The hash of design k of `design` under seed s. */
static uint64_t pin_run_with(pin_design_fn fill, int k, int s) {
    ytr_desc d;
    pin_track tr[YTR_MAX_TRACKS];
    uint64_t seed = 0x5EED0000ULL + (uint64_t)(unsigned)s * 7919u;
    uint64_t h = 0xCBF29CE484222325ULL;
    ytr_trial_info ti;
    const ytr_trial* hist;
    int i, n, rc, nc, c, mid;
    size_t sz;
    unsigned char rec[8];
    const char* meta;

    memset(tr, 0, sizeof(tr));
    memset(pin_records, 0, sizeof(pin_records));
    if (!fill(k, &d, tr, &seed)) return 0;
    if (!ytr_open(&pin_t, &d)) return pin_str(h, ytr_error(&pin_t));
    h = pin_str(h, ytr_error(&pin_t));
    n = ytr_n_scheduled(&pin_t);
    for (i = 0; i < n; i++) h = pin_int(h, ytr_condition_at(&pin_t, i));
    mid = n / 2;
    for (i = 0; i < 20000; i++) {
        if (i % 29 == 0) h = pin_int(h, ytr_mark_break(&pin_t));
        rc = ytr_next(&pin_t, &ti);
        h = pin_int(h, rc);
        if (rc < 0) break;
        if (i % 23 == 11) h = pin_int(h, ytr_mark_break(&pin_t));
        h = pin_info(h, &ti);
        if (ti.track >= 0) tr[ti.track].n++;
        if (ti.track < 0 && !ti.practice && !ti.warmup && i % 13 == 7) {
            h = pin_int(h, ytr_requeue(&pin_t));
        } else {
            int j;
            for (j = 0; j < 8; j++) rec[j] = (unsigned char)(i * 31 + j);
            h = pin_int(h, ytr_update(&pin_t, (i * 7 + 3) % 4 - 1, (i % 5 == 0) ? NULL : rec));
        }
        if (i == mid) {
            sz = ytr_save_size(&pin_t);
            if (sz <= sizeof(pin_snap) && ytr_save(&pin_t, pin_snap, sizeof(pin_snap)) == (int)sz)
                h = pin_snap_fnv(h, sz);
            h = pin_int(h, (long)sz);
        }
    }
    h = pin_int(h, ytr_done(&pin_t) ? 1 : 0);
    h = pin_int(h, ytr_n_run(&pin_t));
    h = pin_int(h, ytr_n_done(&pin_t));
    hist = ytr_history(&pin_t, &n);
    for (i = 0; i < n; i++) {
        h = pin_int(h, hist[i].condition); h = pin_int(h, hist[i].track);
        h = pin_int(h, hist[i].flags); h = pin_int(h, hist[i].rep);
        h = pin_int(h, hist[i].block); h = pin_int(h, hist[i].outcome);
        if (d.record_size > 0) h = pin_fnv(h, ytr_record(&pin_t, i), d.record_size);
        if (ytr_format_row(&pin_t, i, pin_line, sizeof(pin_line)) > 0) h = pin_str(h, pin_line);
    }
    nc = ytr_n_conditions(&pin_t);
    for (c = 0; c < nc; c++) {
        double p = ytr_proportion(&pin_t, c, 1);
        h = pin_int(h, ytr_n_valid(&pin_t, c));
        h = pin_int(h, ytr_count(&pin_t, c, 0));
        h = pin_int(h, ytr_count(&pin_t, c, YTR_INVALID));
        h = pin_int(h, ytr_count(&pin_t, c, YTR_REQUEUE));
        if (p == p) h = pin_fnv(h, &p, sizeof(p));
    }
    if (ytr_format_header(&pin_t, pin_line, sizeof(pin_line)) > 0) h = pin_str(h, pin_line);
    if (ytr_format_meta(&pin_t, pin_line, sizeof(pin_line)) > 0) {
        meta = strchr(pin_line, ' ');           /* drop "ysp_trials=<version>" */
        h = pin_str(h, meta ? meta : pin_line);
    }
    sz = ytr_save_size(&pin_t);
    if (sz <= sizeof(pin_snap) && ytr_save(&pin_t, pin_snap, sizeof(pin_snap)) == (int)sz)
        h = pin_snap_fnv(h, sz);
    return pin_int(h, (long)sz);
}

static uint64_t pin_run(int k, int s) { return pin_run_with(pin_design, k, s); }

#endif /* YSP_TRIALS_PINS_RUN_H */
