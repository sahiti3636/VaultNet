#ifndef AUDIT_H
#define AUDIT_H

/*
 * Append-only audit log.
 *
 * Every entry is written under a mutex so lines never interleave.
 * A global monotonically-increasing sequence counter gives each entry
 * a unique, ordered identifier.
 *
 * Format (pipe-delimited for easy grep/awk):
 *   seq|ISO-8601-ts|EVENT|username|detail|amount|RESULT
 */

/* Call once at startup. Returns 0 on success, -1 on error. */
int audit_init(const char *log_path);

/* Call at shutdown to flush and close the log file. */
void audit_close(void);

/*
 * Log one event.  amount may be 0.0 for non-monetary events.
 * result should be "OK" or "ERR".
 */
void audit_log(const char *event, const char *username,
               const char *detail, double amount,
               const char *result);

/*
 * Scan the audit log and write the last `n` lines whose username field
 * matches `username` to the file descriptor `fd`.
 * Returns the number of lines written.
 */
int audit_history(const char *username, int n, int fd);

#endif /* AUDIT_H */
