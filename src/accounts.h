#ifndef ACCOUNTS_H
#define ACCOUNTS_H

#include "protocol.h"
#include <pthread.h>
#include <stdint.h>

/*
 * Locking design (see accounts.c for details):
 *
 *  1. g_reg_rwlock  – reader-writer lock protecting the accounts[] array
 *     and num_accounts count.  Taken as WRITE for REGISTER; READ for all
 *     other operations that look up an existing account.
 *
 *  2. Account.lock  – per-account mutex protecting balance and state fields.
 *     For single-account ops: take g_reg_rwlock(READ) → take account.lock.
 *     For transfers: take g_reg_rwlock(READ) → take BOTH account locks in
 *     ascending account_id order to prevent deadlock.
 *
 *  3. g_file_mtx    – mutex serializing writes to the accounts data file.
 *     Taken AFTER account lock(s) are held (never the reverse).
 *     fcntl F_SETLKW is applied inside for OS-level exclusion as well.
 *
 * Lock ordering (must never reverse):
 *   g_reg_rwlock(W or R) → account.lock (low id first) → g_file_mtx
 */

typedef struct {
    uint64_t        account_id;
    char            username[MAX_USERNAME];
    char            password_hash[MAX_HASH];
    char            salt[SALT_HEX_LEN];
    double          balance;
    int             active;        /* 1 = valid slot */
    pthread_mutex_t lock;          /* per-account balance lock */
} Account;

/* ---------- lifecycle ---------- */

/* Call once at startup. Returns 0 on success, -1 on error. */
int  accounts_init(const char *data_file);

/* Call at shutdown to flush and free. */
void accounts_shutdown(void);

/* ---------- lookup ---------- */

/*
 * Find an active account by username.
 * Returns a LOCKED Account* (caller must unlock via accounts_unlock), or NULL.
 * Caller must hold g_reg_rwlock(READ) via accounts_rdlock/accounts_rdunlock.
 */
Account *accounts_find_locked(const char *username);

/* Release an account lock acquired via accounts_find_locked. */
void accounts_unlock(Account *a);

/* Convenience: take the registry read-lock. */
void accounts_rdlock(void);
void accounts_rdunlock(void);

/* ---------- operations ---------- */

/*
 * Register a new account.  Returns 0 on success, -1 if username taken or
 * account limit reached.  Persists immediately.
 */
int accounts_register(const char *username, const char *password);

/*
 * Verify credentials.  Returns 1 on match, 0 on failure.
 * Does NOT require the registry lock to be held by the caller.
 */
int accounts_authenticate(const char *username, const char *password);

/*
 * Balance query.  Caller must hold g_reg_rwlock(READ) and account.lock.
 */
double accounts_balance(Account *a);

/*
 * Deposit amount into account.  Caller must hold g_reg_rwlock(READ) and
 * account.lock.  Persists the change.  Returns 0 on success, -1 on error.
 */
int accounts_deposit(Account *a, double amount);

/*
 * Withdraw amount from account.  Returns 0 on success, -1 if insufficient
 * funds or invalid amount.  Caller must hold rdlock + account.lock.
 */
int accounts_withdraw(Account *a, double amount);

/*
 * Transfer amount from `src` to `dst`.  Both accounts must NOT be locked
 * by the caller — this function acquires them in the correct order.
 * Caller must hold g_reg_rwlock(READ).  Returns 0 on success, -1 on error.
 */
int accounts_transfer(Account *src, Account *dst, double amount);

#endif /* ACCOUNTS_H */
