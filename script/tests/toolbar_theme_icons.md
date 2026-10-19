# Linux toolbar system icons regression tests

Build the normal native Linux `Throne` target with
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`, then use the same compiler/Qt environment:

```sh
python3 script/tests/toolbar_theme_icons.py /path/to/build
```

The runner links the real application objects, replacing only startup `main`.
It runs separate processes for an absent/default-off preference and a persisted
opt-in preference (246 assertions per process). Both use the actual MainWindow toolbar and Basic Settings
widgets, temporary SQLite/HOME directories, and synthetic local icon themes.
No theme assets are downloaded or installed into the desktop.

Coverage includes exact bundled/themed icon pixels, partially populated and
missing themes, transitions in both directions, theme/style/palette refresh,
Cancel/Accept and cold settings reload, and unchanged tray/taskbar outputs.
The settings message sink is captured for the fixture so no engine settings
side effects run. `EngineHost` is constructed without `init`; no core, VPN,
system proxy, or external network service is started.

Logs are written under `toolbar-theme-icons-tests` in the supplied build.
The test environment uses Qt's offscreen platform. It does not establish live
notification behavior for every KDE/GNOME integration or Windows/macOS build.
The production API uses QString icon names compatible with Qt 6.2; validation
with the available toolchain runs Qt 6.8.2.
