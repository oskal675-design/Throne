# Installed Linux icon themes

This extends the optional toolbar system-icon support. Basic Settings → Style
keeps the default-off checkbox and adds a chooser for the System default and
installed, non-hidden themes found in Qt's icon theme search paths. Choosing a
named theme also affects Qt standard icons; the chooser tooltip explains this.
Tray, taskbar and connection-status icons retain their separate existing policy.
No icon packs are bundled, installed or downloaded by this feature.

The stored value is a directory id, not a translated name. Empty means follow
the desktop. Named overrides apply at the next GUI startup, before icon/style
construction; changing the selection never previews a global theme. Cancel
does not save it. An unavailable saved id stays visible and recoverable in the
chooser, and the toolbar uses bundled icons until a valid selection is applied
on restart. Per-icon fallback and inherited themes otherwise use Qt's loader.

## Regression tests

Build the native Linux `Throne` target with
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`, using the same compiler/Qt environment:

```sh
python3 script/tests/toolbar_theme_icons.py /path/to/build
python3 script/tests/installed_icon_themes.py /path/to/build
```

Both runners reuse the real application object files. The installed-theme suite
uses the actual MainWindow and Basic Settings widgets, synthetic local PNG
themes, and temporary SQLite/HOME directories. Ten separate scenarios cover:

- Old settings with absent keys; opted-in System; full, partial, inherited,
  cyclic, missing and hidden named themes; disabled opt-in; and 2x scaling
- Root precedence for metadata, hidden/invalid definitions masking lower
  definitions, malformed indexes, duplicate display names, stable ids,
  localized and region-specific labels
- Exact toolbar pixels, bundled fallbacks, light/dark style refreshes,
  Cancel/Accept/reopen and no live theme mutation
- Additional fresh subprocesses reading the accepted SQLite settings and
  verifying named-theme pixels and System following a changed desktop theme
- Unchanged custom tray/taskbar pixels, inactive core/VPN/proxy state,
  restart notifications only for a changed effective named override

Logs and 1x/2x real-dialog screenshots are written below
`installed-icon-themes-tests` in the build directory. The original toolbar suite
retains its separate logs and broader all-status icon checks.

The settings message sink is captured, so no ordinary engine settings side
effects run. `EngineHost` is created without `init`. No service, VPN, system
proxy, external network request or real user settings are used. Qt's offscreen
platform verifies widget rendering; it does not establish live desktop-theme
notification behavior on GNOME/KDE, which this restart-applied chooser does not
promise. The production code uses APIs available in Qt 6.2; the available local
toolchain for validation is Qt 6.8.2. Theme formats unsupported by a packaged
Qt image plugin fall back through Qt and the bundled toolbar icons.
