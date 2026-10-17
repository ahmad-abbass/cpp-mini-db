# Command protocol

The CLI reads lines from stdin. The TCP service reads newline-terminated command lines and responds with one ASCII JSON object plus a newline per command. TCP reads can contain partial or multiple commands; the server retains incomplete lines and processes pipelined lines in order. EOF in the middle of a TCP line discards that line. The CLI accepts its final line without a newline.

## Tokens and byte strings

Command names are case-insensitive ASCII. Keys and values are case-sensitive byte strings. Separate unquoted tokens with spaces or tabs. Use double quotes for empty strings or embedded whitespace. A closing quote must be followed by whitespace or the end of the line.

Both quoted and unquoted tokens support these escapes:

| Escape | Byte |
| --- | --- |
| `\\` | Backslash |
| `\"` | Double quote |
| `\n` | Newline |
| `\r` | Carriage return |
| `\t` | Tab |
| `\0` | Zero |
| `\xHH` | One byte encoded as two hex digits |

Unknown or incomplete escapes are errors. Raw newline and zero bytes must be escaped. For a text newline in a value, send `SET note "first\nsecond"` as one physical line.

JSON response strings encode bytes outside printable ASCII as `\u00XX`. These are byte values, not a Unicode transcoding. The Python client converts response strings with Latin-1 to recover the original bytes. It encodes `str` inputs as UTF-8, so decode textual results explicitly with `.decode("utf-8")`. Raw byte inputs round-trip unchanged.

## Commands

| Command | Successful response fields beyond `ok: true` |
| --- | --- |
| `PING` | `value: "PONG"` |
| `SET key value` | None, or `queued: true` inside a batch |
| `GET key` | `value: string` or `value: null` for absence |
| `DEL key` | None, or `queued: true`; deleting absence is harmless |
| `EXISTS key` | Boolean `value` |
| `SIZE` | Integer `value` |
| `SCAN prefix [limit]` | `items: [[key, value], ...]` in bytewise lexicographic key order |
| `CLEAR` | None, or `queued: true` |
| `BEGIN` | None; starts a private write batch |
| `COMMIT` | None; atomically applies the pending batch |
| `ROLLBACK` | None; discards pending writes |
| `SYNC` | None; synchronizes the current log |
| `COMPACT` | None; rewrites live data and reclaims obsolete log history |
| `STATS` | `keys`, `log_bytes`, `recovered_tail_bytes`, `sync_on_write`, `format_version` |
| `QUIT` | None; closes the session and discards pending writes |

SCAN defaults to 100 rows and accepts an integer limit in 0..1000. It is a bounded prefix query, not a paginated export. If the result exceeds the raw or JSON byte limit, the command returns an error rather than a partial result. Each SCAN is consistent; multiple SCAN calls are not a transaction.

## Batches and errors

Only SET, DEL, CLEAR, COMMIT, ROLLBACK, and QUIT are allowed while a batch is active. Queued writes are invisible to every database reader until COMMIT, including the same session. Nested batches are rejected. Operations apply in order; a later SET or DEL overrides an earlier operation on the same key, and CLEAR removes all preceding state.

An invalid command or storage error cancels pending writes. COMMIT and ROLLBACK without an active batch are errors. Disconnect and QUIT discard pending writes. Committing an empty batch succeeds without writing a frame.

Command errors have this shape:

```json
{"ok": false, "code": "invalid_command", "error": "wrong number of arguments"}
```

Other codes are `storage_error`, `busy`, and `request_too_large`. A normal invalid command leaves a TCP connection usable, while `busy` and `request_too_large` close it. Storage errors can leave the shared database handle poisoned; restarting after inspecting the failure is required. The API does not promise that an errored write never committed.

## Resource bounds

| Resource | Bound |
| --- | --- |
| Key or value | 1 MiB each |
| Physical command line | 9 MiB, excluding newline |
| Encoded batch | 16 MiB; at most 100,000 operations |
| SCAN raw copied bytes | 16 MiB |
| Serialized JSON response | 16 MiB, excluding newline |
| Worker threads | Default 4; configurable 1..32 |
| Waiting connections | Four per worker |
| Receive/send timeout | Default 30 seconds; configurable 1..300 |

A timeout applies to a blocking socket operation, not a total connection lifetime. An active client can occupy a worker for a long time. The queue is bounded, but the database's total live memory and log growth are not quota-limited.

## Server options

```text
minidb_server DIRECTORY [--port 7070] [--workers 4] [--idle-timeout 30] [--buffered]
```

Port 0 requests an available ephemeral port. The first stdout line is a readiness JSON object containing the selected port. The server always binds to `127.0.0.1`; no remote bind option is supplied. SIGINT or SIGTERM triggers orderly shutdown. The server has no authentication or TLS.

The CLI accepts `minidb_cli DIRECTORY [--buffered]`. Buffered mode skips per-write synchronization; SYNC, compaction, and clean CLI/server shutdown provide explicit synchronization points. An abrupt crash or power loss before a barrier can lose buffered writes.

## Python client behavior

`Client` supports context management, `ping`, `set`, `get`, `delete`, `scan`, `stats`, `execute`, and `batch`. Use one connection per thread. `get` returns `bytes` or `None`; `scan` returns `(bytes, bytes)` pairs.

The batch context commits on normal exit and rolls back on exceptions. If an error is caught inside the context after the server has aborted the batch, the client blocks further commands except rollback until the context is exited. It does not let subsequent writes silently become standalone commits.

A transport failure closes the connection and leaves the request outcome potentially unknown. Reconnect explicitly and inspect state. Automatic retries, exactly-once delivery, and replay deduplication are not provided.
