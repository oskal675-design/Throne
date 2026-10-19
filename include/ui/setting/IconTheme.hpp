#pragma once

#include <QList>
#include <QString>

namespace IconTheme {
    struct Entry {
        QString id; // Directory name, never the translated display name.
        QString name;
    };

    // Installed, non-hidden themes in Qt's current search paths. The first index.theme wins.
    QList<Entry> Installed();

    // Called only at GUI startup, before any window/style icons are constructed.
    // Empty id follows the desktop. Unavailable saved ids retain bundled toolbar icons.
    void ApplyStartup(bool enabled, const QString &id);
    bool ToolbarThemeAvailable();
}
