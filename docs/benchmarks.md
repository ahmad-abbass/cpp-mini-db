# Benchmarks

The benchmark programs verify values before reporting success. They isolate their temporary files, label durability mode, emit JSON, and include enough workload parameters to reproduce the experiment. The checked-in results are observations from a shared container, not advertised throughput or comparisons with another database.

## Embedded workload

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
./build-release/minidb_bench --operations 2000 --value-bytes 128
./build-release/minidb_bench --operations 500 --value-bytes 128 --durable
```

The default mode is buffered; `--durable` synchronizes every mutation. Each run measures:

1. Inserts into a new database.
2. Random-order GET with seed 240218 and value verification.
3. Updates concentrated on up to 100 hot keys.
4. One hundred prefix scans returning up to 100 rows.
5. Synchronization followed by compaction.
6. Reopen after compaction, followed by verification of every live value.

Each workload emits elapsed time, throughput, and p50/p95/p99 per-call latency. Timing includes the workload lambda and its verification/bookkeeping. Compaction and reopen each have one observation, so their percentiles are the same single duration, not a latency distribution. Log bytes before and after compaction describe space reclaimed for that particular history.

Parameters accept 1..1,000,000 operations and 1..1,048,576 value bytes. Large combinations consume substantial memory and storage because every live value remains in the index. Use small values first. A SCAN workload that exceeds the documented result-byte bound will fail rather than benchmark a partial result.

## Network workload

```bash
python3 benchmarks/network_benchmark.py --server build-release/minidb_server \
  --clients 4 --operations-per-client 250 --value-bytes 128
```

The script starts its own temporary server, with one worker per benchmark client. Each thread creates a connection, writes a distinct key, reads the same key, and verifies its bytes. The reported operation is a **SET/GET pair**, not one database request. Pair latency includes both round trips; wall-clock throughput also includes client setup, scheduling, and connection closure after server readiness. Final key count and an explicit SYNC are checked before cleanup.

This mode is durable by default. Add `--buffered` to measure buffered writes. Do not compare pair throughput directly with embedded single-operation throughput.

## Reading the sample results

Raw results and metadata are under [`benchmarks/results`](../benchmarks/results). `environment.json` records the compiler, kernel, visible CPU model, filesystem type, UTC timestamp, and exact commands for the corresponding runs.

The shared environment has unknown host contention and underlying storage behavior. A single run is not statistically stable. Buffered and durable examples also use different operation counts, so they are separate demonstrations, not a controlled ratio. Cache state, filesystem, value size, key distribution, concurrency, build flags, and synchronization mode all affect the result.

For a comparison, use the same compiler and workload parameters, run repeated trials on a controlled machine, preserve each raw result, and report the distribution. Never present buffered-write numbers as durable-write performance. The code does not contain invented speedup claims or fixed expected timing thresholds.
