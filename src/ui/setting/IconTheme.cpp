#include "include/ui/setting/IconTheme.hpp"

#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QLocale>
#include <QSet>
#include <QSettings>

#include <algorithm>

namespace {
    bool toolbarThemeAvailable = true;

    // QSettings interprets commas in INI values as string lists, including names.
    QString displayText(const QVariant &value) {
        return (value.metaType().id() == QMetaType::QStringList ? value.toStringList().join(QLatin1Char(','))
                                                              : value.toString()).simplified();
    }

    bool validDirectory(const QString &path) {
        if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains(QLatin1Char('\\'))) return false;
        for (const auto &part : path.split(QLatin1Char('/')))
            if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) return false;
        return true;
    }

    QString themeName(QSettings &index) {
        QStringList locales;
        for (auto language : QLocale().uiLanguages()) {
            language.replace(QLatin1Char('-'), QLatin1Char('_'));
            locales << language;
        }
        // Keep Qt's most-specific-first ordering: pt must not precede pt_BR.
        const auto preferred = locales;
        for (const auto &language : preferred) locales << language.section(QLatin1Char('_'), 0, 0);
        locales.removeDuplicates();
        for (const auto &locale : locales) {
            const auto name = displayText(index.value(QStringLiteral("Icon Theme/Name[%1]").arg(locale)));
            if (!name.isEmpty()) return name;
        }
        return displayText(index.value(QStringLiteral("Icon Theme/Name")));
    }

    bool validTheme(QSettings &index) {
        // A theme needs a base name and at least one correctly described icon directory.
        if (displayText(index.value(QStringLiteral("Icon Theme/Name"))).isEmpty()) return false;
        auto directories = index.value(QStringLiteral("Icon Theme/Directories")).toStringList();
        directories += index.value(QStringLiteral("Icon Theme/ScaledDirectories")).toStringList();
        bool found = false;
        for (const auto &directory : directories) {
            bool sizeOk = false, scaleOk = false;
            const int size = index.value(directory + QStringLiteral("/Size")).toInt(&sizeOk);
            const int scale = index.value(directory + QStringLiteral("/Scale"), 1).toInt(&scaleOk);
            const auto type = index.value(directory + QStringLiteral("/Type"), QStringLiteral("Threshold")).toString();
            if (!validDirectory(directory) || !sizeOk || size <= 0 || !scaleOk || scale <= 0 ||
                (type != QLatin1String("Fixed") && type != QLatin1String("Scalable") && type != QLatin1String("Threshold")))
                return false;
            found = true;
        }
        return found && index.status() == QSettings::NoError;
    }
}

namespace IconTheme {
    QList<Entry> Installed() {
        QList<Entry> themes;
        QSet<QString> seen;
        for (const auto &path : QIcon::themeSearchPaths()) {
            const QDir root(path);
            for (const auto &id : root.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name)) {
                const auto file = root.filePath(id + QStringLiteral("/index.theme"));
                if (seen.contains(id) || !QFileInfo(file).isFile()) continue;
                // A hidden/invalid first definition must not expose a lower-priority one.
                seen.insert(id);
                QSettings index(file, QSettings::IniFormat);
                if (index.value(QStringLiteral("Icon Theme/Hidden"), false).toBool() || !validTheme(index)) continue;
                themes.append({id, themeName(index)});
            }
        }
        std::sort(themes.begin(), themes.end(), [](const Entry &a, const Entry &b) {
            const int order = QString::localeAwareCompare(a.name, b.name);
            return order == 0 ? a.id < b.id : order < 0;
        });
        return themes;
    }

    void ApplyStartup(bool enabled, const QString &id) {
        toolbarThemeAvailable = true;
        if (!enabled || id.isEmpty()) return;
        const auto themes = Installed();
        const auto found = std::find_if(themes.cbegin(), themes.cend(), [&](const Entry &theme) { return theme.id == id; });
        toolbarThemeAvailable = found != themes.cend();
        if (toolbarThemeAvailable) QIcon::setThemeName(id);
    }

    bool ToolbarThemeAvailable() {
        return toolbarThemeAvailable;
    }
}
