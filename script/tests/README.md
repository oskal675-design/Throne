# Subscription focus regression

On Linux, configure and build the application with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run:

```sh
python3 script/tests/subscription_focus.py /path/to/build
```

The runner supports native Ninja and Unix Makefiles builds. It uses the same compiler, flags, libraries, generated files and application objects as the build, replacing only `src/main.cpp` with the harness. Keep the Qt library/plugin environment used for your build; for a nonstandard Qt installation this may require `LD_LIBRARY_PATH` and `QT_PLUGIN_PATH`.

The GUI test uses Qt's offscreen platform, a disposable SQLite database and a loopback HTTP server. It drives the real clipboard import action and dialogs and checks actual tab selection, stored selection and profile-table contents. Coverage includes URL and AddSub imports, cancellation, HTTP failure, repeated links, navigation during a slow download, existing-group updates, API group creation, tab reordering and deletion before queued event delivery. It also records existing FIFO behavior: a queued URL creates its group only when the worker reaches it, and that group receives focus then. A separate `QCoreApplication` run verifies the event and import without widgets.

The harness never calls `EngineHost::init`, starts the proxy core, enables TUN/system proxy, or accesses real subscriptions. Background remote-list updates and association registration are disabled in the disposable configuration. Logs and a screenshot are written to the build directory's `subscription-focus-tests` subdirectory.
