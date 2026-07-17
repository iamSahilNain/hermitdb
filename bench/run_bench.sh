#!/usr/bin/env bash
# Benchmark matrix per SPEC §7. [Claude Code]
#
#   {hermitdb 1/2/4/8 threads, real redis-server} x {SET,GET,INCR}
#     x {fsync always,everysec,no} x pipeline {1,16}
#
# Honesty rule: the README table is generated from these outputs and must be
# reproducible with this script on stated hardware. No cherry-picking.
#
# Usage (inside the dev container, from the repo root):
#   ./bench/run_bench.sh [path-to-hermitdb-binary]
#
# Requires: redis-benchmark (redis-tools). Compares against redis-server too
# if it is installed; rows are skipped (and said so) when a config can't run
# yet (e.g. --threads>1 before CP5, --wal before CP4).

set -euo pipefail

BINARY="${1:-./build/hermitdb}"
OUT_DIR="$(dirname "$0")/results"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RAW="$OUT_DIR/raw_$STAMP"
TABLE="$OUT_DIR/table_$STAMP.md"
PORT=6390
N_REQUESTS="${N_REQUESTS:-1000000}"
CLIENTS=50

mkdir -p "$RAW"
command -v redis-benchmark >/dev/null || { echo "redis-benchmark not found (apt install redis-tools)"; exit 1; }
[ -x "$BINARY" ] || { echo "server binary not found at $BINARY (run: make build)"; exit 1; }

wait_port() {
  for _ in $(seq 1 100); do
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && { exec 3>&-; return 0; }
    sleep 0.1
  done
  return 1
}

# start_server <name> <cmd...> -> echoes PID, or empty if it refused to start
start_server() {
  local name="$1"; shift
  local data_dir; data_dir="$(mktemp -d)"
  "$@" >"$RAW/server_$name.log" 2>&1 &
  local pid=$!
  if ! wait_port "$PORT"; then
    kill -9 "$pid" 2>/dev/null || true
    echo ""
    return 0
  fi
  echo "$pid"
}

run_row() {  # <label> <pipeline>
  local label="$1" pipeline="$2"
  echo "  bench: $label P=$pipeline"
  redis-benchmark -p "$PORT" -t set,get,incr -n "$N_REQUESTS" -c "$CLIENTS" \
    -P "$pipeline" --csv >"$RAW/${label}_P${pipeline}.csv" 2>"$RAW/${label}_P${pipeline}.err" || true
}

bench_config() {  # <label> <server-cmd...>
  local label="$1"; shift
  local pid
  pid="$(start_server "$label" "$@")"
  if [ -z "$pid" ]; then
    echo "  SKIP $label — server refused this config (checkpoint not implemented yet?)"
    echo "$label: SKIPPED" >>"$RAW/skipped.txt"
    return 0
  fi
  for p in 1 16; do run_row "$label" "$p"; done
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

echo "== hermitdb configurations =="
for threads in 1 2 4 8; do
  DATA="$(mktemp -d)"
  bench_config "hermit_t${threads}_nofsync" \
    "$BINARY" --port="$PORT" --threads="$threads" --data-dir="$DATA"
done
for policy in always everysec no; do
  DATA="$(mktemp -d)"
  bench_config "hermit_t1_wal_${policy}" \
    "$BINARY" --port="$PORT" --data-dir="$DATA" --wal --fsync="$policy"
done

echo "== real redis (baseline) =="
if command -v redis-server >/dev/null; then
  bench_config "redis_baseline" redis-server --port "$PORT" --save '' --appendonly no
else
  echo "  redis-server not installed; baseline skipped" | tee -a "$RAW/skipped.txt"
fi

echo "== fsync microbenchmark (anchors the CP4 story) =="
python3 - "$RAW/fsync_micro.txt" <<'EOF'
import os, sys, tempfile, time
fd, path = tempfile.mkstemp()
N = 200
t0 = time.perf_counter()
for i in range(N):
    os.write(fd, b"x" * 64)
    os.fsync(fd)
per = (time.perf_counter() - t0) / N * 1e6
os.close(fd); os.unlink(path)
line = f"fsync cost on this disk: {per:.1f} us per fsync ({N} samples, 64B appends)"
print(line)
open(sys.argv[1], "w").write(line + "\n")
EOF

echo "== emitting markdown table =="
python3 - "$RAW" "$TABLE" <<'EOF'
import csv, glob, os, sys
raw, out = sys.argv[1], sys.argv[2]
rows = []
for path in sorted(glob.glob(os.path.join(raw, "*_P*.csv"))):
    label = os.path.basename(path)[:-4]
    with open(path) as f:
        for rec in csv.DictReader(f):
            if not rec.get("test"):
                continue
            rows.append((label, rec["test"], float(rec["rps"]),
                         rec.get("p50_latency_ms", "?"), rec.get("p99_latency_ms", "?")))
with open(out, "w") as f:
    f.write("| config | op | ops/sec | p50 ms | p99 ms |\n|---|---|---|---|---|\n")
    for label, test, rps, p50, p99 in rows:
        f.write(f"| {label} | {test} | {rps:,.0f} | {p50} | {p99} |\n")
    skipped = os.path.join(raw, "skipped.txt")
    if os.path.exists(skipped):
        f.write("\nSkipped configs (not implemented yet):\n\n")
        for line in open(skipped):
            f.write(f"- {line.strip()}\n")
    micro = os.path.join(raw, "fsync_micro.txt")
    if os.path.exists(micro):
        f.write("\n" + open(micro).read())
print(f"wrote {out}")
EOF

echo "done. raw: $RAW  table: $TABLE"
