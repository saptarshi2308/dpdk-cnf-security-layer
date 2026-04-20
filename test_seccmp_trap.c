/* Simple test: apply CNF security layer then invoke a forbidden syscall (socket)
 * Expected: SIGSYS handler runs, writes a short message to the audit log or stderr,
 * and the process exits.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include "cnf_security_layer.h"

int main(void) {
    int rc = apply_cnf_security_layer();
    if (rc != 0) {
        fprintf(stderr, "apply_cnf_security_layer returned %d\n", rc);
    }

    /* Attempt a syscall not in the CNF whitelist: create a socket */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    /* If the process is still running, print the result and exit normally */
    if (s >= 0) {
        printf("socket() returned %d (unexpected)\n", s);
        close(s);
    } else {
        perror("socket");
    }

    close_cnf_security_layer();
    return 0;
}
