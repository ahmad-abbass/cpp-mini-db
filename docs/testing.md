# Testing and failure investigation

The tests exercise observable behavior through the library API, command executables, real TCP sockets, and an independent Python decoder. Checks remain active in Release builds. CTest groups several named cases into suites; a suite count is not a count of individual assertions or fuzz inputs.

## Test layers

| Suite | Coverage |
| --- | --- |
| `key_value_store` | 16 original API cases, including 5,000 reference-model operations |
| `basic_usage` | Original in-memory example executable |
| `persistent_database` | 16 named storage cases, described below |
| `command_line` | 8 real CLI cases, syntax, all byte values, batches, reopen, and limits |
| `wal_inspector` | 6 cross-language checks using Python struct/zlib |
| `tcp_server` | 13 real-process network/client cases |
| `embedded_benchmark_smoke` | Small verified storage benchmark |
| `network_benchmark_smoke` | Small concurrent, verified network benchmark |
| `complete_demo` | Temporary server, batch, compaction, restart, and independent inspection |

The demo suite is included when the Linux server and Python integration tools are available. Benchmark suites also require `MINIDB_BUILD_BENCHMARKS=ON`.

## Storage failure tests

- Round-trip binary and empty strings through a real log and reopen the database.
- Verify ordered batches, duplicate keys, CLEAR, no-op batches, and invalid-batch atomicity.
- Try a second owner before and after compaction, and verify the lock is released on close.
- Cut a multi-operation final batch at every byte boundary; only the earlier committed state may recover.
- Corrupt a payload and ensure opening fails without rewriting the evidence.
- Check CRC against the standard golden value and reject malformed operation structure.
- Reclaim obsolete records, reopen compacted state, and append after compaction.
- Compare 2,000 deterministic batches against a reference map while repeatedly reopening and compacting.
- Run concurrent writers while a reader checks that a two-key SCAN never sees half a batch.
- Compact live data larger than one frame and enforce the raw scan byte budget.
- Force a genuine partial write using `RLIMIT_FSIZE` in a child process. Verify the handle is poisoned and reopen discards only the incomplete tail.
- Force compaction to run out of allowed file size before rename. Verify the original log remains readable and appendable, and the temporary file is removed.
- Feed 20,000 deterministic malformed payloads to the strict decoder.
- Corrupt a frame length and ensure v2's header CRC prevents accidental tail truncation.
- Open and append a v1 log, compact it into v2, append again, and reopen.

The child-process tests use `_exit` after their checks to avoid running inherited cleanup twice. Their purpose is fault behavior, not child-process leak certification.

## Network tests

The server starts on an ephemeral loopback port in a temporary directory. Tests use actual sockets and the public Python client. They cover binary and Arabic UTF-8 data, batch contexts and exceptions, caught-error regressions, fragmented/pipelined requests, eight concurrent clients, SIGKILL/restart, disconnect, shutdown with idle/queued clients, timeout, second-process ownership, oversized requests, queue saturation, and oversized JSON responses.

The SIGKILL case waits for a successful write response before killing the server. It verifies that acknowledged data reopens and pending batch data does not appear. This is a process-crash test; the OS and device remain running.

## Sanitizers and CI

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DMINIDB_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel 2
ctest --test-dir build-sanitize --output-on-failure
```

CI uses GCC Release, GCC Debug with AddressSanitizer/UndefinedBehaviorSanitizer, and Clang Debug with the same sanitizers. Hosted sanitizer jobs enable leak detection. Local execution environments that restrict leak scanning may require `ASAN_OPTIONS=detect_leaks=0`; this does not replace hosted leak checks.

## Coverage-guided fuzzing

```bash
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug -DMINIDB_BUILD_FUZZER=ON
cmake --build build-fuzz --target wal_fuzzer --parallel 2
./build-fuzz/wal_fuzzer -max_total_time=60 -max_len=4096
```

The harness exercises the batch payload decoder. Successful decodes must re-encode to the same canonical payload; malformed data must raise `CorruptionError`. The Clang CI job runs a bounded ten-second fuzz session under sanitizers. See [LLVM's libFuzzer documentation](https://llvm.org/docs/LibFuzzer.html).

Fuzzing does not currently cover the entire filesystem recovery path, network transport, or syscall schedule. No exhaustive power-loss model, ThreadSanitizer result, Windows storage implementation, or distributed-system correctness claim is made.
