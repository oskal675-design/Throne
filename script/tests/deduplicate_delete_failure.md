# Remove Duplicates deletion-failure regression

Build the Linux application with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run:

```sh
python3 script/tests/deduplicate_delete_failure.py /path/to/build
```

Keep the Qt library environment used for your build. The runner supports native Ninja and Unix Makefiles builds. It reuses the application's compiler flags, generated headers, libraries and actual production objects, replacing only `src/main.cpp` with the harness.

The suite invokes the actual MainWindow Remove Duplicates action with synthetic profiles and disposable SQLite/configuration state. It runs Qt offscreen and constructs `EngineHost` without calling `init`. No core, VPN, TUN, system proxy or network fixture is started, and no user configuration is loaded.

Coverage:

- Failed first-statement deletion preserves database rows, cached and persisted group order, visible rows and cached profile objects; a healthy retry removes the duplicate.
- Actual SQLite write contention from a second connection preserves retryability. The fixture temporarily shortens its database busy timeout to keep the test fast.
- Failure in the second SQL deletion chunk rolls back the first chunk, preserves group membership and profile caches, and permits a successful retry across both chunks.
- Independent group-cache reload verifies persisted membership after failure and success.
- Repeated slots containing the same profile ID collapse without issuing a SQL deletion.
- Mixed repeated slots and identical profiles with distinct IDs retain the first listed profile and unrelated configurations.
- Declining/accepting confirmation and a repeated no-op action retain their existing behavior.

This regression covers the caller's handling of `BatchDeleteProfiles` failure. It does not make group saves transactional with profile deletion, change duplicate-comparison criteria, repair previously hidden rows, or exercise stopping a real running connection.

Logs are written under `deduplicate-delete-failure-tests` in the build directory. Temporary database directories are removed after each process exits.
