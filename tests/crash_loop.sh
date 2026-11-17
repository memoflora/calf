#!/usr/bin/env bash
# Kills the server with SIGKILL many times and checks the WAL after each
# restart against the client logs.
# Usage: tests/crash_loop.sh [seed] [build_dir]. Exit code 0 if all checks pass.
# The port is CALF_TEST_PORT (default 17170).
set -euo pipefail

SEED=${1:-}
if [[ -z "$SEED" ]]; then SEED=$(( $(date +%s) % 32768 )); fi
BUILD=${2:-build}
PORT=${CALF_TEST_PORT:-17170}
RUNS=50
echo "seed $SEED"
RANDOM=$SEED

SERVER="$BUILD/server"
CLIENT="$BUILD/client"
DIR=$(mktemp -d)
SPID=""
STARTS=0

cleanup() {
    if [[ -n "$SPID" ]]; then kill -9 "$SPID" 2>/dev/null || true; fi
    rm -rf "$DIR"
}
trap cleanup EXIT

# Prints to stderr and exits. Inside $(...) this ends only the subshell, so
# always assign $(...) to a variable: set -e then sees the failed assignment.
fail() {
    echo "FAIL: $*" >&2
    echo "seed $SEED" >&2
    exit 1
}

# Starts the server on wal $1 and waits until it listens. Its stderr goes to
# $DIR/start_<n>.err, and LAST_ERR names that file.
start_server() {
    STARTS=$((STARTS + 1))
    LAST_ERR="$DIR/start_$STARTS.err"
    "$SERVER" "$PORT" "$1" 2>"$LAST_ERR" &
    SPID=$!
    for _ in $(seq 400); do
        if grep -q "listening on" "$LAST_ERR"; then return; fi
        if ! kill -0 "$SPID" 2>/dev/null; then
            cat "$LAST_ERR"
            fail "server exited during start"
        fi
        sleep 0.05
    done
    fail "server did not listen within 20 s"
}

# TSan reports a race and keeps running, so a sanitizer report shows only in
# stderr. Fail on it, or the sanitizer builds of this test check nothing more.
check_stderr() {
    if grep -qE "Sanitizer|runtime error" "$1"; then
        cat "$1" >&2
        fail "sanitizer report in $1"
    fi
}

kill_server() {
    kill -9 "$SPID"
    wait "$SPID" 2>/dev/null || true
    SPID=""
    check_stderr "$LAST_ERR"
}

dump() {
    "$SERVER" --dump "$1" >"$2" 2>"$DIR/dump.err" || fail "dump of $1 failed"
    check_stderr "$DIR/dump.err"
}

check() {
    python3 "$DIR/check.py" "$@" || fail "check $*"
}

cat >"$DIR/check.py" <<'EOF'
import struct
import sys
from collections import OrderedDict


def load(path):
    """Returns the records of a dump: a list of [(name, ts, bits), ...]."""
    recs = OrderedDict()
    with open(path) as f:
        for line in f:
            idx, name, ts, bits = line.split()
            recs.setdefault(int(idx), []).append((name, int(ts), bits))
    return list(recs.values())


def bits(ts):
    return "%016x" % struct.unpack("<Q", struct.pack("<d", float(ts)))[0]


def cmd_check(dump, start, logs):
    recs = load(dump)[start:]
    sent, acked, rejected = {}, set(), set()
    for path in logs:
        with open(path) as f:
            for line in f:
                kind, name, ts0, k = line.split()
                key = (name, int(ts0))
                if kind == "S":
                    sent[key] = int(k)
                elif kind == "A":
                    acked.add(key)
                else:
                    rejected.add(key)

    seen = set()
    for i, rec in enumerate(recs):
        key = (rec[0][0], rec[0][1])
        if key not in sent:
            sys.exit("record %d: %s was never sent" % (start + i, key))
        want = [(key[0], key[1] + j, bits(key[1] + j)) for j in range(sent[key])]
        if rec != want:
            sys.exit("record %d: partial or wrong: %s" % (start + i, rec))
        if key in seen:
            sys.exit("record %d: %s appears twice" % (start + i, key))
        seen.add(key)

    for key in acked - seen:
        sys.exit("acked request %s is missing" % (key,))
    for key in rejected & seen:
        sys.exit("rejected request %s is present" % (key,))
    print("records %d, acked %d, present but not acked %d, sent and absent %d"
          % (len(recs), len(acked), len(seen - acked), len(set(sent) - seen)))


def main():
    cmd = sys.argv[1]
    if cmd == "count":
        print(len(load(sys.argv[2])))
    elif cmd == "prefix":
        a, b, n = load(sys.argv[2]), load(sys.argv[3]), int(sys.argv[4])
        if len(a) < n or len(b) < n or a[:n] != b[:n]:
            sys.exit("first %d records differ" % n)
    elif cmd == "check":
        cmd_check(sys.argv[2], int(sys.argv[3]), sys.argv[4:])
    else:
        sys.exit("unknown command " + cmd)


main()
EOF

# Case 1: cut the file inside its last record.
W="$DIR/trunc.wal"
start_server "$W"
"$CLIENT" 200 4 "$PORT" --samples 4 --log "$DIR/t1.log" >/dev/null
kill_server
dump "$W" "$DIR/t_before.txt"
check check "$DIR/t_before.txt" 0 "$DIR/t1.log" >/dev/null
N=$(check count "$DIR/t_before.txt")
python3 -c "import os,sys; os.truncate(sys.argv[1], os.path.getsize(sys.argv[1]) - 5)" "$W"
start_server "$W"
grep -q "torn tail" "$LAST_ERR" || fail "truncate case: no torn tail reported"
grep -q "truncated to" "$LAST_ERR" || fail "truncate case: file not truncated"
"$CLIENT" 100 4 "$PORT" --samples 4 --log "$DIR/t2.log" >/dev/null || fail "truncate case: appends failed"
kill_server
dump "$W" "$DIR/t_after.txt"
check prefix "$DIR/t_before.txt" "$DIR/t_after.txt" $((N - 1))
RESULT=$(check check "$DIR/t_after.txt" $((N - 1)) "$DIR/t2.log")
echo "truncate case: $RESULT"

# Case 2: flip one byte in the middle of the file.
W="$DIR/corrupt.wal"
start_server "$W"
"$CLIENT" 200 4 "$PORT" --samples 4 --log "$DIR/c1.log" >/dev/null
kill_server
dump "$W" "$DIR/c_before.txt"
N=$(check count "$DIR/c_before.txt")
python3 - "$W" <<'EOF'
import os, sys
path = sys.argv[1]
pos = os.path.getsize(path) // 2
with open(path, "r+b") as f:
    f.seek(pos)
    b = f.read(1)[0]
    f.seek(pos)
    f.write(bytes([b ^ 0xFF]))
EOF
start_server "$W"
grep -q "torn tail" "$LAST_ERR" || fail "corrupt case: no torn tail reported"
grep -q "truncated to" "$LAST_ERR" || fail "corrupt case: file not truncated"
dump "$W" "$DIR/c_mid.txt"
J=$(check count "$DIR/c_mid.txt")
if (( J == 0 || J >= N )); then fail "corrupt case: $J of $N records kept"; fi
check prefix "$DIR/c_before.txt" "$DIR/c_mid.txt" "$J"
"$CLIENT" 100 4 "$PORT" --samples 4 --log "$DIR/c2.log" >/dev/null || fail "corrupt case: appends failed"
kill_server
dump "$W" "$DIR/c_after.txt"
check prefix "$DIR/c_before.txt" "$DIR/c_after.txt" "$J"
RESULT=$(check check "$DIR/c_after.txt" "$J" "$DIR/c2.log")
echo "corrupt case: kept $J of $N records; $RESULT"

# Crash loop: one WAL for all runs.
W="$DIR/crash.wal"
LOGS=()
TORN=0
for run in $(seq "$RUNS"); do
    start_server "$W"
    if grep -q "torn tail" "$LAST_ERR"; then TORN=$((TORN + 1)); fi
    dump "$W" "$DIR/d.txt"
    check check "$DIR/d.txt" 0 ${LOGS[@]+"${LOGS[@]}"} >/dev/null

    LOG="$DIR/run$run.log"
    LOGS+=("$LOG")
    "$CLIENT" 1000000 8 "$PORT" --samples 4 --log "$LOG" >/dev/null 2>&1 &
    CPID=$!
    DELAY=$((RANDOM % 301))
    sleep "$(printf '%d.%03d' $((DELAY / 1000)) $((DELAY % 1000)))"
    kill_server
    wait "$CPID" || true
done

start_server "$W"
if grep -q "torn tail" "$LAST_ERR"; then TORN=$((TORN + 1)); fi
dump "$W" "$DIR/d.txt"
kill_server
RESULT=$(check check "$DIR/d.txt" 0 "${LOGS[@]}")
echo "crash loop: $RUNS runs, $TORN restarts found a torn tail"
echo "crash loop: $RESULT"
ACKED=$(echo "$RESULT" | sed -E 's/.*acked ([0-9]+),.*/\1/')
if (( ACKED == 0 )); then fail "no request was acked, so nothing was checked"; fi
echo "PASS"
