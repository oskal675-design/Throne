# Connections column widths regression

Build the native Linux application with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, then run:

```sh
python3 script/tests/connection_column_widths.py /path/to/build
```

The runner reuses the actual application's objects and replaces only its startup
`main`. It constructs the real main window, connection header and models with
synthetic connections and a temporary database. `EngineHost` is not initialized:
no core, VPN, TUN, system proxy or actual inbound listener is started. Changes to
the synthetic inbound-address setting exercise Source-column visibility only.

Coverage:

- Existing automatic defaults and shortened display names/full tooltips.
- Header-menu actions on every visible column and existing Traffic/Speed choices.
- Real mouse dragging of all four middle columns.
- Full Unicode outbound names on process and connection rows in manual mode.
- Width stability across polling, larger traffic values, sorting, filtering,
  expand/collapse, window resizing and horizontal scrolling.
- Filter-field alignment after resizing and scrolling.
- Source hide/show, filter/sort cleanup and retained hidden-column width.
- Debounced settings persistence, named logical widths, recreated main window,
  switching between modes, reset and malformed saved settings.
- Actual double-click sizing of huge outbound names, bounded widths round-trip,
  and consistent bounds when seeding manual sizes from automatic content sizing.

`--probe-only` runs only the initial feature-availability checks, so the identical
probe can demonstrate the missing action on unmodified dev without pretending
that the new feature's behavioral tests ran there. `--skip-build` is available
when the exact build has already been verified.

The offscreen Qt platform is used. Windows/macOS native behavior and packaging
are outside this runner's coverage. New UI strings use `tr()` and fall back to
English until translations are provided.
