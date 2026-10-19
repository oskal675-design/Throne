// Reuse fixture/pixel/dialog helpers, never a copy of the production implementation.
#define main toolbarThemeRegressionMain
#include "toolbar_theme_icons.cpp"
#undef main

#include <QComboBox>
#include <QLocale>
#include <QProcess>
#include <QSettings>

#include "include/ui/app/GuiStartup.h"
#include "include/ui/setting/IconTheme.hpp"

namespace {
void setThemeValue(const QString &root, const QString &id, const QString &key, const QVariant &value) {
    QSettings index(root + '/' + id + "/index.theme", QSettings::IniFormat);
    index.setValue(key, value);
    index.sync();
    if (index.status() != QSettings::NoError) std::abort();
}

QString reloadedTheme(const QString &path) {
    Configs::Database database(path.toStdString());
    Configs::SettingsRepo reloaded(database);
    return reloaded.toolbar_icon_theme;
}

QComboBox *chooser(DialogBasicSettings *dialog) {
    auto *combo = dialog->findChild<QComboBox *>("toolbar_icon_theme");
    check(combo && combo->isVisible(), "actual Linux installed-theme chooser is visible");
    if (!combo) finish();
    return combo;
}

int restartCheck(int argc, char **argv) {
    QApplication app(argc, argv);
    const QString database = QString::fromLocal8Bit(argv[2]), root = QString::fromLocal8Bit(argv[3]);
    const QString desktop = QString::fromLocal8Bit(argv[4]), expected = QString::fromLocal8Bit(argv[5]);
    Configs::SetBasePath(QFileInfo(database).absolutePath());
    Configs::initDB(database.toStdString());
    QIcon::setThemeSearchPaths({root});
    QIcon::setFallbackThemeName("empty-fixture");
    QIcon::setThemeName(desktop); // A newly discovered desktop theme, not a cached old override.
    GuiStartup::SetupIconTheme();
    const auto &settings = Configs::dataManager->settingsRepo;
    const auto active = expected.isEmpty() ? desktop : expected;
    check(settings->toolbar_icon_theme == expected && QIcon::themeName() == active,
          "fresh process applies persisted named/System selection");
    check(IconTheme::ToolbarThemeAvailable(), "fresh process restores available theme state");
    const unsigned mask = active == "partial-fixture" ? 0x15 : 0x1f;
    for (size_t i = 0; i < icons.size(); ++i) {
        const QIcon reference = mask & (1u << i) ? fixtureIcon(root, active, i) : QIcon(icons[i].resource);
        check(pixels(QIcon::fromTheme(icons[i].themeName, QIcon(icons[i].resource))) == pixels(reference),
              QStringLiteral("fresh process resolves persisted theme pixels: %1").arg(icons[i].themeName));
    }
    return failures ? 1 : 0;
}
}

int main(int argc, char **argv) {
#ifndef Q_OS_LINUX
    return 77;
#endif
    if (argc == 6 && QByteArray(argv[1]) == "--restart-check") return restartCheck(argc, argv);
    const QString scenario = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("old-settings");
    if (argc > 2 && QByteArray(argv[2]) == "--scale-2") qputenv("QT_SCALE_FACTOR", "2");
    QTemporaryDir isolated(QDir::currentPath() + "/fixture-XXXXXX");
    if (!isolated.isValid()) return 2;
    for (const char *key : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"})
        qputenv(key, isolated.path().toUtf8());
    const auto runtime = isolated.path() + "/runtime";
    QDir().mkpath(runtime);
    QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QLocale::setDefault(QLocale::c());
    QDir::setCurrent(isolated.path());
    Configs::SetBasePath(isolated.path());
    SetDeviceIdStore(isolated.path());
    const auto databasePath = isolated.path() + "/test.db";
    Configs::initDB(databasePath.toStdString());
    auto &settings = Configs::dataManager->settingsRepo;
    check(settings->toolbar_icon_theme.isEmpty() && !settings->use_system_toolbar_icons,
          "fresh settings retain default-off toolbar and system theme selection");
    settings->url_scheme_auto_register = false;
    settings->file_assoc_auto_register = false;
    settings->sub_auto_update = -30;
    settings->route_auto_update = -1440;
    settings->sub_send_hwid = false;
    settings->net_use_proxy = false;
    settings->remember_enable = false;
    settings->remember_tun = false;
    settings->remember_system_proxy = false;
    settings->disable_tray = true;
    settings->kill_switch = false;
    settings->remote_api_enable = false;
    settings->exit_country_lookup = false;
    settings->theme = "System";
    settings->use_custom_icons = true;
    settings->follow_status_in_taskbar = true;
    for (const auto &list : Configs::dataManager->ipListsRepo->GetAllIpLists()) {
        list->auto_update = false;
        Configs::dataManager->ipListsRepo->SaveHeader(list);
    }
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    QDir().mkpath("icons");
    for (size_t i = 0; i < statuses.size(); ++i)
        if (!fixtureImage(31 + int(i)).save(QStringLiteral("icons/%1.png").arg(statusNames[i]))) return 2;

    const QString root = isolated.path() + "/themes", lower = isolated.path() + "/lower";
    const auto desktop = writeTheme(root, "desktop-fixture", 0x1f, 1);
    const auto selected = writeTheme(root, "chosen-directory-id", 0x1f, 11);
    const auto partial = writeTheme(root, "partial-fixture", 0x15, 21);
    const auto empty = writeTheme(root, "empty-fixture", 0, 0);
    const auto hidden = writeTheme(root, "hidden-fixture", 0x1f, 41);
    const auto dotTheme = writeTheme(root, ".dot-theme", 0, 0);
    const auto inherited = writeTheme(root, "inherited-fixture", 0, 0);
    const auto cycle = writeTheme(root, "cycle-a", 0, 0);
    writeTheme(root, "cycle-b", 0, 0);
    setThemeValue(root, inherited, "Icon Theme/Inherits", selected);
    setThemeValue(root, cycle, "Icon Theme/Inherits", "cycle-b");
    setThemeValue(root, "cycle-b", "Icon Theme/Inherits", cycle);
    writeTheme(root, "hicolor", 0, 0);
    setThemeValue(root, selected, "Icon Theme/Name", "Friendly Name");
    setThemeValue(root, selected, "Icon Theme/Name[fr]", "Nom localise");
    setThemeValue(root, selected, "Icon Theme/Name[pt]", "Language");
    setThemeValue(root, selected, "Icon Theme/Name[pt_BR]", "Brazil");
    setThemeValue(root, hidden, "Icon Theme/Hidden", true);
    writeTheme(lower, hidden, 0x1f, 51);
    // Exercise metadata precedence without depending on Qt's cross-root PNG lookup order.
    writeTheme(lower, selected, 0, 61);
    setThemeValue(lower, selected, "Icon Theme/Name", "Wrong lower priority name");
    const auto duplicate = writeTheme(root, "another-directory-id", 0x1f, 71);
    setThemeValue(root, duplicate, "Icon Theme/Name", "Friendly Name");
    const auto invalid = writeTheme(root, "invalid-size", 0x1f, 81);
    setThemeValue(root, invalid, "32x32/actions/Size", 0);
    writeTheme(lower, invalid, 0x1f, 91);
    const auto invalidPath = writeTheme(root, "invalid-path", 0, 0);
    setThemeValue(root, invalidPath, "Icon Theme/Directories", "../outside");
    setThemeValue(root, invalidPath, "../outside/Size", 32);
    const auto unnamed = writeTheme(root, "unnamed", 0, 0);
    setThemeValue(root, unnamed, "Icon Theme/Name", "");
    const auto malformed = writeTheme(root, "malformed-index", 0, 0);
    QFile brokenIndex(root + '/' + malformed + "/index.theme");
    if (!brokenIndex.open(QIODevice::Append)) return 2;
    brokenIndex.write("\n[unterminated\n");
    brokenIndex.close();
    QDir().mkpath(root + "/missing-index");
    const auto lowerOnly = writeTheme(lower, "missing-index", 0, 0);
    QDir().mkpath(root + "/not-a-theme");
    QIcon::setThemeSearchPaths({root, lower});
    QIcon::setFallbackThemeName(empty);
    QIcon::setThemeName(desktop);

    const auto entries = IconTheme::Installed();
    auto find = [&](const QString &id) {
        return std::find_if(entries.cbegin(), entries.cend(), [&](const auto &entry) { return entry.id == id; });
    };
    check(find(selected) != entries.cend() && find(selected)->name == "Friendly Name",
          "first search root supplies metadata and stable directory id");
    check(find(hidden) == entries.cend() && find(invalid) == entries.cend(),
          "hidden and invalid first definitions mask lower-priority themes");
    check(find(invalidPath) == entries.cend() && find(unnamed) == entries.cend() &&
          find(malformed) == entries.cend() && find("not-a-theme") == entries.cend(),
          "malformed directories, missing names and absent indexes are omitted");
    check(find(lowerOnly) != entries.cend(), "directory without index does not mask a lower valid index");
    check(find(dotTheme) != entries.cend(), "dot-prefixed directory is selectable unless metadata marks it Hidden");
    check(find(duplicate) != entries.cend() && find(duplicate)->name == find(selected)->name,
          "same display name keeps distinct selectable directory ids");
    QLocale::setDefault(QLocale("fr_FR"));
    const auto localized = IconTheme::Installed();
    check(std::any_of(localized.cbegin(), localized.cend(), [&](const auto &entry) {
        return entry.id == selected && entry.name == "Nom localise";
    }), "installed-theme label uses localized metadata");
    QLocale::setDefault(QLocale("pt_BR"));
    const auto regional = IconTheme::Installed();
    check(std::any_of(regional.cbegin(), regional.cend(), [&](const auto &entry) {
        return entry.id == selected && entry.name == "Brazil";
    }), "regional display name wins over language-only fallback");
    QLocale::setDefault(QLocale::c());
    check(QIcon::themeName() == desktop, "enumeration never changes the global theme");

    const bool enabled = scenario != "old-settings" && scenario != "disabled";
    const QString savedId = scenario == "selected" || scenario == "disabled" ? selected :
                            scenario == "partial" ? partial : scenario == "hidden" ? hidden :
                            scenario == "inherited" ? inherited : scenario == "cycle" ? cycle :
                            scenario == "missing" ? QStringLiteral("removed-theme") : QString();
    settings->use_system_toolbar_icons = enabled;
    settings->toolbar_icon_theme = savedId;
    settings->Save();
    if (scenario == "old-settings")
        Configs::dataManager->getDatabase().execThrow(
            "DELETE FROM settings WHERE key IN ('use_system_toolbar_icons', 'toolbar_icon_theme')");
    settings = std::make_unique<Configs::SettingsRepo>(Configs::dataManager->getDatabase());
    check(settings->toolbar_icon_theme == savedId && settings->use_system_toolbar_icons == enabled,
          "cold repository load preserves ids and old-settings defaults");
    GuiStartup::SetupIconTheme();
    const bool available = scenario != "hidden" && scenario != "missing";
    const QString active = enabled && available && !savedId.isEmpty() ? savedId : desktop;
    const unsigned mask = !enabled || !available || scenario == "cycle" ? 0 : scenario == "partial" ? 0x15 : 0x1f;
    const QString pixelTheme = scenario == "inherited" ? selected : active;
    check(QIcon::themeName() == active && IconTheme::ToolbarThemeAvailable() == available,
          "real startup applies only available opted-in named themes");
    check(QIcon::themeSearchPaths() == QStringList({root, lower}), "startup preserves Qt theme search paths");

    const RuntimeSnapshot runtimeBefore(*settings);
    API::defaultClient = new API::Client;
    LogThread = new QThread;
    LogThread->start();
    Engine::RuntimeOptions options;
    options.systemProxy = false;
    options.resume = false;
    options.blockingPrivilegeCheck = false;
    options.exitCountryLookup = false;
    auto *host = Engine::EngineHost::create(options); // No init, no network or core.
    auto *window = new MainWindow;
    window->resize(1000, 700);
    window->show();
    pump();
    const auto buttons = findButtons(window);
    expectToolbar(buttons, root, pixelTheme, mask, "cold startup " + scenario);
    auto *nativeTray = window->findChild<QSystemTrayIcon *>();
    check(nativeTray, "actual status tray exists");
    if (!nativeTray) finish();
    const auto trayBefore = pixels(nativeTray->icon()), taskbarBefore = pixels(QApplication::windowIcon());
    check(trayBefore == pixels(QIcon("icons/Off.png")) && taskbarBefore == trayBefore,
          "cold theme override preserves custom tray and taskbar pixels");
    for (const auto &style : {QStringLiteral("BlackSoft"), QStringLiteral("LightBlue")}) {
        themeManager()->ApplyTheme(style, true);
        changeEvent(window, QEvent::StyleChange);
        expectToolbar(buttons, root, pixelTheme, mask, "named theme under " + style);
        check(QIcon::themeName() == active && pixels(nativeTray->icon()) == trayBefore &&
              pixels(QApplication::windowIcon()) == taskbarBefore,
              "light/dark style refresh preserves selected theme and status icons");
    }
    themeManager()->ApplyTheme("System", true);
    int capturedMessages = 0;
    std::vector<DialogMessage> messages;
    SetDialogMessageSink([&](MwMessage command, const QStringList &args) {
        ++capturedMessages;
        messages.push_back({command, args});
    });

    int finished = -1;
    auto dialog = openSettings(window, finished);
    auto *combo = chooser(dialog);
    auto *toggle = dialog->findChild<QCheckBox *>("use_system_toolbar_icons");
    check(combo->currentData().toString() == savedId && combo->isEnabled() == enabled,
          "chooser loads saved id and follows checkbox enablement");
    check(combo->findData(hidden) < 0 || savedId == hidden, "hidden themes cannot be newly selected");
    check(combo->findData(duplicate) >= 0 && combo->findData(selected) >= 0 &&
          combo->itemText(combo->findData(duplicate)) != combo->itemText(combo->findData(selected)),
          "same-name themes have visibly distinguishable labels");
    if (!available) check(combo->currentText().contains("unavailable"), "missing/hidden saved id is shown as unavailable");
    if (scenario == "selected") {
        const auto screenshot = qEnvironmentVariable("THRONE_TEST_ARTIFACT_DIR") +
            QStringLiteral("/chooser-%1x.png").arg(dialog->devicePixelRatioF());
        check(dialog->grab().save(screenshot), "actual named-theme settings screenshot is saved");
    }
    const QString target = savedId == selected ? partial : selected;
    toggle->setChecked(true);
    combo->setCurrentIndex(combo->findData(target));
    check(QIcon::themeName() == active && settings->toolbar_icon_theme == savedId && reloadedTheme(databasePath) == savedId,
          "selection has no preview or persistence side effects before OK");
    expectToolbar(buttons, root, pixelTheme, mask, "pending choice");
    clickDialogButton(dialog, QDialogButtonBox::Cancel);
    check(finished == QDialog::Rejected && capturedMessages == 0 && settings->toolbar_icon_theme == savedId &&
          settings->use_system_toolbar_icons == enabled && reloadedTheme(databasePath) == savedId,
          "Cancel preserves theme id, checkbox, database and message delivery");
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(dialog.isNull(), "cancelled chooser is deleted normally");

    dialog = openSettings(window, finished);
    combo = chooser(dialog);
    check(combo->currentData().toString() == savedId, "reopening after Cancel restores saved selection");
    toggle = dialog->findChild<QCheckBox *>("use_system_toolbar_icons");
    toggle->setChecked(true);
    combo->setCurrentIndex(combo->findData(target));
    clickDialogButton(dialog, QDialogButtonBox::Ok);
    check(finished == QDialog::Accepted && settings->toolbar_icon_theme == target && settings->use_system_toolbar_icons,
          "OK accepts selected directory id and opt-in");
    check(capturedMessages == 1 && messages.back().command == MwMessage::UpdateSettings &&
          messages.back().args == QStringList{MwArg::NeedRestart}, "named-theme change requests only the existing restart notification");
    check(QIcon::themeName() == active, "accepting a named theme never switches the global theme live");
    settings->Save();
    check(reloadedTheme(databasePath) == target && reloadedToolbarValue(databasePath),
          "accepted id survives a fresh database reload");
    check(QProcess::execute(QCoreApplication::applicationFilePath(),
          {"--restart-check", databasePath, root, desktop, target}) == 0,
          "a separate restarted process reads accepted choice and resolves exact pixels");
    changeEvent(window, QEvent::ThemeChange);
    expectToolbar(buttons, root, pixelTheme, available && scenario != "cycle" ? (scenario == "partial" ? 0x15 : 0x1f) : 0,
                  "accepted choice waits for restart");
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(dialog.isNull(), "accepted chooser is deleted normally");

    dialog = openSettings(window, finished);
    combo = chooser(dialog);
    toggle = dialog->findChild<QCheckBox *>("use_system_toolbar_icons");
    toggle->setChecked(false);
    check(!combo->isEnabled() && combo->currentData().toString() == target,
          "bundled mode disables chooser without discarding chosen directory id");
    clickDialogButton(dialog, QDialogButtonBox::Ok);
    check(!settings->use_system_toolbar_icons && settings->toolbar_icon_theme == target &&
          messages.back().args == QStringList{MwArg::NeedRestart} && QIcon::themeName() == active,
          "disabling a named override requests restart without mutating the current global theme");
    changeEvent(window, QEvent::ThemeChange);
    expectToolbar(buttons, root, active, 0, "bundled opt-out takes effect without waiting for restart");
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // Returning to System clears the override, but still does not restore a stale
    // cached desktop theme in this process. The next process rediscovers the desktop.
    dialog = openSettings(window, finished);
    combo = chooser(dialog);
    dialog->findChild<QCheckBox *>("use_system_toolbar_icons")->setChecked(true);
    combo->setCurrentIndex(0);
    clickDialogButton(dialog, QDialogButtonBox::Ok);
    check(settings->toolbar_icon_theme.isEmpty() && QIcon::themeName() == active &&
          messages.back().args.isEmpty(), "System default clears disabled id without another restart notification or live mutation");
    settings->Save();
    check(reloadedTheme(databasePath).isEmpty(), "System selection persists as empty directory id");
    check(QProcess::execute(QCoreApplication::applicationFilePath(),
          {"--restart-check", databasePath, root, selected, QString()}) == 0,
          "a separate process follows a changed desktop theme after System selection");
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    dialog = openSettings(window, finished);
    clickDialogButton(dialog, QDialogButtonBox::Ok);
    check(messages.back().args.isEmpty(), "accepting unchanged System choice requests no restart");
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(pixels(nativeTray->icon()) == trayBefore && pixels(QApplication::windowIcon()) == taskbarBefore &&
          settings->use_custom_icons && settings->follow_status_in_taskbar,
          "chooser acceptance leaves actual custom tray/status policy unchanged");
    check(runtimeBefore.matches(*settings) && host->connectionState() == Engine::ConnectionState::Idle &&
          host->supervisor()->state() == Engine::CoreState::NotStarted && host->supervisor()->corePid() == 0 &&
          !host->runningSnapshot() && !host->runningTun() && !host->testsRunning() &&
          QNetworkProxy::applicationProxy().type() == QNetworkProxy::NoProxy,
          "chooser tests never activate core, VPN, proxy, tests or network settings");
    finish();
}
