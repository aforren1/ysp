/* player/sim.c - the stepped session: --sim's session, stepped one frame at
 * a time by its caller (the designer's preview). docs/player.md,
 * "Reference: the stepped session"; the plan in docs/designer.md, "Plan of
 * step 5".
 *
 * The session's time is its own frame grid, not a clock: frame k's onset
 * is a fixed time plus k periods, and its flip record comes with frame
 * k + 1, as the SIM screen gives them on the virtual clock. So the session
 * needs no virtual clock, no screen and no thread of its own, and a caller
 * that steps it late changes no time in its data. The data set's writer is
 * the one thread, as in a real run.
 */
#include "player.h"

#include <stdio.h>
#include <string.h>

/* --sim's first frame is the SIM screen's vblank 7 from YRT_VIRTUAL_T0_NS:
 * the screen's open shows 6 warm-up frames (ysp/screen.h, yscr__warmup()).
 * The grid starts there too, so a stepped session's times equal --sim's
 * (test_player_sim compares the files). */
enum { PL_SIM_FIRST_VBLANK = 7 };

/* live() at a trial's start (run.c, pl_start_trial()): the trial is not
 * running yet, so its leaf, row and template come from the walk */
static bool pl_sim_ask(pl_player* p, int trial) {
    pl_sim_where w;
    if (!p->sim.live) return false;
    pl_sim_where_now(p, &w);
    w.trial = trial;
    w.leaf = p->leaf ? p->leaf->node->path : NULL;
    w.condition = p->ti.condition;
    w.tmpl = p->tmpl ? p->tmpl->def->name : NULL;
    w.state = NULL;
    w.phase = PL_SIM_EVENTS;
    return p->sim.live(p->sim.ctx, &w);
}

static void pl_sim_end(pl_player* p);

int pl_sim_load(pl_player* p, const pl_sim_desc* d, pl_check_result* r) {
    int i;
    memset(p, 0, sizeof *p);
    memset(r, 0, sizeof *r);
    p->stand_in = true;
    p->sim.on = true;
    p->sim.width = d->width > 0 ? d->width : 1920;
    p->sim.height = d->height > 0 ? d->height : 1080;
    p->sim.period = d->period_ns > 0 ? d->period_ns : 16666667;
    p->sim.t0 = YRT_VIRTUAL_T0_NS + PL_SIM_FIRST_VBLANK * p->sim.period;
    p->sim.gl_caps = d->gl_caps;
    p->sim.cache = d->cache;
    p->sim.gl_proc = d->gl_proc;
    p->sim.gl_ctx = d->gl_ctx;
    p->sim.share = d->share;
    p->sim.cal = d->cal;
    p->sim.rig = d->rig;
    p->sim.live = d->live;
    p->sim.on_code = d->on_code;
    p->sim.ctx = d->ctx;
    p->sim.ask = pl_sim_ask;
    p->sim.end = pl_sim_end;
    p->sim.last_rt = YRSP_NO_TIME;
    p->args.sim = true;
    p->args.have_seed = true;
    p->args.seed = d->seed;
    p->args.def_text = d->def_text;
    p->args.def_len = d->def_text ? d->def_len : 0;
    p->args.out_dir = d->out_dir;
    p->args.participant = d->participant;
    p->args.rt_s = d->rt_s;
    p->args.sim_period_ns = p->sim.period;
    p->pack_reader = !d->pack.path && !d->pack.data && d->pack.reader;
    if (d->n_fields < 0 || d->n_fields > PL_MAX_FIELDS) {
        pl_fail(p, "the stepped session: %d session fields; at most %d", d->n_fields, PL_MAX_FIELDS);
    } else if (!d->out_dir || !d->out_dir[0]) {
        pl_fail(p, "the stepped session needs out_dir, a folder for its data set that does not exist yet");
    } else if (d->width < 0 || d->height < 0 || d->period_ns < 0) {
        pl_fail(p, "the stepped session: the display's size and period must be above 0 (or 0 for 1920 x 1080 at 60 Hz)");
    } else {
        p->args.n_fields = d->n_fields;
        for (i = 0; i < d->n_fields; i++) p->args.fields[i] = d->fields[i];
        if (pl_load_from(p, &d->pack) == 0) (void)pl_run_prepare(p);
    }
    r->status = !p->failed ? PL_CHECK_OK : p->report.n_errors > 0 ? PL_CHECK_DEFINITION : PL_CHECK_REFUSED;
    r->report = p->arena_open ? &p->report : NULL;
    r->message = p->failed ? p->error : "";
    r->object = p->failed ? p->fail_obj : PL_OBJ_NONE;
    r->object_name = r->object ? p->fail_name : NULL;
    r->needs = p->needs;
    r->n_needs = p->n_needs;
    if (!p->failed) p->sim.stage = 1;
    return r->status;
}

int pl_sim_attach(pl_player* p, int64_t budget_ns) {
    if (!p->sim.on || p->sim.stage == 0 || !p->gfx_open) return -1;
    if (p->sim.attached) return 1;
    if (p->sim.gl_caps) {
        /* ysp/gfx.h's replay of the recorded calls, at most the budget a
         * call. It primes every program the load recorded, the text ones
         * that pl_gfx_warm() made too, without ygfx_prime()'s wait for the
         * GPU, which would add 0.6 ms (RX 6600) to 9.5 ms (llvmpipe) to the
         * UI's frame (docs/gfx.md, "v0.23.0") */
        int rc = ygfx_attach(&p->gfx, p->sim.gl_proc, p->sim.gl_ctx, budget_ns > 0 ? budget_ns : INT64_MAX);   /* gfx's 0 is one step */
        if (rc < 0) { pl_fail(p, "the gfx: %s", ygfx_error(&p->gfx)); return -1; }
        if (rc == 0) return 0;
    } else {
        (void)budget_ns;
        (void)ygfx_prime(&p->gfx);
    }
    p->sim.attached = true;
    return 1;
}

int pl_sim_frame(pl_player* p, bool draw) {
    yscr_frame f;
    yscr_record* rec = &p->sim.rec;
    if (!p->sim.on || p->sim.stage == 0 || p->failed) return -1;
    if (p->sim.stage >= 3) return 1;
    if (draw && !p->sim.attached) {
        pl_fail(p, "pl_sim_frame(): a drawn frame before pl_sim_attach() completed");
        return -1;
    }
    if (p->sim.stage == 1 && p->sim.stopped) {
        p->sim.stage = 2;
        p->sim.drain_i = 0;
    }
    if (p->sim.stage == 2 && !pl_run_drain_more(p, p->sim.drain_i)) {
        p->sim.stage = 3;
        return 1;
    }
    if (draw && p->sim.gl_caps) ygfx_reset_state(&p->gfx);   /* the UI drew since gfx's last call */
    memset(&f, 0, sizeof f);
    f.index = p->sim.frame;
    f.period = p->sim.period;
    f.onset = p->sim.t0 + f.index * f.period;
    f.vblank = PL_SIM_FIRST_VBLANK + f.index;
    if (p->sim.has_rec) {
        f.done = rec;
        f.n_done = 1;
        f.last = rec;
    }
    p->sim.draw = draw;
    if (p->sim.stage == 1) {
        int r = pl_run_frame(p, &f);
        if (r < 0) return -1;
        if (r == 1) {
            p->sim.stage = 2;
            p->sim.drain_i = 0;
        }
    } else {
        if (pl_run_drain(p, &f, p->sim.drain_i) < 0) return -1;
        p->sim.drain_i++;
    }
    /* the flip: shown on its vblank, as the SIM screen's record of it */
    memset(rec, 0, sizeof *rec);
    rec->index = f.index;
    rec->target = rec->planned = rec->onset = f.onset;
    rec->path = YSCR_PATH_SIMULATED;
    rec->tier = YSCR_TIER_SIM;
    if (p->def->min_tier && YSCR_TIER_SIM > p->def->min_tier) rec->flags |= YSCR_FLIP_BELOW_TIER;
    p->sim.has_rec = true;
    p->sim.frame++;
    if (p->sim.stage == 2 && !pl_run_drain_more(p, p->sim.drain_i)) {
        p->sim.stage = 3;
        return 1;
    }
    return 0;
}

/* pl_close()'s first step: a session that did not reach its end is
 * stopped, its end's frames run with no draw, and pl_run_finish() writes
 * the last rows and session.json. Here, not in pl_sim_frame(): the
 * writer's join and the disk wait belong on the thread that closes. */
static void pl_sim_end(pl_player* p) {
    if (p->failed || p->sim.stage == 0 || p->sim.stage == 4) return;
    if (p->sim.stage == 1) pl_sim_stop(p);
    while (p->sim.stage < 3 && pl_sim_frame(p, false) == 0) { }
    if (p->sim.stage == 3 && !p->failed) (void)pl_run_finish(p);
    p->sim.stage = 4;
}

/* An input's time: after the last frame's onset, at or before the next's */
static bool pl_sim_in_time(const pl_player* p, int64_t t_ns) {
    int64_t next = pl_sim_next_onset(p);
    return p->sim.on && p->sim.stage == 1 && !p->sim.stopped && !p->failed && t_ns <= next && t_ns > next - p->sim.period;
}

static int pl_sim_push(pl_player* p, const yrsp_input* ev) {
    if (p->sim.n_in >= PL_SIM_INPUT_CAP) return -1;
    if (p->sim.n_in > 0 && ev->t < p->sim.in[p->sim.n_in - 1].t) return -1;   /* a collector's input goes in time order */
    p->sim.in[p->sim.n_in++] = *ev;
    return 0;
}

int pl_sim_key(pl_player* p, int64_t t_ns, int scancode, bool down) {
    yrsp_input ev;
    if (!pl_sim_in_time(p, t_ns) || scancode <= 0) return -1;
    memset(&ev, 0, sizeof ev);
    ev.t = t_ns;
    ev.type = down ? YRSP_PRESS : YRSP_RELEASE;
    ev.kind = YRSP_KIND_KEYBOARD;
    ev.control = (uint32_t)scancode;
    ev.value = down ? 1.0f : 0.0f;
    ev.stamp = YRSP_TIER_SIM;
    return pl_sim_push(p, &ev);
}

int pl_sim_choice(pl_player* p, int64_t t_ns, int choice, bool down) {
    yrsp_input ev;
    const yrsp_collector* c;
    if (!pl_sim_in_time(p, t_ns) || !p->in_trial || !p->tmpl || !(c = p->tmpl->collector) || choice < 0 || choice >= p->tmpl->n_choices) return -1;
    memset(&ev, 0, sizeof ev);
    ev.t = t_ns;
    ev.type = down ? YRSP_PRESS : YRSP_RELEASE;
    /* as the scripted participant presses it (run.c, pl_frame()) */
    ev.kind = c->ch[choice].kind;
    ev.control = c->ch[choice].control;
    ev.value = down ? 1.0f : 0.0f;
    ev.stamp = YRSP_TIER_SIM;
    return pl_sim_push(p, &ev);
}

int pl_sim_sample(pl_player* p, int64_t t_ns, int role, float x, float y) {
    if (!pl_sim_in_time(p, t_ns) || role < 0 || role >= p->def->n_sigroles) return -1;
    return pl_v2_sample_push(p, t_ns, role, x, y);
}

int64_t pl_sim_next_onset(const pl_player* p) { return p->sim.t0 + p->sim.frame * p->sim.period; }

void pl_sim_where_now(const pl_player* p, pl_sim_where* w) {
    memset(w, 0, sizeof *w);
    w->trial = p->global_trial - 1;
    w->leaf = p->in_trial && p->leaf ? p->leaf->node->path : NULL;
    w->condition = p->in_trial ? p->ti.condition : -1;
    w->tmpl = p->in_trial && p->tmpl ? p->tmpl->def->name : NULL;
    w->state = pl_v2_state_now(p);
    w->phase = !p->in_trial ? PL_SIM_BETWEEN : p->responding ? PL_SIM_RESPONSE : p->after_built ? PL_SIM_AFTER : PL_SIM_EVENTS;
    w->frame = p->sim.frame;
    w->t_ns = p->sim.frame * p->sim.period;
    w->outcome = p->sim.last_has_outcome ? p->sim.last_outcome : NULL;
    w->response = p->sim.last_has_response ? p->sim.last_response : NULL;
    w->rt_ns = p->sim.last_rt;
}

bool pl_sim_texture(const pl_player* p, ygfx_native* out) {
    memset(out, 0, sizeof *out);
    if (p->sim.on && p->sim.attached && p->sim.gl_caps && p->gfx_open) return ygfx_output_native((ygfx_gfx*)&p->gfx, out) == YGFX_OK;
    return false;
}

void pl_sim_stop(pl_player* p) {
    if (!p->sim.on || p->sim.stage != 1 || p->sim.stopped) return;
    /* as an operator's stop of a real run (run.c, pl_run()): the running
     * trial has no row, every finished one keeps its row */
    p->aborted = true;
    p->abort_reasons = YSCR_ABORT_REQUEST;
    p->abort_presses = 0;
    p->abort_ns = pl_sim_next_onset(p);
    p->abort_trial = p->in_trial ? p->global_trial - 1 : -1;
    p->in_trial = false;
    p->sim.stopped = true;
    p->sim.n_in = 0;
}

void pl_sim_detach(pl_player* p) {
    if (!p->sim.on) return;
    if (p->gfx_open) {
        ygfx_close(&p->gfx);
        p->gfx_open = false;
    }
    p->sim.attached = false;
}
