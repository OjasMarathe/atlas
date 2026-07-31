#!/usr/bin/env bash
# Atlas self-healing demo, on real processes (no test harness).
#
#   ./scripts/demo-self-healing.sh
#
# Starts a metadata node + 4 storage nodes, uploads a file, kills one of the nodes holding it,
# and watches the cluster detect the failure and re-replicate the chunk onto a healthy node —
# then downloads the file and verifies the bytes are identical.
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD="${BUILD:-./build}"
RUN="${TMPDIR:-/tmp}/atlas-demo-$$"
export ATLAS_METADATA=127.0.0.1:50050

for binary in "$BUILD/atlas_node" "$BUILD/atlas"; do
  [[ -x "$binary" ]] || { echo "missing $binary — build first: cmake --build build" >&2; exit 1; }
done

mkdir -p "$RUN"
pids=()
cleanup() {
  kill "${pids[@]}" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -rf "$RUN"
}
trap cleanup EXIT INT TERM

step() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

step "starting the cluster (1 metadata + 4 storage)"
ATLAS_ROLE=metadata ATLAS_NODE_ID=metadata ATLAS_LISTEN=127.0.0.1:50050 \
  ATLAS_DATA_DIR="$RUN/metadata" ATLAS_HEAL_INTERVAL_MS=1000 ATLAS_FAILURE_THRESHOLD=2 \
  "$BUILD/atlas_node" > "$RUN/metadata.log" 2>&1 &
pids+=($!)
sleep 1

for i in 1 2 3 4; do
  ATLAS_ROLE=storage ATLAS_NODE_ID="node$i" ATLAS_LISTEN="127.0.0.1:5005$i" \
    ATLAS_ADVERTISE="127.0.0.1:5005$i" ATLAS_DATA_DIR="$RUN/node$i" \
    ATLAS_METADATA=127.0.0.1:50050 "$BUILD/atlas_node" > "$RUN/node$i.log" 2>&1 &
  pids+=($!)
done
sleep 3
"$BUILD/atlas" nodes

step "uploading a file"
head -c 200000 /dev/urandom | base64 > "$RUN/payload.txt"
"$BUILD/atlas" put demo.txt "$RUN/payload.txt"
"$BUILD/atlas" info demo.txt

holder=$("$BUILD/atlas" info demo.txt | awk '/holders:/ {print $NF; exit}')
step "killing $holder — one of the nodes holding this file"
port="5005${holder#node}"
victim_pid=$(lsof -nP -iTCP:"$port" -sTCP:LISTEN -t | head -1)
kill -9 "$victim_pid"
echo "killed $holder (pid $victim_pid, port $port)"

step "waiting for the cluster to notice and heal"
for _ in $(seq 1 20); do
  sleep 1
  grep -q "healed" "$RUN/metadata.log" && break
done
grep -E "nodes down|healed" "$RUN/metadata.log" | tail -5 || echo "(no healing logged yet)"

step "placement after healing"
"$BUILD/atlas" info demo.txt

step "the file still downloads, byte-for-byte"
"$BUILD/atlas" get demo.txt "$RUN/roundtrip.txt"
if cmp -s "$RUN/payload.txt" "$RUN/roundtrip.txt"; then
  printf '\033[32mOK — downloaded file is identical to the original\033[0m\n'
else
  printf '\033[31mFAIL — downloaded file differs\033[0m\n'
  exit 1
fi
