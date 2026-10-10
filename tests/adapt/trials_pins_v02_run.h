/* trials_pins_v02_run.h - v0.2 designs for the scripted sessions of
 * trials_pins_run.h, whose hashes pin the v0.2.0 behavior of
 * ysp/trials.h: tables, order lists, draws, subsets, groups, units,
 * balance and the rules text, each through the same script as the v0.1
 * pins (re-queues, marked breaks, records, a mid-session snapshot, the
 * tallies and the format lines). trials_pin_gen.c prints the constants
 * of trials_pins_v02.h from it; trials_test.c checks a newer
 * header against them, so a later feature (jitter, v0.2.1) cannot move a
 * v0.2.0 draw. Needs trials_pins_run.h first and the v0.2 API. */
#ifndef YSP_TRIALS_PINS_V02_RUN_H
#define YSP_TRIALS_PINS_V02_RUN_H

#define PIN2_N_DESIGNS 16

static uint64_t pin2_arena[(1u << 15) / 8];
static uint64_t pin2_rules_arena[(1u << 17) / 8];
static ytb_table pin2_tab;

static const char pin2_cond_csv[] =
    "target,contrast,word,list,n\n"
    "a,0.25,cat,1,2\na,0.5,dog,1,1\nb,0.25,\"new york\",2,2\nb,0.5,ox,2,1\n"
    "catch,0,emu,1,1\ncatch,0,yak,2,1\n";
static const char pin2_block_csv[] =
    "block,lvl,item\nA,x,1\nA,y,2\nB,x,3\nB,y,4\nC,x,5\nC,y,6\nD,x,7\nD,y,8\n";
static const char pin2_unit_csv[] =
    "kind,item\nprime,1\nprime,2\ntarget,1\ntarget,2\nfill,1\nfill,2\nfill,3\nfill,4\n";
static const char pin2_chunk_csv[] =
    "seq,part\ns1,cue\ns1,probe\ns2,cue\ns2,probe\n,solo\n,solo2\n";
static const char pin2_bal_csv[] = "lvl,side\nA,l\nA,r\nB,l\nB,r\nC,l\nC,r\n";

static const int    pin2_list[9] = { 5, 4, 3, 2, 1, 0, 0, 1, 3 };
static const double pin2_weights[6] = { 1, 2, 0, 0.5, 3, 1 };
static const int    pin2_glist[4] = { 2, 0, 3, 1 };

static bool pin2_csv(const char* csv) {
    ytb_csv_desc cd;
    memset(&cd, 0, sizeof(cd));
    cd.text = csv;
    cd.len = strlen(csv);
    cd.arena = pin2_arena;
    cd.arena_size = sizeof(pin2_arena);
    return ytb_open_csv(&pin2_tab, &cd);
}

/* Fill `d` for v0.2 design k (participant number k mod 4 where one is
 * used). Returns false past the last design. */
static bool pin2_design(int k, ytr_desc* d, pin_track* tr, uint64_t* seed) {
    memset(d, 0, sizeof(*d));
    d->rng = ytr_splitmix;
    d->rng_ctx = seed;
    if (k >= PIN2_N_DESIGNS) return false;
    switch (k) {
    case 0:
        if (!pin2_csv(pin2_cond_csv)) return false;
        d->table = &pin2_tab; d->reps = 2; d->order = YTR_ORDER_SEQUENTIAL; d->n_practice = 2; break;
    case 1:
        if (!pin2_csv(pin2_cond_csv)) return false;
        d->table = &pin2_tab; d->reps = 3; d->subset = 4; d->order = YTR_ORDER_FULL_RANDOM; break;
    case 2:
        if (!pin2_csv(pin2_cond_csv)) return false;
        d->table = &pin2_tab; d->order = YTR_ORDER_WITH_REPLACEMENT; d->draws = 25;
        d->weights = pin2_weights; d->requeue_gap = 2; break;
    case 3:
        if (!pin2_csv(pin2_cond_csv)) return false;
        d->table = &pin2_tab; d->order = YTR_ORDER_LIST; d->order_list = pin2_list; d->n_order_list = 9; break;
    case 4:
        if (!pin2_csv(pin2_cond_csv)) return false;
        d->table = &pin2_tab; d->reps = 4; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_max_run(0, YTR_ANY_LEVEL, 1); d->n_constraints = 1;
        d->block_size = 6; d->n_warmup = 1; d->n_practice = 2; d->requeue_gap = 2; break;
    case 5:
        if (!pin2_csv(pin2_block_csv)) return false;
        d->table = &pin2_tab; d->reps = 2; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_max_run(1, YTR_ANY_LEVEL, 1); d->n_constraints = 1;
        d->groups.mode = YTR_GROUPS_BLOCKED; d->groups.factor = 0;
        d->groups.order = YTR_GROUP_ORDER_BALANCED_LATIN; d->groups.participant = 3; d->n_warmup = 1; break;
    case 6:
        if (!pin2_csv(pin2_block_csv)) return false;
        d->table = &pin2_tab; d->reps = 2; d->order = YTR_ORDER_FULL_RANDOM;
        d->groups.mode = YTR_GROUPS_ALTERNATE; d->groups.factor = 0;
        d->groups.order = YTR_GROUP_ORDER_RANDOM; break;
    case 7:
        if (!pin2_csv(pin2_block_csv)) return false;
        d->table = &pin2_tab; d->order = YTR_ORDER_WITH_REPLACEMENT; d->draws = 5;
        d->groups.mode = YTR_GROUPS_BLOCKED; d->groups.factor = 0;
        d->groups.order = YTR_GROUP_ORDER_LATIN; d->groups.participant = 1; break;
    case 8:
        if (!pin2_csv(pin2_unit_csv)) return false;
        d->table = &pin2_tab; d->reps = 3; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_followed_by(0, 0, 1);
        d->constraints[1] = ytr_max_run(0, 2, 2); d->n_constraints = 2;
        d->block_size = 5; d->requeue_gap = 3; break;
    case 9:
        if (!pin2_csv(pin2_chunk_csv)) return false;
        d->table = &pin2_tab; d->reps = 3; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_chunk(0); d->n_constraints = 1; d->requeue_gap = 1; break;
    case 10:
        if (!pin2_csv(pin2_unit_csv)) return false;
        tr[0].stop = 9; tr[1].stop = 6;
        d->table = &pin2_tab; d->reps = 2; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_preceded_by(0, 1, 0); d->n_constraints = 1;
        d->tracks[0] = ytr_track(&tr[0], pin_track_done);
        d->tracks[1] = ytr_track(&tr[1], pin_track_done);
        d->n_tracks = 2; d->track_rate = 0.4; break;
    case 11:
        if (!pin2_csv(pin2_bal_csv)) return false;
        d->table = &pin2_tab; d->reps = 3; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_balance(0);
        d->constraints[1] = ytr_max_run(1, YTR_ANY_LEVEL, 2); d->n_constraints = 2; break;
    case 12:
        if (!pin2_csv(pin2_bal_csv)) return false;
        d->table = &pin2_tab; d->reps = 2; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_balance_flags(0, YTR_BALANCE_NO_REPEAT | YTR_BALANCE_NO_LEADIN);
        d->n_constraints = 1; d->requeue_gap = 2; break;
    case 13:
        if (!pin2_csv(pin2_block_csv)) return false;
        d->table = &pin2_tab; d->reps = 1; d->order = YTR_ORDER_RANDOM;
        d->groups.mode = YTR_GROUPS_BLOCKED; d->groups.factor = 0;
        d->groups.order = YTR_GROUP_ORDER_LIST; d->groups.list = pin2_glist; d->groups.n_list = 4; break;
    case 14: {
        static const char rules[] =
            "order constrained\nweight n\nwhere list=@participant\nmax_run target 1\n"
            "practice 2 from target=catch\nblock_size 3\n";
        ytr_rules_desc rd;
        char err[256];
        if (!pin2_csv(pin2_cond_csv)) return false;
        d->table = &pin2_tab;
        memset(&rd, 0, sizeof(rd));
        rd.text = rules;
        rd.len = sizeof(rules) - 1;
        rd.table = &pin2_tab;
        rd.participant = 1;
        rd.arena = pin2_rules_arena;
        rd.arena_size = sizeof(pin2_rules_arena);
        if (ytr_rules(d, &rd, err, sizeof(err)) != 0) return false;
        break;
    }
    default:
        if (!pin2_csv(pin2_cond_csv)) return false;
        d->table = &pin2_tab; d->reps = 3; d->order = YTR_ORDER_CONSTRAINED;
        d->constraints[0] = ytr_min_gap(0, 2, 1);
        d->constraints[1] = ytr_first_not(0, 2); d->n_constraints = 2;
        d->record_size = 8; d->records = pin_records; d->requeue_gap = 2; break;
    }
    return true;
}

static uint64_t pin2_run(int k, int s) { return pin_run_with(pin2_design, k, s); }

#endif /* YSP_TRIALS_PINS_V02_RUN_H */
