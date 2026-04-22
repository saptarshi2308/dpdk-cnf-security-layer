/*
 * Robust implementation of the CNF security layer.
 * - Keeps audit_fd file-scoped (not in header)
 * - Uses a minimal, async-signal-safe SIGSYS handler
 * - Performs return-code checks for seccomp operations
 * - Opens audit file with safer flags (O_CLOEXEC, O_NOFOLLOW if available)
 */

#include "cnf_security_layer.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <seccomp.h>

/* file-scoped audit FD - not exposed in the header */
static int audit_fd = -1;

/* Atomic counter: number of seccomp violations detected */
static _Atomic int violation_count = 0;

/* Helper: write a JSON audit line. Not async-signal-safe. */
static void write_audit_log(const char* event_type, const char* details) {
    char buffer[512];
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = 0; ts.tv_nsec = 0;
    }
    int len = snprintf(buffer, sizeof(buffer),
                       "{\"timestamp\": %ld.%09ld, \"event\": \"%s\", \"details\": \"%s\"}\n",
                       (long)ts.tv_sec, (long)ts.tv_nsec, event_type, details);
    if (len <= 0) return;
// THIS IS A LOW LEVEL CALL FOR THE MEMORY ADDRESS
    if (audit_fd >= 0) {
        ssize_t w = write(audit_fd, buffer, (size_t)len);
        (void)w;
    } else {
        /* fallback to stderr */
        ssize_t w = write(STDERR_FILENO, buffer, (size_t)len);
        (void)w;
    }
}

/* Async-signal-safe integer to ASCII (base 10). Returns pointer to string. */
static char* itoa_signal_safe(long x, char *buf, size_t bufsz) {
    if (bufsz == 0) return buf;
    char tmp[32];
    int pos = 0;
    unsigned long ux = (x < 0) ? (unsigned long)(-x) : (unsigned long)x;
    if (ux == 0) tmp[pos++] = '0';
    while (ux > 0 && pos < (int)sizeof(tmp)-1) {
        tmp[pos++] = (char)('0' + (ux % 10));
        ux /= 10;
    }
    if (x < 0 && pos < (int)sizeof(tmp)-1) tmp[pos++] = '-';
    /* reverse into provided buffer */
    int out = 0;
    while (pos > 0 && (size_t)out < bufsz - 1) {
        buf[out++] = tmp[--pos];
    }
    buf[out] = '\0';
    return buf;
}

/* Minimal, async-signal-safe SIGSYS handler. Sets atomic flag instead of exiting. */
static void sigsys_handler(int signum, siginfo_t *info, void *context) {
    (void)signum; (void)context;
    
    /* Increment violation counter atomically */
    atomic_fetch_add(&violation_count, 1);
    
    /* Write a minimal message to audit log (async-signal-safe) */
    char msg[128];
    const char *prefix = "SECURITY: unauthorized syscall: ";
    size_t pfx_len = strlen(prefix);
    if (pfx_len > sizeof(msg) - 2) pfx_len = sizeof(msg) - 2;
    memcpy(msg, prefix, pfx_len);
    char numbuf[32];
    long sc = -1;
#ifdef __linux__
    if (info) sc = (long)info->si_syscall;
#endif
    itoa_signal_safe(sc, numbuf, sizeof(numbuf));
    size_t nlen = strnlen(numbuf, sizeof(numbuf));
    if (pfx_len + nlen + 2 > sizeof(msg)) nlen = sizeof(msg) - pfx_len - 2;
    memcpy(msg + pfx_len, numbuf, nlen);
    msg[pfx_len + nlen] = '\n';
    ssize_t w;
    if (audit_fd >= 0) {
        w = write(audit_fd, msg, pfx_len + nlen + 1);
        (void)w;
    } else {
        w = write(STDERR_FILENO, msg, pfx_len + nlen + 1);
        (void)w;
    }
    /* Return to caller instead of exiting - caller will detect violation via flag */
}

int apply_cnf_security_layer(void) {
    struct timespec start_time, end_time;
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        start_time.tv_sec = 0; start_time.tv_nsec = 0;
    }

    /* 1. Initialize enterprise audit log */
    int flags = O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    audit_fd = open("/tmp/dpdk_security_audit.json", flags, S_IRUSR | S_IWUSR | S_IRGRP);
    if (audit_fd < 0) {
        /* fallback: keep audit_fd == -1 and continue */
        int saved = errno;
        char errbuf[128];
        int l = snprintf(errbuf, sizeof(errbuf), "[SECURITY] failed to open audit log: %s\n", strerror(saved));
        if (l > 0) {
            ssize_t w = write(STDERR_FILENO, errbuf, (size_t)l);
            (void)w;
        }
    } else {
        write_audit_log("INITIALIZATION", "Starting CNF Security Boot Sequence");
    }

    /* 2. Register signal handler for SIGSYS */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = sigsys_handler;
    if (sigaction(SIGSYS, &sa, NULL) != 0) {
        write_audit_log("WARN", "Failed to register SIGSYS handler");
    }

    /* 3. Build a default-deny seccomp filter that allows necessary syscalls */
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_TRAP);
    if (ctx == NULL) {
        write_audit_log("ERROR", "seccomp_init failed");
        return -1;
    }

    if (seccomp_attr_set(ctx, SCMP_FLTATR_CTL_TSYNC, 1) != 0) {
        /* non-fatal: log and continue */
        write_audit_log("INFO", "seccomp CTL_TSYNC not available or failed");
    }

    /* Helper macro to add rule and log on failure */
#define ADD_ALLOW(syscall_name) do { \
        if (seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(syscall_name), 0) != 0) { \
            char msg[128]; snprintf(msg, sizeof(msg), "seccomp allow failed: %s", #syscall_name); \
            write_audit_log("WARN", msg); \
        } \
    } while (0)

    /* core I/O and process control */
    ADD_ALLOW(read);
    ADD_ALLOW(write);
    ADD_ALLOW(exit);
    ADD_ALLOW(exit_group);

    /* time / sleeping */
    ADD_ALLOW(clock_gettime);
    ADD_ALLOW(nanosleep);

    /* threading / futex */
    ADD_ALLOW(futex);
    ADD_ALLOW(rt_sigreturn);

    /* DPDK and libs commonly need memory and file operations */
    ADD_ALLOW(mmap);
    ADD_ALLOW(munmap);
    ADD_ALLOW(mprotect);
    ADD_ALLOW(brk);
    ADD_ALLOW(open);
    ADD_ALLOW(close);
    ADD_ALLOW(fstat);
    ADD_ALLOW(ioctl);
    ADD_ALLOW(fcntl);

    /* scheduling and identification */
    ADD_ALLOW(getpid);
    ADD_ALLOW(gettid);
    ADD_ALLOW(sched_yield);

    /* polling and epoll used by some runtimes */
    ADD_ALLOW(poll);
    ADD_ALLOW(epoll_wait);
    ADD_ALLOW(epoll_ctl);

#undef ADD_ALLOW

    write_audit_log("INFO", "Seccomp rules prepared, loading into kernel");

    int rc = seccomp_load(ctx);
    if (rc != 0) {
        write_audit_log("ERROR", "seccomp_load failed");
    }
    seccomp_release(ctx);

    if (clock_gettime(CLOCK_MONOTONIC, &end_time) != 0) {
        end_time.tv_sec = 0; end_time.tv_nsec = 0;
    }
    long latency_ns = (end_time.tv_sec - start_time.tv_sec) * 1000000000L + (end_time.tv_nsec - start_time.tv_nsec);
    char latency_msg[128];
    snprintf(latency_msg, sizeof(latency_msg), "Seccomp profile loaded. Latency overhead: %ld ns", latency_ns);
    write_audit_log("PERFORMANCE_METRIC", latency_msg);
    write_audit_log("STATUS", rc == 0 ? "ZERO_TRUST_LOCKDOWN_ACTIVE" : "ZERO_TRUST_LOCKDOWN_FAILED");

    return rc;
}

void close_cnf_security_layer(void) {
    if (audit_fd >= 0) {
        close(audit_fd);
        audit_fd = -1;
    }
}

int cnf_security_violation_detected(void) {
    return atomic_load(&violation_count) > 0 ? 1 : 0;
}

int cnf_security_violation_count(void) {
    return atomic_load(&violation_count);
}

