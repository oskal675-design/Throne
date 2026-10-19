// Native-widget regression coverage. Link this main against the real Throne objects.
// All PNGs, settings and application state are disposable; no core is initialized.
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QNetworkProxy>
#include <QPointer>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <vector>

#include "include/api/RPC.h"
#include "include/database/DatabaseManager.h"
#include "include/database/IpListsRepo.h"
#include "include/engine/EngineHost.h"
#include "include/engine/supervisor/CoreSupervisor.h"
#include "include/global/Configs.hpp"
#include "include/global/DeviceDetailsHelper.hpp"
#include "include/ui/mainwindow.h"
#include "include/ui/setting/dialog_basic_settings.h"
#include "include/ui/setting/Icon.hpp"
#include "include/ui/setting/ThemeManager.hpp"

namespace {
int checks = 0, failures = 0;

void check(bool ok, const QString &label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.toUtf8().constData());
    std::fflush(stdout);
}

[[noreturn]] void finish() {
    std::printf("RESULT assertions=%d failures=%d\n", checks, failures);
    std::fflush(stdout);
    // The normal app owns permanent worker loops. The runner removes the enclosing
    // temporary working directory, including this process's QTemporaryDir.
    std::_Exit(failures ? 1 : 0);
}

void pump() {
    QApplication::processEvents(QEventLoop::AllEvents, 20);
}

struct ToolbarIcon {
    const char *button;
    const char *themeName;
    const char *resource;
};
constexpr std::array<ToolbarIcon, 5> icons{{
    {"toolButton_program", "system-run", ":/icon/b-system-run.png"},
    {"toolButton_preferences", "preferences-system", ":/icon/b-preferences.png"},
    {"toolButton_testing", "folder", ":/icon/b-testing-button.png"},
    {"toolButton_routing", "network-workgroup", ":/icon/b-network-routing.png"},
    {"toolButton_tools", "applications-utilities", ":/icon/b-tools-button.png"},
}};
constexpr std::array<Icon::TrayIconStatus, 5> statuses{{
    Icon::TrayIconStatus::None, Icon::TrayIconStatus::Connecting,
    Icon::TrayIconStatus::Running, Icon::TrayIconStatus::SystemProxy, Icon::TrayIconStatus::Vpn,
}};
constexpr std::array<const char *, 5> statusNames{{"Off", "Connecting", "Throne", "Proxy", "Tun"}};
using Buttons = std::array<QToolButton *, icons.size()>;
using Pictures = std::array<QImage, icons.size()>;

QImage pixels(const QIcon &icon, const QSize &size = QSize(32, 32)) {
    return icon.pixmap(size, QIcon::Normal, QIcon::Off).toImage().convertToFormat(QImage::Format_RGBA8888);
}

QImage fixtureImage(int seed) {
    QImage image(32, 32, QImage::Format_ARGB32);
    const QColor base = QColor::fromHsv((seed * 47) % 360, 215, 225);
    const QColor stripe = QColor::fromHsv((seed * 47 + 137) % 360, 160, 175);
    image.fill(base);
    // Asymmetry makes both a wrong mapping and accidental mirroring detectable.
    for (int y = 4; y < 23; ++y)
        for (int x = 3; x < 9; ++x) image.setPixelColor(x, y, stripe);
    image.setPixelColor(25, 26, Qt::black);
    return image;
}

QString writeTheme(const QString &root, const QString &name, unsigned mask, int seed) {
    const auto directory = root + '/' + name;
    if (!QDir().mkpath(directory + "/32x32/actions")) std::abort();
    QFile index(directory + "/index.theme");
    if (!index.open(QIODevice::WriteOnly)) std::abort();
    index.write("[Icon Theme]\nName=" + name.toUtf8() +
                "\nComment=Disposable regression fixture\nDirectories=32x32/actions\n"
                "[32x32/actions]\nSize=32\nType=Fixed\nContext=Actions\n");
    index.close();
    for (size_t i = 0; i < icons.size(); ++i) {
        if ((mask & (1u << i)) && !fixtureImage(seed + int(i)).save(
                directory + "/32x32/actions/" + icons[i].themeName + ".png")) std::abort();
    }
    return name;
}

QIcon fixtureIcon(const QString &root, const QString &theme, size_t index) {
    return QIcon(root + '/' + theme + "/32x32/actions/" + icons[index].themeName + ".png");
}

Buttons findButtons(MainWindow *window) {
    Buttons result{};
    for (size_t i = 0; i < icons.size(); ++i) {
        result[i] = window->findChild<QToolButton *>(icons[i].button);
        check(result[i], QStringLiteral("actual toolbar widget exists: %1").arg(icons[i].button));
    }
    for (auto *button : result) if (!button) finish();
    return result;
}

void expectToolbar(const Buttons &buttons, const QString &root, const QString &theme,
                   unsigned themeMask, const QString &context) {
    for (size_t i = 0; i < icons.size(); ++i) {
        const QIcon expected = themeMask & (1u << i) ? fixtureIcon(root, theme, i) : QIcon(icons[i].resource);
        bool equal = !expected.isNull() && !buttons[i]->icon().isNull();
        // Include the actual 24px display size and the 32px fixture's native pixels.
        for (const QSize size : {buttons[i]->iconSize(), QSize(32, 32)}) {
            const auto actual = pixels(buttons[i]->icon(), size);
            const auto reference = pixels(expected, size);
            equal = equal && !actual.isNull() && !reference.isNull() && actual == reference;
        }
        check(equal, context + QStringLiteral(": %1 exact pixels").arg(icons[i].button));
    }
}

void poison(const Buttons &buttons) {
    const QIcon sentinel(QPixmap::fromImage(fixtureImage(91)));
    for (auto *button : buttons) button->setIcon(sentinel);
}

void changeEvent(MainWindow *window, QEvent::Type type) {
    QEvent event(type);
    QApplication::sendEvent(window, &event);
    pump();
}

QString savedToolbarValue() {
    auto query = Configs::dataManager->getDatabase().queryThrow(
        "SELECT value FROM settings WHERE key = 'use_system_toolbar_icons'");
    return query->executeStep() ? QString::fromStdString(query->getColumn(0).getString()) : QString();
}

bool reloadedToolbarValue(const QString &path) {
    Configs::Database database(path.toStdString());
    Configs::SettingsRepo reloaded(database);
    return reloaded.use_system_toolbar_icons;
}

struct RuntimeSnapshot {
    bool coreRunning, vpn, systemProxy, remember, rememberVpn, rememberProxy;
    bool killSwitch, remoteApi, netProxy, netInsecure, sendHwid, exitLookup;
    int started, subUpdate, routeUpdate;
    explicit RuntimeSnapshot(const Configs::SettingsRepo &s)
        : coreRunning(s.core_running), vpn(s.spmode_vpn), systemProxy(s.spmode_system_proxy),
          remember(s.remember_enable), rememberVpn(s.remember_tun), rememberProxy(s.remember_system_proxy),
          killSwitch(s.kill_switch), remoteApi(s.remote_api_enable), netProxy(s.net_use_proxy),
          netInsecure(s.net_insecure), sendHwid(s.sub_send_hwid), exitLookup(s.exit_country_lookup),
          started(s.started_id), subUpdate(s.sub_auto_update), routeUpdate(s.route_auto_update) {}
    bool matches(const Configs::SettingsRepo &s) const {
        const RuntimeSnapshot now(s);
        return coreRunning == now.coreRunning && vpn == now.vpn && systemProxy == now.systemProxy &&
            remember == now.remember && rememberVpn == now.rememberVpn && rememberProxy == now.rememberProxy &&
            killSwitch == now.killSwitch && remoteApi == now.remoteApi && netProxy == now.netProxy &&
            netInsecure == now.netInsecure && sendHwid == now.sendHwid && exitLookup == now.exitLookup &&
            started == now.started && subUpdate == now.subUpdate && routeUpdate == now.routeUpdate;
    }
};

struct DialogMessage { MwMessage command; QStringList args; };

// Use the actual menu action and actual dialog buttons, including the window's
// ordinary deferred deletion path. No copied checkbox or alternate settings UI.
QPointer<DialogBasicSettings> openSettings(MainWindow *window, int &finished) {
    finished = -1;
    auto *action = window->findChild<QAction *>("menu_basic_settings");
    check(action, "Basic Settings menu action exists");
    if (!action) finish();
    action->trigger();
    pump();
    QPointer<DialogBasicSettings> dialog;
    for (auto *candidate : window->findChildren<DialogBasicSettings *>())
        if (candidate->isVisible()) dialog = candidate;
    check(!dialog.isNull(), "Basic Settings menu opens the real dialog");
    if (!dialog) finish();
    QObject::connect(dialog, &QDialog::finished, [ &finished ](int result) { finished = result; });
    auto *box = dialog->findChild<QCheckBox *>("use_system_toolbar_icons");
    check(box, "Linux toolbar system-icons checkbox exists");
    if (!box) finish();
    // Reveal the style page before asserting the Linux-only control is shown.
    for (auto *tabs : dialog->findChildren<QTabWidget *>())
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->widget(i)->isAncestorOf(box)) tabs->setCurrentIndex(i);
    pump();
    check(box->isVisible() && box->isEnabled(), "toolbar system-icons checkbox is visible and enabled on Linux");
    return dialog;
}

void clickDialogButton(DialogBasicSettings *dialog, QDialogButtonBox::StandardButton which) {
    auto *box = dialog->findChild<QDialogButtonBox *>("buttonBox");
    auto *button = box ? box->button(which) : nullptr;
    check(button && button->isEnabled(), which == QDialogButtonBox::Ok ? "real OK button is enabled" : "real Cancel button is enabled");
    if (!button || !button->isEnabled()) finish();
    button->click();
}
} // namespace

int main(int argc, char **argv) {
#ifndef Q_OS_LINUX
    std::puts("SKIP toolbar system-icon integration is Linux-only");
    return 77;
#endif
    bool startupEnabled = false;
    for (int i = 1; i < argc; ++i) startupEnabled |= QByteArray(argv[i]) == "--startup-enabled";
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
    QDir::setCurrent(isolated.path());
    Configs::SetBasePath(isolated.path());
    SetDeviceIdStore(isolated.path());
    const auto databasePath = isolated.path() + "/test.db";
    Configs::initDB(databasePath.toStdString());
    auto &settings = Configs::dataManager->settingsRepo;
    check(!settings->use_system_toolbar_icons, "fresh settings default to bundled toolbar icons");
    // Model a pre-feature database with no key, even if initial secret generation saved all defaults.
    Configs::dataManager->getDatabase().execThrow("DELETE FROM settings WHERE key = 'use_system_toolbar_icons'");
    check(savedToolbarValue().isEmpty() && !reloadedToolbarValue(databasePath), "missing persisted key reloads as opt-out");

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
    Pictures customTray, customTaskbar, bundledTray, bundledTaskbar;
    for (size_t i = 0; i < statuses.size(); ++i) {
        customTray[i] = pixels(Icon::GetTrayIcon(statuses[i]));
        customTaskbar[i] = pixels(Icon::GetTaskbarIcon(statuses[i]));
        check(customTray[i] == pixels(QIcon(QStringLiteral("icons/%1.png").arg(statusNames[i]))),
              QStringLiteral("custom status fixture is active: %1").arg(statusNames[i]));
    }
    settings->use_custom_icons = false;
    for (size_t i = 0; i < statuses.size(); ++i) {
        bundledTray[i] = pixels(Icon::GetTrayIcon(statuses[i]));
        bundledTaskbar[i] = pixels(Icon::GetTaskbarIcon(statuses[i]));
        check(!bundledTray[i].isNull(), QStringLiteral("bundled status fixture exists: %1").arg(statusNames[i]));
    }
    settings->use_custom_icons = true;

    const QString root = isolated.path() + "/themes";
    const auto full = writeTheme(root, "throne-fixture-full", 0x1f, 1);
    const auto second = writeTheme(root, "throne-fixture-second", 0x1f, 11);
    const auto partial = writeTheme(root, "throne-fixture-partial", 0x15, 21);
    const auto empty = writeTheme(root, "throne-fixture-empty", 0, 0);
    writeTheme(root, "hicolor", 0, 0);
    QIcon::setThemeSearchPaths({root});
    QIcon::setFallbackSearchPaths({});
    QIcon::setFallbackThemeName(empty);
    QIcon::setThemeName(full);
    for (size_t i = 0; i < icons.size(); ++i)
        check(pixels(QIcon::fromTheme(icons[i].themeName)) == pixels(fixtureIcon(root, full, i)),
              QStringLiteral("isolated theme resolves fixture pixels: %1").arg(icons[i].themeName));
    settings->use_system_toolbar_icons = startupEnabled;
    settings->Save();
    check(reloadedToolbarValue(databasePath) == startupEnabled, "startup toolbar preference survives a fresh repository reload");
    if (!startupEnabled)
        Configs::dataManager->getDatabase().execThrow("DELETE FROM settings WHERE key = 'use_system_toolbar_icons'");
    settings = std::make_unique<Configs::SettingsRepo>(Configs::dataManager->getDatabase());
    check(settings->use_system_toolbar_icons == startupEnabled &&
          (startupEnabled || savedToolbarValue().isEmpty()),
          startupEnabled ? "MainWindow will use the reloaded persisted opt-in" : "MainWindow will use an old database with no toolbar preference key");
    const RuntimeSnapshot runtimeBefore(*settings);
    API::defaultClient = new API::Client;
    LogThread = new QThread;
    LogThread->start();
    Engine::RuntimeOptions options;
    options.systemProxy = false;
    options.resume = false;
    options.blockingPrivilegeCheck = false;
    options.exitCountryLookup = false;
    auto *host = Engine::EngineHost::create(options); // Deliberately never call init().
    auto *window = new MainWindow;
    window->resize(900, 650);
    window->show();
    pump();
    const auto buttons = findButtons(window);
    expectToolbar(buttons, root, full, startupEnabled ? 0x1f : 0,
                  startupEnabled ? "persisted opt-in startup" : "default-off startup despite installed theme");
    auto *nativeTray = window->findChild<QSystemTrayIcon *>();
    check(nativeTray, "actual Linux tray icon object exists");
    if (!nativeTray) finish();
    const auto actualTrayBefore = pixels(nativeTray->icon());
    const auto actualTaskbarBefore = pixels(QApplication::windowIcon());
    check(actualTrayBefore == customTray[0] && actualTaskbarBefore == customTaskbar[0],
          "actual startup tray and taskbar retain the custom Off icon");
    int capturedMessages = 0;
    std::vector<DialogMessage> messages;
    SetDialogMessageSink([&](MwMessage command, const QStringList &args) {
        ++capturedMessages;
        messages.push_back({command, args});
    });

    auto expectUnrelatedState = [&](const QString &context, const QString &theme) {
        check(QIcon::themeName() == theme && QIcon::fallbackThemeName() == empty &&
              QIcon::themeSearchPaths() == QStringList{root} && QIcon::fallbackSearchPaths().isEmpty(),
              context + ": global theme selection and paths remain unchanged");
        check(settings->use_custom_icons && settings->follow_status_in_taskbar && settings->disable_tray &&
              pixels(nativeTray->icon()) == actualTrayBefore &&
              pixels(QApplication::windowIcon()) == actualTaskbarBefore,
              context + ": actual custom tray/taskbar and their preferences are unchanged");
        bool customUnchanged = true, bundledUnchanged = true;
        for (size_t i = 0; i < statuses.size(); ++i)
            customUnchanged = customUnchanged && pixels(Icon::GetTrayIcon(statuses[i])) == customTray[i] &&
                              pixels(Icon::GetTaskbarIcon(statuses[i])) == customTaskbar[i];
        settings->use_custom_icons = false; // Fixture-only probe of the other existing icon mode.
        for (size_t i = 0; i < statuses.size(); ++i)
            bundledUnchanged = bundledUnchanged && pixels(Icon::GetTrayIcon(statuses[i])) == bundledTray[i] &&
                               pixels(Icon::GetTaskbarIcon(statuses[i])) == bundledTaskbar[i];
        settings->use_custom_icons = true;
        check(customUnchanged && bundledUnchanged, context + ": all five custom and bundled status icons remain unchanged");
        check(runtimeBefore.matches(*settings) && host->connectionState() == Engine::ConnectionState::Idle &&
              host->supervisor()->state() == Engine::CoreState::NotStarted && host->supervisor()->corePid() == 0 &&
              !host->runningSnapshot() && !host->runningTun() && !host->testsRunning() &&
              QNetworkProxy::applicationProxy().type() == QNetworkProxy::NoProxy,
              context + ": core, VPN, system proxy, network flags and tests remain inactive");
    };
    expectUnrelatedState("startup", full);

    settings->use_system_toolbar_icons = true;
    for (const auto &[theme, mask, label] : std::array<std::tuple<QString, unsigned, QString>, 5>{{
             {full, 0x1f, "full theme"}, {partial, 0x15, "partial theme uses per-icon fallbacks"},
             {empty, 0, "missing theme uses bundled fallbacks"}, {second, 0x1f, "missing-to-full theme transition"},
             {full, 0x1f, "full-to-full theme transition"}}}) {
        QIcon::setThemeName(theme);
        poison(buttons);
        changeEvent(window, QEvent::ThemeChange);
        expectToolbar(buttons, root, theme, mask, label);
        expectUnrelatedState(label, theme);
    }
    // A non-existent theme (no index.theme) must be as safe as an empty one.
    const QString nonexistent = QStringLiteral("throne-fixture-does-not-exist");
    QIcon::setThemeName(nonexistent);
    poison(buttons);
    changeEvent(window, QEvent::ThemeChange);
    expectToolbar(buttons, root, nonexistent, 0, "nonexistent desktop theme");
    expectUnrelatedState("nonexistent desktop theme", nonexistent);

    QIcon::setThemeName(full);
    for (const auto type : {QEvent::ThemeChange, QEvent::StyleChange, QEvent::PaletteChange}) {
        for (const bool enabled : {false, true}) {
            settings->use_system_toolbar_icons = enabled;
            poison(buttons);
            changeEvent(window, type);
            expectToolbar(buttons, root, full, enabled ? 0x1f : 0,
                          QStringLiteral("event %1 with opt-in %2").arg(int(type)).arg(enabled));
        }
    }
    for (const bool enabled : {false, true}) {
        settings->use_system_toolbar_icons = enabled;
        poison(buttons);
        const bool emitted = QMetaObject::invokeMethod(themeManager(), "themeChanged", Qt::DirectConnection,
                                                      Q_ARG(QString, themeManager()->current_theme));
        check(emitted, "existing ThemeManager signal can be delivered");
        expectToolbar(buttons, root, full, enabled ? 0x1f : 0, "ThemeManager signal restores toolbar pixels");
        for (const auto &style : {QStringLiteral("BlackSoft"), QStringLiteral("LightBlue")}) {
            poison(buttons);
            themeManager()->ApplyTheme(style, true);
            pump();
            expectToolbar(buttons, root, full, enabled ? 0x1f : 0,
                          QStringLiteral("%1 app style with opt-in %2").arg(style).arg(enabled));
            expectUnrelatedState(style, full);
        }
    }
    themeManager()->ApplyTheme("System", true);
    settings->use_system_toolbar_icons = false;
    settings->Save();
    changeEvent(window, QEvent::ThemeChange);

    // Cancel and OK are tested in both directions. Capture the application's
    // UpdateSettings message; call the real repository Save ourselves instead of
    // EngineHost::applySettingsChange, whose ordinary responsibilities exceed UI QA.
    for (const bool target : {true, false}) {
        const bool before = settings->use_system_toolbar_icons;
        const auto savedBefore = savedToolbarValue();
        int finished = -1;
        auto dialog = openSettings(window, finished);
        auto *toggle = dialog->findChild<QCheckBox *>("use_system_toolbar_icons");
        check(toggle->isChecked() == before, "dialog loads the current toolbar preference");
        const int messagesBefore = capturedMessages;
        toggle->click();
        check(toggle->isChecked() == target && settings->use_system_toolbar_icons == before,
              "checkbox change is deferred until OK");
        expectToolbar(buttons, root, full, before ? 0x1f : 0, "unaccepted dialog leaves toolbar unchanged");
        clickDialogButton(dialog, QDialogButtonBox::Cancel);
        check(finished == QDialog::Rejected && capturedMessages == messagesBefore &&
              settings->use_system_toolbar_icons == before && savedToolbarValue() == savedBefore &&
              reloadedToolbarValue(databasePath) == before,
              "Cancel leaves live preference, database and UpdateSettings delivery untouched");
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(dialog.isNull(), "cancelled real settings dialog is deleted normally");

        dialog = openSettings(window, finished);
        toggle = dialog->findChild<QCheckBox *>("use_system_toolbar_icons");
        check(toggle->isChecked() == before, "reopening after Cancel retains the old value");
        toggle->click();
        clickDialogButton(dialog, QDialogButtonBox::Ok);
        check(finished == QDialog::Accepted && settings->use_system_toolbar_icons == target,
              "OK accepts the toolbar preference");
        check(capturedMessages == messagesBefore + 1 && messages.back().command == MwMessage::UpdateSettings,
              "OK emits exactly one existing UpdateSettings message");
        check(!messages.empty() && messages.back().args.isEmpty(),
              "toolbar-only setting requests no restart, VPN, proxy, tray or other special changes");
        settings->Save();
        check(savedToolbarValue() == (target ? "true" : "false") && reloadedToolbarValue(databasePath) == target,
              "accepted value persists through the generic bool map and fresh repository reload");
        changeEvent(window, QEvent::ThemeChange);
        expectToolbar(buttons, root, full, target ? 0x1f : 0, "accepted preference is honored by live toolbar refresh");
        expectUnrelatedState("accepted toolbar-only setting", full);
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(dialog.isNull(), "accepted real settings dialog is deleted normally");
    }
    check(capturedMessages == 2, "only the two approved settings accepts produce messages");
    check(!settings->use_system_toolbar_icons && !reloadedToolbarValue(databasePath),
          "final opt-out is persisted and reloadable");
    expectToolbar(buttons, root, full, 0, "final bundled toolbar is restored");
    expectUnrelatedState("final state", full);
    finish();
}
