// Invokes the real log context menu and save dialog with isolated synthetic state.
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QScrollBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include "include/api/RPC.h"
#include "include/database/DatabaseManager.h"
#include "include/database/IpListsRepo.h"
#include "include/engine/EngineHost.h"
#include "include/global/Configs.hpp"
#include "include/global/DeviceDetailsHelper.hpp"
#include "include/ui/mainwindow.h"

static int checks = 0, failures = 0;
static void check(bool ok, const char *label) {
    ++checks; if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); std::fflush(stdout);
}
static bool waitFor(const std::function<bool()> &ready) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return ready();
}
static QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
static void writeFixture(const QString &path, const QByteArray &text) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size()) std::abort();
}
static void finish() {
    std::printf("RESULT assertions=%d failures=%d\n", checks, failures); std::fflush(stdout);
    // The normal app owns permanent worker loops; the Python runner removes all temporary state.
    std::_Exit(failures ? 1 : 0);
}

int main(int argc, char **argv) {
    QTemporaryDir isolated(QDir::currentPath() + "/fixture-XXXXXX");
    if (!isolated.isValid()) return 2;
    for (const char *key : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) qputenv(key, isolated.path().toUtf8());
    const auto runtime = isolated.path() + "/runtime"; QDir().mkpath(runtime);
    QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv); QDir::setCurrent(isolated.path());
    Configs::SetBasePath(isolated.path()); SetDeviceIdStore(isolated.path());
    Configs::initDB((isolated.path() + "/test.db").toStdString());
    auto &settings = Configs::dataManager->settingsRepo;
    settings->url_scheme_auto_register = false; settings->file_assoc_auto_register = false;
    settings->sub_auto_update = -30; settings->route_auto_update = -1440;
    settings->sub_send_hwid = false; settings->net_use_proxy = false;
    settings->remember_enable = false; settings->disable_tray = true; settings->kill_switch = false;
    settings->max_log_line = 500;
    for (const auto &list : Configs::dataManager->ipListsRepo->GetAllIpLists()) {
        list->auto_update = false; Configs::dataManager->ipListsRepo->SaveHeader(list);
    }
    settings->Save();
    API::defaultClient = new API::Client;
    LogThread = new QThread; LogThread->start();
    Engine::RuntimeOptions options; options.systemProxy = false; options.resume = false;
    options.blockingPrivilegeCheck = false; options.exitCountryLookup = false;
    Engine::EngineHost::create(options); // No init(), core, network modes or user configuration.
    auto *window = new MainWindow;
    auto *log = window->findChild<QTextBrowser *>("masterLogBrowser");
    auto *tabs = window->findChild<QTabWidget *>("stats_widget");
    auto *search = window->findChild<QLineEdit *>("logSearchEdit");
    check(log && tabs && search, "actual log widgets are present");
    if (!log || !tabs || !search) finish();
    tabs->setCurrentWidget(window->findChild<QWidget *>("Logs"));
    window->resize(900, 650); window->show();
    QApplication::processEvents();

    auto withMenu = [&](const std::function<void(QMenu *)> &run) {
        QTimer::singleShot(0, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) std::abort();
            run(menu);
        });
        QMetaObject::invokeMethod(window, "on_masterLogBrowser_customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, QPoint(4, 4)));
    };
    auto clear = [&] {
        withMenu([&](QMenu *menu) {
            QAction *clearAction = nullptr;
            for (auto *action : menu->actions()) if (action->text() == "Clear") clearAction = action;
            if (!clearAction) std::abort();
            menu->close(); clearAction->trigger();
        });
    };
    clear();
    bool available = false;
    withMenu([&](QMenu *menu) {
        auto *action = menu->findChild<QAction *>("actionSaveDisplayedLog");
        available = action != nullptr;
        check(available, "context menu offers Save displayed log");
        if (action) check(!action->isEnabled(), "empty log disables export");
        menu->close();
    });
    if (!available) finish(); // The same availability check fails on unmodified dev.

    struct SaveResult { bool dialog = false; int warnings = 0; int overwrites = 0; };
    auto save = [&](const QString &path, bool cancel = false, const std::function<void(QFileDialog *)> &duringDialog = {}) {
        SaveResult result;
        QTimer responder;
        bool handled = false;
        QObject::connect(&responder, &QTimer::timeout, [&] {
            if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                if (message->text().startsWith("Cannot write to:")) {
                    ++result.warnings;
                    message->accept();
                } else if (auto *yes = message->button(QMessageBox::Yes)) {
                    ++result.overwrites; yes->click();
                } else {
                    check(false, "unexpected modal message"); message->reject();
                }
                return;
            }
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            if (!dialog || handled) return;
            handled = true; result.dialog = true;
            if (duringDialog) duringDialog(dialog);
            if (cancel) { dialog->reject(); return; }
            auto *fileName = dialog->findChild<QLineEdit *>("fileNameEdit");
            if (!fileName) std::abort();
            fileName->setText(path);
            // Return from this timer before accept() can open the overwrite confirmation.
            QTimer::singleShot(0, dialog, [dialog] { QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection); });
        });
        responder.start(1);
        withMenu([&](QMenu *menu) {
            auto *action = menu->findChild<QAction *>("actionSaveDisplayedLog");
            if (!action || !action->isEnabled()) std::abort();
            menu->close(); action->trigger();
        });
        responder.stop();
        check(result.dialog, "export opens the actual Save dialog");
        return result;
    };
    auto append = [&](const QString &text, const QString &last) {
        MW_show_log(text);
        check(waitFor([&] { return log->toPlainText().contains(last); }), "synthetic log reaches the production view");
    };

    std::puts("CASE UTF-8, multiline, selection and cancellation");
    const QString unicode = QString::fromUtf8("first <b>literal</b>\nПривет café 世界 🔒\nlast\tline");
    append(unicode, "last\tline");
    check(log->toPlainText() == unicode, "display retains multiline literal text and Unicode");
    auto cursor = log->textCursor(); cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor); log->setTextCursor(cursor);
    const auto cancelPath = isolated.path() + "/cancelled.txt";
    const auto cancelled = save(cancelPath, true);
    check(cancelled.warnings == 0 && !QFile::exists(cancelPath), "cancellation writes no file or error");
    check(log->toPlainText() == unicode && log->textCursor().hasSelection(), "cancellation preserves log and selection");
    const auto unicodePath = isolated.path() + "/unicode.txt";
    const auto saved = save(unicodePath);
    check(saved.warnings == 0 && readFile(unicodePath) == unicode.toUtf8(), "export is exact UTF-8 plain text, independent of selection");

    std::puts("CASE snapshot before the modal dialog processes new logs");
    const auto before = log->toPlainText();
    const auto snapshotPath = isolated.path() + "/snapshot.txt";
    save(snapshotPath, false, [&](QFileDialog *) {
        append("arrived while Save dialog was open", "arrived while Save dialog was open");
    });
    check(log->toPlainText() != before, "new logs may update the view while choosing a path");
    check(readFile(snapshotPath) == before.toUtf8(), "export retains the snapshot from before the dialog");

    std::puts("CASE filtered view and no hidden historical logs");
    clear();
    append("KEEP first\nhidden synthetic marker\nKEEP second", "KEEP second");
    QToolButton *jump = nullptr, *filter = nullptr;
    for (auto *button : window->findChildren<QToolButton *>())
        if (button->toolTip() == "New logs are held while the view is scrolled up") jump = button;
    if (jump) for (auto *button : jump->parentWidget()->findChildren<QToolButton *>())
        if (button->isCheckable()) filter = button;
    check(jump && filter, "actual log follow and filter controls are present");
    if (!jump || !filter) finish();
    filter->click(); search->setText("KEEP");
    check(waitFor([&] { return log->toPlainText() == "KEEP first\nKEEP second"; }), "production filter hides nonmatching lines");
    QDir().mkpath(isolated.path() + "/logs");
    writeFixture(isolated.path() + "/logs/throne.log", "historical synthetic marker\n");
    const auto filteredPath = isolated.path() + "/filtered.txt";
    save(filteredPath);
    check(readFile(filteredPath) == QByteArray("KEEP first\nKEEP second"), "only displayed filtered lines are exported");
    filter->click();
    check(waitFor([&] { return log->toPlainText().contains("hidden synthetic marker"); }), "export did not discard filtered-out view history");

    std::puts("CASE paused view excludes held new arrivals");
    clear();
    QStringList lines;
    for (int i = 0; i < 100; ++i) lines << QString("visible line %1").arg(i, 3, 10, QChar('0'));
    append(lines.join('\n'), "visible line 099");
    check(waitFor([&] { return log->verticalScrollBar()->maximum() > 0; }), "log is tall enough to pause follow mode");
    log->verticalScrollBar()->setValue(0);
    QApplication::processEvents();
    const auto paused = log->toPlainText();
    MW_show_log("held synthetic marker");
    check(waitFor([&] { return jump->text().contains("1 new"); }), "new arrival is held by the actual paused view");
    const auto pausedPath = isolated.path() + "/paused.txt";
    save(pausedPath);
    check(readFile(pausedPath) == paused.toUtf8(), "paused export includes all displayed lines but excludes held arrivals");
    check(!log->toPlainText().contains("held synthetic marker") && log->verticalScrollBar()->value() == 0,
          "saving does not release held lines or move the scrollbar");
    jump->click();
    check(waitFor([&] { return log->toPlainText().contains("held synthetic marker"); }), "held logs remain available after export");

    std::puts("CASE retained-line limit and overwrite");
    clear(); settings->max_log_line = 5;
    lines.clear(); for (int i = 0; i < 10; ++i) lines << QString("retained %1").arg(i);
    append(lines.join('\n'), "retained 9");
    check(log->document()->blockCount() == 5 && log->toPlainText().startsWith("retained 5"), "production retention removes older lines");
    const auto retained = log->toPlainText().toUtf8();
    const auto overwritePath = isolated.path() + "/existing.txt";
    writeFixture(overwritePath, "previous content");
    const auto overwritten = save(overwritePath);
    check(overwritten.overwrites == 1 && overwritten.warnings == 0, "existing file uses ordinary overwrite confirmation");
    check(readFile(overwritePath) == retained, "confirmed overwrite contains only retained displayed lines");

    std::puts("CASE write failure preserves the existing file");
    const auto lockedDir = isolated.path() + "/locked"; QDir().mkpath(lockedDir);
    const auto failedPath = lockedDir + "/existing.txt"; writeFixture(failedPath, "must survive");
    const auto failed = save(failedPath, false, [&](QFileDialog *dialog) {
        // Change only this fixture after the ordinary dialog validates its destination.
        QObject::connect(dialog, &QFileDialog::accepted, [=] {
            QFile::setPermissions(failedPath, QFileDevice::ReadOwner);
            QFile::setPermissions(lockedDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        });
    });
    QFile::setPermissions(lockedDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile::setPermissions(failedPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    check(failed.warnings == 1, "failed write presents an error through the real UI");
    check(readFile(failedPath) == QByteArray("must survive"), "failed export does not truncate an existing file");
    check(log->toPlainText().toUtf8() == retained, "failed export preserves the displayed log");
    check(!settings->core_running && settings->started_id < 0 && !settings->spmode_vpn && !settings->spmode_system_proxy,
          "no core or network modes started");
    finish();
}
