/*
 * accounts.c – in-memory account store backed by WAL + snapshot.
 *
 * Snapshot format (text, one header line + one line per account):
 *   # VaultNet snapshot lsn=<N> count=<M>
 *   account_id:username:password_hash:salt:balance_paise
 *
 * Durability model:
 *   Every mutating operation appends a WAL record and blocks until it is
 *   fsynced (via wal_append).  The accounts file is only rewritten at
 *   checkpoints (every WAL_CHECKPOINT_INTERVAL ops or on clean shutdown).
 *   Recovery: load snapshot → replay WAL records with lsn > snap_lsn.
 *
 * Lock ordering (unchanged from Phase 1):
 *   g_reg_rwlock(W or R) → g_file_mtx → account.lock(s, ascending id)
 */

#include "accounts.h"
#include "auth.h"
#include "wal.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdatomic.h>

/* ── global state ─────────────────────────────────────────────────────────── */

static Account          g_accounts[MAX_ACCOUNTS];
static int              g_num_accounts = 0;
static uint64_t         g_next_id      = 1;
static uint64_t         g_snap_lsn     = 0;  /* LSN at time of last snapshot */

static pthread_rwlock_t g_reg_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t  g_file_mtx   = PTHREAD_MUTEX_INITIALIZER;

static char g_snap_file[256];

/* Op counter for checkpoint triggering */
static atomic_uint_fast64_t g_op_count = 0;

static int sync_snapshot_directory(void) {
    char dir[sizeof(g_snap_file)];
    const char *slash = strrchr(g_snap_file, '/');
    if (slash) {
        size_t len = (size_t)(slash - g_snap_file);
        if (len >= sizeof(dir)) return -1;
        if (len == 0) len = 1;
        memcpy(dir, g_snap_file, len);
        dir[len] = '\0';
    } else {
        strcpy(dir, ".");
    }
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

/* ── internal helpers ─────────────────────────────────────────────────────── */

static Account *find_account_unlocked(const char *username) {
    for (int i = 0; i < g_num_accounts; i++) {
        Account *a = &g_accounts[i];
        if (a->active && strncmp(a->username, username, MAX_USERNAME) == 0)
            return a;
    }
    return NULL;
}

static Account *find_by_id_unlocked(uint64_t id) {
    for (int i = 0; i < g_num_accounts; i++) {
        Account *a = &g_accounts[i];
        if (a->active && a->account_id == id)
            return a;
    }
    return NULL;
}

/* ── snapshot I/O ─────────────────────────────────────────────────────────── */

/*
 * Write all active accounts to SNAP_FILE_TMP, fsync, rename to g_snap_file,
 * then truncate the WAL.  Caller MUST hold g_file_mtx.
 */
int accounts_checkpoint(void) {
    char tmp[280];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_snap_file);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("checkpoint: open"); return -1; }

    FILE *fp = fdopen(fd, "w");
    if (!fp) { perror("checkpoint: fdopen"); close(fd); return -1; }

    uint64_t snap_lsn = wal_last_lsn();
    fprintf(fp, "# VaultNet snapshot lsn=%" PRIu64 " count=%d\n",
            snap_lsn, g_num_accounts);

    for (int i = 0; i < g_num_accounts; i++) {
        Account *a = &g_accounts[i];
        if (!a->active) continue;
        fprintf(fp, "%" PRIu64 ":%s:%s:%s:%" PRId64 "\n",
                a->account_id, a->username,
                a->password_hash, a->salt,
                a->balance_paise);
    }

    if (fflush(fp) != 0) { fclose(fp); return -1; }
    if (fsync(fd)   != 0) { fclose(fp); return -1; }
    fclose(fp);

    if (rename(tmp, g_snap_file) < 0) { perror("checkpoint: rename"); return -1; }
    if (sync_snapshot_directory() < 0) {
        perror("checkpoint: fsync directory");
        return -1;
    }

    g_snap_lsn = snap_lsn;
    return wal_truncate();
}

static int load_snapshot(void) {
    FILE *fp = fopen(g_snap_file, "r");
    if (!fp) {
        if (errno == ENOENT) return 0;  /* first run */
        perror("load_snapshot");
        return -1;
    }

    char line[MAX_LINE];
    /* First line: header */
    if (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#') {
            /* # VaultNet snapshot lsn=<N> count=<M> */
            uint64_t lsn = 0;
            sscanf(line, "# VaultNet snapshot lsn=%" SCNu64, &lsn);
            g_snap_lsn = lsn;
        } else {
            /* Legacy format without header — rewind */
            rewind(fp);
        }
    }

    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        if (g_num_accounts >= MAX_ACCOUNTS) {
            fprintf(stderr, "load_snapshot: account limit reached\n");
            break;
        }

        Account *a = &g_accounts[g_num_accounts];
        memset(a, 0, sizeof(*a));
        char *p = line, *tok;

        tok = strsep(&p, ":"); if (!tok) continue;
        a->account_id = (uint64_t)strtoull(tok, NULL, 10);
        tok = strsep(&p, ":"); if (!tok) continue;
        strncpy(a->username, tok, MAX_USERNAME - 1);
        tok = strsep(&p, ":"); if (!tok) continue;
        strncpy(a->password_hash, tok, MAX_HASH - 1);
        tok = strsep(&p, ":"); if (!tok) continue;
        strncpy(a->salt, tok, SALT_HEX_LEN - 1);
        tok = strsep(&p, ":"); if (!tok) continue;
        a->balance_paise = (int64_t)strtoll(tok, NULL, 10);

        a->active = 1;
        pthread_mutex_init(&a->lock, NULL);
        if (a->account_id >= g_next_id)
            g_next_id = a->account_id + 1;
        g_num_accounts++;
    }
    fclose(fp);
    return 0;
}

/* ── lifecycle ─────────────────────────────────────────────────────────────── */

int accounts_init(const char *snap_file) {
    for (int i = 0; i < g_num_accounts; i++)
        if (g_accounts[i].active) pthread_mutex_destroy(&g_accounts[i].lock);
    g_num_accounts = 0;
    g_next_id = 1;
    g_snap_lsn = 0;
    atomic_store(&g_op_count, 0);
    strncpy(g_snap_file, snap_file, sizeof(g_snap_file) - 1);
    g_snap_file[sizeof(g_snap_file) - 1] = '\0';
    memset(g_accounts, 0, sizeof(g_accounts));
    return load_snapshot();
}

uint64_t accounts_snap_lsn(void) {
    return g_snap_lsn;
}

void accounts_shutdown(void) {
    pthread_mutex_lock(&g_file_mtx);
    accounts_checkpoint();
    pthread_mutex_unlock(&g_file_mtx);

    pthread_rwlock_wrlock(&g_reg_rwlock);
    for (int i = 0; i < g_num_accounts; i++) {
        if (g_accounts[i].active) {
            pthread_mutex_destroy(&g_accounts[i].lock);
            g_accounts[i].active = 0;
        }
    }
    g_num_accounts = 0;
    pthread_rwlock_unlock(&g_reg_rwlock);
}

/* ── checkpoint trigger ───────────────────────────────────────────────────── */

/* Called after each wal_append (with g_file_mtx and relevant locks still held). */
static void maybe_checkpoint(void) {
    if ((atomic_fetch_add(&g_op_count, 1) + 1) % WAL_CHECKPOINT_INTERVAL == 0)
        accounts_checkpoint();
}

/* ── WAL record application (used during replay) ─────────────────────────── */

void accounts_apply_wal_record(const WALRecord *r) {
    switch ((WALType)r->type) {

    case WAL_REGISTER: {
        if (g_num_accounts >= MAX_ACCOUNTS) break;
        /* Skip if this account_id is already in memory (e.g., from snapshot) */
        if (find_by_id_unlocked(r->account_id)) break;
        Account *a = &g_accounts[g_num_accounts];
        memset(a, 0, sizeof(*a));
        a->account_id = r->account_id;
        strncpy(a->username,     r->username1,     MAX_USERNAME - 1);
        strncpy(a->password_hash, r->password_hash, MAX_HASH - 1);
        strncpy(a->salt,         r->salt,          SALT_HEX_LEN - 1);
        a->balance_paise = 0;
        a->active = 1;
        pthread_mutex_init(&a->lock, NULL);
        if (a->account_id >= g_next_id)
            g_next_id = a->account_id + 1;
        g_num_accounts++;
        break;
    }

    case WAL_DEPOSIT:
    case WAL_WITHDRAW: {
        Account *a = find_account_unlocked(r->username1);
        if (a) a->balance_paise = r->bal1_after;
        break;
    }

    case WAL_TRANSFER: {
        Account *src = find_account_unlocked(r->username1);
        Account *dst = find_account_unlocked(r->username2);
        if (src) src->balance_paise = r->bal1_after;
        if (dst) dst->balance_paise = r->bal2_after;
        break;
    }
    }
}

/* ── public operations ────────────────────────────────────────────────────── */

int accounts_register(const char *username, const char *password) {
    pthread_rwlock_wrlock(&g_reg_rwlock);

    for (int i = 0; i < g_num_accounts; i++) {
        if (g_accounts[i].active &&
            strncmp(g_accounts[i].username, username, MAX_USERNAME) == 0) {
            pthread_rwlock_unlock(&g_reg_rwlock);
            return -1;
        }
    }
    if (g_num_accounts >= MAX_ACCOUNTS) {
        pthread_rwlock_unlock(&g_reg_rwlock);
        return -1;
    }

    Account *a = &g_accounts[g_num_accounts];
    memset(a, 0, sizeof(*a));
    a->account_id    = g_next_id;
    strncpy(a->username, username, MAX_USERNAME - 1);
    if (generate_salt(a->salt, sizeof(a->salt)) != 0 ||
        hash_password(password, a->salt, a->password_hash,
                      sizeof(a->password_hash)) != 0) {
        pthread_rwlock_unlock(&g_reg_rwlock);
        return -1;
    }
    a->balance_paise = 0;
    pthread_mutex_init(&a->lock, NULL);

    pthread_mutex_lock(&g_file_mtx);
    int rc = wal_append(WAL_REGISTER, a->account_id,
                        a->username, NULL,
                        a->password_hash, a->salt,
                        0, 0, 0);
    if (rc == 0) {
        a->active = 1;
        g_num_accounts++;
        g_next_id++;
        maybe_checkpoint();
    } else {
        pthread_mutex_destroy(&a->lock);
        memset(a, 0, sizeof(*a));
    }
    pthread_mutex_unlock(&g_file_mtx);

    pthread_rwlock_unlock(&g_reg_rwlock);
    return rc;
}

int accounts_authenticate(const char *username, const char *password) {
    pthread_rwlock_rdlock(&g_reg_rwlock);
    Account *a = find_account_unlocked(username);
    if (!a) { pthread_rwlock_unlock(&g_reg_rwlock); return 0; }

    pthread_mutex_lock(&a->lock);
    int ok = verify_password(password, a->salt, a->password_hash);
    pthread_mutex_unlock(&a->lock);

    pthread_rwlock_unlock(&g_reg_rwlock);
    return ok;
}

int accounts_get_balance(const char *username, int64_t *bal_out) {
    pthread_rwlock_rdlock(&g_reg_rwlock);
    Account *a = find_account_unlocked(username);
    if (!a) { pthread_rwlock_unlock(&g_reg_rwlock); return -1; }

    pthread_mutex_lock(&a->lock);
    *bal_out = a->balance_paise;
    pthread_mutex_unlock(&a->lock);

    pthread_rwlock_unlock(&g_reg_rwlock);
    return 0;
}

int accounts_deposit(const char *username, int64_t paise, int64_t *bal_out) {
    if (paise <= 0) return -1;

    pthread_rwlock_rdlock(&g_reg_rwlock);
    pthread_mutex_lock(&g_file_mtx);

    Account *a = find_account_unlocked(username);
    if (!a) {
        pthread_mutex_unlock(&g_file_mtx);
        pthread_rwlock_unlock(&g_reg_rwlock);
        return -1;
    }

    pthread_mutex_lock(&a->lock);
    if (paise > INT64_MAX - a->balance_paise) {
        *bal_out = a->balance_paise;
        pthread_mutex_unlock(&a->lock);
        pthread_mutex_unlock(&g_file_mtx);
        pthread_rwlock_unlock(&g_reg_rwlock);
        return -1;
    }
    int64_t next_balance = a->balance_paise + paise;
    int rc = wal_append(WAL_DEPOSIT, a->account_id,
                        username, NULL, NULL, NULL,
                        paise, next_balance, 0);
    if (rc == 0) {
        a->balance_paise = next_balance;
        maybe_checkpoint();
    }
    *bal_out = a->balance_paise;
    pthread_mutex_unlock(&a->lock);

    pthread_mutex_unlock(&g_file_mtx);
    pthread_rwlock_unlock(&g_reg_rwlock);
    return rc;
}

int accounts_withdraw(const char *username, int64_t paise, int64_t *bal_out) {
    if (paise <= 0) return -1;

    pthread_rwlock_rdlock(&g_reg_rwlock);
    pthread_mutex_lock(&g_file_mtx);

    Account *a = find_account_unlocked(username);
    if (!a) {
        pthread_mutex_unlock(&g_file_mtx);
        pthread_rwlock_unlock(&g_reg_rwlock);
        *bal_out = 0;
        return -1;
    }

    pthread_mutex_lock(&a->lock);
    *bal_out = a->balance_paise;
    if (a->balance_paise < paise) {
        pthread_mutex_unlock(&a->lock);
        pthread_mutex_unlock(&g_file_mtx);
        pthread_rwlock_unlock(&g_reg_rwlock);
        return -1;
    }
    int64_t next_balance = a->balance_paise - paise;
    int rc = wal_append(WAL_WITHDRAW, a->account_id,
                        username, NULL, NULL, NULL,
                        paise, next_balance, 0);
    if (rc == 0) {
        a->balance_paise = next_balance;
        maybe_checkpoint();
    }
    *bal_out = a->balance_paise;
    pthread_mutex_unlock(&a->lock);

    pthread_mutex_unlock(&g_file_mtx);
    pthread_rwlock_unlock(&g_reg_rwlock);
    return rc;
}

int accounts_transfer(const char *src_user, const char *dst_user,
                      int64_t paise, int64_t *src_bal_out) {
    if (paise <= 0) return -1;

    pthread_rwlock_rdlock(&g_reg_rwlock);
    pthread_mutex_lock(&g_file_mtx);

    Account *src = find_account_unlocked(src_user);
    Account *dst = find_account_unlocked(dst_user);

    if (!src || !dst || src == dst) {
        pthread_mutex_unlock(&g_file_mtx);
        pthread_rwlock_unlock(&g_reg_rwlock);
        if (src_bal_out) *src_bal_out = 0;
        return -1;
    }

    Account *first  = (src->account_id < dst->account_id) ? src : dst;
    Account *second = (src->account_id < dst->account_id) ? dst : src;

    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);

    *src_bal_out = src->balance_paise;
    int rc = -1;
    if (src->balance_paise >= paise &&
        paise <= INT64_MAX - dst->balance_paise) {
        int64_t next_src = src->balance_paise - paise;
        int64_t next_dst = dst->balance_paise + paise;
        rc = wal_append(WAL_TRANSFER, src->account_id,
                        src_user, dst_user, NULL, NULL,
                        paise, next_src, next_dst);
        if (rc == 0) {
            src->balance_paise = next_src;
            dst->balance_paise = next_dst;
            maybe_checkpoint();
        }
        *src_bal_out = src->balance_paise;
    }

    pthread_mutex_unlock(&second->lock);
    pthread_mutex_unlock(&first->lock);
    pthread_mutex_unlock(&g_file_mtx);
    pthread_rwlock_unlock(&g_reg_rwlock);
    return rc;
}
