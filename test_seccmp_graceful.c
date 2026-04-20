/*
 * Test harness for graceful seccomp violation handling.
 * 
 * This test forks a child process:
 * - child: applies CNF security layer, attempts a blocked syscall (socket),
 *          the SIGSYS handler sets a flag instead of exiting.
 * - parent: waits for child to indicate a violation, then gracefully collects
 *           child status without harsh termination.
 * 
 * Demonstrates: graceful shutdown, atomic flag checking, parent coordination.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include "cnf_security_layer.h"

void child_work(void) {
    int rc = apply_cnf_security_layer();
    if (rc != 0) {
        fprintf(stderr, "[CHILD] apply_cnf_security_layer failed: %d\n", rc);
        exit(1);
    }

    printf("[CHILD] Seccomp profile applied. Attempting blocked syscall (ptrace)...\n");
    fflush(stdout);

    /* Attempt a syscall not in the CNF whitelist: ptrace */
    int ret = ptrace(PTRACE_TRACEME, 0, NULL, NULL);
    if (ret == 0) {
        printf("[CHILD] ptrace() unexpectedly succeeded\n");
        exit(2);
    }

    /* After the SIGSYS signal, check if violation was detected */
    if (cnf_security_violation_detected()) {
        int vcount = cnf_security_violation_count();
        printf("[CHILD] Violation detected! Count: %d. Performing graceful shutdown.\n", vcount);
        fflush(stdout);

        /* Simulate some cleanup work */
        sleep(1);
        printf("[CHILD] Cleanup complete. Exiting gracefully.\n");
        fflush(stdout);

        close_cnf_security_layer();
        exit(0);
    } else {
        printf("[CHILD] No violation detected (unexpected).\n");
        exit(3);
    }
}

int main(void) {
    printf("[PARENT] Forking test child...\n");
    fflush(stdout);

    pid_t child_pid = fork();
    if (child_pid < 0) {
        perror("fork");
        return 1;
    }

    if (child_pid == 0) {
        /* Child process */
        child_work();
        _exit(EXIT_FAILURE);  /* should not reach here */
    }

    /* Parent process */
    printf("[PARENT] Child PID: %d. Waiting for child...\n", child_pid);
    fflush(stdout);

    int status;
    pid_t wpid = waitpid(child_pid, &status, 0);
    if (wpid < 0) {
        perror("waitpid");
        return 1;
    }

    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        printf("[PARENT] Child exited with code %d\n", exit_code);
        if (exit_code == 0) {
            printf("[PARENT] SUCCESS: Child performed graceful shutdown after violation.\n");
            return 0;
        } else {
            printf("[PARENT] FAILURE: Child exited with unexpected code.\n");
            return 1;
        }
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        printf("[PARENT] Child terminated by signal %d\n", sig);
        printf("[PARENT] FAILURE: Child should not have been killed by signal.\n");
        return 1;
    }

    printf("[PARENT] Unexpected status.\n");
    return 1;
}
