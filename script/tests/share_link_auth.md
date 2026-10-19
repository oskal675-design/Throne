# Share-link authentication regression

On Linux, configure and build the application with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run:

```sh
python3 script/tests/share_link_auth.py /path/to/build
```

The runner supports native Ninja and Unix Makefiles builds. It uses the application's compiler, flags, generated files, libraries and object files, replacing only `src/main.cpp` with the harness. Keep the Qt library environment used for the build; a nonstandard Qt installation may require `LD_LIBRARY_PATH`.

The harness calls the actual HTTP and SOCKS parsers/exporters in a `QCoreApplication`. It covers username-only links, explicit empty and nonempty passwords, Base64-looking literal usernames, legacy Base64 credentials (including empty fields, padding, percent escaping and UTF-8), credential delimiters, and export/import round trips for SOCKS, SOCKS4, SOCKS4a, SOCKS5, HTTP and HTTPS.

No normal application startup, user configuration, database, proxy core, TUN/system proxy, widgets or network access is used. All credentials and endpoints are synthetic fixtures. The runner writes `share-link-auth-tests/regression.log` in the build directory.
