# Learning guide

The goal is to understand the implementation well enough to change it, diagnose a failure, and explain its tradeoffs. Reading a repository or owning its commits alone does not establish independent authorship or mastery of every component.

## Read in this order

1. **Original memory store.** Read `key_value_store.hpp`, `.cpp`, and its tests. Explain ownership, copied return values, absence versus an empty string, and hash-table complexity.
2. **Persistent API.** Read `database.hpp`. Identify which operations mutate state, which return copies, what a batch means, and why the database is not copyable or movable.
3. **Record encoding.** Read `wal.cpp` beside the file-format reference. Manually encode one small SET. Check little-endian integers and calculate its CRC with Python's zlib.
4. **Commit path.** Follow `Impl::commit`, `prepare`, and `apply`. Explain the order of validation, allocation, append, sync, and memory publication.
5. **Recovery and compaction.** Follow `recover` and `compact`. Relate each syscall to a possible crash point. Explain why the lock file is separate from the replaced log.
6. **Command sessions.** Trace BEGIN, SET, invalid input, COMMIT, and disconnect. Explain why reads are not allowed in a pending batch.
7. **TCP lifecycle.** Follow accept, queue, worker, command parsing, response, timeout, and shutdown. Describe the cost of one idle connection occupying a worker.
8. **Independent evidence.** Run the WAL inspector, fault tests, client tests, and benchmarks. Distinguish what each test proves from what it does not establish.

## Explain these scenarios aloud

- An empty value is stored under a missing-looking key. How does GET distinguish the two cases?
- SET succeeds on disk but the TCP response is lost. Is retry always safe? What changes for a batch containing CLEAR or future non-idempotent operations?
- A writer stores two related keys. Why can one SCAN see them consistently while two GET calls can observe different commits?
- The process dies halfway through a frame. Which bytes does recovery retain, and why are partial operations not visible?
- A committed record's declared length is damaged. Why does v2 checksum the header separately?
- Compaction fails before rename, after rename, or during directory synchronization. Which state is still known?
- Why does `fsync(data.log)` not replace synchronizing its directory after file replacement?
- What tradeoff led to an ordered map for the persistent index and a hash map for the original in-memory store?
- How does preparing map nodes before append avoid an allocation failure after the log commit?
- Why does the Python client refuse further writes after an error aborts its batch context?
- How would the worker-pool design behave with thousands of idle clients?
- What do the SIGKILL and file-size-limit tests leave untested about real power failure?

## Small changes to make and test

These are exercises, not completed features:

- Add a read-only command that reports the value length without copying the value.
- Add a missing parser regression case and verify it fails before fixing the parser.
- Extend the inspector with a summary-only mode for very long logs.
- Design an exclusive-after-key scan cursor, including empty and binary key cases.
- Design offline export/import with a versioned format and atomic import boundaries.
- Compare index designs with one controlled workload and clearly labeled durability settings.

For a larger feature, first write observable behavior and failure cases, create a branch, implement it, run the relevant tests, and open a pull request. Explain the design yourself before adding it to a CV. Describe team and AI contributions accurately; do not imply independent implementation or external review that did not occur.
