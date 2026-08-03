#!/usr/bin/env bash
# Run an Atlas cluster natively on localhost: 1 metadata node (50050) + 3 storage nodes
# (50051-50053), which register themselves into the ring on startup.
#
#   ./scripts/run-cluster-local.sh        (Ctrl-C to stop everything)
#
# Then, from another shell:
#   export ATLAS_METADATA=127.0.0.1:50050
#   ./build/atlas nodes
#   ./build/atlas put notes.txt ./some-file
#   ./build/atlas info notes.txt
#
# For the scripted failure/heal walkthrough, use ./scripts/demo-self-healing.sh instead.
set -euo pipefail

cd "$(dirname "$0")/.."
BIN="${1:-./build/atlas_node}"
if [[ ! -x "$BIN" ]]; then
  echo "binary not found: $BIN  (build first: cmake --preset default && cmake --build build)" >&2
  exit 1
fi

RUN="${TMPDIR:-/tmp}/atlas-cluster-$$"
mkdir -p "$RUN"

pids=()
cleanup() {
  kill "${pids[@]}" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -rf "$RUN"
}
trap cleanup EXIT INT TERM

ATLAS_ROLE=metadata ATLAS_NODE_ID=metadata ATLAS_LISTEN=127.0.0.1:50050 \
  ATLAS_DATA_DIR="$RUN/metadata" "$BIN" & pids+=($!)

# Storage nodes retry their join, so they tolerate the metadata node still coming up.
for i in 1 2 3; do
  ATLAS_ROLE=storage ATLAS_NODE_ID="node$i" ATLAS_LISTEN="127.0.0.1:5005$i" \
    ATLAS_ADVERTISE="127.0.0.1:5005$i" ATLAS_DATA_DIR="$RUN/node$i" \
    ATLAS_METADATA=127.0.0.1:50050 "$BIN" & pids+=($!)
done

wait
