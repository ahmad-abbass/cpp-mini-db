# Changelog

## 0.2.0

- Added the persistent Database API, atomic ordered batches, prefix scans, explicit synchronization, directory locking, and compaction.
- Added WAL v2 header and payload checksums, v1 compatibility, incomplete-tail recovery, and format inspection.
- Added command sessions, a scriptable CLI, a bounded Linux TCP service, and a Python client.
- Added verified embedded and network benchmarks with recorded sample runs.
- Added real short-write and compaction-failure tests, process-crash tests, concurrency and protocol regression cases, cross-language validation, and a Clang fuzz harness.
- Added an end-to-end demo and architecture, protocol, testing, benchmark, and learning documentation.

## 0.1.0

- Added the original C++17 in-memory key-value library, usage example, 16 API test cases, 5,000 reference-model operations, CMake/CTest, and GCC/Clang CI.
