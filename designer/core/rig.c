/* rig.c - rig profiles: list, load with the calibration, the banner, the
 * preview's counts (dsg.h, step 6; docs/designer.md, "Rig profiles").
 * OWNED BY PART G.
 *
 * Part S's stub: no profile is listed or loaded, so the check and the
 * preview run with none, as before step 6. Part G replaces this file. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "dsg.h"

#include <stdio.h>

#define NOT_BUILT "rig profiles are not built yet (designer step 6, part G)"

int dsg_rig_list(char (*names)[DSG_RIG_NAME], int cap, char* err, size_t err_cap) {
    (void)names;
    (void)cap;
    snprintf(err, err_cap, NOT_BUILT);
    return -1;
}

int dsg_rig_load(const char* name, dsg_rig** out, char* err, size_t cap) {
    (void)name;
    *out = NULL;
    snprintf(err, cap, NOT_BUILT);
    return -1;
}

void dsg_rig_free(dsg_rig* r) { (void)r; }

const char* dsg_rig_name(const dsg_rig* r) {
    (void)r;
    return "";
}

const char* dsg_rig_path(const dsg_rig* r) {
    (void)r;
    return "";
}

const struct yrig_profile* dsg_rig_profile(const dsg_rig* r) {
    (void)r;
    return NULL;
}

const struct ycol_cal* dsg_rig_cal(const dsg_rig* r) {
    (void)r;
    return NULL;
}

bool dsg_rig_changed(const dsg_rig* r) {
    (void)r;
    return false;
}

int dsg_rig_summary(const dsg_rig* r, char* out, size_t cap) {
    (void)r;
    return snprintf(out, cap, "Rig: none");
}

bool dsg_rig_counts_per_unit(const dsg_rig* r, const char* role, int role_units, double out[2]) {
    (void)r;
    (void)role;
    (void)role_units;
    out[0] = out[1] = 1.0;
    return false;
}
