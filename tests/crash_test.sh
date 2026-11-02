#!/usr/bin/env bash
# tests/crash_test.sh – WAL crash-recovery test
#
# Verifies that a SIGKILL of the server (simulating a crash) does not lose
# any operation that was acknowledged to the client.
#
# Protocol:
#   1. Start server, register alice, deposit SEED.
#   2. Do N deposits of STEP each (all acknowledged).
#   3. Kill the server with SIGKILL (crash, no clean shutdown).
#   4. Restart the server.
#   5. Query alice's balance and verify it equals SEED + N*STEP.
#      (WAL replay must have recovered all acknowledged deposits.)

set -uo pipefail

BINDIR="${BINDIR:-./bin}"
SERVER_BIN="$BINDIR/vaultnet_server"
PORT=18081   # different port to avoid collision with other tests
HOST="127.0.0.1"

SEED=500     # initial deposit (rupees, sent to DEPOSIT command)
STEP=10      # amount per operation (rupees)
OPS=20       # operations to perform before crash
# Expected final balance in rupees (two decimal places), e.g. "700.00"
EXPECTED_BAL="$(( SEED + OPS * STEP )).00"

NC_CMD=""
for c in nc ncat netcat; do
    if command -v "$c" &>/dev/null; then NC_CMD="$c"; break; fi
done
[[ -z "$NC_CMD" ]] && { echo "FATAL: netcat not found"; exit 1; }

send_cmds() {
    printf '%s\n' "$1" | "$NC_CMD" -w 4 "$HOST" "$PORT" 2>/dev/null || true
}

# ── cleanup ────────────────────────────────────────────────────────────────────
cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -f vaultnet.snap vaultnet.snap.tmp vaultnet.wal vaultnet_audit.log vaultnet.lock
}
trap cleanup EXIT

# ── 1. Clean start ─────────────────────────────────────────────────────────────
pkill -f "vaultnet_server $PORT" 2>/dev/null || true
sleep 0.2
rm -f vaultnet.snap vaultnet.snap.tmp vaultnet.wal vaultnet_audit.log vaultnet.lock

"$SERVER_BIN" "$PORT" &
SERVER_PID=$!
sleep 0.6
echo "Server started (PID $SERVER_PID)"

# ── 2. Register and seed ────────────────────────────────────────────────────────
send_cmds "$(printf 'REGISTER alice alicepass\nQUIT')"                             > /dev/null
SEED_RESP=$(send_cmds "$(printf 'LOGIN alice alicepass\nDEPOSIT %s\nLOGOUT\nQUIT' "$SEED")")
if ! grep -q 'OK deposited' <<< "$SEED_RESP"; then
    echo "FATAL: initial deposit was not acknowledged"
    exit 1
fi
echo "Registered alice, seeded $SEED rupees"

# ── 3. Perform OPS deposits (all acknowledged before crash) ─────────────────────
echo "Performing $OPS acknowledged deposits of $STEP rupees each..."
for ((i=0; i<OPS; i++)); do
    resp=$(send_cmds "$(printf 'LOGIN alice alicepass\nDEPOSIT %s\nLOGOUT\nQUIT' "$STEP")")
    if ! echo "$resp" | grep -q "OK deposited"; then
        echo "FATAL: deposit $i not acknowledged: $resp"
        exit 1
    fi
done
echo "All $OPS deposits acknowledged by server."

# ── 4. Crash the server ────────────────────────────────────────────────────────
echo "Crashing server with SIGKILL..."
kill -KILL "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
sleep 0.4  # let the OS release the port
echo "Server killed."

# ── 5. Restart ────────────────────────────────────────────────────────────────
echo "Restarting server..."
"$SERVER_BIN" "$PORT" &
SERVER_PID=$!
sleep 0.6
echo "Server restarted (PID $SERVER_PID)"

# ── 6. Verify recovered balance ────────────────────────────────────────────────
RESP=$(send_cmds "$(printf 'LOGIN alice alicepass\nBALANCE\nLOGOUT\nQUIT')")
echo ""
echo "Recovery response: $RESP"
echo ""

BAL=$(echo "$RESP" | grep -oE 'balance [0-9]+(\.[0-9]+)?' | awk '{print $2}' | tail -1)

echo "──────────────────────────────────────────────────────"
printf "  expected balance  : %s rupees\n" "$EXPECTED_BAL"
printf "  recovered balance : %s rupees\n" "${BAL:-<unreadable>}"
echo "──────────────────────────────────────────────────────"

if [[ "${BAL:-}" == "$EXPECTED_BAL" ]]; then
    echo "  RESULT: PASS – all acknowledged operations survived the crash"
else
    echo "  RESULT: FAIL – expected $EXPECTED_BAL, got ${BAL:-<none>}"
    echo ""
    echo "WAL/snapshot files:"
    ls -la vaultnet.snap vaultnet.wal 2>/dev/null || true
    echo "Snapshot contents:"
    cat vaultnet.snap 2>/dev/null || true
    exit 1
fi

echo ""
echo "Crash recovery test complete."
