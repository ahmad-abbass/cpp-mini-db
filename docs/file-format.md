# File format and recovery

A database directory contains `LOCK` and `data.log`. A compaction in progress may also have a `compact-XXXXXX` temporary file. A process crash can leave an unused temporary file; startup ignores it. The directory must be owned by one cooperating process, with the live index fitting in memory.

## Current format

The eight-byte file header is ASCII `MDBWAL2` followed by a newline. All integer fields below are unsigned 32-bit little-endian values. No native structs or platform padding are written to disk.

Each frame has this layout:

| Relative byte offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | Payload byte length |
| 4 | 4 | CRC-32 of the payload |
| 8 | 4 | CRC-32 of the preceding eight header bytes |
| 12 | payload length | Ordered batch payload |

CRC uses the reflected IEEE CRC-32 polynomial `0xedb88320`, initial state `0xffffffff`, and final XOR `0xffffffff`. The golden vector `123456789` yields `0xcbf43926`. Python's `zlib.crc32` independently validates the same bytes. CRC detects accidental corruption; it is not cryptographic authentication.

The payload begins with the operation count, followed by ordered operations:

| Tag | Operation | Remaining fields |
| --- | --- | --- |
| 1 | SET | Key length, value length, key bytes, value bytes |
| 2 | ERASE | Key length, key bytes |
| 3 | CLEAR | None |

Empty keys and values are allowed. Lengths refer to bytes. Unknown tags, extra bytes, invalid lengths, zero operation counts, or excessive operation counts are rejected. An empty API batch is a no-op and does not create a frame.

## Limits

| Item | Maximum |
| --- | --- |
| One key or value | 1 MiB |
| One encoded payload | 16 MiB |
| Operations in one batch | 100,000 |
| Combined raw bytes returned by one SCAN | 16 MiB |

These bounds apply to the record parser before allocating its payload or field objects. Complete input batches are validated before any log append.

## Recovery policy

Startup validates the file header and then walks frame boundaries in order. For v2, the header CRC is checked before trusting the declared payload length. Each complete payload must also have a valid CRC and valid operation structure before it is applied.

- EOF exactly at a frame boundary is clean.
- A short final frame header or a valid header with a short final payload is considered an interrupted append. Startup truncates that incomplete tail to the last complete frame and synchronizes the truncation.
- A complete invalid header, checksum failure, malformed operation, unsupported file header, or invalid declared length is corruption. Startup throws without rewriting those bytes.
- `STATS` reports the number of discarded tail bytes for the current open operation. Opening the already-repaired file later reports zero.

A committed batch is not split across frames. A truncated final batch therefore does not expose just its first operations. Checksums cannot distinguish all possible damage, and the policy assumes ordinary append interruption rather than an adversarially rewritten file.

## Version 1 compatibility

The first storage milestone used `MDBWAL1\n` and an eight-byte frame prefix containing only payload length and payload CRC. Existing v1 files remain readable and receive v1-formatted appends until compaction. Compaction rewrites the live state with a v2 header and frames, then switches subsequent appends to v2.

Unlike v2, v1 does not checksum its frame header. Damage to a v1 length can be indistinguishable from an incomplete final record. Use compaction to upgrade a valid legacy database. Cross-language tests generate a v1 fixture in Python, open it in C++, compact it, and validate the result independently.

## Compaction sequence

1. Hold the database's unique lock and create a private temporary file in the same directory.
2. Write a v2 file header and the current live SET operations. Split the snapshot into frames of at most 1,000 operations and 16 MiB of payload each.
3. Synchronize the temporary file.
4. Rename it over `data.log`, switch the active descriptor to the replacement, and synchronize the directory.

Before rename, failure removes the temporary file and preserves the old log. After rename, a synchronization failure poisons the handle because the durable directory state is uncertain. The lock remains on `LOCK`, which is never replaced during compaction.

## Inspection

Run `python3 tools/inspect_wal.py DIRECTORY/data.log` against a stopped database or consistent copy. Exit status 0 means clean, 1 means an I/O or validation error, and 2 means an incomplete tail. The tool reports but never repairs the log. `--show-values` adds base64 keys and values; default output contains metadata only.
