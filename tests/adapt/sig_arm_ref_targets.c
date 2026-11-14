/* sig_arm_ref_targets.c - the reviewer's oracle (sig_arm_ref.c, unchanged)
 * with the two targets of docs/sig.md, "Exactness", as failing checks, for
 * the mutation runner (tests/mutate/sig.toml): it runs itself as the
 * oracle and reads the oracle's summary. "FAIL line 1": a case the
 * oracle's B cannot run is not counted in arm_unverified; "FAIL line 2": a
 * wrong case has no count at all. ctest uses sig_arm_ref_check.cmake for
 * the same criterion. POSIX (popen). */
#define _POSIX_C_SOURCE 200809L
#define main sig_arm_ref_main
#include "sig_arm_ref.c"
#undef main

int main(int argc, char** argv) {
    char cmd[4096], line[4096];
    FILE* p;
    long counted = -1, cannot = -1, bare = -1, a, b;
    int rc = 0;
    if (getenv("SIG_REF_INNER")) return sig_arm_ref_main(argc, argv);
    snprintf(cmd, sizeof cmd, "SIG_REF_INNER=1 '%s'", argv[0]);
    p = popen(cmd, "r");
    if (!p) { printf("FAIL line 3: the oracle did not run\n"); return 1; }
    while (fgets(line, sizeof line, p)) {
        const char* q;
        if (strncmp(line, "cases ", 6) == 0) fputs(line, stdout);
        if ((q = strstr(line, "header arm_unverified: B-cannot ")) && sscanf(q, "header arm_unverified: B-cannot %ld of %ld", &a, &b) == 2) {
            counted = a;
            cannot = b;
            fputs(line, stdout);
        }
        if ((q = strstr(line, "stale count, ")) && sscanf(q, "stale count, %ld with none", &a) == 1) bare = a;
    }
    pclose(p);
    if (counted < 0 || bare < 0) { printf("FAIL line 3: no summary from the oracle\n"); return 1; }
    if (counted != cannot) { printf("FAIL line 1: %ld of %ld cases B cannot run counted\n", counted, cannot); rc = 1; }
    if (bare) { printf("FAIL line 2: %ld wrong cases with no count\n", bare); rc = 1; }
    return rc;
}
