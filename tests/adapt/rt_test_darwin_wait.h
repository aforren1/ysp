/* rt_test_darwin_wait.h - the waits of ysp/rt.h on macOS (v0.10.1): a thread
 * without a real-time policy waits on a critical kevent timer, because
 * Darwin coalesces its nanosleep() and timed condition waits by a quarter to
 * a half of the wait (docs/rt.md, The wait on macOS). Included by
 * rt_test.c; macOS only.
 *
 * The bound is 500 us of lateness at the median of a 5 ms wait without a
 * spin. The coalesced wait was 1.27 to 2.5 ms late there (2026-11-05, M2 Mac
 * mini); the critical timer 34 to 42 us. A machine that is loaded enough to
 * make a critical timer 500 us late at the median would not run this test
 * meaningfully anyway. */
#if defined(__APPLE__)
#include <fcntl.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#define DW_BOUND_NS 500000
#define DW_N 15

static int dw_cmp(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

/* The median lateness of DW_N waits of 5 ms, spin 0. */
static int64_t dw_median_late(void) {
    int64_t late[DW_N];
    for (int i = 0; i < DW_N; i++) {
        int64_t t = yrt_now_ns() + 5000000;
        late[i] = yrt_sleep_until(t, 0);
    }
    qsort(late, DW_N, sizeof late[0], dw_cmp);
    return late[DW_N / 2];
}

/* The count of open descriptors below 1024: one less after a kqueue is
 * closed. */
static int dw_open_fds(void) {
    int n = 0;
    for (int fd = 0; fd < 1024; fd++)
        if (fcntl(fd, F_GETFD) != -1) n++;
    return n;
}

static int64_t dw_thread_late;

static void* dw_ordinary(void* arg) {
    (void)arg;
    dw_thread_late = dw_median_late();
    return NULL;
}

static void* dw_elevated(void* arg) {
    (void)arg;
    yrt_policy p = yrt_thread_elevate(NULL);
    printf("  darwin waits: elevated thread got %s\n", yrt_policy_name(p));
    dw_thread_late = dw_median_late();
    return NULL;
}

/* ran is the release of late and at to the polling thread. */
typedef struct { atomic_int ran; int64_t late; int64_t at; } dw_job;

static int dw_ran(dw_job* j) { return atomic_load_explicit(&j->ran, memory_order_acquire); }

static void dw_job_fn(void* ctx, const yrt_job_info* info) {
    dw_job* j = (dw_job*)ctx;
    j->late = info->late_ns;
    j->at = info->at_ns;
    atomic_store_explicit(&j->ran, info->flushed ? 2 : 1, memory_order_release);
}

static void test_darwin_waits(void) {
    pthread_t th;
    int fd0, fd1;

    /* the caller's thread, ordinary: the critical timer */
    fd0 = dw_open_fds();
    dw_thread_late = -1;
    pthread_create(&th, NULL, dw_ordinary, NULL);
    pthread_join(th, NULL);
    fd1 = dw_open_fds();
    printf("  darwin waits: ordinary thread, 5 ms, median %.1f us late\n", dw_thread_late / 1e3);
    CHECK(dw_thread_late >= 0 && dw_thread_late < DW_BOUND_NS,
          "an ordinary thread's 5 ms wait must not be coalesced (median under 500 us late)");
    CHECK_I(fd1, fd0, "the thread's exit must close its wait's kqueue");

    /* the main thread, then yrt_thread_cleanup() and a wait after it */
    int64_t m = dw_median_late();
    CHECK(m < DW_BOUND_NS, "the main thread's 5 ms wait must not be coalesced");
    fd0 = dw_open_fds();
    yrt_thread_cleanup();
    fd1 = dw_open_fds();
    CHECK_I(fd1, fd0 - 1, "yrt_thread_cleanup() must close this thread's kqueue");
    m = dw_median_late();
    CHECK(m < DW_BOUND_NS, "a wait after yrt_thread_cleanup() must open a new kqueue and not be coalesced");
    yrt_thread_cleanup();

    /* a TIME_CONSTRAINT thread keeps nanosleep(), which is not coalesced */
    dw_thread_late = -1;
    pthread_create(&th, NULL, dw_elevated, NULL);
    pthread_join(th, NULL);
    printf("  darwin waits: elevated thread, 5 ms, median %.1f us late\n", dw_thread_late / 1e3);
    CHECK(dw_thread_late >= 0 && dw_thread_late < DW_BOUND_NS, "an elevated thread's 5 ms wait");

    /* a fork child: the parent's kqueue is not inherited, and the child's
     * waits must open their own */
    (void)dw_median_late();   /* this thread has a kqueue now */
    {
        pid_t pid = fork();
        if (pid == 0) _exit(dw_median_late() < DW_BOUND_NS ? 0 : 1);
        int st = 0;
        waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a fork child's 5 ms wait must not be coalesced");
    }

    /* a worker without the TIME_CONSTRAINT rung: its own kqueue */
    {
        yrt_worker w;
        yrt_worker_desc d;
        dw_job j;
        int64_t late[DW_N];
        memset(&w, 0, sizeof w);
        memset(&d, 0, sizeof d);
        d.no_elevate = true;
        d.spin_ns = 1;   /* the coarse wait alone */
        CHECK(yrt_worker_open(&w, &d), "worker open");
        CHECK_I(yrt_worker_policy(&w), YRT_POLICY_NORMAL, "no_elevate gives NORMAL");
        for (int i = 0; i < DW_N; i++) {
            memset(&j, 0, sizeof j);
            yrt_worker_submit(&w, yrt_now_ns() + 5000000, dw_job_fn, &j);
            while (!dw_ran(&j)) yrt_sleep_ns(1000000);
            late[i] = j.late;
        }
        qsort(late, DW_N, sizeof late[0], dw_cmp);
        printf("  darwin waits: normal worker, 5 ms, median %.1f us late\n", late[DW_N / 2] / 1e3);
        CHECK(late[DW_N / 2] < DW_BOUND_NS, "a normal worker's 5 ms wait must not be coalesced");

        /* a replacement wakes the timed wait: a job 1 s ahead, replaced
         * after 10 ms by one 5 ms ahead */
        dw_job far, near;
        memset(&far, 0, sizeof far);
        memset(&near, 0, sizeof near);
        yrt_worker_submit(&w, yrt_now_ns() + 1000000000, dw_job_fn, &far);
        yrt_sleep_ns(10000000);
        int64_t t_near = yrt_now_ns() + 5000000;
        yrt_worker_submit(&w, t_near, dw_job_fn, &near);
        int64_t give_up = yrt_now_ns() + 500000000;
        while (!dw_ran(&near) && yrt_now_ns() < give_up) yrt_sleep_ns(1000000);
        CHECK(dw_ran(&near) == 1 && near.at - t_near < 100000000,
              "a submit must wake the worker's timed wait (the replacement ran on time)");
        CHECK(!dw_ran(&far), "the replaced job must not run");

        /* a close wakes it too: the job 1 s ahead is flushed at once */
        memset(&far, 0, sizeof far);
        yrt_worker_submit(&w, yrt_now_ns() + 1000000000, dw_job_fn, &far);
        yrt_sleep_ns(10000000);
        int64_t t_close = yrt_now_ns();
        fd0 = dw_open_fds();
        yrt_worker_close(&w);
        int64_t close_ns = yrt_now_ns() - t_close;
        fd1 = dw_open_fds();
        printf("  darwin waits: close during a 1 s wait took %.2f ms\n", close_ns / 1e6);
        CHECK(close_ns < 100000000 && dw_ran(&far) == 2, "a close must wake the worker's timed wait and flush its job");
        CHECK_I(fd1, fd0 - 1, "yrt_worker_close() must close the worker's kqueue");
    }
}
#endif
