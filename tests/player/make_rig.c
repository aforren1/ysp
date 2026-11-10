/* make_rig.c - a rig profile and a nominal calibration for the player's
 * tests: the view and the calibration that trial_2afc_adaptive --sim uses
 * as its nominal ones (570 mm, 0.27 mm a pixel, sRGB primaries, gamma 2.2,
 * 100 cd/m2), so a run of the program with --rig and one of the player
 * have one geometry and one light.
 *
 * Usage: make_rig DIR [required_role family key]...
 * Writes DIR/profile.json and DIR/display.yspcal. */
#include "player.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    static const float srgb_xy[4][2] = { { 0.64f, 0.33f }, { 0.30f, 0.60f }, { 0.15f, 0.06f }, { 0.3127f, 0.3290f } };
    static ycol_cal cal;
    static yrig_profile prof;
    static unsigned char bytes[sizeof(ycol_cal)];
    char path[1024], err[300];
    FILE* fp;
    int n, i;
    if (argc < 2) return 2;
    if (ycol_cal_nominal(&cal, srgb_xy, 100.0f, 2.2) < 0) return 1;
    n = ycol_cal_save(&cal, bytes, sizeof bytes);
    if (n < 0) return 1;
    snprintf(path, sizeof path, "%s/display.yspcal", argv[1]);
    if (!(fp = fopen(path, "wb"))) return 1;
    fwrite(bytes, 1, (size_t)n, fp);
    fclose(fp);
    yrig_init(&prof, "player-test");
    prof.view.set = true;
    prof.view.distance_mm = 570.0;
    prof.view.width_mm = 0.27 * 1920.0;
    prof.view.width_px = 1920;
    prof.display.set = true;
    snprintf(prof.display.calibration, sizeof prof.display.calibration, "display.yspcal");
    {
        static const char H[] = "0123456789abcdef";
        uint8_t d[32];
        yrig_sha256(bytes, (size_t)n, d);
        for (i = 0; i < 32; i++) { prof.display.calibration_sha256[i * 2] = H[d[i] >> 4]; prof.display.calibration_sha256[i * 2 + 1] = H[d[i] & 15]; }
        prof.display.calibration_sha256[64] = 0;
    }
    snprintf(path, sizeof path, "%s/profile.json", argv[1]);
    if (yrig_save_file(&prof, path, err, sizeof err) != YRIG_OK) { fprintf(stderr, "make_rig: %s\n", err); return 1; }
    return 0;
}
