/* eval_test.c - the player's evaluator and op tables, without a pack.
 *
 * A definition goes through the loader with no host; a session field, a
 * lookup, a duration and a built-in are read as the frame path reads them;
 * op tables are built and checked. tests/player/player_test.py covers what
 * needs a pack and a run. */
#include "player.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static pl_player p;

static const char DEF[] =
    "{\"format\":\"ysp-experiment\",\"version\":1,\"name\":\"t\","
    "\"session\":{\"fields\":{\"do_it\":{\"type\":\"integer\",\"required\":true},\"participant\":{\"type\":\"integer\",\"required\":true},\"lang\":{\"type\":\"string\",\"required\":true}}},"
    "\"devices\":{\"resp\":{\"direction\":\"in\",\"family\":[\"keyboard\"],\"required\":true},"
    "\"z_trig\":{\"direction\":\"out\",\"family\":[\"triggerbox\"],\"required\":true},"
    "\"a_trig\":{\"direction\":\"out\",\"family\":[\"triggerbox\"],\"required\":true}},"
    "\"stimuli\":{\"s\":{\"kind\":\"shape\",\"w\":2,\"h\":2},\"t\":{\"kind\":\"shape\",\"w\":1,\"h\":1}},"
    "\"trials\":{\"x\":{\"events\":[{\"on\":[\"s\",\"t\"],\"stagger_s\":0.1},"
    "{\"wait_s\":{\"by\":\"$lang\",\"map\":{\"en\":0.25,\"fr\":0.5}}},{\"at_s\":1.5},"
    "{\"to\":\"s\",\"props\":{\"x\":3},\"duration_s\":0.2,\"delay_s\":0.1,\"ease\":\"cosine\"},"
    "{\"then\":\"s\",\"props\":{\"y\":4},\"duration_s\":0.3},"
    "{\"trigger\":7,\"role\":\"a_trig\"},{\"trigger\":\"$participant\",\"role\":\"z_trig\"},{\"mark\":\"m1\"},"
    "{\"if\":\"$do_it\",\"steps\":[{\"off\":\"t\"}]},{\"unless\":\"$do_it\",\"steps\":[{\"off\":\"s\"}]}],"
    "\"after\":[{\"off\":\"s\"}],\"columns\":{}}},"
    "\"timeline\":[{\"name\":\"n\",\"trial\":\"x\"}]}";

static void test_values(void) {
    yjs_arena a;
    yexp_def* d = NULL;
    static yexp_report rep;
    pl_env e;
    pl_val v;
    char buf[64];
    int64_t ns;
    double num;
    bool b;
    memset(&p, 0, sizeof p);
    yjs_arena_init_heap(&a, 0);
    CHECK(yexp_load(&a, DEF, strlen(DEF), NULL, &d, &rep) == YEXP_OK, "load: %s", rep.n_diags ? rep.diags[0].msg : "?");
    if (!d) return;
    p.def = d;
    p.field_values = (pl_val*)pl_alloc(&p, sizeof(pl_val) * 3);
    /* the form sorts fields by name: do_it, lang, participant */
    CHECK(strcmp(d->fields[0].name, "do_it") == 0 && strcmp(d->fields[1].name, "lang") == 0, "field order");
    p.field_values[1].kind = YEXP_V_STRING; p.field_values[1].str = "fr";
    p.field_values[0].kind = YEXP_V_NUMBER; p.field_values[0].flags = YEXP_VF_INT; p.field_values[0].i = 1; p.field_values[0].num = 1;
    p.field_values[2].kind = YEXP_V_NUMBER; p.field_values[2].flags = YEXP_VF_INT; p.field_values[2].i = 5; p.field_values[2].num = 5;
    p.participant = 5;
    /* the loader sizes these from the definition; this test has no loader */
    p.max_text = 64;
    p.text_buf = (char*)pl_alloc(&p, (size_t)p.max_text + 1);
    memset(&e, 0, sizeof e);

    /* a session field */
    CHECK(pl_var(&p, &e, "lang", &v) == 0 && v.kind == YEXP_V_STRING && !strcmp(v.str, "fr"), "session field text");
    CHECK(pl_var(&p, &e, "participant", &v) == 0 && v.i == 5, "session field int");
    /* a lookup by a session field, in a duration */
    {
        const yexp_step* s = &d->templates[0].events[1];
        CHECK(pl_eval_ns(&p, &e, s->t, &ns) == 0 && ns == 500000000, "lookup duration: %lld", (long long)ns);
    }
    /* canonical texts of numbers */
    {
        pl_val x;
        memset(&x, 0, sizeof x);
        x.kind = YEXP_V_NUMBER;
        x.num = 0.5;
        pl_val_text(&x, buf, sizeof buf);
        CHECK(!strcmp(buf, "0.5"), "text of 0.5: %s", buf);
        x.flags = YEXP_VF_INT;
        x.i = 7;
        x.num = 7;
        pl_val_text(&x, buf, sizeof buf);
        CHECK(!strcmp(buf, "7"), "text of int 7: %s", buf);
        x.flags = 0;
        x.num = 0.1 + 0.2;
        pl_val_text(&x, buf, sizeof buf);
        CHECK(!strcmp(buf, "0.30000000000000004"), "shortest round trip: %s", buf);
        x.kind = YEXP_V_BOOL;
        x.i = 1;
        pl_val_text(&x, buf, sizeof buf);
        CHECK(!strcmp(buf, "true"), "text of true");
    }
    /* a bool read from a 0/1 session field */
    {
        p.field_values[0].i = 0; p.field_values[0].num = 0;
        CHECK(pl_eval_var_bool(&p, &e, "do_it", &b) == 0 && !b, "0 is false");
        p.field_values[0].i = 1; p.field_values[0].num = 1;
        CHECK(pl_eval_var_bool(&p, &e, "do_it", &b) == 0 && b, "1 is true");
        p.field_values[0].i = 2; p.field_values[0].num = 2;
        p.failed = 0;
        CHECK(pl_eval_var_bool(&p, &e, "do_it", &b) < 0 && strstr(p.error, "neither true nor false"), "2 is no bool: %s", p.error);
        p.failed = 0;
        p.field_values[0].i = 1; p.field_values[0].num = 1;
    }
    /* a variable that nothing defines, and an unknown built-in phase */
    p.failed = 0;
    CHECK(pl_var(&p, &e, "nope", &v) < 0 && strstr(p.error, "nope"), "undefined: %s", p.error);
    p.failed = 0;
    CHECK(pl_var(&p, &e, "response", &v) < 0, "a response before after");
    p.failed = 0;
    e.phase = PL_PH_AFTER;
    e.rt_ns = 12345;
    CHECK(pl_var(&p, &e, "rt_s", &v) == 0 && v.i == 12345 && (v.flags & YEXP_VF_DURATION), "rt_s is a duration: %lld", (long long)v.i);
    e.rt_ns = YRSP_NO_TIME;
    CHECK(pl_var(&p, &e, "rt_s", &v) == 0 && v.kind == YEXP_V_STRING, "rt_s empty");
    e.phase = PL_PH_ROW;
    CHECK(pl_var(&p, &e, "estimate", &v) == 0 && v.kind == YEXP_V_STRING, "estimate empty with no method");
    e.method = "st"; e.estimate = 0.25;
    CHECK(pl_var(&p, &e, "estimate", &v) == 0 && v.num == 0.25, "estimate");
    /* a duration's edge */
    CHECK(pl_eval_num(&p, &e, d->templates[0].events[2].t, &num) == 0 && num == 1.5, "at_s value");

    /* op tables */
    CHECK(pl_channels_assign(&p) == 0, "channels: %s", p.error);
    CHECK(p.n_channels >= 4, "channels: %d", p.n_channels);
    CHECK(pl_trigger_channel(&p, "a_trig") == 0 && pl_trigger_channel(&p, "z_trig") == 1, "trigger channels are in the order of the role names");
    {
        ytl_op ops[64];
        int64_t cursor = 0;
        int n, i;
        memset(&e, 0, sizeof e);
        e.node = NULL;
        p.env = e;
        n = pl_build_ops(&p, d->templates[0].events, d->templates[0].n_events, ops, 64, &cursor);
        CHECK(n > 0, "build: %s", p.error);
        CHECK(n <= pl_ops_bound(d->templates[0].events, d->templates[0].n_events), "ops within the bound");
        /* ON s at 0, WAIT 0.1, ON t, AT back to 0 (the stagger leaves the cursor) */
        CHECK(ops[0].op == YTL_OP_ON, "first ON");
        CHECK(ops[1].op == YTL_OP_WAIT && ops[1].t_ns == 100000000, "stagger WAIT");
        CHECK(ops[2].op == YTL_OP_ON && ops[2].ch != ops[0].ch, "second ON");
        CHECK(ops[3].op == YTL_OP_AT && ops[3].t_ns == 0, "cursor returns");
        CHECK(ops[4].op == YTL_OP_WAIT && ops[4].t_ns == 500000000, "the lookup's wait: %lld", (long long)ops[4].t_ns);
        CHECK(ops[5].op == YTL_OP_AT && ops[5].t_ns == 1500000000, "at");
        CHECK(ops[6].op == YTL_OP_TO && ops[6].tween.ease == YTL_EASE_COSINE && ops[6].tween.delay_ns == 100000000, "to");
        CHECK(ops[7].op == YTL_OP_THEN && ops[7].tween.duration_ns == 300000000, "then");
        CHECK(ops[8].op == YTL_OP_TRIGGER && ops[8].ch == 0 && ops[8].code == 7, "a_trig channel 0");
        CHECK(ops[9].op == YTL_OP_TRIGGER && ops[9].ch == 1 && ops[9].code == 5, "z_trig channel 1 with the participant's value");
        CHECK(ops[10].op == YTL_OP_MARK, "mark");
        CHECK(cursor == 1800000000, "cursor after then: %lld", (long long)cursor);
        /* do_it 1: the `if` branch (off t), the `unless` skipped */
        p.failed = 0;
        for (i = 0; i < n; i++) (void)ops[i];
        CHECK(ops[11].op == YTL_OP_OFF, "the if body is in the table");
        CHECK(n == 12, "the unless body is not: n %d", n);
    }
    /* a trigger code out of a port's byte is refused */
    {
        p.field_values[2].i = 300; p.field_values[2].num = 300;
        ytl_op ops[64];
        int64_t c = 0;
        p.failed = 0;
        CHECK(pl_build_ops(&p, d->templates[0].events, d->templates[0].n_events, ops, 64, &c) < 0 && strstr(p.error, "0..255"), "code range: %s", p.error);
        p.failed = 0;
        p.field_values[2].i = 5; p.field_values[2].num = 5;
    }
    /* the op table cap is honored */
    {
        ytl_op ops[2];
        int64_t c = 0;
        p.failed = 0;
        CHECK(pl_build_ops(&p, d->templates[0].events, d->templates[0].n_events, ops, 2, &c) < 0, "op table full is an error");
        p.failed = 0;
    }
    /* marks: one index per name */
    CHECK(pl_mark_index(&p, "m1") == pl_mark_index(&p, "m1"), "mark index stable");
    CHECK(pl_mark_index(&p, "m2") != pl_mark_index(&p, "m1"), "marks differ");
    yjs_arena_free(&a);
    pl_close(&p);
}

static void test_seeds(void) {
    int it0[1] = { 0 }, it1[1] = { 1 }, it23[2] = { 2, 3 };
    /* vectors computed with Python's hashlib (the pin of D8's node seed,
     * player/lower.c pl_node_seed) */
    CHECK(pl_node_seed(7, "main", it0, 1) == 14875456903190042380ULL, "seed vector main 0: %llu", (unsigned long long)pl_node_seed(7, "main", it0, 1));
    CHECK(pl_node_seed(7, "main", it1, 1) == 16929687978315136847ULL, "seed vector main 1");
    CHECK(pl_node_seed(8, "main", it0, 1) == 14523804642930790314ULL, "seed vector: S differs");
    CHECK(pl_node_seed(7, "a/b", it23, 2) == 14153712869762679814ULL, "seed vector: a chain of iterations");
    CHECK(pl_node_seed(7, "main", it0, 1) != pl_node_seed(7, "main", it1, 1), "an iteration changes the seed");
}

/* the host's role check against a real rig profile */
static void test_role(void) {
    static yrig_profile prof;
    yexp_host h;
    char msg[200];
    yexp_role role;
    const char* fam[1] = { "triggerbox" };
    memset(&p, 0, sizeof p);
    yrig_init(&prof, "rig");
    p.rig = &prof;
    p.have_rig = true;
    pl_host_init(&p, &h);
    memset(&role, 0, sizeof role);
    role.name = "trig";
    role.family = fam;
    role.n_family = 1;
    role.required = true;
    CHECK(h.role(h.ctx, &role, msg, sizeof msg) != 0 && strstr(msg, "bound"), "unbound required role: %s", msg);
    p.args.sim = true;
    CHECK(h.role(h.ctx, &role, msg, sizeof msg) == 0, "--sim: the participant stands in for the device");
    p.args.sim = false;
    {
        yrig_role* r = yrig_add(&prof, "trig");
        CHECK(r != NULL, "yrig_add");
        snprintf(r->family, sizeof r->family, "parallel");
        snprintf(r->key, sizeof r->key, "x");
    }
    CHECK(h.role(h.ctx, &role, msg, sizeof msg) != 0 && strstr(msg, "parallel"), "wrong family: %s", msg);
    snprintf(yrig_find(&prof, "trig")->family, 16, "triggerbox");
    CHECK(h.role(h.ctx, &role, msg, sizeof msg) == 0, "a family the role lists");
    pl_close(&p);
}

int main(void) {
    test_values();
    test_seeds();
    test_role();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
