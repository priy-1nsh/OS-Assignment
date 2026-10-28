/* Shared harness for Program A (CPU-bound) and Program B (I/O-bound). */
#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>

#define NCHILD 8

struct report { int id; int64_t start_ns; int64_t end_ns; };

static pid_t child_pids[NCHILD];

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* if `timeout` kills us, do not leave orphans hogging core 0 */
static void on_term(int sig)
{
    (void)sig;
    for (int i = 0; i < NCHILD; i++)
        if (child_pids[i] > 0) kill(child_pids[i], SIGKILL);
    _exit(1);
}

static const char *policy_name(int p)
{
    switch (p) {
    case SCHED_FIFO:  return "SCHED_FIFO";
    case SCHED_RR:    return "SCHED_RR";
    case SCHED_OTHER: return "SCHED_OTHER";
    default:          return "UNKNOWN";
    }
}

static int run_benchmark(const char *title, void (*task)(int))
{
    int fds[2];
    struct report rep[NCHILD];
    int got = 0;

    if (pipe(fds) < 0) { perror("pipe"); return 1; }
    signal(SIGTERM, on_term);

    int64_t ref = now_ns();                       /* reference timestamp */

    for (int i = 0; i < NCHILD; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid == 0) {
            struct report r;
            r.id = i;
            r.start_ns = now_ns();                /* first action in child */
            close(fds[0]);
            task(i);
            r.end_ns = now_ns();                  /* just before exit */
            if (write(fds[1], &r, sizeof r) != (ssize_t)sizeof r) _exit(2);
            close(fds[1]);
            _exit(0);
        }
        child_pids[i] = pid;
    }
    close(fds[1]);

    for (int i = 0; i < NCHILD; i++) waitpid(child_pids[i], NULL, 0);
    int64_t total_ns = now_ns() - ref;

    struct rusage ru;
    getrusage(RUSAGE_CHILDREN, &ru);

    while (got < NCHILD &&
           read(fds[0], &rep[got], sizeof rep[0]) == (ssize_t)sizeof rep[0])
        got++;
    close(fds[0]);
    if (got != NCHILD) fprintf(stderr, "warning: only %d/%d reports\n", got, NCHILD);

    double sum_r = 0, max_r = 0, sum_t = 0, max_t = 0;
    for (int i = 0; i < got; i++) {
        double r = (rep[i].start_ns - ref) / 1e6;     /* ms */
        double t = (rep[i].end_ns   - ref) / 1e6;
        sum_r += r; sum_t += t;
        if (r > max_r) max_r = r;
        if (t > max_t) max_t = t;
    }
    double total_s = total_ns / 1e9;
    int pol = sched_getscheduler(0);

    printf("%s\n--------------------------------------------------\n", title);
    if (pol == SCHED_RR) {
        struct timespec q;
        sched_rr_get_interval(0, &q);
        printf("Scheduling Policy: SCHED_RR\n");
        printf("Time Quantum: %.0fms\n", q.tv_sec * 1e3 + q.tv_nsec / 1e6);
    } else {
        printf("Scheduling Policy: %s\n", policy_name(pol));
    }
    printf("  Involuntary Context Switches: %ld\n", ru.ru_nivcsw);
    printf("  Voluntary Context Switches: %ld\n", ru.ru_nvcsw);
    printf("  Average Response Time: %.2fms\n", got ? sum_r / got : 0.0);
    printf("  Maximum Response Time: %.2fms\n", max_r);
    printf("  Average Turnaround Time: %.2fms\n", got ? sum_t / got : 0.0);
    printf("  Maximum Turnaround Time: %.2fms\n", max_t);
    printf("  Throughput: %.2f children/s\n", NCHILD / total_s);
    printf("  Total Elapsed Time: %.2fs\n\n", total_s);
    return 0;
}
#endif
