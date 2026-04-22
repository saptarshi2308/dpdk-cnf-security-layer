/*
 * latency_benchmark.c
 * 
 * Standalone multi-run latency benchmark for the CNF Security Layer.
 * Measures seccomp filter installation overhead, per-syscall overhead,
 * and violation detection latency across N iterations.
 *
 * Each iteration forks a child process (seccomp filters are per-process
 * and cannot be removed once loaded), so we get a clean measurement
 * each time.
 *
 * Build:
 *   gcc -O3 -o latency_benchmark latency_benchmark.c cnf_security_layer.c -lseccomp -lm
 *
 * Usage:
 *   ./latency_benchmark [iterations] [output.csv]
 *   Default: 100 iterations, output to latency_results.csv
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>

#include "cnf_security_layer.h"

/* ---- Timing helpers ---- */

static inline long long timespec_to_ns(struct timespec *ts) {
    return (long long)ts->tv_sec * 1000000000LL + (long long)ts->tv_nsec;
}

static inline long long measure_ns(struct timespec *start, struct timespec *end) {
    return timespec_to_ns(end) - timespec_to_ns(start);
}

/* ---- Statistics ---- */

typedef struct {
    long long min_ns;
    long long max_ns;
    double    mean_ns;
    double    median_ns;
    double    stddev_ns;
    double    p95_ns;
    double    p99_ns;
    int       count;
} latency_stats_t;

static int cmp_ll(const void *a, const void *b) {
    long long va = *(const long long *)a;
    long long vb = *(const long long *)b;
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

static void compute_stats(long long *samples, int n, latency_stats_t *out) {
    if (n <= 0) { memset(out, 0, sizeof(*out)); return; }

    qsort(samples, n, sizeof(long long), cmp_ll);

    out->count  = n;
    out->min_ns = samples[0];
    out->max_ns = samples[n - 1];

    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)samples[i];
    out->mean_ns = sum / n;

    if (n % 2 == 0)
        out->median_ns = ((double)samples[n/2 - 1] + (double)samples[n/2]) / 2.0;
    else
        out->median_ns = (double)samples[n/2];

    double var = 0;
    for (int i = 0; i < n; i++) {
        double diff = (double)samples[i] - out->mean_ns;
        var += diff * diff;
    }
    out->stddev_ns = sqrt(var / n);

    int p95_idx = (int)(0.95 * n);
    int p99_idx = (int)(0.99 * n);
    if (p95_idx >= n) p95_idx = n - 1;
    if (p99_idx >= n) p99_idx = n - 1;
    out->p95_ns = (double)samples[p95_idx];
    out->p99_ns = (double)samples[p99_idx];
}

static void print_stats(const char *label, latency_stats_t *s) {
    printf("  %-35s\n", label);
    printf("    Iterations : %d\n",       s->count);
    printf("    Min        : %lld ns\n",  s->min_ns);
    printf("    Max        : %lld ns\n",  s->max_ns);
    printf("    Mean       : %.1f ns\n",  s->mean_ns);
    printf("    Median     : %.1f ns\n",  s->median_ns);
    printf("    Std Dev    : %.1f ns\n",  s->stddev_ns);
    printf("    P95        : %.1f ns\n",  s->p95_ns);
    printf("    P99        : %.1f ns\n",  s->p99_ns);
    printf("\n");
}

/* ---- Shared memory for child→parent results ---- */

typedef struct {
    long long seccomp_setup_ns;    /* Time to install seccomp filter */
    long long allowed_syscall_ns;  /* Time for an allowed syscall (clock_gettime) */
    long long violation_check_ns;  /* Time to check violation flag */
    int       valid;               /* 1 if measurement succeeded */
} child_result_t;

/* ---- Benchmark: Seccomp Setup Latency ---- */

/*
 * Each iteration forks a child that:
 *   1. Measures apply_cnf_security_layer() setup time
 *   2. Measures time for an allowed syscall post-seccomp
 *   3. Measures time for cnf_security_violation_detected() check
 *   4. Writes results to shared memory
 */
static void run_child_benchmark(child_result_t *result) {
    struct timespec t0, t1, t2, t3, t4, t5;

    /* --- Measure 1: Seccomp filter installation --- */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = apply_cnf_security_layer();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (rc != 0) {
        result->valid = 0;
        _exit(1);
    }
    result->seccomp_setup_ns = measure_ns(&t0, &t1);

    /* --- Measure 2: Allowed syscall overhead (clock_gettime is whitelisted) --- */
    struct timespec dummy;
    clock_gettime(CLOCK_MONOTONIC, &t2);
    for (int i = 0; i < 1000; i++) {
        clock_gettime(CLOCK_REALTIME, &dummy);
    }
    clock_gettime(CLOCK_MONOTONIC, &t3);
    result->allowed_syscall_ns = measure_ns(&t2, &t3) / 1000; /* per-call average */

    /* --- Measure 3: Violation check latency --- */
    clock_gettime(CLOCK_MONOTONIC, &t4);
    for (int i = 0; i < 10000; i++) {
        (void)cnf_security_violation_detected();
    }
    clock_gettime(CLOCK_MONOTONIC, &t5);
    result->violation_check_ns = measure_ns(&t4, &t5) / 10000; /* per-call average */

    result->valid = 1;
    close_cnf_security_layer();
    _exit(0);
}

/* ---- Benchmark: Baseline (no seccomp) ---- */

static void run_child_baseline(child_result_t *result) {
    struct timespec t0, t1, dummy;

    /* Measure allowed syscall WITHOUT seccomp for comparison */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 1000; i++) {
        clock_gettime(CLOCK_REALTIME, &dummy);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    result->seccomp_setup_ns   = 0;
    result->allowed_syscall_ns = measure_ns(&t0, &t1) / 1000;
    result->violation_check_ns = 0;
    result->valid = 1;
    _exit(0);
}

/* ---- Main ---- */

int main(int argc, char **argv) {
    int iterations = 100;
    const char *csv_file = "latency_results.csv";

    if (argc >= 2) iterations = atoi(argv[1]);
    if (argc >= 3) csv_file   = argv[2];
    if (iterations < 1) iterations = 1;
    if (iterations > 10000) iterations = 10000;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║     CNF Security Layer — Multi-Run Latency Benchmark       ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  Iterations : %-6d                                       ║\n", iterations);
    printf("║  Output CSV : %-45s║\n", csv_file);
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    /* Allocate arrays for results */
    long long *setup_samples   = calloc(iterations, sizeof(long long));
    long long *syscall_samples = calloc(iterations, sizeof(long long));
    long long *vcheck_samples  = calloc(iterations, sizeof(long long));
    long long *baseline_samples = calloc(iterations, sizeof(long long));

    if (!setup_samples || !syscall_samples || !vcheck_samples || !baseline_samples) {
        fprintf(stderr, "ERROR: Failed to allocate sample arrays\n");
        return 1;
    }

    /* Shared memory for child results */
    child_result_t *shared = mmap(NULL, sizeof(child_result_t),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* ---- Phase 1: Baseline measurements (no seccomp) ---- */
    printf("[Phase 1/2] Collecting baseline measurements (no seccomp)...\n");
    int baseline_ok = 0;
    for (int i = 0; i < iterations; i++) {
        memset(shared, 0, sizeof(*shared));

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); continue; }

        if (pid == 0) {
            run_child_baseline(shared);
            _exit(1); /* unreachable */
        }

        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && shared->valid) {
            baseline_samples[baseline_ok] = shared->allowed_syscall_ns;
            baseline_ok++;
        }

        if ((i + 1) % 25 == 0 || i == iterations - 1) {
            printf("  ... baseline %d/%d complete\n", i + 1, iterations);
        }
    }

    /* ---- Phase 2: Security layer measurements ---- */
    printf("[Phase 2/2] Collecting seccomp security layer measurements...\n");
    int valid_count = 0;
    for (int i = 0; i < iterations; i++) {
        memset(shared, 0, sizeof(*shared));

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); continue; }

        if (pid == 0) {
            run_child_benchmark(shared);
            _exit(1); /* unreachable */
        }

        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && shared->valid) {
            setup_samples[valid_count]   = shared->seccomp_setup_ns;
            syscall_samples[valid_count] = shared->allowed_syscall_ns;
            vcheck_samples[valid_count]  = shared->violation_check_ns;
            valid_count++;
        }

        if ((i + 1) % 25 == 0 || i == iterations - 1) {
            printf("  ... seccomp %d/%d complete (%d valid)\n",
                   i + 1, iterations, valid_count);
        }
    }

    if (valid_count == 0) {
        fprintf(stderr, "\nERROR: No valid measurements collected!\n");
        fprintf(stderr, "Make sure libseccomp is installed: sudo apt install libseccomp-dev\n");
        return 1;
    }

    /* ---- Compute statistics ---- */
    latency_stats_t setup_stats, syscall_stats, vcheck_stats, baseline_stats;
    compute_stats(setup_samples,    valid_count,  &setup_stats);
    compute_stats(syscall_samples,  valid_count,  &syscall_stats);
    compute_stats(vcheck_samples,   valid_count,  &vcheck_stats);
    compute_stats(baseline_samples, baseline_ok,  &baseline_stats);

    /* ---- Print results ---- */
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                    BENCHMARK RESULTS                       ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  Valid measurements: %d / %d                               \n", valid_count, iterations);
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");

    print_stats("[1] Seccomp Filter Installation (one-time cost)", &setup_stats);
    print_stats("[2] Per-Syscall Latency (WITH seccomp)", &syscall_stats);
    print_stats("[3] Baseline Per-Syscall (NO seccomp)", &baseline_stats);
    print_stats("[4] Violation Check (atomic read)", &vcheck_stats);

    /* Overhead calculation */
    if (baseline_stats.mean_ns > 0 && syscall_stats.mean_ns > 0) {
        double overhead_ns = syscall_stats.mean_ns - baseline_stats.mean_ns;
        double overhead_pct = (overhead_ns / baseline_stats.mean_ns) * 100.0;
        printf("  ┌─────────────────────────────────────────────────────────┐\n");
        printf("  │ SECCOMP OVERHEAD PER SYSCALL                           │\n");
        printf("  │   Absolute : %+.1f ns                                  \n", overhead_ns);
        printf("  │   Relative : %+.2f%%                                   \n", overhead_pct);
        printf("  └─────────────────────────────────────────────────────────┘\n\n");
    }

    /* ---- Write CSV ---- */
    FILE *fp = fopen(csv_file, "w");
    if (fp) {
        fprintf(fp, "iteration,seccomp_setup_ns,syscall_with_seccomp_ns,syscall_baseline_ns,violation_check_ns\n");
        int max_rows = valid_count > baseline_ok ? valid_count : baseline_ok;
        for (int i = 0; i < max_rows; i++) {
            fprintf(fp, "%d", i + 1);
            if (i < valid_count)
                fprintf(fp, ",%lld,%lld", setup_samples[i], syscall_samples[i]);
            else
                fprintf(fp, ",,");
            if (i < baseline_ok)
                fprintf(fp, ",%lld", baseline_samples[i]);
            else
                fprintf(fp, ",");
            if (i < valid_count)
                fprintf(fp, ",%lld", vcheck_samples[i]);
            else
                fprintf(fp, ",");
            fprintf(fp, "\n");
        }

        /* Summary row */
        fprintf(fp, "\n# SUMMARY\n");
        fprintf(fp, "# seccomp_setup: min=%lld max=%lld mean=%.1f median=%.1f stddev=%.1f p95=%.1f p99=%.1f\n",
            setup_stats.min_ns, setup_stats.max_ns, setup_stats.mean_ns,
            setup_stats.median_ns, setup_stats.stddev_ns, setup_stats.p95_ns, setup_stats.p99_ns);
        fprintf(fp, "# syscall_seccomp: min=%lld max=%lld mean=%.1f median=%.1f stddev=%.1f p95=%.1f p99=%.1f\n",
            syscall_stats.min_ns, syscall_stats.max_ns, syscall_stats.mean_ns,
            syscall_stats.median_ns, syscall_stats.stddev_ns, syscall_stats.p95_ns, syscall_stats.p99_ns);
        fprintf(fp, "# syscall_baseline: min=%lld max=%lld mean=%.1f median=%.1f stddev=%.1f p95=%.1f p99=%.1f\n",
            baseline_stats.min_ns, baseline_stats.max_ns, baseline_stats.mean_ns,
            baseline_stats.median_ns, baseline_stats.stddev_ns, baseline_stats.p95_ns, baseline_stats.p99_ns);

        fclose(fp);
        printf("  Results saved to: %s\n\n", csv_file);
    } else {
        fprintf(stderr, "  WARNING: Could not write CSV to %s: %s\n\n", csv_file, strerror(errno));
    }

    /* Cleanup */
    free(setup_samples);
    free(syscall_samples);
    free(vcheck_samples);
    free(baseline_samples);
    munmap(shared, sizeof(child_result_t));

    return 0;
}
