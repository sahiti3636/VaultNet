#ifndef PROTOCOL_H
#define PROTOCOL_H

/* Line-based text protocol constants */

#define PROTO_OK  "OK"
#define PROTO_ERR "ERR"

/* Maximum lengths */
#define MAX_USERNAME   64
#define MAX_PASSWORD   128
#define MAX_LINE       512
#define MAX_HASH       129   /* 128 hex chars + NUL */
#define SALT_LEN       16    /* bytes of random salt */
#define SALT_HEX_LEN   33    /* hex-encoded salt + NUL */

/* Thread pool / queue sizing */
#define THREADPOOL_SIZE   64
#define QUEUE_CAPACITY    256

/* Account limits */
#define MAX_ACCOUNTS      2048
#define HISTORY_PER_ACCT  50

/* Server defaults */
#define DEFAULT_PORT      8080
#define BACKLOG           128

/* Persistence files */
#define ACCOUNTS_FILE  "vaultnet_accounts.dat"
#define AUDIT_FILE     "vaultnet_audit.log"

#endif /* PROTOCOL_H */
