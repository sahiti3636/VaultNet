# VaultNet

A multi-threaded TCP banking server written in C11.
Features POSIX sockets, pthreads, WAL-backed crash recovery, PBKDF2-HMAC-SHA256 password hashing,
optional TLS 1.2+, and a load generator with latency percentiles — all under `-Wall -Wextra -Werror`.

---

## Features

| Area | Detail |
|---|---|
| **Concurrency** | 64-worker bounded thread pool; `pthread_rwlock_t` + per-account `pthread_mutex_t`; strict lock-ordering to prevent deadlocks |
| **Durability** | Binary WAL (write-ahead log) with CRC-32, group commit, and crash recovery; atomic `rename()` snapshots |
| **Auth** | PBKDF2-HMAC-SHA256 (600 000 iterations for new hashes, 32-byte key, `RAND_bytes` salt, `CRYPTO_memcmp` constant-time verify) |
| **Transport** | Optional TLS 1.2+ via OpenSSL `VNConn` abstraction; plaintext fallback; `--tls` client flag |
| **Precision** | Integer money in `int64_t` paise (1/100 of a unit) — no floating-point arithmetic |
| **Networking** | epoll-based accept loop on Linux with self-pipe shutdown; `accept()`-loop fallback on macOS |
| **Testing** | 37 unit tests, concurrency conservation proof, crash-recovery test, TLS smoke test, TSan + ASan builds |
| **Load testing** | C load generator with p50/p95/p99/max latency reporting |

---

## Requirements

| Tool | Version |
|---|---|
| GCC | 11+ (or Clang 12+) |
| OpenSSL | 3.x (via `pkg-config`; falls back to `brew --prefix openssl@3` on macOS) |
| GNU Make | any modern version |
| POSIX OS | Linux (primary) or macOS (development) |

On Ubuntu/Debian:

```bash
sudo apt-get install gcc make libssl-dev
```

On macOS:

```bash
brew install openssl@3
```

---

## Building

```bash
make              # server + client  →  bin/vaultnet_server, bin/vaultnet_client
make unit         # unit test runner →  bin/vaultnet_unit
make loadgen      # load generator  →  bin/vaultnet_loadgen
make tsan         # ThreadSanitizer builds
make asan         # AddressSanitizer + UBSan builds
make clean        # remove obj/, bin/, and all data files
```

All targets build with `-Wall -Wextra -Werror -pthread -O2 -std=c11 -D_GNU_SOURCE`.

---

## Running

### Plain-text mode

```bash
# Server
./bin/vaultnet_server <port>
# e.g.
./bin/vaultnet_server 18080

# Client (interactive)
./bin/vaultnet_client [host [port]]
./bin/vaultnet_client 127.0.0.1 18080
```

### TLS mode

Generate a self-signed dev certificate (valid 10 years, localhost + 127.0.0.1 SANs):

```bash
make gen-cert          # creates vaultnet.crt and vaultnet.key
# or directly:
bash gen_cert.sh
```

Start the server with cert and key:

```bash
./bin/vaultnet_server 18080 vaultnet.crt vaultnet.key
```

Connect with TLS:

```bash
./bin/vaultnet_client 127.0.0.1 18080 --tls --cafile vaultnet.crt
```

Run the TLS smoke test end-to-end:

```bash
make tls-smoke
```

### Graceful shutdown

Send `SIGINT` (Ctrl-C) or `SIGTERM`.  The server stops accepting new connections, drains
in-flight work from the thread pool, checkpoints the WAL, flushes the audit log, and exits cleanly.

### Persistent files

| File | Contents |
|---|---|
| `vaultnet.snap` | Atomic text snapshot of all accounts |
| `vaultnet.wal` | Write-ahead log (binary, CRC-32 records) |
| `vaultnet_audit.log` | Append-only human-readable audit trail |
| `vaultnet.lock` | Advisory lockfile preventing duplicate server instances |

---

## Protocol Reference

Line-based text protocol (newline-terminated).  The client sends one command per line;
the server replies with `OK …` or `ERR …`.  Multi-line responses end with `END n records`.

| Command | Auth required | Description |
|---|---|---|
| `REGISTER <user> <pass>` | No | Create a new account; password ≥ 8 chars |
| `LOGIN <user> <pass>` | No | Start an authenticated session |
| `LOGOUT` | Yes | End the session (remains connected) |
| `BALANCE` | Yes | Show current balance |
| `DEPOSIT <amount>` | Yes | Add funds (integer units; e.g. `DEPOSIT 100` → adds 100.00) |
| `WITHDRAW <amount>` | Yes | Remove funds (rejected if insufficient balance) |
| `TRANSFER <to_user> <amount>` | Yes | Atomic transfer to another account |
| `HISTORY` | Yes | Last 50 transactions from the audit log |
| `QUIT` | No | Close the connection |

Example session:

```
> REGISTER alice s3cret99
< OK account created
> LOGIN alice s3cret99
< OK logged in as alice
> DEPOSIT 500
< OK deposited 500.00 new_balance 500.00
> TRANSFER bob 100
< OK transferred 100.00 to bob new_balance 400.00
> HISTORY
< OK history for alice:
< 1|2026-10-30T12:00:00|REGISTER|alice|new account|0.00|OK
< 2|2026-10-30T12:00:01|LOGIN|alice||0.00|OK
< 3|2026-10-30T12:00:02|DEPOSIT|alice||500.00|OK
< 4|2026-10-30T12:00:03|TRANSFER|alice|to bob|100.00|OK
< END 4 records
> QUIT
< OK goodbye
```

---

## Testing

### Unit tests (39 assertions)

```bash
make unit
./bin/vaultnet_unit
```

Covers: account register + auth, deposit/withdraw, transfer, balance reads,
WAL crash recovery (simulated mid-write kill), and double-restart recovery.
Uses `/tmp/vaultnet_unit.{snap,wal}` — no side effects in the project directory.

### Concurrency conservation test

```bash
bash test_concurrency.sh
```

Spawns 60 concurrent clients (30 depositors + 30 transferrers) against two accounts
seeded with 100 000 each, then verifies `alice + bob` equals the expected total —
proving no money is created or destroyed under heavy concurrent access.

```
RESULT: PASS – money conserved under 60 concurrent clients
```

### Crash recovery test

```bash
bash tests/crash_test.sh
```

Performs a sequence of deposits, kills the server mid-way with `kill -9` (bypasses
shutdown handler), restarts, and verifies every acknowledged operation survived.

### TLS smoke test

```bash
bash tests/tls_smoke.sh [port]
```

Requires the server to be running with TLS on the given port. Tests `REGISTER`,
`LOGIN`, `DEPOSIT`, `BALANCE`, `HISTORY`, bad-password rejection, and rejection
of a certificate signed by an unrelated CA.

```
RESULT: PASS – TLS handshake + REGISTER + DEPOSIT + BALANCE + bad-auth all OK
```

### All tests in one command

```bash
make check
```

Runs unit tests, concurrency test, and crash recovery test in sequence.

### Sanitizer builds

```bash
# ThreadSanitizer — detects data races and lock-order violations
make tsan
TSAN_OPTIONS=halt_on_error=1 ./bin/vaultnet_server_tsan 18080 &
# ... drive it with clients ...

# AddressSanitizer + UBSan — detects memory errors and undefined behaviour
make asan
ASAN_OPTIONS="halt_on_error=1 detect_leaks=1" ./bin/vaultnet_server_asan 18080 &
```

---

## Load Testing

```bash
make bench
# or manually:
./bin/vaultnet_loadgen -h 127.0.0.1 -p 18080 -t 12 -c 16 -w 2
```

Flags:

| Flag | Default | Description |
|---|---|---|
| `-h HOST` | `127.0.0.1` | Server hostname |
| `-p PORT` | `18080` | Server port |
| `-t SECS` | `10` | Test duration in seconds |
| `-c CONNS` | `8` | Number of concurrent connections |
| `-w WARM` | `2` | Warm-up period in seconds (excluded from stats) |

Each worker registers once, then loops `LOGIN → DEPOSIT 1 → LOGOUT`.
Latency is tracked per-operation using `clock_gettime(CLOCK_MONOTONIC)`.

Example from a local 12-second run with 16 connections:

```
Throughput         : 750.3 ops/sec
Latency  p50       : 19661 µs
Latency  p95       : 33831 µs
Latency  p99       : 41281 µs
Latency  max       : 56010 µs
```

The figures above were measured before the PBKDF2 work factor changed from
100,000 to 600,000 iterations. Each acknowledged write waits for a WAL `fsync()`.
Independent account mutations
can now queue records concurrently, and the WAL writer reports records, `fsync`
calls, and average batch size on shutdown. In an 8-second local run on an Apple
M3 Pro with internal SSD, the old lock design had a 1.00 average batch at 64
connections; the new design measured 1.25. The current benchmark logs in for
every transaction, so PBKDF2 work still limits throughput (721.0 before and
675.3 ops/sec after in those runs). With checkpoints moved to a background
worker, a repeat at 64 connections measured 702.2 ops/sec and a maximum
latency of 259,992 µs versus 332,613 µs with request-path checkpoints.
These are observations from one run each.

---

## Architecture

### Thread and I/O model

```
main thread
  └── accept loop (epoll on Linux, blocking accept() on macOS)
        └── accept() → push ClientConn* to 256-slot job queue
              └── 64 worker threads → optional TLS handshake → command loop
```

On Linux, the accept loop uses `epoll_create1` with `EPOLLIN` on both the listen socket
and a self-pipe.  The signal handler writes 1 byte to the pipe, which wakes `epoll_wait`
without races.

### Lock ordering (deadlock-free by design)

Mutating operations acquire locks in this order:

1. **`g_ckpt_rwlock`** in read mode — prevents a checkpoint from observing a
   WAL record before its balance is applied.
2. **`g_reg_rwlock`** — write-locked for `REGISTER`, read-locked for other
   mutations.
3. **`account.lock`** — protects each balance.
   For `TRANSFER`, both account locks are acquired in ascending `account_id` order
   to prevent a cycle in the lock graph. A checkpoint takes `g_ckpt_rwlock` in
   write mode after in-flight mutations finish.

### WAL and crash recovery

Every successful balance mutation follows this sequence:

1. Build a 342-byte WAL record with a CRC-32 checksum.
2. The WAL writer writes and `fsync()`s the record. It can batch records already
   queued together.
3. Apply the new balance in memory and acknowledge the command.
4. A background worker wakes after 500 mutations or every 3 seconds, writes
   and syncs a text snapshot, atomically renames it, syncs its directory, then
   truncates the WAL. Requests only signal it. Shutdown stops the worker and
   takes one final checkpoint.

On startup, the server replays any WAL records whose LSN exceeds the snapshot's
checkpoint LSN, restoring all acknowledged operations after a crash.
LSN tracking uses `_Atomic uint64_t g_last_lsn` (safe under TSan).

### Password security

`src/auth.c` implements PBKDF2-HMAC-SHA256:

- 16-byte random salt via `RAND_bytes`
- 600 000 iterations for new hashes, 32-byte derived key
- Stored in separate salt and hash fields in the account snapshot and WAL
- New hashes carry their iteration count (`pbkdf2_sha256$600000$<hex>`);
  the old 100,000-iteration bare-hex format remains readable
- Verification uses `CRYPTO_memcmp` (constant-time) to prevent timing attacks
- Login copies the salt and hash under the account lock, then runs PBKDF2
  after releasing it, so balance operations are not blocked by hashing

### TLS abstraction (`VNConn`)

`src/tlsutil.h` defines a `VNConn` — a `{int fd; SSL *ssl}` pair.  All I/O goes through:

```c
ssize_t vn_readline (VNConn *c, char *buf, size_t maxlen);
int     vn_sendline (VNConn *c, const char *line);
int     vn_sendlinef(VNConn *c, const char *fmt, ...);
void    vn_close   (VNConn *c);
```

When `ssl == NULL`, these use plain `read()`/`write()`. Supplying certificate and
key paths starts the server in TLS mode; otherwise the port serves plaintext.
The TLS client verifies the server certificate and host name by default. Use
`--cafile vaultnet.crt` for the generated development certificate, or omit it
to use the system trust store. `--insecure` disables verification explicitly
and prints a warning; it is intended only for local testing.

---

## Source File Layout

```
src/
  server.c      Main loop, accept/epoll, client dispatch, signal handling
  client.c      Interactive CLI client, --tls flag, VNConn I/O
  accounts.c    In-memory account store, snapshot persistence
  accounts.h
  auth.c        PBKDF2-HMAC-SHA256 hash, salt generation, constant-time verify
  auth.h
  audit.c       Append-only audit log with atomic sequence counter
  audit.h
  wal.c         Binary WAL: append, replay, group commit, checkpoint, CRC-32
  wal.h
  tlsutil.c     VNConn TLS abstraction, OpenSSL context setup
  tlsutil.h
  netutil.c     readline/sendline helpers (plain fd)
  netutil.h
  protocol.h    Shared constants (max username/password length, amount limits)
  threadpool.c  Bounded circular-queue thread pool (64 workers, 256-slot queue)

tests/
  unit.c        39-assertion unit test suite (no server process required)
  loadgen.c     C load generator with latency percentiles
  crash_test.sh Crash recovery integration test
  tls_smoke.sh  TLS end-to-end smoke test

gen_cert.sh     Self-signed dev certificate generator (10-year, localhost SANs)
test_concurrency.sh  Money-conservation concurrency stress test
Makefile
.github/workflows/ci.yml  Two-job CI: build+test+TLS+bench, TSan+ASan
```

---

## CI

Two GitHub Actions jobs on every push to `main`:

**`build-and-test`** (ubuntu-latest):
- Installs `libssl-dev`, builds server + client + unit + loadgen
- Runs unit tests, concurrency test, crash recovery test
- Generates a cert and runs the TLS smoke test
- Runs a benchmark smoke (8 s, 8 connections)

**`sanitizers`** (ubuntu-latest):
- Builds with `-fsanitize=thread` and runs the TSan concurrency stress test
- Builds with `-fsanitize=address,undefined` and runs the ASan smoke test

---

## Limitations and Known Trade-offs

- **Single WAL file**: suitable for a single-node server; no replication.
- **fsync bottleneck**: each acknowledged mutation waits for WAL durability;
  batching reduces the number of syncs when independent operations overlap.
- **Max accounts**: compile-time constant `MAX_ACCOUNTS` (default 2048) in `protocol.h`.
- **HISTORY**: scans the audit log linearly; scales to ~50 k entries before becoming slow.
