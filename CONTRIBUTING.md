# Contributing

Use a branch for a concrete feature or fix and keep each change reviewable. Suggested names are `feat/prefix-cursor`, `fix/recovery-boundary`, or `docs/format-example`. Commit actual work with accurate timestamps and descriptions. Do not create empty activity or imply reviews that did not happen.

Before opening a pull request:

1. State the observable problem or new behavior.
2. Add meaningful behavior or failure tests for changes to storage, parsing, or concurrency.
3. Run the relevant suites in Release and, for C++ changes, a sanitizer build.
4. Update affected commands, formats, bounds, and compatibility notes.
5. Run `git diff --check` and inspect the complete diff.

PR descriptions should explain what changed, why, the validation actually performed, and any remaining limits. Merge only after the relevant CI jobs pass. Using a merge commit preserves the feature's branch boundary; it is not evidence of independent review.

If a format change affects existing files, specify and test the compatibility or migration policy. If I/O fails after a write could have reached storage, preserve the uncertain-outcome contract rather than claiming rollback. Never repair a corrupt file by silently discarding a complete invalid record.

AI-assisted contributions are welcome when clearly described. The contributor must understand the proposed behavior and avoid unsupported correctness, durability, performance, or authorship claims. Do not upload university assignment solutions to this independent project.
