/* mut_real.c - the player's real-window checks for the mutation runner
 * (tests/mutate/player_*.toml, test "real"): the player's sources compiled
 * into one program with SDL, and tests/player/real_test.py run with this
 * program as the player. Needs an X display, /dev/uinput and the keyboard
 * rule (real_test.py), YSP_SDL3 for the build, and YPL_REPO and YPL_BIN (a
 * build folder with player_make_rig and ypak). YPL_REAL_CASES names the
 * cases (default "oddball,noafter,lost,rule,rule2,lateread,fewer,abort,panic,refuse").
 *
 * Exit 0 when every case holds; a mutant that survives shows a branch of
 * the real path that no case can see. */
#include "impl.c"
#include "eval.c"
#include "host.c"
#include "lower.c"
#include "run.c"
#define main pl_cli_main
#include "main.c"
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char** argv) {
    char cmd[4096], self[1024], work[256], cases[256];
    const char* repo = getenv("YPL_REPO");
    const char* bin = getenv("YPL_BIN");
    const char* base = getenv("YPL_WORK") ? getenv("YPL_WORK") : getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    ssize_t n;
    int rc, fails = 0;
    char* c, *save;
    if (getenv("YPL_AS_PLAYER")) return pl_cli_main(argc, argv);
    n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0 || !repo || !bin) { printf("mut_real needs YPL_REPO and YPL_BIN\n"); return 2; }
    self[n] = 0;
    snprintf(work, sizeof work, "%s/pl_mut_real_XXXXXX", base);
    if (!mkdtemp(work)) { printf("no work folder under %s\n", base); return 2; }
    snprintf(cases, sizeof cases, "%s", getenv("YPL_REAL_CASES") ? getenv("YPL_REAL_CASES") : "oddball,noafter,lost,rule,rule2,lateread,fewer,abort,panic,refuse");
    /* one case a run: a case that kills its player leaves the next its own */
    for (c = strtok_r(cases, ",", &save); c; c = strtok_r(NULL, ",", &save)) {
        snprintf(cmd, sizeof cmd,
                 "YPL_AS_PLAYER=1 python3 '%s/tests/player/real_test.py' --bin '%s' --player '%s' --out '%s/%s' --only %s --trials 8 > '%s/%s.txt' 2>&1; "
                 "st=$?; grep -E 'FAIL|failures' '%s/%s.txt' | head -12; exit $st",
                 repo, bin, self, work, c, c, work, c, work, c);
        rc = system(cmd);
        if (WEXITSTATUS(rc) != 0) fails++;
    }
    snprintf(cmd, sizeof cmd, "rm -r '%s'", work);
    (void)!system(cmd);
    printf("mut_real: %d cases failed\n", fails);
    return fails ? 1 : 0;
}
