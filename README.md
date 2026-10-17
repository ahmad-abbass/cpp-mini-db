# C++ Mini DB

[![C++ CI](https://github.com/ahmad-abbass/cpp-mini-db/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/ahmad-abbass/cpp-mini-db/actions/workflows/ci.yml)

A C++17 key-value storage system with a persistent write-ahead log, atomic write batches, ordered prefix scans, a concurrent TCP service, and a dependency-free Python client. Built as a systems programming learning project to explore what happens between a database API call and the bytes on disk.

The full system targets **Linux**, including a Linux environment in WSL. The original portable in-memory `KeyValueStore` remains available. This is an educational single-process engine with an index that fits in memory, not a production database.

## What works

- Persistent string keys and values, including empty strings and arbitrary bytes.
- Atomic ordered write batches with last-write-wins behavior within a batch.
- WAL v2 with separate header and payload CRC-32 checksums; v1 compatibility and upgrade through compaction.
- Recovery of incomplete final records, with complete corrupted records rejected instead of silently removed.
- `fsync` before acknowledging writes by default; explicit buffered mode and `SYNC` barriers.
- Exclusive ownership of a database directory through a separate lock file.
- Concurrent reads, serialized writes, consistent bounded prefix-scan results, and compaction by synchronized file replacement.
- Scriptable CLI and a loopback TCP server with a fixed worker pool, bounded queue, request limits, and timeouts.
- Python client with binary data, UTF-8 string inputs, and a write-batch context manager.
- Independent read-only WAL inspection, verified benchmarks, fault tests, and sanitizer/fuzzer CI.

## Build and test

Requirements: CMake 3.20+, a C++17 compiler, Linux development tools, and Python 3.9+ for integration tests, the client, and tools. No third-party C++ libraries or Python packages are required. GCC and Clang are covered in CI.

```bash
git clone https://github.com/ahmad-abbass/cpp-mini-db.git
cd cpp-mini-db
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

For only the library and executables, use `-DBUILD_TESTING=OFF` to avoid the test-time Python requirement. Disable the optional benchmark executable with `-DMINIDB_BUILD_BENCHMARKS=OFF`.

## Run the complete demo

```bash
python3 tools/demo.py --server build/minidb_server
```

The demo owns a temporary database and server. It commits a batch, reads UTF-8 data, compacts the log, restarts the server, checks recovered values, and validates the stopped log through the independent inspector. It cleans up its temporary files and uses an automatically selected loopback port.

## Use the CLI

```bash
./build/minidb_cli ./data
```

Enter one command per line; each response is one JSON object:

```text
SET language "C++17"
GET language
BEGIN
SET "user:1" "Ahmad"
SET "user:2" "Maya"
COMMIT
SCAN "user:" 10
STATS
COMPACT
QUIT
```

`BEGIN` buffers writes privately. `COMMIT` writes them as one atomic batch. Reads inside a batch are rejected and abort that batch. This interface does not provide SQL transactions or a read-your-writes snapshot. An error, disconnect, or `QUIT` discards queued writes. See the [protocol reference](docs/protocol.md) for escaping, responses, and limits.

The CLI also accepts piped input and returns a nonzero exit code if a command fails. To reopen the same directory, first stop the process that owns it.

## Use the TCP service and Python client

Start the service in one terminal:

```bash
./build/minidb_server ./data --port 7070 --workers 4
```

Use another terminal:

```bash
PYTHONPATH=clients/python python3 - <<'PY'
from minidb_client import Client

with Client(port=7070) as db:
    with db.batch():
        db.set("language", "C++17")
        db.set("greeting", "مرحبا")
    print(db.get("language").decode("utf-8"))
    print(db.get("greeting").decode("utf-8"))
    print(db.scan("lang"))
PY
```

The server binds to `127.0.0.1`. It has no authentication or TLS and is intended for local experiments. A client connection belongs to one thread; use separate connections across threads. Values returned by the Python client are `bytes`, and a missing key returns `None`.

## Embed the C++ API

```cpp
#include "minidb/database.hpp"

minidb::Database db("./data");
db.write({minidb::Mutation::set("language", "C++17"),
          minidb::Mutation::set("mode", "persistent")});
auto value = db.get("language"); // std::optional<std::string>, owning copy
auto rows = db.scan("lang", 10); // ordered, consistent copy
db.compact();
```

Link against the CMake target `minidb::minidb`. `Database` is non-copyable and non-movable because it owns file locks and synchronization. The original `minidb::KeyValueStore` is a separate in-memory type using `std::unordered_map` and the Rule of Zero.

## Inspect a stopped log

```bash
python3 tools/inspect_wal.py ./data/data.log
```

The inspector reports format version, frame offsets, operation counts, checksums, and incomplete tails without changing the file. Keys and values are hidden unless `--show-values` is supplied, which emits base64. Inspect a stopped database or a consistent copy, not a file changing under a live writer.

## Measure and investigate

```bash
./build/minidb_bench --operations 2000 --value-bytes 128
./build/minidb_bench --operations 500 --value-bytes 128 --durable
python3 benchmarks/network_benchmark.py --server build/minidb_server --clients 4
```

Both benchmarks use temporary databases and verify their results. They report latency percentiles and throughput, label the durability mode, and avoid touching an existing user database. [Benchmark notes](docs/benchmarks.md) explain the workloads and the limitations of the checked-in sample runs.

## Documentation

| Read | Purpose |
| --- | --- |
| [Architecture](docs/architecture.md) | Components, write ordering, concurrency, and design tradeoffs |
| [File format](docs/file-format.md) | Byte layout, recovery policy, compatibility, and compaction |
| [Protocol](docs/protocol.md) | Commands, escaping, errors, batches, client behavior, and limits |
| [Testing](docs/testing.md) | Fault injection, reference models, cross-language checks, sanitizers, and fuzzing |
| [Benchmarks](docs/benchmarks.md) | Reproducible commands, measurements, and interpretation |
| [Learning guide](docs/learning-guide.md) | Source reading order, exercises, and interview discussion questions |
| [Contributing](CONTRIBUTING.md) | Feature branches, validation, and pull request expectations |
| [Changelog](CHANGELOG.md) | Implemented milestones |

## Limits and next work

All live data is held in memory. Startup replays the log; compaction blocks writes and scans while it replaces the log. Only one process owns a directory. There is no SQL, replication, multi-process shared access, online backup, paginated scan cursor, TTL, or multi-version transaction isolation.

The test suite covers process interruption, byte truncation, corruption, concurrency, and selected I/O failures. It does not certify behavior under every filesystem, physical power failure, failing device, or network filesystem. Parent directories newly created by `create_directories` are not recursively synchronized; provision a durable database directory before relying on power-loss durability. A write or sync error can have an uncertain commit outcome: the handle is poisoned and must be reopened. See the architecture and format notes before experimenting with failure recovery.

Possible next milestones are paginated scans, offline export/import, broader syscall fault injection, and a separate storage index experiment. These are planned, not implemented features.

## Development

Repository owner: [Ahmad Abbas](https://github.com/ahmad-abbass). This is an independent learning project, separate from university assignment solutions. AI assistance was used for implementation, test design, review, and documentation. The branches and pull requests record actual feature development and validation; they do not represent independent human review.
