# setTimeout lifecycle regression

Run on Linux with Qt 6 development packages and GCC or Clang:

```sh
cmake -S script/tests/set_timeout -B build-set-timeout
cmake --build build-set-timeout
ctest --test-dir build-set-timeout --output-on-failure
```

For a nonstandard Qt installation, use its `CMAKE_PREFIX_PATH` and library environment.
This standalone target compiles the real `src/global/Utils.cpp`; it does not copy or
mock `setTimeout`. It uses a `QCoreApplication` only and never initializes Throne,
accesses user configuration, starts the core/VPN, or contacts any service.

The regression observes timer events with `QCoreApplication::notify` and keeps
`QPointer`s to their receivers. After expiry and deferred deletion, those pointers
must be null. This detects the original orphaned-timer bug even though both the
original and fixed code correctly suppress the canceled callback. No private Qt
API or leak sanitizer is needed.

Coverage includes asynchronous zero delay, all production delays (200, 300 and
2000 ms), single execution, coarse timer policy, same-thread and cross-thread
callback affinity, null contexts, context destruction before expiry, repeated
cancellation, and context deletion inside the callback. Lifecycle observation is
limited to the same-thread cases. The timer-lifetime checks fail against the
original implementation.
