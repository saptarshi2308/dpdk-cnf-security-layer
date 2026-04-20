#ifndef CNF_SECURITY_LAYER_H
#define CNF_SECURITY_LAYER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cnf_security_layer.h
 * Public header for the CNF security layer.
 * Implementation lives in cnf_security_layer.c to avoid multiple-definition
 * issues and to keep signal/async-safety confined to the .c file.
 */

/* Install the container/network-function security layer.
 * Returns 0 on success (seccomp loaded), or a negative value on error.
 */
int apply_cnf_security_layer(void);

/* Clean up resources used by the security layer (closes audit log). */
void close_cnf_security_layer(void);

/* Check if a seccomp violation has occurred (atomic read).
 * Returns 1 if a violation was detected, 0 otherwise.
 */
int cnf_security_violation_detected(void);

/* Get the count of violations detected so far (atomic read). */
int cnf_security_violation_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CNF_SECURITY_LAYER_H */

