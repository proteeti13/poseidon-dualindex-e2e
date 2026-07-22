#!/usr/bin/env bash
# End-to-end SNAP benchmark runner for the DualIndex learned index.
#
# Three phases per dataset (smallest first: wiki_vote, roadnet_ca, web_google):
#   1. --gen_queries          generate the 4 persisted workloads (SEED=42).
#   2. --measure_scan_overhead  UNTIMED instrumented pass that measures the true
#      FloodSourceSort scan_overhead -> queries/<ds>_scan_overhead.txt. Runs the
#      SCAN_DEBUG binary (build_scan/) via LD_LIBRARY_PATH so it loads its own
#      instrumented libposeidon_core.so.
#   3. timed benchmark        the clean, counter-free build, with
#      --print_batch_stats and --perf. Reads the sidecar for the scan_overhead
#      column; the timed path never touches the counter.
#
# The per-dataset storage dir dualindex_<ds>/ is wiped before EVERY phase so each
# process rebuilds the graph from the raw edge list with an identical,
# deterministic node-id assignment (identical dataset fingerprint across gen /
# overhead / timed). ONLY that storage dir is ever removed -- never queries/ or
# results/. (Reusing the dir across processes was observed to drift node ids and
# break the fingerprint check, so we always start each phase clean.)
#
# Usage:
#   ./run_all.sh                         # all three datasets, in order
#   ./run_all.sh wiki_vote               # just one (used for the dry-gate)
#   ./run_all.sh roadnet_ca web_google   # a subset
set -euo pipefail

cd "$(dirname "$0")"
ROOT="$(pwd)"

DATA=/home/proteeti/RSMI/datasets
BENCH=build/bench_dualindex               # clean, counter-free (timing)
SCAN_BENCH=build_scan/bench_dualindex     # SCAN_DEBUG instrumented (overhead pass)
SCAN_LD="$ROOT/build_scan"                # instrumented libposeidon_core.so lives here
NQ=100000                                 # queries per workload, all datasets

# dataset_name -> raw SNAP edge list (From<TAB>To, '#' comments)
edge_file() {
  case "$1" in
    wiki_vote)  echo "$DATA/wiki-Vote.txt" ;;
    roadnet_ca) echo "$DATA/roadNet-CA.txt" ;;
    web_google) echo "$DATA/web-Google.txt" ;;
    *) echo "UNKNOWN_DATASET" ; return 1 ;;
  esac
}

mkdir -p logs

for b in "$BENCH" "$SCAN_BENCH"; do
  [ -x "$b" ] || { echo "ERROR: missing binary $b (build it first)"; exit 1; }
done

run_one() {
  local name="$1"
  local file; file="$(edge_file "$name")"
  [ -f "$file" ] || { echo "ERROR: edge list not found: $file"; exit 1; }
  local store="dualindex_${name}"

  echo "==================== $name ($file) ===================="

  echo "[1/3] gen_queries  -> logs/${name}_gen.log"
  rm -rf "$store"
  "$BENCH" --gen_queries --dataset_file "$file" --dataset_name "$name" \
           --num_queries "$NQ" 2>&1 | tee "logs/${name}_gen.log"

  echo "[2/3] measure_scan_overhead (untimed, instrumented) -> logs/${name}_overhead.log"
  rm -rf "$store"
  LD_LIBRARY_PATH="$SCAN_LD" "$SCAN_BENCH" --measure_scan_overhead \
           --dataset_file "$file" --dataset_name "$name" \
           --num_queries "$NQ" 2>&1 | tee "logs/${name}_overhead.log"

  echo "[3/3] timed benchmark -> logs/${name}_bench.log"
  rm -rf "$store"
  "$BENCH" --dataset_file "$file" --dataset_name "$name" \
           --num_queries "$NQ" --print_batch_stats --perf 2>&1 | tee "logs/${name}_bench.log"

  rm -rf "$store"
  echo "==================== $name done ===================="
}

if [ "$#" -gt 0 ]; then
  for d in "$@"; do run_one "$d"; done
else
  run_one wiki_vote
  run_one roadnet_ca
  run_one web_google
fi

echo "run_all.sh: all requested datasets complete."
