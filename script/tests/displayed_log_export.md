# Displayed log export regression

Build the native Linux application with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run:

```sh
python3 script/tests/displayed_log_export.py /path/to/build
```

Preserve the Qt library environment used for your build. The runner supports native Ninja and Unix Makefiles builds. It reuses the application's compiler flags, generated headers, libraries and actual production objects, replacing only `src/main.cpp` with the test harness.

The suite invokes the real log context menu, QAction, Qt Save dialog, overwrite confirmation and error message. Logs pass through the production log queue, filter, hold queue and retention logic. Qt's non-native dialogs are used only in the offscreen test so it can select temporary files deterministically.

Coverage:

- Export availability and disabled behavior for an empty log.
- Cancellation without file creation or view changes.
- Exact UTF-8 plain text, multiple lines, Unicode and literal markup.
- Partial text selection does not limit the saved document.
- New logs arriving while the Save dialog is open do not alter its captured snapshot.
- Filters, paused/held arrivals and retained-line limits are respected.
- Historical on-disk log content is not included.
- Ordinary overwrite confirmation and write-failure reporting.
- Failed export preserves the existing file and displayed log. The failure fixture makes only its temporary destination read-only after the dialog has validated it, then restores permissions.

All state is disposable; no real user configuration is loaded. `EngineHost` is constructed without calling `init`. No core, VPN, TUN, system proxy, network fixture or upload is started. This is displayed-document export, not a full-session archive or redaction feature.

The same harness on unmodified dev stops at the expected missing-export-action assertion. Logs are written under `displayed-log-export-tests` in the build directory. Temporary database/export directories are removed after each process exits.
