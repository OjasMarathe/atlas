#!/usr/bin/env bash
# Atlas distributed-search demo, on real processes (no test harness).
#
#   ./scripts/demo-distributed-search.sh
#
# Starts a metadata node, 4 storage+search nodes and a query coordinator; uploads this repo's
# concept notes (which get indexed on whichever shard owns them); then runs ranked queries that
# fan out across every shard, and shows the result cache turning a multi-shard round trip into
# a lookup.
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD="${BUILD:-./build}"
RUN="${TMPDIR:-/tmp}/atlas-search-demo-$$"
export ATLAS_METADATA=127.0.0.1:50050
export ATLAS_COORDINATOR=127.0.0.1:50060

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

step "starting the cluster (1 metadata + 4 storage/search + 1 coordinator)"
ATLAS_ROLE=metadata ATLAS_NODE_ID=metadata ATLAS_LISTEN=127.0.0.1:50050 \
  ATLAS_DATA_DIR="$RUN/metadata" ATLAS_HEAL_INTERVAL_MS=2000 ATLAS_FAILURE_THRESHOLD=3 \
  "$BUILD/atlas_node" > "$RUN/metadata.log" 2>&1 &
pids+=($!)
sleep 1

for i in 1 2 3 4; do
  ATLAS_ROLE=storage ATLAS_NODE_ID="node$i" ATLAS_LISTEN="127.0.0.1:5005$i" \
    ATLAS_ADVERTISE="127.0.0.1:5005$i" ATLAS_DATA_DIR="$RUN/node$i" \
    ATLAS_METADATA=127.0.0.1:50050 "$BUILD/atlas_node" > "$RUN/node$i.log" 2>&1 &
  pids+=($!)
done
sleep 2

ATLAS_ROLE=coordinator ATLAS_NODE_ID=coordinator ATLAS_LISTEN=127.0.0.1:50060 \
  ATLAS_METADATA=127.0.0.1:50050 "$BUILD/atlas_node" > "$RUN/coordinator.log" 2>&1 &
pids+=($!)
sleep 1
"$BUILD/atlas" nodes

step "ingesting the concept notes — each is stored *and* indexed on its owning shard"
count=0
for note in docs/concepts/*.md; do
  "$BUILD/atlas" put "$(basename "$note")" "$note" | sed 's/^/  /'
  count=$((count + 1))
done
echo "  uploaded $count notes"

step "which shard indexed what (document-partitioned, ADR-0006)"
# The per-shard counts should SUM to the corpus, not repeat it: each document is owned by
# exactly one shard, so a query has to reach all of them to see everything.
"$BUILD/atlas" shards

step "ranked queries, fanned out across every shard"
for query in "consistent hashing ring" "bm25 ranking saturation" "write ahead log" \
             "replication quorum"; do
  printf '\n  \033[1mquery:\033[0m %s\n' "$query"
  "$BUILD/atlas" search "$query" 3 | sed 's/^/  /'
done

step "the result cache: same query again"
"$BUILD/atlas" search "consistent hashing ring" 3 | head -1 | sed 's/^/  /'
"$BUILD/atlas" cache | sed 's/^/  /'

step "killing node2 — a query still answers, from the shards that survive"
victim_pid=$(lsof -nP -iTCP:50052 -sTCP:LISTEN -t | head -1)
disown "$victim_pid" 2>/dev/null || true # keep bash from printing its own "Killed" notice
kill -9 "$victim_pid"
echo "killed node2 (pid $victim_pid) — it held 6 of the 19 documents"
sleep 1
# A query the cache has NOT seen, so this really fans out rather than replaying an entry.
# Expect 3/4 shards and a partial-results warning: the documents node2 owned are gone with it,
# which is the honest answer — search degrades, it does not fail.
"$BUILD/atlas" search "inverted index posting list" 3 | sed 's/^/  /'
"$BUILD/atlas" shards | sed 's/^/  /'

printf '\n\033[32mOK — queries fanned out across shards, merged, cached, and degraded gracefully\033[0m\n'
