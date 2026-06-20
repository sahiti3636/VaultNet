#include "audit.h"
#include "netutil.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>

#define AUDIT_LINE_MAX 512
#define HISTORY_SCAN_MAX 100000  /* scan at most this many lines */

static FILE            *g_log_fp  = NULL;
static pthread_mutex_t  g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint64_t         g_seq     = 0;

static void iso8601_now(char *buf, size_t len) {
    time_t t = time(NULL);
    struct tm tm_info;
    gmtime_r(&t, &tm_info);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm_info);
}

int audit_init(const char *log_path) {
    pthread_mutex_lock(&g_log_mtx);
    g_log_fp = fopen(log_path, "a");
    if (!g_log_fp) {
        perror("audit_init: fopen");
        pthread_mutex_unlock(&g_log_mtx);
        return -1;
    }
    /* Disable buffering so each write reaches the OS immediately */
    setvbuf(g_log_fp, NULL, _IOLBF, 0);
    pthread_mutex_unlock(&g_log_mtx);
    return 0;
}

void audit_close(void) {
    pthread_mutex_lock(&g_log_mtx);
    if (g_log_fp) {
        fflush(g_log_fp);
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    pthread_mutex_unlock(&g_log_mtx);
}

void audit_log(const char *event, const char *username,
               const char *detail, double amount,
               const char *result) {
    char ts[32];
    iso8601_now(ts, sizeof(ts));

    pthread_mutex_lock(&g_log_mtx);
    uint64_t seq = ++g_seq;
    if (g_log_fp) {
        fprintf(g_log_fp, "%llu|%s|%s|%s|%s|%.2f|%s\n",
                (unsigned long long)seq,
                ts,
                event    ? event    : "",
                username ? username : "",
                detail   ? detail   : "",
                amount,
                result   ? result   : "");
    }
    pthread_mutex_unlock(&g_log_mtx);
}

/*
 * Collect up to `n` matching lines by scanning the entire log file.
 * We store pointers to matching lines in a ring buffer and then emit
 * the most recent `n` of them.
 */
int audit_history(const char *username, int n, int fd) {
    if (!g_log_fp || n <= 0) return 0;

    /* Snapshot the current log path by re-opening for reading */
    pthread_mutex_lock(&g_log_mtx);
    fflush(g_log_fp);
    pthread_mutex_unlock(&g_log_mtx);

    FILE *rfp = fopen(AUDIT_FILE, "r");
    if (!rfp) return 0;

    /* Ring buffer of matched lines (heap-allocated to avoid large stack) */
    char **ring = calloc((size_t)n, sizeof(char *));
    if (!ring) { fclose(rfp); return 0; }

    int head = 0, count = 0, scanned = 0;
    char linebuf[AUDIT_LINE_MAX];

    while (scanned++ < HISTORY_SCAN_MAX && fgets(linebuf, sizeof(linebuf), rfp)) {
        /* Format: seq|ts|EVENT|username|detail|amount|result */
        /* We match on the username field (field index 3, 0-based) */
        char *fields[7];
        char copy[AUDIT_LINE_MAX];
        strncpy(copy, linebuf, sizeof(copy) - 1);
        copy[sizeof(copy)-1] = '\0';

        int fi = 0;
        char *tok = strtok(copy, "|");
        while (tok && fi < 7) { fields[fi++] = tok; tok = strtok(NULL, "|"); }

        if (fi < 7) continue;
        if (strncmp(fields[3], username, MAX_USERNAME) != 0) continue;

        /* Store into ring buffer */
        free(ring[head]);
        ring[head] = strdup(linebuf);
        head = (head + 1) % n;
        if (count < n) count++;
    }
    fclose(rfp);

    /* Emit in order: oldest first */
    int written = 0;
    int start = (count < n) ? 0 : head;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % n;
        if (ring[idx]) {
            /* strip trailing newline before sendline adds its own */
            size_t len = strlen(ring[idx]);
            if (len > 0 && ring[idx][len-1] == '\n')
                ring[idx][len-1] = '\0';
            sendline_fd(fd, ring[idx]);
            written++;
        }
    }

    for (int i = 0; i < n; i++) free(ring[i]);
    free(ring);
    return written;
}
