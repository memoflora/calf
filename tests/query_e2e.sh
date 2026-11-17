#!/usr/bin/env bash
# Writes through the client, then checks the samples with the query tool,
# before and after a restart. Usage: tests/query_e2e.sh [build_dir]
set -u

BUILD="${1:-build}"
PORT=17272
DIR=$(mktemp -d)
WAL="$DIR/e2e.wal"
PID=""

cleanup() {
    [ -n "$PID" ] && { kill -9 "$PID"; wait "$PID"; } 2>/dev/null
    rm -rf "$DIR"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*"
    for f in "$DIR"/server*.err; do [ -f "$f" ] && { echo "--- $f"; cat "$f"; }; done
    exit 1
}

# $1 names the stderr file, so each run of the server keeps its own.
start_server() {
    "$BUILD/server" "$PORT" "$WAL" 2>"$DIR/$1.err" &
    PID=$!
    for _ in $(seq 400); do
        grep -q "listening on" "$DIR/$1.err" && return
        kill -0 "$PID" 2>/dev/null || fail "server exited at start"
        sleep 0.05
    done
    fail "server did not start"
}

stop_server() {
    kill -9 "$PID"
    wait "$PID" 2>/dev/null
    PID=""
}

# Queries every series in the client logs over its whole range and writes
# "name ts value_bits" lines to $1. The series names and ranges come from
# the S lines, so a sample written by a request without an A still shows.
# The loop reads from < <(...), not from a pipe: a pipe runs the loop in a
# subshell, and fail there would not stop the script.
query_all() {
    : >"$1"
    local name lo hi out
    while read -r name lo hi; do
        out=$("$BUILD/query" "$name" "$lo" "$hi" "$PORT" 2>/dev/null) ||
            fail "query $name $lo $hi failed"
        [ -n "$out" ] && printf '%s\n' "$out" | sed "s/^/$name /" >>"$1"
    done < <(awk '$1 == "S" {
            if (!($2 in lo) || $3 < lo[$2]) lo[$2] = $3
            e = $3 + $4 - 1
            if (!($2 in hi) || e > hi[$2]) hi[$2] = e
        } END { for (n in lo) print n, lo[n], hi[n] }' "$DIR"/*.log)
    sort -o "$1" "$1"
}

# Every A request must be present with all K samples and value = ts. Every
# returned sample must come from an S request, with the right bits.
check() {
    python3 - "$1" "$DIR"/*.log <<'EOF' || fail "check of $1"
import struct, sys
got = set()
for line in open(sys.argv[1]):
    name, ts, bits = line.split()
    got.add((name, int(ts), bits))
sent, acked = set(), set()
for path in sys.argv[2:]:
    for line in open(path):
        kind, name, ts0, k = line.split()
        for ts in range(int(ts0), int(ts0) + int(k)):
            sample = (name, ts, struct.pack(">d", float(ts)).hex())
            if kind == "S":
                sent.add(sample)
            elif kind == "A":
                acked.add(sample)
missing = acked - got
extra = got - sent
print(f"{sys.argv[1]}: {len(got)} samples, {len(acked)} acked, "
      f"{len(missing)} missing, {len(extra)} unexpected")
sys.exit(1 if missing or extra or not acked else 0)
EOF
}

start_server server1
"$BUILD/client" 200 4 "$PORT" --samples 4 --log "$DIR/c1.log" >/dev/null ||
    fail "client writes failed"
query_all "$DIR/q1.txt"
check "$DIR/q1.txt"

# Read-your-writes: the client closes its connection after the OK, and the
# query tool opens a second one.
"$BUILD/client" 1 1 "$PORT" --samples 4 --log "$DIR/c2.log" >/dev/null ||
    fail "read-your-writes write failed"
read -r kind name ts0 k <<<"$(grep '^A ' "$DIR/c2.log")"
[ "$kind" = A ] || fail "read-your-writes write was not acked"
n=$("$BUILD/query" "$name" "$ts0" "$((ts0 + k - 1))" "$PORT" 2>/dev/null | wc -l | tr -d ' ')
[ "$n" -eq "$k" ] || fail "read-your-writes: $n of $k samples"
echo "read-your-writes: $n of $k samples"

query_all "$DIR/q2.txt"
check "$DIR/q2.txt"

stop_server
start_server server2
query_all "$DIR/q3.txt"
check "$DIR/q3.txt"
cmp -s "$DIR/q2.txt" "$DIR/q3.txt" || fail "results differ after restart"
echo "after restart: same $(wc -l <"$DIR/q3.txt" | tr -d ' ') samples"

if grep -qE "Sanitizer|runtime error" "$DIR"/server*.err; then
    fail "sanitizer report in server stderr"
fi
echo "query_e2e: all passed"
