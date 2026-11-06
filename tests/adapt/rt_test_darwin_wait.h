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
#include <sys/sysctl.h>
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

/* 1 in a virtual machine. On the GitHub macOS runner (a VM) a 5 ms wait
 * was late at the median by, on 2026-11-06: critical timer 73 to 100 us in
 * one run and 2522 us (half the wait) in the next, on the same code;
 * nanosleep() on a TIME_CONSTRAINT thread 918 and 1964 us. The M2 Mac mini
 * gives 22 to 47 us and 14 to 16 us, also under taskpolicy -b, -l 5 and
 * -t 5. The guest's timers, not the header, and they change from run to
 * run, so in a VM the lateness bounds are printed, not checked; the
 * descriptor and wake checks hold everywhere. */
static int dw_vm = -1;
static int dw_in_vm(void) {
    if (dw_vm < 0) {
        int v = 0;
        size_t n = sizeof v;
        dw_vm = sysctlbyname("kern.hv_vmm_present", &v, &n, NULL, 0) == 0 && v == 1;
    }
    return dw_vm;
}
#define DW_LATE_OK(late, what) do { if (!dw_in_vm()) CHECK((late) < DW_BOUND_NS, what); } while (0)

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

    if (dw_in_vm()) printf("  darwin waits: a virtual machine: lateness printed, not checked\n");

    /* the caller's thread, ordinary: the critical timer */
    fd0 = dw_open_fds();
    dw_thread_late = -1;
    pthread_create(&th, NULL, dw_ordinary, NULL);
    pthread_join(th, NULL);
    fd1 = dw_open_fds();
    printf("  darwin waits: ordinary thread, 5 ms, median %.1f us late\n", dw_thread_late / 1e3);
    CHECK(dw_thread_late >= 0, "an ordinary thread's waits ran");
    DW_LATE_OK(dw_thread_late, "an ordinary thread's 5 ms wait must not be coalesced (median under 500 us late)");
    CHECK_I(fd1, fd0, "the thread's exit must close its wait's kqueue");

    /* the main thread, then yrt_thread_cleanup() and a wait after it */
    int64_t m = dw_median_late();
    DW_LATE_OK(m, "the main thread's 5 ms wait must not be coalesced");
    fd0 = dw_open_fds();
    yrt_thread_cleanup();
    fd1 = dw_open_fds();
    CHECK_I(fd1, fd0 - 1, "yrt_thread_cleanup() must close this thread's kqueue");
    m = dw_median_late();
    DW_LATE_OK(m, "a wait after yrt_thread_cleanup() must open a new kqueue and not be coalesced");
    yrt_thread_cleanup();

    /* a TIME_CONSTRAINT thread keeps nanosleep(), which is not coalesced */
    dw_thread_late = -1;
    pthread_create(&th, NULL, dw_elevated, NULL);
    pthread_join(th, NULL);
    printf("  darwin waits: elevated thread, 5 ms, median %.1f us late\n", dw_thread_late / 1e3);
    CHECK(dw_thread_late >= 0, "an elevated thread's 5 ms wait ran");
    DW_LATE_OK(dw_thread_late, "an elevated thread's 5 ms wait");

    /* a fork child: the parent's kqueue is not inherited, and the child's
     * first wait must open its own (one descriptor more), not use the
     * parent's number. The child sends its median and that count back. */
    (void)dw_median_late();   /* this thread has a kqueue now */
    {
        int pfd[2];
        int64_t r[2] = { -1, -1 };
        CHECK(pipe(pfd) == 0, "pipe for the fork child");
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]);
            int c0 = dw_open_fds();
            int64_t out[2];
            out[0] = dw_median_late();
            out[1] = dw_open_fds() - c0;
            _exit(write(pfd[1], out, sizeof out) == (ssize_t)sizeof out ? 0 : 1);
        }
        close(pfd[1]);
        CHECK(read(pfd[0], r, sizeof r) == (ssize_t)sizeof r, "the fork child's report");
        close(pfd[0]);
        int st = 0;
        waitpid(pid, &st, 0);
        printf("  darwin waits: fork child, 5 ms, median %.1f us late, %lld descriptor(s) opened\n",
               r[0] / 1e3, (long long)r[1]);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the fork child exited cleanly");
        CHECK_I((int)r[1], 1, "a fork child's first wait must open its own kqueue");
        CHECK(r[0] >= 0, "the fork child's waits ran");
        DW_LATE_OK(r[0], "a fork child's 5 ms wait must not be coalesced");
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
        DW_LATE_OK(late[DW_N / 2], "a normal worker's 5 ms wait must not be coalesced");

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
