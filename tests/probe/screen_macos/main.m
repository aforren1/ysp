/* screen_macos_probe: measures what macOS, Metal and ANGLE give a display
 * backend. One section per run:
 *   screen_macos_probe SECTION [options]
 * docs/screen_macos_probe.md explains the sections, the options and the
 * output. */
#import <Cocoa/Cocoa.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "probe.h"

static void usage(void) {
    puts("usage: screen_macos_probe SECTION [options]\n"
         "sections:\n"
         "  env      machine, clocks, displays and modes, Metal, ANGLE extensions and configs\n"
         "  egl      SDL3's EGL path on Cocoa with ANGLE: config, CAMetalLayer, swap timing\n"
         "  share    a CAMetalLayer the probe owns, ANGLE drawing into it: client buffers, sync\n"
         "  links    CVDisplayLink, CADisplayLink and presentedTime against each other\n"
         "  present  CAMetalLayer present timing: depth, presentAtTime, queue, late frames\n"
         "  bits     10-bit and half-float configurations through ANGLE and CAMetalLayer\n"
         "  input    key timestamps from CGEventPost: SDL, NSEvent and mach time\n"
         "  link     frames from CAMetalDisplayLink, preferredFrameLatency 1, 2, default\n"
         "options:\n"
         "  --out DIR        output folder (default ./ysp_macos_probe_<date>)\n"
         "  --angle DIR      ANGLE's libEGL.dylib and libGLESv2.dylib (or YSP_ANGLE_DIR)\n"
         "  --tag NAME       suffix for this condition's CSV files\n"
         "  --kind K         window (800x600), borderless (display-sized), space (fullscreen space),\n"
         "                   fullscreen (SDL fullscreen without a space), exclusive (mode change)\n"
         "  --hz HZ          exclusive: the refresh rate to switch to\n"
         "  --runs a,b,...   present: asap,pipe,quant,ahead,late,sleep,queue,mindur (default all but\n"
         "                   ahead); egl: swap; link: free,target_wait,target_at (default all)\n"
         "  --frames N       frames per run (default 300)\n"
         "  --reps N         present quant: presents per cell (default 20)\n"
         "  --seconds S      links: length (default 20)\n"
         "  --drawables N    CAMetalLayer maximumDrawableCount, 2 or 3 (default 3)\n"
         "  --sync-off       CAMetalLayer displaySyncEnabled = NO\n"
         "  --interval N     egl: swap interval (default 1)\n"
         "  --bits N         egl: 8, 10 or 16 bits per channel (default 8)\n"
         "  --backend B      ANGLE back end for egl and share: default, metal, gl (default metal)\n"
         "  --heavy N        GPU load: N dependent shader iterations per pixel\n"
         "  --keys N         input: key presses (default 200)\n"
         "  --allow-locked   open windows even when the screen is locked (nothing is shown)");
}

int main(int argc, char** argv) {
    @autoreleasepool {
        const char* section = NULL;
        g_opt.frames = 300;
        g_opt.reps = 20;
        g_opt.seconds = 20;
        g_opt.drawables = 3;
        g_opt.interval = 1;
        g_opt.bits = 8;
        g_opt.keys = 200;
        g_opt.kind = "window";
        g_opt.backend = "metal";
        g_opt.angle = getenv("YSP_ANGLE_DIR");
        g_opt.hud = getenv("MTL_HUD_ENABLED") != NULL;
        for (int i = 1; i < argc; i++) {
            const char* a = argv[i];
            const char* v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (strcmp(a, name) == 0 && v && ++i)
            if (OPT("--out")) g_opt.out = v;
            else if (OPT("--angle")) g_opt.angle = v;
            else if (OPT("--tag")) g_opt.tag = v;
            else if (OPT("--kind")) g_opt.kind = v;
            else if (OPT("--runs")) g_opt.runs = v;
            else if (OPT("--backend")) g_opt.backend = v;
            else if (OPT("--hz")) g_opt.hz = atof(v);
            else if (OPT("--seconds")) g_opt.seconds = atof(v);
            else if (OPT("--frames")) g_opt.frames = atoi(v);
            else if (OPT("--reps")) g_opt.reps = atoi(v);
            else if (OPT("--drawables")) g_opt.drawables = atoi(v);
            else if (OPT("--interval")) g_opt.interval = atoi(v);
            else if (OPT("--bits")) g_opt.bits = atoi(v);
            else if (OPT("--heavy")) g_opt.heavy = atoi(v);
            else if (OPT("--keys")) g_opt.keys = atoi(v);
            else if (strcmp(a, "--sync-off") == 0) g_opt.sync_off = 1;
            else if (strcmp(a, "--allow-locked") == 0) g_opt.allow_locked = 1;
            else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(); return 0; }
            else if (a[0] != '-' && !section) section = a;
            else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
#undef OPT
        }
        if (!section) { usage(); return 2; }
        static char outbuf[256];
        if (!g_opt.out) {
            time_t now = time(NULL);
            strftime(outbuf, sizeof outbuf, "ysp_macos_probe_%Y%m%d_%H%M%S", localtime(&now));
            g_opt.out = outbuf;
        }
        if (!g_opt.angle) g_opt.angle = ".";

        /* SDL's EGL hints must be set before SDL_Init loads the video driver. */
        if (strcmp(section, "egl") == 0 || strcmp(section, "bits") == 0) {
            static char egl[1024], gles[1024];
            snprintf(egl, sizeof egl, "%s/libEGL.dylib", g_opt.angle);
            snprintf(gles, sizeof gles, "%s/libGLESv2.dylib", g_opt.angle);
            SDL_SetHint(SDL_HINT_EGL_LIBRARY, egl);
            SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, gles);
            SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
            SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
        }
        if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
        out_init(section);
        P("command:");
        for (int i = 0; i < argc; i++) P(" %s", argv[i]);
        P("\n");
        int rc = 2;
        if (strcmp(section, "env") == 0) rc = sec_env();
        else if (strcmp(section, "egl") == 0) rc = sec_egl();
        else if (strcmp(section, "share") == 0) rc = sec_share();
        else if (strcmp(section, "links") == 0) rc = sec_links();
        else if (strcmp(section, "present") == 0) rc = sec_present();
        else if (strcmp(section, "bits") == 0) rc = sec_bits();
        else if (strcmp(section, "input") == 0) rc = sec_input();
        else if (strcmp(section, "link") == 0) rc = sec_link();
        else { fprintf(stderr, "unknown section %s\n", section); usage(); }
        SDL_Quit();
        if (g_sum) fclose(g_sum);
        return rc;
    }
}
