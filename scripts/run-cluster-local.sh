#!/usr/bin/env bash
# Run a 3-node Atlas cluster natively on localhost (ports 50051-50053).
# Usage: ./scripts/run-cluster-local.sh   (Ctrl-C to stop all three)
set -euo pipefail

BIN="${1:-./build/atlas_node}"
if [[ ! -x "$BIN" ]]; then
  echo "binary not found: $BIN  (build first: cmake --preset default && cmake --build build)" >&2
  exit 1
fi

pids=()
cleanup() { kill "${pids[@]}" 2>/dev/null || true; }
trap cleanup EXIT INT TERM

ATLAS_NODE_ID=node1 ATLAS_LISTEN=127.0.0.1:50051 ATLAS_PEERS=127.0.0.1:50052,127.0.0.1:50053 "$BIN" & pids+=($!)
ATLAS_NODE_ID=node2 ATLAS_LISTEN=127.0.0.1:50052 ATLAS_PEERS=127.0.0.1:50051,127.0.0.1:50053 "$BIN" & pids+=($!)
ATLAS_NODE_ID=node3 ATLAS_LISTEN=127.0.0.1:50053 ATLAS_PEERS=127.0.0.1:50051,127.0.0.1:50052 "$BIN" & pids+=($!)

wait
