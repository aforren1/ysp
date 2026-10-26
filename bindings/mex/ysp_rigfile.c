/* ysp_rigfile.c - MATLAB/Octave MEX binding for ysp/rigfile.h (builds to ysp_rigfile.<mexext>)
 *
 * Command dispatch: the first argument is a command string; 'load',
 * 'load_file', 'parse' and 'new' return a uint64 counter handle to a
 * profile, never reused.
 *
 *     p = ysp_rigfile('load');                   % the user's profile.json
 *     info = ysp_rigfile('info', p);             % rig, notes, hash, roles, view, ppd
 *     cellfun(@disp, info.notes);                % what the checks found
 *     h = ysp_device('open', ysp_rigfile('desc', p, 'resp'));
 *     src = ysp_rigfile('source', p, ysp_device('info', h));   % tier 1 only when checked
 *     ysp_rigfile('close', p);
 *
 * A role's device opens through ysp_device: 'desc' gives the struct that
 * ysp_device('open', ...) takes (yrig_desc()), so the two MEX files stay
 * apart, as their headers' Python distributions do. Every time is seconds;
 * lengths are mm, as in the file. Errors: ysp_rigfile:missing (no file, no
 * role), :folder (no rig folder, or one that another user can write),
 * :json, :format, :io, :arg, :full.
 *
 * Build with build.m.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define PM_MOD "ysp_rigfile"
#include "ysp_mex_util.h"

#define YSP_RIGFILE_IMPLEMENTATION
#include "ysp/rigfile.h"

#define RM_BINDING_VERSION "1.0.0"

static void rm_destroy(void* p) { free(p); }

static void rm_fail(int rc, const char* msg) {
    const char* id = rc == YRIG_ERR_MISSING ? "missing" : rc == YRIG_ERR_FOLDER ? "folder" : rc == YRIG_ERR_JSON ? "json"
                   : rc == YRIG_ERR_FORMAT ? "format" : rc == YRIG_ERR_IO ? "io" : rc == YRIG_ERR_FULL ? "full" : "arg";
    pm_err(id, "%s", msg && msg[0] ? msg : "ysp_rig: error");
}

static mxArray* rm_d(double v) { return mxCreateDoubleScalar(v); }
static mxArray* rm_empty(void) { return mxCreateDoubleMatrix(0, 0, mxREAL); }
static double rm_s(int64_t ns) { return (double)ns / 1e9; }

static mxArray* rm_lb(const yrig_loopback* lb, int latency) {
    static const char* fl[] = { "n", "p5", "median", "p95", "max", "date", "sha256", "check", "check_name" };
    static const char* fb[] = { "n", "lo", "hi", "date", "sha256", "check", "check_name" };
    mxArray* s;
    if (!lb->set) return rm_empty();
    if (latency) {
        s = mxCreateStructMatrix(1, 1, 9, fl);
        mxSetField(s, 0, "p5", rm_d(rm_s(lb->p5_ns)));
        mxSetField(s, 0, "median", rm_d(rm_s(lb->median_ns)));
        mxSetField(s, 0, "p95", rm_d(rm_s(lb->p95_ns)));
        mxSetField(s, 0, "max", rm_d(rm_s(lb->max_ns)));
    } else {
        s = mxCreateStructMatrix(1, 1, 7, fb);
        mxSetField(s, 0, "lo", rm_d(rm_s(lb->lo_ns)));
        mxSetField(s, 0, "hi", rm_d(rm_s(lb->hi_ns)));
    }
    mxSetField(s, 0, "n", rm_d(lb->n));
    mxSetField(s, 0, "date", mxCreateString(lb->date));
    mxSetField(s, 0, "sha256", mxCreateString(lb->sha256));
    mxSetField(s, 0, "check", rm_d(lb->check));
    mxSetField(s, 0, "check_name", mxCreateString(yrig_check_name(lb->check)));
    return s;
}

static mxArray* rm_opt_s(int64_t ns) { return ns < 0 ? rm_empty() : rm_d(rm_s(ns)); }

static void rm_role_into(mxArray* s, size_t k, const yrig_role* r) {
    static const char* fbtn[] = { "code", "name" };
    mxArray* b = mxCreateStructMatrix(1, (size_t)r->n_buttons, 2, fbtn);
    int i;
    for (i = 0; i < r->n_buttons && i < YRIG_MAX_BUTTONS; i++) {
        mxSetField(b, (size_t)i, "code", mxCreateString(r->buttons[i].code));
        mxSetField(b, (size_t)i, "name", mxCreateString(r->buttons[i].name));
    }
    mxSetField(s, k, "name", mxCreateString(r->name));
    mxSetField(s, k, "family", mxCreateString(r->family));
    mxSetField(s, k, "family_id", rm_d(r->family_id));
    mxSetField(s, k, "key", mxCreateString(r->key));
    mxSetField(s, k, "baud", rm_d(r->baud));
    mxSetField(s, k, "latched", mxCreateLogicalScalar(r->latched));
    mxSetField(s, k, "ftdi_latency", rm_opt_s(r->ftdi_latency_ns));
    mxSetField(s, k, "pulse", rm_opt_s(r->pulse_ns));
    mxSetField(s, k, "buttons", b);
    mxSetField(s, k, "latency", rm_lb(&r->latency, 1));
    mxSetField(s, k, "bounds", rm_lb(&r->bounds, 0));
}

static const char* rm_role_fields[] = { "name", "family", "family_id", "key", "baud", "latched", "ftdi_latency", "pulse",
                                        "buttons", "latency", "bounds" };

static mxArray* rm_info(const yrig_profile* p) {
    static const char* f[] = { "rig", "written", "dir", "notes", "hash", "ppd", "view", "display", "roles" };
    static const char* fv[] = { "distance_mm", "width_mm", "width_px" };
    static const char* fd[] = { "onset_offset", "latency", "calibration", "calibration_sha256", "calibration_check",
                                "calibration_check_name" };
    mxArray* s = mxCreateStructMatrix(1, 1, 9, f);
    mxArray *notes, *roles;
    char hex[65];
    int i;
    mxSetField(s, 0, "rig", mxCreateString(p->rig));
    mxSetField(s, 0, "written", mxCreateString(p->written));
    mxSetField(s, 0, "dir", mxCreateString(p->dir));
    notes = mxCreateCellMatrix(1, (size_t)p->n_notes);
    for (i = 0; i < p->n_notes && i < YRIG_MAX_NOTES; i++) mxSetCell(notes, (size_t)i, mxCreateString(p->notes[i]));
    mxSetField(s, 0, "notes", notes);
    mxSetField(s, 0, "hash", mxCreateString(yrig_hash(p, hex) == YRIG_OK ? hex : ""));
    mxSetField(s, 0, "ppd", rm_d(yrig_ppd(p)));
    if (p->view.set) {
        mxArray* v = mxCreateStructMatrix(1, 1, 3, fv);
        mxSetField(v, 0, "distance_mm", rm_d(p->view.distance_mm));
        mxSetField(v, 0, "width_mm", rm_d(p->view.width_mm));
        mxSetField(v, 0, "width_px", rm_d(p->view.width_px));
        mxSetField(s, 0, "view", v);
    } else {
        mxSetField(s, 0, "view", rm_empty());
    }
    if (p->display.set) {
        mxArray* d = mxCreateStructMatrix(1, 1, 6, fd);
        mxSetField(d, 0, "onset_offset", rm_d(rm_s(p->display.onset_offset_ns)));
        mxSetField(d, 0, "latency", rm_lb(&p->display.latency, 1));
        mxSetField(d, 0, "calibration", mxCreateString(p->display.calibration));
        mxSetField(d, 0, "calibration_sha256", mxCreateString(p->display.calibration_sha256));
        mxSetField(d, 0, "calibration_check", rm_d(p->display.check));
        mxSetField(d, 0, "calibration_check_name", mxCreateString(yrig_check_name(p->display.check)));
        mxSetField(s, 0, "display", d);
    } else {
        mxSetField(s, 0, "display", rm_empty());
    }
    roles = mxCreateStructMatrix(1, (size_t)p->n_roles, 11, rm_role_fields);
    for (i = 0; i < p->n_roles && i < YRIG_MAX_ROLES; i++) rm_role_into(roles, (size_t)i, &p->role[i]);
    mxSetField(s, 0, "roles", roles);
    return s;
}

static mxArray* rm_source(const yin_source* src) {
    static const char* f[] = { "kind", "tier", "partial", "device", "lo", "hi", "note" };
    mxArray* s = mxCreateStructMatrix(1, 1, 7, f);
    mxSetField(s, 0, "kind", rm_d(src->kind));
    mxSetField(s, 0, "tier", rm_d(src->tier));
    mxSetField(s, 0, "partial", rm_d(src->partial));
    mxSetField(s, 0, "device", rm_d(src->device));
    mxSetField(s, 0, "lo", rm_d(rm_s(src->lo_ns)));
    mxSetField(s, 0, "hi", rm_d(rm_s(src->hi_ns)));
    mxSetField(s, 0, "note", mxCreateString(src->note ? src->note : ""));
    return s;
}

static int rm_family(const mxArray* a) {
    int f;
    if (mxIsChar(a)) {
        char* n = pm_string(a, "family");
        if (pm_lower_eq(n, "lsl")) return YRIG_FAMILY_LSL;
        for (f = YBOX_XID; f <= YBOX_FAMILY_LAST; f++)
            if (pm_lower_eq(n, ybox_family_name(f))) return f;
        pm_err("arg", "family: unknown name '%s'", n);
    }
    return pm_int(a, "family");
}

static yrig_profile* rm_new(void) {
    yrig_profile* p = (yrig_profile*)calloc(1, sizeof *p);
    if (!p) pm_err("memory", "out of memory");
    return p;
}

void mexFunction(int nlhs, mxArray* plhs[], int nrhs, const mxArray* prhs[]) {
    char* cmd;
    char err[512];
    yrig_profile* p;
    int rc;
    pm_destroy = rm_destroy;
    if (nrhs < 1 || !mxIsChar(prhs[0])) pm_err("usage", "first argument must be a command string");
    cmd = mxArrayToString(prhs[0]);
    err[0] = '\0';

    if (strcmp(cmd, "version") == 0) {
        plhs[0] = mxCreateString(RM_BINDING_VERSION);
        if (nlhs > 1) plhs[1] = mxCreateString(yrig_version());
        return;
    }
    if (strcmp(cmd, "dir") == 0 || strcmp(cmd, "path") == 0) {
        char d[512], path[700];
        if (yrig_dir(d, sizeof d) != YRIG_OK)
            rm_fail(YRIG_ERR_FOLDER, "no rig folder: the user's config folder is not set, or another user can write it");
        if (cmd[0] == 'd') { plhs[0] = mxCreateString(d); return; }
        rc = yrig_path(d, nrhs > 1 ? pm_string(prhs[1], "name") : NULL, path, sizeof path);
        if (rc != YRIG_OK) rm_fail(rc, "path: a profile name is 1 to 63 of A-Z a-z 0-9 _ -");
        plhs[0] = mxCreateString(path);
        return;
    }
    if (strcmp(cmd, "check_name") == 0) {
        pm_nargs(nrhs, 2, "name = ysp_rigfile('check_name', check)");
        plhs[0] = mxCreateString(yrig_check_name(pm_int(prhs[1], "check")));
        return;
    }
    if (strcmp(cmd, "load") == 0 || strcmp(cmd, "load_file") == 0 || strcmp(cmd, "parse") == 0 ||
        strcmp(cmd, "new") == 0) {
        char* a1 = nrhs > 1 && !mxIsEmpty(prhs[1]) ? pm_string(prhs[1], cmd[0] == 'p' ? "text" : "name") : NULL;
        if ((strcmp(cmd, "load_file") == 0 || cmd[0] == 'p') && !a1) pm_err("usage", "ysp_rigfile('%s', %s)", cmd, cmd[0] == 'p' ? "text, [dir]" : "path");
        p = rm_new();
        if (cmd[0] == 'n') {
            if (a1 && (strlen(a1) < 1 || strlen(a1) > 63)) { free(p); pm_err("arg", "rig: 1 to 63 bytes"); }
            yrig_init(p, a1 ? a1 : "rig");
            rc = YRIG_OK;
        } else if (strcmp(cmd, "load") == 0) {
            rc = yrig_load(p, a1, err, sizeof err);
        } else if (cmd[0] == 'l') {
            rc = yrig_load_file(p, a1, err, sizeof err);
        } else {
            char* dir = nrhs > 2 && !mxIsEmpty(prhs[2]) ? pm_string(prhs[2], "dir") : NULL;
            rc = yrig_parse(p, a1, strlen(a1), dir, err, sizeof err);
        }
        if (rc != YRIG_OK) { free(p); rm_fail(rc, err); }
        plhs[0] = pm_handle_out(pm_register(p));
        return;
    }

    pm_nargs(nrhs, 2, "ysp_rigfile(command, profile, ...)");
    p = (yrig_profile*)pm_lookup(prhs[1]);
    if (strcmp(cmd, "close") == 0) { pm_unregister(p); free(p); return; }
    if (strcmp(cmd, "info") == 0) { plhs[0] = rm_info(p); return; }
    if (strcmp(cmd, "hash") == 0) {
        char hex[65];
        if (yrig_hash(p, hex) != YRIG_OK) rm_fail(YRIG_ERR_FORMAT, "hash: the profile has a value the format cannot hold");
        plhs[0] = mxCreateString(hex);
        return;
    }
    if (strcmp(cmd, "text") == 0) {
        size_t n = yrig_write(p, NULL, 0);
        char* buf;
        if (n == 0) rm_fail(YRIG_ERR_FORMAT, "text: the profile has a value the format cannot hold");
        buf = (char*)mxMalloc(n + 1);
        (void)yrig_write(p, buf, n + 1);
        plhs[0] = mxCreateString(buf);
        return;
    }
    if (strcmp(cmd, "role") == 0) {
        const yrig_role* r;
        char* name;
        pm_nargs(nrhs, 3, "r = ysp_rigfile('role', p, name)");
        name = pm_string(prhs[2], "name");
        r = yrig_find(p, name);
        if (!r) pm_err("missing", "the rig profile \"%s\" has no role \"%.31s\"", p->rig, name);
        plhs[0] = mxCreateStructMatrix(1, 1, 11, rm_role_fields);
        rm_role_into(plhs[0], 0, r);
        return;
    }
    if (strcmp(cmd, "desc") == 0) {
        static const char* f[] = { "role", "family", "key", "baud", "latched", "pulse" };
        ydev_desc d;
        pm_nargs(nrhs, 3, "desc = ysp_rigfile('desc', p, role)");
        memset(&d, 0, sizeof d);
        rc = yrig_desc(p, pm_string(prhs[2], "role"), &d, err, sizeof err);
        if (rc != YRIG_OK) rm_fail(rc, err);
        /* pulse only when the rig fixes it: ysp_device('open') reads [] as
         * the default, so leave the field out instead */
        plhs[0] = mxCreateStructMatrix(1, 1, d.pulse_ns > 0 ? 6 : 5, f);
        mxSetField(plhs[0], 0, "role", mxCreateString(d.role));
        mxSetField(plhs[0], 0, "family", rm_d(d.family));
        mxSetField(plhs[0], 0, "key", mxCreateString(d.key));
        mxSetField(plhs[0], 0, "baud", rm_d(d.baud));
        mxSetField(plhs[0], 0, "latched", mxCreateLogicalScalar(d.latched));
        if (d.pulse_ns > 0) mxSetField(plhs[0], 0, "pulse", rm_d(rm_s(d.pulse_ns)));
        return;
    }
    if (strcmp(cmd, "source") == 0) {
        yin_source s;
        pm_nargs(nrhs, 3, "src = ysp_rigfile('source', p, role_or_device_info)");
        if (mxIsChar(prhs[2])) {
            memset(&s, 0, sizeof s);
            s = yrig_source_role(p, pm_string(prhs[2], "role"), s);
        } else {
            /* ysp_device('info', h): role, family, kind, device, the fields
             * ydev_source() reads */
            const mxArray* a = prhs[2];
            const mxArray *r, *f, *k, *dv;
            ydev_device* dev;
            if (!mxIsStruct(a) || !(r = mxGetField(a, 0, "role")) || !(f = mxGetField(a, 0, "family")) ||
                !(k = mxGetField(a, 0, "kind")) || !(dv = mxGetField(a, 0, "device")))
                pm_err("arg", "source: a role name, or the struct of ysp_device('info', h)");
            dev = (ydev_device*)mxCalloc(1, sizeof *dev);
            dev->d.role = pm_string(r, "role");
            dev->d.family = pm_int(f, "family");
            dev->d.kind = pm_int(k, "kind");
            dev->d.device = (uint32_t)pm_int(dv, "device");
            s = yrig_source(p, dev);
        }
        plhs[0] = rm_source(&s);
        return;
    }
    if (strcmp(cmd, "bind") == 0) {
        pm_nargs(nrhs, 5, "ysp_rigfile('bind', p, role, family, key)");
        rc = yrig_bind(p, pm_string(prhs[2], "role"), rm_family(prhs[3]), pm_string(prhs[4], "key"));
        if (rc == YRIG_ERR_FULL) rm_fail(rc, "bind: the profile has 32 roles (YRIG_MAX_ROLES)");
        if (rc != YRIG_OK)
            rm_fail(rc, "bind: a role name of 1 to 31 of A-Z a-z 0-9 _ . -, a key below 160 bytes without a quote, "
                        "and 'lsl:' keys for the lsl family only");
        return;
    }
    if (strcmp(cmd, "remove") == 0) {
        pm_nargs(nrhs, 3, "removed = ysp_rigfile('remove', p, role)");
        plhs[0] = mxCreateLogicalScalar(yrig_remove(p, pm_string(prhs[2], "role")));
        return;
    }
    if (strcmp(cmd, "set_view") == 0) {
        double dist, width, px;
        pm_nargs(nrhs, 5, "ysp_rigfile('set_view', p, distance_mm, width_mm, width_px)");
        dist = pm_finite(prhs[2], "distance_mm");
        width = pm_finite(prhs[3], "width_mm");
        px = pm_int(prhs[4], "width_px");
        if (!(dist > 0 && dist <= 100000) || !(width > 0 && width <= 100000) || px < 1 || px > 100000)
            pm_err("arg", "set_view: distance_mm and width_mm above 0 and at most 100000, width_px 1 to 100000");
        p->view.set = true;
        p->view.distance_mm = dist;
        p->view.width_mm = width;
        p->view.width_px = (int32_t)px;
        return;
    }
    if (strcmp(cmd, "clear_view") == 0) { memset(&p->view, 0, sizeof p->view); return; }
    if (strcmp(cmd, "save") == 0) {
        rc = yrig_save(p, nrhs > 2 && !mxIsEmpty(prhs[2]) ? pm_string(prhs[2], "name") : NULL, err, sizeof err);
        if (rc != YRIG_OK) rm_fail(rc, err);
        return;
    }
    if (strcmp(cmd, "save_file") == 0) {
        pm_nargs(nrhs, 3, "ysp_rigfile('save_file', p, path)");
        rc = yrig_save_file(p, pm_string(prhs[2], "path"), err, sizeof err);
        if (rc != YRIG_OK) rm_fail(rc, err);
        return;
    }
    pm_err("usage", "unknown command '%s'", cmd);
}
