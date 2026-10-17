# C++ Mini DB

A small key-value database project, built step by step in C++17 to explore data structures, resource ownership, storage, and testing.

## Project status

**Milestone 1: in-memory core.** The store, a runnable API example, automated tests, and a CMake build are implemented. GitHub Actions is configured for GCC and Clang. The interactive command-line interface and file persistence are still planned: data currently lives only in memory and is lost when the process exits.

## Goals

- Design a clear, testable C++ API for storing and retrieving string keys and values.
- Apply RAII and explicit ownership to manage resources safely.
- Learn how an in-memory index and file storage fit together.
- Document design choices, complexity, limitations, and failure behavior.

## Implemented

- A reusable `minidb::KeyValueStore` library with insert/update, lookup, membership, deletion, clear, and size queries.
- Owning string keys and values, including empty strings and embedded null bytes.
- Lookups that return independent copies through `std::optional`, distinguishing a missing key from an empty value.
- 16 named test cases, including 5,000 deterministic operations compared against an independent `std::map` reference model.
- A basic usage executable, CMake/CTest support, and an optional AddressSanitizer + UndefinedBehaviorSanitizer build.
- A CI configuration for GCC Debug with sanitizers, GCC Release, and Clang Debug with sanitizers.

The initial scope is a single-process, single-threaded educational tool. SQL, networking, transactions, concurrent access, encryption, and crash-safe recovery are outside the first release. It must not be used for production or sensitive data.

## Build and run

Requires CMake 3.20+ and a C++17 compiler. There are no third-party C++ dependencies or configure-time downloads.

```bash
git clone https://github.com/ahmad-abbass/cpp-mini-db.git
cd cpp-mini-db
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/minidb_example
```

Example output:

```text
language = C++
updated language = C++17
entries = 1
after erase = NOT FOUND
```

For the individual test-case results, run `./build/minidb_tests`. These commands assume a single-configuration generator on Linux/macOS; with a multi-configuration generator, build and test with `--config Debug` and `-C Debug`, respectively, and use the configuration-specific executable directory. Other platforms are not yet covered by CI.

### Sanitizer build (GCC or Clang)

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DMINIDB_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

Test checks remain active in Release builds; they do not rely on `assert()`.

## API behavior

| Operation | Result |
| --- | --- |
| `set(key, value)` | Inserts or replaces; returns `true` for a new key, `false` for an update. |
| `get(key)` | Returns a copied value, or `std::nullopt` if absent. |
| `contains(key)` | Reports whether the key exists, without inserting it. |
| `erase(key)` | Returns `true` if an entry was removed; missing keys are harmless. |
| `clear()` | Removes all entries; the store can be reused. |
| `size()` / `empty()` | Reports the current entry count or whether it is empty. |

Keys are case-sensitive. The store owns its inputs; changing the caller's strings does not change stored data. Copies of a store have independent contents. It is not thread-safe.

## Planned interactive interface

This example illustrates the intended CLI behavior; these commands are not implemented yet.

```text
> SET language "C++"
OK
> GET language
C++
> EXISTS language
true
> DEL language
OK
> GET language
NOT FOUND
> EXIT
```

Command syntax and persistence behavior will be finalized alongside the implementation and tests.

## Implementation plan

- [x] Define the public store API and expected behavior.
- [x] Implement the in-memory store, tests, and runnable example.
- [x] Add CMake builds and a CI workflow.
- [ ] Add a command parser and interactive CLI.
- [ ] Specify a file format and implement validated save/load.
- [ ] Add persistence and CLI integration tests.
- [ ] Measure performance and document reproducible results.

## Design direction

The index uses `std::unordered_map<std::string, std::string>`. Lookup, membership, insert/update, and deletion have average constant-time container costs, but worst-case costs can be linear in the entry count. Hashing and comparing keys takes work proportional to the relevant key lengths; copying values through `get()` or input strings through `set()` adds string-copy costs. Insertions can also trigger rehashing. `size()` and `empty()` are constant-time; `clear()` is linear in the number of entries. Stored data occupies space proportional to the entries and their string contents.

The implementation follows the Rule of Zero: standard containers own all resources, so custom destructors and manual allocation are unnecessary. Returning a copy from `get()` keeps the result independent of later updates, deletion, or destruction of the store. A custom index may be explored later as a separate, tested extension.

The future storage layer will remain separate from the in-memory store so that serialization and parsing can be tested independently. No performance or durability guarantees are claimed at this stage.

## Layout

- `include/minidb/`: public library interface.
- `src/`: implementation.
- `examples/`: runnable API usage.
- `tests/`: dependency-free tests and reference-model checks.
- `.github/workflows/`: continuous integration.

## Author

[Ahmad Abbas](https://github.com/ahmad-abbass)

This is an independent learning project, not a published university assignment solution. Development milestones and completed work will be recorded as the implementation progresses.
