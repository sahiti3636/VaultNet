/*
 * server.c – VaultNet TCP server
 *
 * Architecture:
 *   - Main thread: accept loop + signal handling
 *   - Worker threads: one per job, drawn from a bounded thread pool
 *   - Each accepted client fd is wrapped in a ClientConn and submitted as a job
 *   - Session state (logged-in username) lives on the stack of the worker
 *
 * Shutdown sequence (SIGINT / SIGTERM):
 *   1. g_running = 0
 *   2. close(g_listen_fd) to unblock accept()
 *   3. threadpool_destroy() drains in-flight work and joins workers
 *   4. audit_close() flushes log
 *   5. accounts_shutdown() final persist
 */

#include "protocol.h"
#include "netutil.h"
#include "accounts.h"
#include "auth.h"
#include "audit.h"
#include "threadpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* ── globals ─────────────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;
static int                   g_listen_fd = -1;
static ThreadPool            g_pool;

/* ── signal handler ──────────────────────────────────────────────────────── */

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
    /* Closing the listen fd wakes up the blocked accept() call */
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

/* ── per-connection job ──────────────────────────────────────────────────── */

typedef struct {
    int  fd;
    char peer_addr[INET_ADDRSTRLEN];
} ClientConn;

/*
 * Parse a non-negative decimal amount from str.
 * Returns 1 on success (amount > 0), 0 on error.
 */
static int parse_amount(const char *str, double *out) {
    if (!str || *str == '\0') return 0;
    char *end;
    errno = 0;
    double v = strtod(str, &end);
    if (errno != 0 || end == str || *end != '\0' || v <= 0.0) return 0;
    *out = v;
    return 1;
}

static void handle_client(void *arg) {
    ClientConn *conn = (ClientConn *)arg;
    int fd = conn->fd;

    /* Per-session state: who is logged in on this connection */
    char session_user[MAX_USERNAME];
    memset(session_user, 0, sizeof(session_user));
    int  logged_in = 0;

    char line[MAX_LINE];
    ssize_t n;

    sendline_fd(fd, "OK VaultNet ready. Send HELP for command list.");

    while ((n = readline_fd(fd, line, sizeof(line))) > 0) {
        /* Tokenise command line */
        char *saveptr = NULL;
        char *cmd  = strtok_r(line, " \t", &saveptr);
        if (!cmd) { sendline_fd(fd, "ERR empty command"); continue; }

        /* ── QUIT ─────────────────────────────────────────────────── */
        if (strcasecmp(cmd, "QUIT") == 0) {
            sendline_fd(fd, "OK goodbye");
            break;
        }

        /* ── REGISTER <user> <pass> ───────────────────────────────── */
        if (strcasecmp(cmd, "REGISTER") == 0) {
            char *user = strtok_r(NULL, " \t", &saveptr);
            char *pass = strtok_r(NULL, " \t", &saveptr);
            if (!user || !pass) {
                sendline_fd(fd, "ERR usage: REGISTER <username> <password>");
                continue;
            }
            if (strlen(user) >= MAX_USERNAME || strlen(pass) >= MAX_PASSWORD) {
                sendline_fd(fd, "ERR username or password too long");
                continue;
            }
            if (accounts_register(user, pass) == 0) {
                audit_log("REGISTER", user, "new account", 0.0, "OK");
                sendline_fd(fd, "OK account created");
            } else {
                audit_log("REGISTER", user, "username taken or limit reached", 0.0, "ERR");
                sendline_fd(fd, "ERR username already taken or server limit reached");
            }
            continue;
        }

        /* ── LOGIN <user> <pass> ──────────────────────────────────── */
        if (strcasecmp(cmd, "LOGIN") == 0) {
            char *user = strtok_r(NULL, " \t", &saveptr);
            char *pass = strtok_r(NULL, " \t", &saveptr);
            if (!user || !pass) {
                sendline_fd(fd, "ERR usage: LOGIN <username> <password>");
                continue;
            }
            if (logged_in) {
                sendline_fd(fd, "ERR already logged in; LOGOUT first");
                continue;
            }
            if (accounts_authenticate(user, pass)) {
                logged_in = 1;
                strncpy(session_user, user, MAX_USERNAME - 1);
                audit_log("LOGIN", user, conn->peer_addr, 0.0, "OK");
                sendlinef(fd, "OK logged in as %s", user);
            } else {
                audit_log("LOGIN", user ? user : "?", "bad credentials", 0.0, "ERR");
                sendline_fd(fd, "ERR invalid username or password");
            }
            continue;
        }

        /* ── LOGOUT ───────────────────────────────────────────────── */
        if (strcasecmp(cmd, "LOGOUT") == 0) {
            if (!logged_in) { sendline_fd(fd, "ERR not logged in"); continue; }
            audit_log("LOGOUT", session_user, "", 0.0, "OK");
            memset(session_user, 0, sizeof(session_user));
            logged_in = 0;
            sendline_fd(fd, "OK logged out");
            continue;
        }

        /* All commands below require authentication */
        if (!logged_in) {
            sendline_fd(fd, "ERR not logged in");
            continue;
        }

        /* ── BALANCE ──────────────────────────────────────────────── */
        if (strcasecmp(cmd, "BALANCE") == 0) {
            accounts_rdlock();
            Account *a = accounts_find_locked(session_user);
            if (!a) {
                accounts_rdunlock();
                sendline_fd(fd, "ERR account not found");
                continue;
            }
            double bal = accounts_balance(a);
            accounts_unlock(a);
            accounts_rdunlock();
            sendlinef(fd, "OK balance %.2f", bal);
            continue;
        }

        /* ── DEPOSIT <amount> ─────────────────────────────────────── */
        if (strcasecmp(cmd, "DEPOSIT") == 0) {
            char *amt_str = strtok_r(NULL, " \t", &saveptr);
            double amount = 0.0;
            if (!parse_amount(amt_str, &amount)) {
                sendline_fd(fd, "ERR invalid amount");
                continue;
            }
            accounts_rdlock();
            Account *a = accounts_find_locked(session_user);
            if (!a) {
                accounts_rdunlock();
                sendline_fd(fd, "ERR account not found");
                continue;
            }
            if (accounts_deposit(a, amount) == 0) {
                double bal = accounts_balance(a);
                accounts_unlock(a);
                accounts_rdunlock();
                audit_log("DEPOSIT", session_user, "", amount, "OK");
                sendlinef(fd, "OK deposited %.2f new_balance %.2f", amount, bal);
            } else {
                accounts_unlock(a);
                accounts_rdunlock();
                audit_log("DEPOSIT", session_user, "failed", amount, "ERR");
                sendline_fd(fd, "ERR deposit failed");
            }
            continue;
        }

        /* ── WITHDRAW <amount> ────────────────────────────────────── */
        if (strcasecmp(cmd, "WITHDRAW") == 0) {
            char *amt_str = strtok_r(NULL, " \t", &saveptr);
            double amount = 0.0;
            if (!parse_amount(amt_str, &amount)) {
                sendline_fd(fd, "ERR invalid amount");
                continue;
            }
            accounts_rdlock();
            Account *a = accounts_find_locked(session_user);
            if (!a) {
                accounts_rdunlock();
                sendline_fd(fd, "ERR account not found");
                continue;
            }
            if (accounts_withdraw(a, amount) == 0) {
                double bal = accounts_balance(a);
                accounts_unlock(a);
                accounts_rdunlock();
                audit_log("WITHDRAW", session_user, "", amount, "OK");
                sendlinef(fd, "OK withdrew %.2f new_balance %.2f", amount, bal);
            } else {
                double bal = accounts_balance(a);
                accounts_unlock(a);
                accounts_rdunlock();
                audit_log("WITHDRAW", session_user, "insufficient funds", amount, "ERR");
                sendlinef(fd, "ERR insufficient funds (balance %.2f)", bal);
            }
            continue;
        }

        /* ── TRANSFER <to_user> <amount> ─────────────────────────── */
        if (strcasecmp(cmd, "TRANSFER") == 0) {
            char *to_user = strtok_r(NULL, " \t", &saveptr);
            char *amt_str = strtok_r(NULL, " \t", &saveptr);
            double amount = 0.0;
            if (!to_user || !parse_amount(amt_str, &amount)) {
                sendline_fd(fd, "ERR usage: TRANSFER <username> <amount>");
                continue;
            }
            if (strncmp(to_user, session_user, MAX_USERNAME) == 0) {
                sendline_fd(fd, "ERR cannot transfer to yourself");
                continue;
            }

            accounts_rdlock();
            /* Find source; do NOT lock it here — accounts_transfer does that */
            Account *src = NULL, *dst = NULL;
            for (int i = 0; i < MAX_ACCOUNTS; i++) {
                /* Access g_accounts directly isn't safe via the public API,
                 * but accounts_find_locked would lock them — we need both
                 * unlocked so accounts_transfer can lock in order.
                 * So we locate pointers without locking. */
                /* We use a private helper below. */
                (void)i; /* handled via accounts_find_unlocked approach */
                break;
            }

            /*
             * We need unlocked pointers to pass to accounts_transfer.
             * accounts_find_locked returns a LOCKED pointer — wrong here.
             * Solution: call accounts_find_locked for src, immediately unlock,
             * find dst the same way, then pass both to accounts_transfer which
             * re-acquires them in correct order.
             *
             * This is safe because:
             *  - accounts_rdlock() prevents any account from being REMOVED
             *    while we hold it (only REGISTER takes the write lock).
             *  - accounts_transfer re-locks in the deterministic order.
             */
            src = accounts_find_locked(session_user);
            if (src) accounts_unlock(src);

            dst = accounts_find_locked(to_user);
            if (dst) accounts_unlock(dst);

            if (!src || !dst) {
                accounts_rdunlock();
                sendline_fd(fd, "ERR one or both accounts not found");
                continue;
            }

            if (accounts_transfer(src, dst, amount) == 0) {
                /* Re-lock src to read the updated balance */
                pthread_mutex_lock(&src->lock);
                double bal = src->balance;
                pthread_mutex_unlock(&src->lock);
                accounts_rdunlock();

                char detail[128];
                snprintf(detail, sizeof(detail), "to:%s", to_user);
                audit_log("TRANSFER", session_user, detail, amount, "OK");
                sendlinef(fd, "OK transferred %.2f to %s new_balance %.2f",
                          amount, to_user, bal);
            } else {
                /* Read src balance for the error message */
                pthread_mutex_lock(&src->lock);
                double bal = src->balance;
                pthread_mutex_unlock(&src->lock);
                accounts_rdunlock();

                char detail[128];
                snprintf(detail, sizeof(detail), "to:%s insufficient", to_user);
                audit_log("TRANSFER", session_user, detail, amount, "ERR");
                sendlinef(fd, "ERR transfer failed (balance %.2f)", bal);
            }
            continue;
        }

        /* ── HISTORY ──────────────────────────────────────────────── */
        if (strcasecmp(cmd, "HISTORY") == 0) {
            sendlinef(fd, "OK history for %s:", session_user);
            int cnt = audit_history(session_user, HISTORY_PER_ACCT, fd);
            sendlinef(fd, "END %d records", cnt);
            continue;
        }

        /* ── HELP ─────────────────────────────────────────────────── */
        if (strcasecmp(cmd, "HELP") == 0) {
            sendline_fd(fd, "OK commands: REGISTER LOGIN LOGOUT BALANCE DEPOSIT WITHDRAW TRANSFER HISTORY QUIT");
            continue;
        }

        sendlinef(fd, "ERR unknown command: %s", cmd);
    }

    if (n < 0) perror("readline_fd");

    close(fd);
    free(conn);
}

/* ── accept loop ─────────────────────────────────────────────────────────── */

static int make_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
        perror("setsockopt SO_REUSEADDR");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc >= 2) port = atoi(argv[1]);

    /* Initialise subsystems */
    if (accounts_init(ACCOUNTS_FILE) < 0) {
        fprintf(stderr, "Failed to load accounts\n");
        return 1;
    }
    if (audit_init(AUDIT_FILE) < 0) {
        fprintf(stderr, "Failed to open audit log\n");
        return 1;
    }
    if (threadpool_init(&g_pool) < 0) {
        fprintf(stderr, "Failed to start thread pool\n");
        return 1;
    }

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* Ignore SIGPIPE so writing to a closed client doesn't kill the server */
    signal(SIGPIPE, SIG_IGN);

    g_listen_fd = make_listen_socket(port);
    if (g_listen_fd < 0) return 1;

    fprintf(stdout, "VaultNet server listening on port %d  "
                    "(pool=%d threads, queue=%d jobs)\n",
            port, THREADPOOL_SIZE, QUEUE_CAPACITY);
    fflush(stdout);

    /* Accept loop */
    struct sockaddr_in client_addr;
    socklen_t addr_len;

    while (g_running) {
        addr_len = sizeof(client_addr);
        int client_fd = accept(g_listen_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EBADF) break; /* shutdown */
            perror("accept");
            continue;
        }

        ClientConn *conn = malloc(sizeof(ClientConn));
        if (!conn) {
            fprintf(stderr, "malloc failed for ClientConn\n");
            close(client_fd);
            continue;
        }
        conn->fd = client_fd;
        inet_ntop(AF_INET, &client_addr.sin_addr,
                  conn->peer_addr, sizeof(conn->peer_addr));

        if (threadpool_submit(&g_pool, handle_client, conn) < 0) {
            fprintf(stderr, "pool full, dropping connection from %s\n",
                    conn->peer_addr);
            sendline_fd(client_fd, "ERR server busy, try again");
            close(client_fd);
            free(conn);
        }
    }

    fprintf(stdout, "\nShutting down...\n");
    fflush(stdout);

    threadpool_destroy(&g_pool);
    accounts_shutdown();
    audit_log("SERVER", "system", "shutdown", 0.0, "OK");
    audit_close();

    fprintf(stdout, "VaultNet shut down cleanly.\n");
    return 0;
}
