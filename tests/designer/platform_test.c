/* platform_test.c - dsg_remove_tree() (designer/core/platform.c; docs/
 * designer.md, "Part S as built"). It deletes files, so every path it
 * touches is under WORK/remove_tree, and every refusal it checks is one
 * that, if it did not hold, would delete only inside that folder: a ".."
 * from the root reaches the sandbox and no further, and the absolute path
 * and the links name the sandbox's own "outside" folder.
 *
 * Usage: test_designer_platform WORK (an absolute folder). Exit 0 on pass.
 *
 *   sandbox/
 *     root/            the root given to dsg_remove_tree()
 *       a/f1, a/sub/f2, a/sub/deep/f3
 *       a/esc       -> ../../outside   a link inside the tree, out of root
 *       a/esc_file  -> ../../outside/keep
 *       b/f4                           root's other child
 *       file5                          a file as rel
 *       link_out    -> ../outside      a link as rel, and on the way
 *       link_in     -> b               a link inside root, as rel
 *     outside/keep                     what the links point at
 *     sibling/keep                     root's sibling
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700   /* nftw(), symlink() under -std=c11 */
#endif
#include "dsg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char box[DSG_PATH_MAX], root[DSG_PATH_MAX];

static const char* at(const char* rel) {
    static char p[4][DSG_PATH_MAX];
    static int k;
    k = (k + 1) & 3;
    if (dsg_join(p[k], sizeof p[k], box, rel)) {
        printf("FAIL: path too long\n");
        exit(1);
    }
    return p[k];
}

static bool exists(const char* rel) { return dsg_is_dir(at(rel)) || dsg_is_file(at(rel)); }

static void put(const char* rel) {
    char err[512];
    if (dsg_write_file(at(rel), "x", 1, err, sizeof err)) {
        printf("FAIL: setup: %s\n", err);
        exit(1);
    }
}

static void mkd(const char* rel) {
    char err[512];
    if (dsg_make_dir(at(rel), err, sizeof err)) {
        printf("FAIL: setup: %s\n", err);
        exit(1);
    }
}

#if !defined(_WIN32)
static bool is_link(const char* rel) {
    struct stat st;
    return lstat(at(rel), &st) == 0 && S_ISLNK(st.st_mode);
}

static void link_to(const char* target, const char* rel) {
    if (symlink(target, at(rel)) != 0) {
        printf("FAIL: setup: symlink %s\n", rel);
        exit(1);
    }
}

/* The test's own cleaner, so a mutant of dsg_remove_tree() cannot make the
 * setup of the next run follow a link: nftw() with FTW_PHYS never does. */
static int rm_one(const char* p, const struct stat* st, int flag, struct FTW* f) {
    (void)st;
    (void)f;
    return flag == FTW_DP ? rmdir(p) : unlink(p);
}
static void clean_box(void) {
    if (dsg_is_dir(box) && nftw(box, rm_one, 16, FTW_DEPTH | FTW_PHYS) != 0) {
        printf("FAIL: cannot clean %s\n", box);
        exit(1);
    }
}
#else
static void clean_box(void) {
    char err[512], parent[DSG_PATH_MAX];
    const char* slash = strrchr(box, '/');
    if (!dsg_is_dir(box) || !slash) return;
    snprintf(parent, sizeof parent, "%.*s", (int)(slash - box), box);
    if (dsg_remove_tree(parent, slash + 1, err, sizeof err)) {
        printf("FAIL: cannot clean %s: %s\n", box, err);
        exit(1);
    }
}
#endif

static void setup(void) {
    char err[512] = "";
    clean_box();
    if (dsg_make_dir(box, err, sizeof err)) {
        printf("FAIL: setup: %s\n", err);
        exit(1);
    }
    mkd("root");
    mkd("root/a");
    mkd("root/a/sub");
    mkd("root/a/sub/deep");
    mkd("root/b");
    mkd("outside");
    mkd("sibling");
    put("root/a/f1");
    put("root/a/sub/f2");
    put("root/a/sub/deep/f3");
    put("root/b/f4");
    put("root/file5");
    put("outside/keep");
    put("sibling/keep");
#if !defined(_WIN32)
    link_to("../../outside", "root/a/esc");
    link_to("../../outside/keep", "root/a/esc_file");
    link_to("../outside", "root/link_out");
    link_to("b", "root/link_in");
#endif
}

/* Every file of the setup outside root/a is still there. */
static bool others_whole(void) {
    return exists("outside/keep") && exists("sibling/keep") && exists("root/b/f4") && exists("root/file5") && dsg_is_dir(root);
}

static void refused(const char* r, const char* why, int line) {
    char err[512] = "";
    int rc = dsg_remove_tree(root, r, err, sizeof err);
    checks++;
    if (rc != -1 || !err[0]) {
        fails++;
        printf("FAIL line %d: '%s' (%s) gave %d, '%s'\n", line, r ? r : "(null)", why, rc, err);
    }
    checks++;
    if (!others_whole() || !exists("root/a/f1") || !exists("root/a/sub/deep/f3")) {
        fails++;
        printf("FAIL line %d: '%s' (%s) removed something\n", line, r ? r : "(null)", why);
    }
}
#define REFUSED(r, why) refused(r, why, __LINE__)

int main(int argc, char** argv) {
    char err[512] = "";
    char abs_out[DSG_PATH_MAX] = "";
    if (argc < 2) {
        printf("usage: test_designer_platform WORK\n");
        return 2;
    }
    /* the sandbox must be the build tree's own folder: refuse a relative
     * WORK, which would put it wherever the test runs */
#if defined(_WIN32)
    if (!(argv[1][0] && argv[1][1] == ':') && argv[1][0] != '/' && argv[1][0] != '\\') {
#else
    if (argv[1][0] != '/') {
#endif
        printf("FAIL: WORK must be absolute: %s\n", argv[1]);
        return 1;
    }
    if (dsg_make_dir(argv[1], err, sizeof err) || dsg_join(box, sizeof box, argv[1], "remove_tree") ||
        dsg_join(root, sizeof root, box, "root")) {
        printf("FAIL: the work folder: %s\n", err);
        return 1;
    }
    setup();

    /* refusals: nothing goes */
    REFUSED(NULL, "no path");
    REFUSED("", "an empty path");
    REFUSED(".", "the root itself");
    REFUSED("a/.", "a '.' name");
    REFUSED("..", "the root's parent");
    REFUSED("../sibling", "the root's sibling");
    REFUSED("a/../..", "up through a name");
    REFUSED("a//sub", "an empty name");
    REFUSED("a/", "a trailing separator");
    snprintf(abs_out, sizeof abs_out, "%s", at("outside"));
    REFUSED(abs_out, "an absolute path");
    REFUSED("C:/outside", "a drive");
    REFUSED("a\\sub", "a backslash");
    {
        char e2[512] = "";
        CHECK(dsg_remove_tree("", "a", e2, sizeof e2) == -1 && exists("root/a/f1"), "an empty root is refused");
    }
#if !defined(_WIN32)
    REFUSED("link_out", "a link out of the root, as the name");
    REFUSED("link_out/keep", "a link out of the root, on the way");
    CHECK(is_link("root/link_out"), "the refused link is still there");
    REFUSED("link_in", "a link inside the root");
    CHECK(is_link("root/link_in") && exists("root/b/f4"), "the link inside and its folder are still there");
#endif

    /* not there: nothing to do */
    CHECK(dsg_remove_tree(root, "missing", err, sizeof err) == 0, "a missing name is not an error: %s", err);
    CHECK(dsg_remove_tree(root, "missing/deeper", err, sizeof err) == 0, "a missing folder on the way is not an error: %s", err);
    CHECK(dsg_remove_tree(root, "file5/x", err, sizeof err) == 0 && exists("root/file5"), "a file on the way: nothing removed");

    /* a file */
    CHECK(dsg_remove_tree(root, "file5", err, sizeof err) == 0 && !exists("root/file5"), "a file is removed: %s", err);
    put("root/file5");

    /* a folder below another */
    CHECK(dsg_remove_tree(root, "a/sub/deep", err, sizeof err) == 0 && !exists("root/a/sub/deep"), "a/sub/deep removed: %s", err);
    CHECK(exists("root/a/sub/f2") && exists("root/a/f1") && others_whole(), "its parent and the rest stay");

    /* the tree with the links in it: the links go, their targets stay */
    CHECK(dsg_remove_tree(root, "a", err, sizeof err) == 0, "a removed: %s", err);
    CHECK(!exists("root/a"), "a is gone, with its files and links");
    CHECK(exists("outside/keep") && dsg_is_dir(at("outside")), "the link inside a went, not what it pointed at");
    CHECK(others_whole(), "root's other child, root's sibling and root itself stay");
#if !defined(_WIN32)
    CHECK(is_link("root/link_out") && is_link("root/link_in"), "root's other links stay");
#endif
    CHECK(dsg_remove_tree(root, "a", err, sizeof err) == 0, "removing it again is not an error: %s", err);

    /* the preview's case: run-N folders under DSG_DIR/preview, one kept */
    mkd("root/run-1");
    mkd("root/run-2");
    put("root/run-1/trials.csv");
    put("root/run-2/trials.csv");
    CHECK(dsg_remove_tree(root, "run-1", err, sizeof err) == 0 && !exists("root/run-1") && exists("root/run-2/trials.csv"),
          "run-1 removed, run-2 kept: %s", err);

    clean_box();
    CHECK(!dsg_is_dir(box), "the sandbox is gone");
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
