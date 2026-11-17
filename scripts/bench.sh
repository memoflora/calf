#!/usr/bin/env bash
# Measures group commit against one sync per request, then bytes per sample.
# Usage: scripts/bench.sh [build_dir]   (default: build, which must be Release)
# Writes bench/results.csv (every run) and bench/results.md (medians).
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "${1:-$root/build}" && pwd)
port=17400
runs=3
samples=10
seconds=${BENCH_SECONDS:-10}

cache="$build/CMakeCache.txt"
build_type=$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$cache")
sanitize=$(sed -n 's/^CALF_SANITIZE:STRING=//p' "$cache")
# A debug or sanitizer build measures the instrumentation, not the sync.
if [[ "$build_type" != "Release" || -n "$sanitize" ]]; then
    echo "refusing: $build is not a plain Release build" \
         "(CMAKE_BUILD_TYPE='$build_type', CALF_SANITIZE='$sanitize')" >&2
    exit 1
fi
for bin in server bench chunk_stats; do
    [[ -x "$build/$bin" ]] || { echo "missing $build/$bin; build it first" >&2; exit 1; }
done

mkdir -p "$root/bench"
waldir=$(mktemp -d "$root/bench/wal.XXXXXX")
pid=
cleanup() {
    [[ -n "$pid" ]] && kill -9 "$pid" 2>/dev/null
    rm -rf "$waldir"
}
trap cleanup EXIT

os=$(uname -s)
if [[ "$os" == "Linux" ]]; then
    fstype=$(findmnt -n -o FSTYPE -T "$waldir")
    # tmpfs is RAM: a sync there costs nothing, so the numbers say nothing
    # about disk durability.
    if [[ "$fstype" == "tmpfs" ]]; then
        echo "refusing: $waldir is on tmpfs" >&2
        exit 1
    fi
    source_dev=$(findmnt -n -o SOURCE -T "$waldir")
    parent=$(lsblk -no PKNAME "$source_dev" 2>/dev/null | head -1)
    disk=$(lsblk -dno MODEL "/dev/${parent:-${source_dev#/dev/}}" 2>/dev/null || true)
    cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')
    cores=$(nproc)
    os_line="$(. /etc/os-release && echo "$PRETTY_NAME"), kernel $(uname -r)"
    sync_call="fdatasync"
elif [[ "$os" == "Darwin" ]]; then
    dev=$(df "$waldir" | tail -1 | awk '{print $1}')
    fstype=$(diskutil info "$dev" | awk -F': *' '/Type \(Bundle\)/ {print $2}')
    # An APFS volume sits in a synthesized container disk. The model name is
    # on the physical store under it.
    whole=$(diskutil info "$dev" | awk -F': *' '/Part of Whole/ {print $2}')
    store=$(diskutil info "$whole" | awk -F': *' '/APFS Physical Store/ {print $2}')
    [[ -n "$store" ]] && whole=$(diskutil info "$store" | awk -F': *' '/Part of Whole/ {print $2}')
    disk=$(diskutil info "$whole" | awk -F': *' '/Media Name/ {print $2}')
    cpu=$(sysctl -n machdep.cpu.brand_string)
    cores=$(sysctl -n hw.ncpu)
    os_line="macOS $(sw_vers -productVersion), kernel $(uname -r)"
    sync_call="fcntl F_FULLFSYNC"
else
    echo "unsupported OS: $os" >&2
    exit 1
fi
cxx=$(sed -n 's/^CMAKE_CXX_COMPILER:FILEPATH=//p' "$cache")
compiler=$("$cxx" --version | head -1)

# Must match sync_fd() in src/wal.cpp.
if [[ "$os" == "Darwin" ]]; then
    sync_note="On macOS, plain fsync does not flush the drive cache, so numbers with it are not durable numbers. calf uses F_FULLFSYNC, which does."
else
    sync_note="fdatasync flushes the data and the drive cache, if the drive honors cache flushes."
fi

env_block=$(cat <<EOF
- OS: $os_line ($(uname -m))
- CPU: $cpu, $cores cores
- Disk: ${disk:-unknown}
- Filesystem: $fstype (WAL in bench/ in the repo)
- Compiler: $compiler, CMAKE_BUILD_TYPE=Release
- Sync call after each WAL append: $sync_call. $sync_note
EOF
)
echo "$env_block"
echo

csv="$root/bench/results.csv"
echo "mode,W,S,requests_per_s,samples_per_s,p50_ms,p99_ms,mean_batch,mean_ms,run" > "$csv"

for mode in group batch1; do
    flag=()
    [[ "$mode" == "batch1" ]] && flag=(--max-batch 1)
    for w in 1 4 16 64; do
        for run in $(seq "$runs"); do
            # New WAL and server per run, so no run inherits state or page
            # cache.
            wal="$waldir/$mode.$w.$run.wal"
            log="$waldir/server.err"
            # ${flag[@]+...}: bash 3.2 (macOS) treats an empty array as unset under -u.
            "$build/server" ${flag[@]+"${flag[@]}"} "$port" "$wal" 2>"$log" &
            pid=$!
            for _ in $(seq 200); do
                grep -q "listening on" "$log" && break
                kill -0 "$pid" 2>/dev/null || { cat "$log" >&2; exit 1; }
                sleep 0.05
            done
            line=$("$build/bench" --writers "$w" --samples "$samples" --seconds "$seconds" \
                       --mode "$mode" "$port" 2>"$waldir/bench.err") ||
                { cat "$waldir/bench.err" >&2; exit 1; }
            mean=$(awk '/^mean_ms/ {print $2}' "$waldir/bench.err")
            echo "$line,$mean,$run" | tee -a "$csv"
            kill -9 "$pid"
            wait "$pid" 2>/dev/null || true
            pid=
            rm -f "$wal"
        done
    done
done

# For each configuration, report the run with the median requests/s as a
# whole row, so p50, p99, and the batch size come from the same run.
median_rows=$(tail -n +2 "$csv" | sort -t, -k1,1 -k2,2n -k4,4n |
    awk -F, -v n="$runs" '{rows[NR] = $0} NR % n == 0 {print rows[NR - int(n / 2)]}')

md="$root/bench/results.md"
{
    echo "# Benchmark results"
    echo
    echo "Generated by scripts/bench.sh on $(date -u +%Y-%m-%d)."
    echo
    echo "## Environment"
    echo
    echo "$env_block"
    echo
    echo "## Group commit"
    echo
    echo "S = $samples samples per request, T = $seconds s measured after 1 s warm-up."
    echo "Each row is the run with the median requests/s out of $runs runs."
    echo "W x R is requests/s times the mean latency (Little's law). It should be close to W."
    echo "All runs are in bench/results.csv."
    echo
    echo "| mode | W | requests/s | samples/s | p50 ms | p99 ms | mean ms | mean batch | W x R |"
    echo "|---|---:|---:|---:|---:|---:|---:|---:|---:|"
    for mode in group batch1; do
        echo "$median_rows" | awk -F, -v m="$mode" '$1 == m {
            printf "| %s | %d | %.0f | %.0f | %.2f | %.2f | %.2f | %.2f | %.2f |\n",
                   $1, $2, $4, $5, $6, $7, $9, $8, $4 * $9 / 1000 }'
    done
    echo
    echo "## Compression (chunk_stats)"
    echo
    echo '```'
    "$build/chunk_stats"
    echo '```'
} > "$md"

echo
cat "$md"
