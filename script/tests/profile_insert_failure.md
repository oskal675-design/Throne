# Profile insertion failure regression

Build the Linux application with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run:

```sh
python3 script/tests/profile_insert_failure.py /path/to/build
```

The runner supports native Ninja and Unix Makefiles builds. It uses the application's compiler flags, generated headers, libraries and production object files, replacing only `src/main.cpp` with this harness. Preserve the Qt library environment used for your build.

The harness uses `QCoreApplication`, a disposable SQLite database and a loopback-only HTTP subscription fixture. It never calls application bootstrap or `EngineHost::init`, starts a proxy core, enables TUN/system proxy, or reads user configurations.

Coverage:

- First-statement and later-statement insert failures, injected with a temporary SQLite `RAISE(ABORT)` trigger.
- Accurate persisted rows, in-memory IDs, outbound IDs, identity-map entries and group membership after a failed batch.
- Retrying the same batch without duplicating already saved rows.
- A real `SQLITE_FULL` failure using the disposable database's `max_page_count`; the notice must include SQLite error code 13.
- ID-allocation failure before insertion.
- Default subscription refreshes of 1, 220 and 501 profiles, spanning SQL chunks and the updater's 500-profile flush boundary.
- Preservation of prior good profiles and freshness timestamps after failed default refreshes, with accurate partial-addition counts.
- Successful retries and unchanged-refresh ID reuse.
- Explicit clear-before-update semantics: old rows are still cleared first, but failure is reported and the update is not marked fresh.
- Quiet import failure and subsequent recovery.

This checks failure reporting, not whole-refresh atomicity. Completed insert chunks are retained. Clear-before-update and pre-existing group metadata writes are not rolled back, and failures in other repository save paths are outside this regression's scope.

The runner writes its log under `profile-insert-failure-tests` in the build directory and removes its temporary database directories after each run.
