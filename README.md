# VaultNet

A multi-threaded TCP banking server written in C11 using POSIX sockets, pthreads, and file locking.  
Demonstrates concurrency-safe account management with no race conditions, no deadlocks, and durable persistence.

---

## Building

```sh
make          # builds both server and client
make server   # server only  → bin/vaultnet_server
make client   # client only  → bin/vaultnet_client
make clean    # remove build artefacts and data files
```

Requirements: GCC, POSIX system (Linux primary; builds on macOS for development).

---

## Running

```sh
# Start the server (default port 8080)
./bin/vaultnet_server [port]

# Connect with the interactive client
./bin/vaultnet_client [host [port]]
```

Server creates two files in the working directory:
- `vaultnet_accounts.dat` – persistent account store  
- `vaultnet_audit.log`    – append-only audit log

Graceful shutdown: send `SIGINT` (Ctrl-C) or `SIGTERM`.  
The server stops accepting, drains in-flight work, flushes logs, and exits cleanly.

---

## Protocol Reference

Line-based text protocol (LF-terminated).  Client sends one command per line;  
server replies with `OK …` or `ERR …`.  Multi-line responses end with `END n records`.

| Command | Auth required | Description |
|---|---|---|
| `REGISTER <user> <pass>` | No | Create a new account |
| `LOGIN <user> <pass>` | No | Start an authenticated session |
| `LOGOUT` | Yes | End the session |
| `BALANCE` | Yes | Show current balance |
| `DEPOSIT <amount>` | Yes | Add funds |
| `WITHDRAW <amount>` | Yes | Remove funds (rejects insufficient balance) |
| `TRANSFER <to_user> <amount>` | Yes | Atomic transfer to another account |
| `HISTORY` | Yes | Last 50 transactions from the audit log |
| `QUIT` | No | Close the connection |

Example session:
```
> REGISTER alice s3cret
< OK account created
> LOGIN alice s3cret
< OK logged in as alice
> DEPOSIT 500
< OK deposited 500.00 new_balance 500.00
> TRANSFER bob 100
< OK transferred 100.00 to bob new_balance 400.00
> HISTORY
< OK history for alice:
< 1|2026-06-20T…|REGISTER|alice|new account|0.00|OK
< …
< END 3 records
> QUIT
< OK goodbye
```

---

## Design Notes

### Threading model

```
main thread
  └── accept loop  ──→  ClientConn* ──→  job queue  ──→  64 worker threads
                                        (bounded, 256-slot circular buffer)
```

Each accepted socket is wrapped in a `ClientConn`, pushed onto a mutex-protected  
circular queue, and picked up by the next idle worker.  Workers block on a condition  
variable when idle, so there is no spin-waiting.  On shutdown, the queue is drained  
before workers exit.

### Deadlock avoidance

Three lock levels, never acquired in reverse order:

1. **`g_reg_rwlock`** (POSIX `pthread_rwlock_t`) — protects the `accounts[]` array  
   and the count of active accounts.  
   - Write-locked only for `REGISTER` (new account creation).  
   - Read-locked for all other operations.

2. **`Account.lock`** (per-account `pthread_mutex_t`) — protects `balance` and account  
   fields.  For single-account operations the worker acquires exactly one.  
   For `TRANSFER`, **both** account locks are acquired in ascending `account_id` order.  
   Because every thread uses the same ordering, a cycle in the lock graph is impossible.

3. **`g_file_mtx`** (`pthread_mutex_t`) — serialises writes to the accounts data file.  
   Always acquired last (after any account locks are held).

### File-locking and consistency

Every balance-changing operation:
1. Modifies the in-memory `Account.balance` under `Account.lock`.
2. Calls `persist_accounts_locked()` under `g_file_mtx`.
3. Inside `persist_accounts_locked`:
   - Opens a `.tmp` file.
   - Acquires an exclusive `fcntl F_SETLKW` write-lock on the `.tmp` fd.
   - Writes all accounts to the `.tmp` file.
   - `fsync()`s the file.
   - Releases the `fcntl` lock.
   - `rename()`s the `.tmp` file over the real file (atomic on POSIX).

This guarantees:
- **No torn writes**: a crash mid-write leaves the previous good copy in place.
- **No lost updates**: `g_file_mtx` serialises all writers, so concurrent operations  
  queue up and each sees the result of the previous one.

### Authentication

Passwords are stored as salted, iterated FNV-1a hashes (see `auth.c`).  
**This is NOT production-grade cryptography** — it is a learning exercise.  
In production, use bcrypt, scrypt, or Argon2.

### Audit log

Every significant event is written to `vaultnet_audit.log` under a dedicated mutex,  
ensuring entries are never interleaved.  A global atomic sequence counter gives each  
entry a monotonically increasing identifier.

Format:
```
seq|ISO-8601-timestamp|EVENT|username|detail|amount|OK_or_ERR
```

---

## Concurrency stress test

```sh
bash test_concurrency.sh
```

The script spawns 60 concurrent clients (30 depositors + 30 transferrers) against  
two shared accounts seeded with 100 000 each, then verifies that `alice + bob` exactly  
equals the expected total — proving no money is created or destroyed under heavy  
concurrent access.

Sample output:

```
Server started (PID …, port 18080)
Spawning 60 background clients (30 depositors + 30 transferrers)...
Waiting for 60 background processes to finish...

alice balance   : 100000.00
bob   balance   : 100300.00
alice + bob     : 200300
base (2×seed)   : 200000
max possible    : 200300  (if all 30 deposits succeeded)

RESULT: PASS – money conserved under 60 concurrent clients
```

The 192-entry audit log (monotonically sequenced) confirms all operations were  
fully serialised at the log level with no interleaving.
