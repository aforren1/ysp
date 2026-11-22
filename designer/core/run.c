/* run.c - Run: the player beside the designer, its command line, its
 * progress (dsg.h, step 6; docs/designer.md, "Run" and "Stop"). OWNED BY
 * PART R.
 *
 * Part S's stub: no run starts, and each call says that Run is not built
 * yet. Part R replaces this file. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "dsg.h"

#include <stdio.h>
#include <string.h>

#define NOT_BUILT "Run is not built yet (designer step 6, part R)"

int dsg_run_args(const dsg_run_settings* s, const char* player, const char* pack, const char* out_dir, dsg_run_argv* out,
                 char* err, size_t cap) {
    (void)s;
    (void)player;
    (void)pack;
    (void)out_dir;
    out->argc = 0;
    out->argv[0] = NULL;
    snprintf(err, cap, NOT_BUILT);
    return -1;
}

int dsg_run_out_dir(const char* folder, const dsg_run_settings* s, int64_t now_unix_s, char* out, size_t cap) {
    (void)folder;
    (void)s;
    (void)now_unix_s;
    if (cap) out[0] = 0;
    return -1;
}

const char* dsg_run_exit_text(int status, char* buf, size_t cap) {
    snprintf(buf, cap, "exit status %d", status);
    return buf;
}

int dsg_player_version(const char* player, const char* tmp_dir, char* out, size_t cap, char* err, size_t err_cap) {
    (void)player;
    (void)tmp_dir;
    if (cap) out[0] = 0;
    snprintf(err, err_cap, NOT_BUILT);
    return -1;
}

int dsg_run_start(dsg_run** out, const dsg_run_desc* d, char* err, size_t cap) {
    (void)d;
    *out = NULL;
    snprintf(err, cap, NOT_BUILT);
    return -1;
}

int dsg_run_stop(dsg_run* r, int64_t now_ns) {
    (void)r;
    (void)now_ns;
    return 0;
}

void dsg_run_kill(dsg_run* r) { (void)r; }

bool dsg_run_tick(dsg_run* r, int64_t now_ns) {
    (void)r;
    (void)now_ns;
    return false;
}

void dsg_run_get_info(const dsg_run* r, dsg_run_info* out) {
    (void)r;
    memset(out, 0, sizeof *out);
    out->state = DSG_RUN_ENDED;
    out->last_response = out->last_outcome = out->log = out->exit_text = out->out_dir = out->log_path = out->command = "";
    out->last_rt_s = -1;
}

bool dsg_run_issue(const dsg_run* r, dsg_issue* out) {
    (void)r;
    memset(out, 0, sizeof *out);
    return false;
}

void dsg_run_free(dsg_run* r) { (void)r; }
