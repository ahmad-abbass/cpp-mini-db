# C++ Mini DB

A small persistent key-value database, built step by step in modern C++ to explore data structures, resource ownership, storage, and testing.

## Project status

**Planning stage.** This repository currently documents the project direction. The database, command-line interface, persistence layer, tests, and build configuration are not implemented yet. The items below are planned work, not completed features.

## Goals

- Design a clear, testable C++ API for storing and retrieving string keys and values.
- Apply RAII and explicit ownership to manage resources safely.
- Learn how an in-memory index and file storage fit together.
- Document design choices, complexity, limitations, and failure behavior.

## Planned first release

- An in-memory key-value store with set, get, contains, and erase operations.
- A command-line interface with readable error messages.
- Saving data to a file and loading it on startup.
- Automated tests covering updates, missing keys, empty values, and persistence round trips.
- A CMake build and a GitHub Actions build-and-test workflow.

The initial scope is a single-process, single-threaded educational tool. SQL, networking, transactions, concurrent access, encryption, and crash-safe recovery are outside the first release. It must not be used for production or sensitive data.

## Proposed interface

This example illustrates the intended behavior; it is not runnable yet.

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

1. Define the public store API and expected behavior.
2. Implement the in-memory store and its tests.
3. Add a command parser and interactive CLI.
4. Specify a file format, then implement save/load with validation.
5. Add build automation and continuous integration.
6. Publish reproducible usage instructions and document measured results.

## Design direction

The proposed implementation uses C++17 and the standard library. The first index will use `std::unordered_map`; its average and worst-case operation costs will be documented accurately. A custom index may be explored later as a separate, tested extension.

Storage will be separated from the in-memory store so that serialization and parsing can be tested independently. No performance or durability guarantees are claimed at this stage.

## Building and testing

Build and test commands will be added once the CMake targets and tests exist.

## Author

[Ahmad Abbas](https://github.com/ahmad-abbass)

This is an independent learning project, not a published university assignment solution. Development milestones and completed work will be recorded as the implementation progresses.
