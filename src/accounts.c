/*
 * accounts.c – in-memory account store with POSIX file-backed persistence.
 *
 * Persistence format (text, one account per line):
 *   account_id:username:password_hash:salt:balance
 *
 * Consistency guarantee:
 *   Every balance-changing operation holds the affected account's mutex while
 *   writing to the file under g_file_mtx + fcntl F_SETLKW.  The file is
 *   fully rewritten atomically (write to tmp, fsync, rename) so a crash mid-
 *   write leaves the previous version intact.
 *
 * See accounts.h for the full lock-ordering contract.
 */

#include "accounts.h"
#include "auth.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>

/* ── global state ────────────────────────────────────────────────────────── */

static Account          g_accounts[MAX_ACCOUNTS];
static int              g_num_accounts = 0;
static uint64_t         g_next_id      = 1;

static pthread_rwlock_t g_reg_rwlock   = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t  g_file_mtx     = PTHREAD_MUTEX_INITIALIZER;

static char             g_data_file[256];

/* ── file I/O helpers ────────────────────────────────────────────────────── */

/* Acquire an exclusive fcntl write-lock on fd. */
static int file_wrlock(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;
    while (fcntl(fd, F_SETLKW, &fl) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* Release an fcntl lock on fd. */
static void file_unlock(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;
    fcntl(fd, F_SETLK, &fl);
}

/*
 * Write all active accounts to a temp file, fsync, then rename over the real
 * file.  Caller must hold g_file_mtx.
 */
static int persist_accounts_locked(void) {
    char tmp_path[280];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_data_file);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("persist: open tmp"); return -1; }

    if (file_wrlock(fd) < 0) {
        perror("persist: wrlock");
        close(fd);
        return -1;
    }

    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        perror("persist: fdopen");
        file_unlock(fd);
        close(fd);
        return -1;
    }

    /* Write header comment */
    fprintf(fp, "# VaultNet accounts v1: account_id:username:hash:salt:balance\n");

    /* g_reg_rwlock is held READ by callers before they modify accounts,
     * so we can safely iterate here. */
    for (int i = 0; i < g_num_accounts; i++) {
        Account *a = &g_accounts[i];
        if (!a->active) continue;
        fprintf(fp, "%llu:%s:%s:%s:%.6f\n",
                (unsigned long long)a->account_id,
                a->username,
                a->password_hash,
                a->salt,
                a->balance);
    }

    if (fflush(fp) != 0) { perror("persist: fflush"); }
    if (fsync(fd)   != 0) { perror("persist: fsync"); }

    file_unlock(fd);
    fclose(fp); /* also closes fd */

    if (rename(tmp_path, g_data_file) < 0) {
        perror("persist: rename");
        return -1;
    }
    return 0;
}

/* Load accounts from g_data_file into g_accounts[]. */
static int load_accounts(void) {
    FILE *fp = fopen(g_data_file, "r");
    if (!fp) {
        if (errno == ENOENT) return 0; /* first run */
        perror("load_accounts: fopen");
        return -1;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';

        if (g_num_accounts >= MAX_ACCOUNTS) {
            fprintf(stderr, "load_accounts: account limit reached\n");
            break;
        }

        Account *a = &g_accounts[g_num_accounts];
        memset(a, 0, sizeof(*a));

        /* Parse: account_id:username:hash:salt:balance */
        char *p = line;
        char *tok;

        tok = strsep(&p, ":"); if (!tok) continue;
        a->account_id = (uint64_t)strtoull(tok, NULL, 10);

        tok = strsep(&p, ":"); if (!tok) continue;
        strncpy(a->username, tok, MAX_USERNAME - 1);

        tok = strsep(&p, ":"); if (!tok) continue;
        strncpy(a->password_hash, tok, MAX_HASH - 1);

        tok = strsep(&p, ":"); if (!tok) continue;
        strncpy(a->salt, tok, SALT_HEX_LEN - 1);

        tok = strsep(&p, ":"); if (!tok) continue;
        a->balance = strtod(tok, NULL);

        a->active = 1;
        pthread_mutex_init(&a->lock, NULL);

        if (a->account_id >= g_next_id)
            g_next_id = a->account_id + 1;

        g_num_accounts++;
    }
    fclose(fp);
    return 0;
}

/* ── public API ──────────────────────────────────────────────────────────── */

int accounts_init(const char *data_file) {
    strncpy(g_data_file, data_file, sizeof(g_data_file) - 1);
    memset(g_accounts, 0, sizeof(g_accounts));
    return load_accounts();
}

void accounts_shutdown(void) {
    pthread_mutex_lock(&g_file_mtx);
    persist_accounts_locked();
    pthread_mutex_unlock(&g_file_mtx);

    /* Destroy per-account mutexes */
    pthread_rwlock_wrlock(&g_reg_rwlock);
    for (int i = 0; i < g_num_accounts; i++) {
        if (g_accounts[i].active)
            pthread_mutex_destroy(&g_accounts[i].lock);
    }
    pthread_rwlock_unlock(&g_reg_rwlock);
}

void accounts_rdlock(void)   { pthread_rwlock_rdlock(&g_reg_rwlock); }
void accounts_rdunlock(void) { pthread_rwlock_unlock(&g_reg_rwlock); }

Account *accounts_find_locked(const char *username) {
    /* Caller must hold g_reg_rwlock(READ) */
    for (int i = 0; i < g_num_accounts; i++) {
        Account *a = &g_accounts[i];
        if (a->active && strncmp(a->username, username, MAX_USERNAME) == 0) {
            pthread_mutex_lock(&a->lock);
            return a;
        }
    }
    return NULL;
}

void accounts_unlock(Account *a) {
    pthread_mutex_unlock(&a->lock);
}

int accounts_register(const char *username, const char *password) {
    /* Take registry WRITE lock — mutually exclusive with all readers */
    pthread_rwlock_wrlock(&g_reg_rwlock);

    /* Check for duplicate username */
    for (int i = 0; i < g_num_accounts; i++) {
        if (g_accounts[i].active &&
            strncmp(g_accounts[i].username, username, MAX_USERNAME) == 0) {
            pthread_rwlock_unlock(&g_reg_rwlock);
            return -1; /* username taken */
        }
    }

    if (g_num_accounts >= MAX_ACCOUNTS) {
        pthread_rwlock_unlock(&g_reg_rwlock);
        return -1; /* no slots */
    }

    Account *a = &g_accounts[g_num_accounts];
    memset(a, 0, sizeof(*a));
    a->account_id = g_next_id++;
    strncpy(a->username, username, MAX_USERNAME - 1);
    generate_salt(a->salt, sizeof(a->salt));
    hash_password(password, a->salt, a->password_hash, sizeof(a->password_hash));
    a->balance = 0.0;
    a->active  = 1;
    pthread_mutex_init(&a->lock, NULL);
    g_num_accounts++;

    /* Persist under the file mutex while still holding the registry write lock */
    pthread_mutex_lock(&g_file_mtx);
    int rc = persist_accounts_locked();
    pthread_mutex_unlock(&g_file_mtx);

    pthread_rwlock_unlock(&g_reg_rwlock);
    return rc;
}

int accounts_authenticate(const char *username, const char *password) {
    accounts_rdlock();
    Account *a = accounts_find_locked(username);
    if (!a) { accounts_rdunlock(); return 0; }

    int ok = verify_password(password, a->salt, a->password_hash);
    accounts_unlock(a);
    accounts_rdunlock();
    return ok;
}

double accounts_balance(Account *a) {
    return a->balance;
}

int accounts_deposit(Account *a, double amount) {
    if (amount <= 0.0) return -1;
    a->balance += amount;

    pthread_mutex_lock(&g_file_mtx);
    int rc = persist_accounts_locked();
    pthread_mutex_unlock(&g_file_mtx);
    return rc;
}

int accounts_withdraw(Account *a, double amount) {
    if (amount <= 0.0)          return -1;
    if (a->balance < amount)    return -1;
    a->balance -= amount;

    pthread_mutex_lock(&g_file_mtx);
    int rc = persist_accounts_locked();
    pthread_mutex_unlock(&g_file_mtx);
    return rc;
}

/*
 * Transfer: locks both accounts in ascending account_id order to prevent
 * deadlock (classic two-resource ordering solution).
 */
int accounts_transfer(Account *src, Account *dst, double amount) {
    if (amount <= 0.0) return -1;
    if (src == dst)    return -1;

    /* Determine locking order by account_id */
    Account *first  = (src->account_id < dst->account_id) ? src : dst;
    Account *second = (src->account_id < dst->account_id) ? dst : src;

    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);

    int rc = -1;
    if (src->balance >= amount) {
        src->balance -= amount;
        dst->balance += amount;

        pthread_mutex_lock(&g_file_mtx);
        rc = persist_accounts_locked();
        pthread_mutex_unlock(&g_file_mtx);
    }

    pthread_mutex_unlock(&second->lock);
    pthread_mutex_unlock(&first->lock);
    return rc;
}
