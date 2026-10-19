# Deleted profile group-reference regression

Build the Linux application with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run:

```sh
python3 script/tests/deleted_profile_references.py /path/to/build
```

Preserve the Qt library environment used for your build. The runner supports native Ninja and Unix Makefiles builds. It reuses the application's compiler flags, generated headers, libraries and actual production objects, replacing only `src/main.cpp` with the harness.

It runs both a headless repository/service suite and an offscreen GUI suite that selects a synthetic profile and triggers the actual Delete action. Every run gets temporary SQLite/configuration state. The subscription preservation case uses a loopback-only HTTP fixture. The GUI constructs `EngineHost` without calling `init`; no core, VPN, TUN or system proxy is started and no user configuration is loaded.

Coverage:

- Front, landing, both fields, own-group and multiple cross-group references.
- API deletion as a parity control, direct repository deletion and `RemoveUnavailable`.
- Cached and persisted reference cleanup, independent group-cache reload and full config generation for surviving profiles.
- Unrelated valid references and pre-existing missing IDs remain untouched.
- Duplicate/missing requested IDs and preserved deletion-count semantics.
- A simulated persisted running-ID guard: that profile and its references survive, while other requested profiles are removed normally.
- A later-statement SQL delete failure in a 1,501-profile batch: all rows roll back, caches/membership/references remain intact, and a healthy retry clears references across both chunks.
- Actual subscription reconciliation retaining a referenced stable ID while deleting its temporary imported row.
- Group-order gaps and actual GUI Delete behavior.

The reference cleanup follows the existing API deletion behavior and uses the existing group-save path. Group-save failures are not made transactional with profile deletion by this change. The regression does not exercise live connections, authentication, or stopping a real running core.

Logs are written under `deleted-profile-references-tests` in the build directory; temporary database directories are removed after each process exits.
