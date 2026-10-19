# Subscription outbound ownership regression

Build the Linux application with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run:

```sh
python3 script/tests/subscription_outbound_owner.py /path/to/build
```

Preserve the Qt library environment used for the build. The runner supports native Ninja and Unix Makefiles builds. It reuses the application's compiler flags, generated headers, libraries and actual production object files, replacing only `src/main.cpp` with the harness. Running the same harness against an unmodified `dev` build reproduces the ownership and credential assertions.

The harness creates a disposable SQLite database and loopback-only HTTP subscription fixture. It imports synthetic OpenConnect and OpenVPN profiles through the production parser and retains their cached entities across refresh, as the GUI and connection manager do. It builds configuration JSON but never starts a core, establishes a VPN connection, or enables TUN/system proxy. It does not load user configurations or real credentials.

Coverage:

- Correct ownership on initial import and independent new-profile insertion.
- Rename-only and credential-changing refreshes that retain existing profile IDs.
- Removal of temporary imported rows after reconciliation.
- Distinct session-only credentials for each VPN profile, set both before and after refresh.
- Credential resolution through the actual outbound builders and `BuildSingBoxConfig`, including the endpoint-to-profile ID mapping.
- Unchanged refreshes without temporary ID allocation.
- Stored subscription credentials and omission of the runtime owner ID from serialization.
- Clearing credential overrides and loading fresh entities from the database.

This is a parser/reconciliation/config-generation regression, not a live VPN authentication test. The runner writes `subscription-outbound-owner-tests/regression.log` in the build directory and removes temporary database directories after execution.
