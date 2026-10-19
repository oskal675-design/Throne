// Integration harness: links the real application objects except src/main.cpp.
// It uses a temporary DB, a loopback HTTP fixture, and the actual MainWindow,
// subscription updater, repository, signal bus, dialogs, and profile model.
// EngineHost::init is deliberately never called: no core, VPN, or system proxy.
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QInputDialog>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QPushButton>
#include <QTabWidget>
#include <QTableView>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <utility>
#include "include/ui/mainwindow.h"
#include "include/api/RPC.h"
#include "include/configs/sub/GroupUpdater.hpp"
#include "include/database/GroupsRepo.h"
#include "include/database/IpListsRepo.h"
#include "include/engine/EngineEvents.h"
#include "include/engine/EngineHost.h"
#include "include/engine/services/GroupService.h"
#include "include/global/DeviceDetailsHelper.hpp"

static int failures = 0;
static int assertions = 0;
static void check(bool condition, const char *label) {
    ++assertions;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (!condition) ++failures;
}
static bool waitFor(const std::function<bool()> &condition, const char *label, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    const bool result = condition();
    if (!result) check(false, label);
    return result;
}
static void pump(int ms = 60) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}
struct Fixture {
    QTcpServer server;
    QList<QPointer<QTcpSocket>> delayed;
    int requests = 0;
    Fixture() {
        if (!server.listen(QHostAddress::LocalHost, 0)) std::abort();
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                auto request = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request] {
                    request->append(socket->readAll());
                    if (!request->contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                    socket->setProperty("handled", true);
                    ++requests;
                    const QByteArray path = request->split(' ').value(1);
                    if (path.startsWith("/slow")) delayed.append(socket);
                    else answer(socket, path.startsWith("/fail"));
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QString url(const QString &path) const {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path);
    }
    static void answer(QTcpSocket *socket, bool failure = false) {
        const QByteArray body = failure ? "fixture failure\n" : "socks://127.0.0.1:1088#FixtureNode\n";
        const QByteArray status = failure ? "503 Service Unavailable" : "200 OK";
        socket->write("HTTP/1.1 " + status + "\r\nContent-Type: text/plain\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    void release() {
        const auto sockets = std::exchange(delayed, {});
        for (const auto &socket : sockets) if (socket) answer(socket);
    }
};

int main(int argc, char **argv) {
    const bool headless = argc > 1 && QByteArray(argv[1]) == "--headless";
    QTemporaryDir isolated;
    if (!isolated.isValid()) return 2;
    qputenv("HOME", isolated.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (isolated.path() + "/xdg-config").toUtf8());
    qputenv("XDG_DATA_HOME", (isolated.path() + "/xdg-data").toUtf8());
    qputenv("XDG_CACHE_HOME", (isolated.path() + "/xdg-cache").toUtf8());
    qputenv("XDG_RUNTIME_DIR", (isolated.path() + "/runtime").toUtf8());
    QDir().mkpath(isolated.path() + "/runtime");
    QFile::setPermissions(isolated.path() + "/runtime", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    std::unique_ptr<QCoreApplication> app;
    if (headless) app = std::make_unique<QCoreApplication>(argc, argv);
    else app = std::make_unique<QApplication>(argc, argv);
    app->setApplicationName("ThroneFocusIntegration");
    QDir::setCurrent(isolated.path());
    Configs::SetBasePath(isolated.path());
    SetDeviceIdStore(isolated.path());
    Configs::initDB((isolated.path() + "/throne.db").toStdString());
    auto &settings = Configs::dataManager->settingsRepo;
    settings->url_scheme_auto_register = false;
    settings->file_assoc_auto_register = false;
    settings->sub_auto_update = -30;
    settings->route_auto_update = -1440;
    settings->sub_send_hwid = false;
    settings->remember_enable = false;
    settings->spmode_vpn = false;
    settings->spmode_system_proxy = false;
    settings->disable_tray = true;
    settings->kill_switch = false;
    settings->net_use_proxy = false;
    for (const auto &list : Configs::dataManager->ipListsRepo->GetAllIpLists()) {
        list->auto_update = false;
        Configs::dataManager->ipListsRepo->SaveHeader(list);
    }
    settings->Save();
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    API::defaultClient = new API::Client;
    auto *groups = Configs::dataManager->groupsRepo.get();
    const int original = settings->current_group;
    Fixture fixture;
    QList<int> created;
    int finished = 0;
    if (headless) {
        QObject::connect(Engine::EngineEvents::instance(), &Engine::EngineEvents::subscriptionNewGroup, app.get(), [&](int gid) { created << gid; });
        Subscription::updater()->SubscribeUrl(fixture.url("/ok"));
        waitFor([&] { return created.size() == 1 && Subscription::updater()->IsIdle(); }, "headless subscription completes");
        check(created.size() == 1 && groups->GetGroup(created.last()) != nullptr, "headless event carries stored group ID");
        check(settings->current_group == original, "headless subscription preserves current group");
        check(groups->GetGroup(created.last())->Profiles().size() == 1, "headless imports fixture profile without widgets");
        check(HeadlessDialogCount() == 0, "headless creates no dialogs");
    } else {
        QApplication::setQuitOnLastWindowClosed(false);
        LogThread = new QThread;
        LogThread->start();
        Engine::RuntimeOptions options;
        options.systemProxy = false;
        options.resume = false;
        options.blockingPrivilegeCheck = false;
        options.exitCountryLookup = false;
        Engine::EngineHost::create(options); // No init(): never starts core/network modes.
        auto *window = new MainWindow;
        auto *tabs = window->findChild<QTabWidget *>("tabWidget");
        auto *importAction = window->findChild<QAction *>("menu_add_from_clipboard");
        check(tabs && importAction, "actual window exposes group tabs and clipboard import action");
        QObject::connect(Engine::EngineEvents::instance(), &Engine::EngineEvents::subscriptionNewGroup, app.get(), [&](int gid) { created << gid; });
        QObject::connect(Subscription::updater(), &Subscription::GroupUpdater::asyncUpdateCallback, app.get(), [&](int) { ++finished; });
        bool acceptInput = true, acceptDeepLink = true;
        int inputDialogs = 0, deepLinkDialogs = 0, reports = 0;
        QString reportDetails;
        QTimer dialogs;
        QObject::connect(&dialogs, &QTimer::timeout, app.get(), [&] {
            if (auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                ++inputDialogs;
                if (acceptInput) {
                    input->setTextValue("Create new subscription group");
                    input->accept();
                } else input->reject();
            } else if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                if (box->windowTitle() == "Add subscription") {
                    ++deepLinkDialogs;
                    if (auto *autoUpdate = box->findChild<QCheckBox *>()) autoUpdate->setChecked(false);
                    box->done(acceptDeepLink ? QMessageBox::Ok : QMessageBox::Cancel);
                } else {
                    ++reports;
                    reportDetails += box->text() + box->detailedText();
                    box->accept();
                }
            }
            for (auto *widget : QApplication::topLevelWidgets()) {
                auto *box = qobject_cast<QMessageBox *>(widget);
                if (!box || !box->isVisible() || box->windowModality() != Qt::NonModal) continue;
                ++reports;
                reportDetails += box->text() + box->detailedText();
                box->accept();
            }
        });
        dialogs.start(5);
        const auto selected = [&] {
            return tabs->tabBar()->tabData(tabs->currentIndex()).toInt();
        };
        const auto isSelected = [&](int gid) {
            return settings->current_group == gid && selected() == gid;
        };
        const auto choose = [&](int gid) {
            tabs->setCurrentIndex(groups->GetGroupsTabOrder().indexOf(gid));
            check(isSelected(gid), "user can switch active group");
        };
        const auto clipboardImport = [&](const QString &text) {
            QApplication::clipboard()->setText(text);
            importAction->trigger();
        };
        const auto awaitFinished = [&](int previous) {
            waitFor([&] { return finished > previous && Subscription::updater()->IsIdle(); }, "subscription completes");
            pump();
        };
        const auto deepLink = [&](const QString &path) {
            return "throne://AddSub/" + QString::fromLatin1((fixture.url(path) + "#Named Fixture").toUtf8().toBase64(QByteArray::Base64UrlEncoding));
        };
        acceptInput = false;
        clipboardImport(fixture.url("/cancel"));
        pump();
        check(inputDialogs == 1 && created.isEmpty() && fixture.requests == 0 && isSelected(original), "cancel URL creation leaves groups, request count, and focus unchanged");
        acceptInput = true;
        clipboardImport(fixture.url("/slow-first"));
        if (!waitFor([&] { return created.size() == 1 && fixture.delayed.size() == 1; }, "first URL creates group before fetch completes")) return 2;
        const int first = created.last();
        check(isSelected(first), "URL import focuses new group before download completes");
        check(groups->GetGroup(first)->Profiles().isEmpty(), "new group is selected while still empty");
        const QString screenshot = qEnvironmentVariable("FOCUS_SCREENSHOT");
        if (!screenshot.isEmpty()) check(window->grab().save(screenshot), "capture actual window screenshot");
        fixture.release();
        awaitFinished(0);
        check(groups->GetGroup(first)->Profiles().size() == 1 && isSelected(first), "successful fetch populates selected new group");
        auto *profileView = window->findChild<QTableView *>("profilesTableView");
        check(profileView && profileView->model()->rowCount() == 1 && tabs->currentWidget()->isAncestorOf(profileView), "actual profile table displays new subscription on selected tab");
        const int beforeSlow = finished;
        clipboardImport(fixture.url("/slow-switch"));
        if (!waitFor([&] { return created.size() == 2 && fixture.delayed.size() == 1; }, "second URL creates group before fetch completes")) return 2;
        check(isSelected(created.last()), "second new subscription receives creation-time focus");
        choose(original);
        fixture.release();
        awaitFinished(beforeSlow);
        check(isSelected(original), "slow-fetch completion does not steal later user selection");
        const int beforeFail = finished;
        clipboardImport(fixture.url("/fail"));
        awaitFinished(beforeFail);
        check(created.size() == 3 && isSelected(created.last()) && groups->GetGroup(created.last())->Profiles().isEmpty(), "failed fetch keeps newly created empty group selected for repair");
        check(reports >= 1 && reportDetails.contains("first download failed"), "failed fetch reports the actual HTTP fixture error");
        const int beforeDeep = finished;
        clipboardImport(deepLink("/ok"));
        awaitFinished(beforeDeep);
        const int named = groups->GetGroupsTabOrder().last();
        check(isSelected(named), "accepted AddSub deeplink focuses its new group");
        check(groups->GetGroup(named)->name == "Named Fixture" && groups->GetGroup(named)->skip_auto_update, "deeplink preserves name and auto-update choice");
        check(groups->GetGroup(named)->Profiles().size() == 1, "deeplink fetch imports actual profile");
        const int beforeCancelCount = groups->GetAllGroupIds().size();
        const int beforeCancelRequests = fixture.requests;
        acceptDeepLink = false;
        clipboardImport(deepLink("/cancel-deeplink"));
        pump();
        check(groups->GetAllGroupIds().size() == beforeCancelCount && fixture.requests == beforeCancelRequests && isSelected(named), "cancel AddSub deeplink leaves groups, requests, and focus unchanged");
        acceptDeepLink = true;
        choose(original);
        const int beforeUpdate = finished;
        Subscription::updater()->RefreshGroup(named);
        awaitFinished(beforeUpdate);
        check(isSelected(original), "background refresh of existing group preserves focus");
        int apiGid = -1;
        const auto apiResult = Engine::GroupService::Add({"API fixture", fixture.url("/ok"), false}, &apiGid);
        pump();
        check(!apiResult && apiGid >= 0 && isSelected(original), "API group creation preserves user's selected group");
        const int beforeRepeat = finished;
        const int beforeRepeatGroups = groups->GetAllGroupIds().size();
        clipboardImport(fixture.url("/ok"));
        awaitFinished(beforeRepeat);
        check(groups->GetAllGroupIds().size() == beforeRepeatGroups + 1 && isSelected(created.last()), "repeated subscription URL remains allowed and selects its own new group");
        choose(named);
        // Queue a real engine event, then delete before the UI handles it.
        auto doomed = Configs::GroupsRepo::NewGroup();
        doomed->name = "Deleted before delivery";
        groups->AddGroup(doomed);
        QThread *emitter = QThread::create([gid = doomed->id] { Engine::EngineEvents::SubscriptionNewGroup(gid); });
        emitter->start();
        emitter->wait();
        delete emitter;
        groups->DeleteGroup(doomed->id);
        pump();
        check(isSelected(named), "queued event for deleted group preserves non-first-tab focus");
        // A creation event must select by ID even if tab order changes before delivery.
        auto reordered = Configs::GroupsRepo::NewGroup();
        reordered->name = "Reordered before delivery";
        groups->AddGroup(reordered);
        emitter = QThread::create([gid = reordered->id] { Engine::EngineEvents::SubscriptionNewGroup(gid); });
        emitter->start();
        emitter->wait();
        delete emitter;
        auto order = groups->GetGroupsTabOrder();
        order.move(order.indexOf(reordered->id), 1);
        groups->SetGroupsTabOrder(order);
        pump();
        check(isSelected(reordered->id) && tabs->currentIndex() == 1, "queued creation selects exact group ID after tab reorder");
        // Record the pre-existing FIFO creation semantics: a URL queued behind
        // another download is not created until that worker job starts.
        auto blocking = groups->GetGroup(first);
        blocking->url = fixture.url("/slow-blocker");
        groups->Save(blocking);
        const int beforeQueue = finished;
        Subscription::updater()->RefreshGroup(first);
        waitFor([&] { return fixture.delayed.size() == 1; }, "existing refresh blocks worker");
        const int beforeQueuedCreate = groups->GetAllGroupIds().size();
        clipboardImport(fixture.url("/ok-queued"));
        pump();
        check(groups->GetAllGroupIds().size() == beforeQueuedCreate, "queued URL retains existing deferred creation semantics");
        clipboardImport(deepLink("/ok-newer"));
        const int newerDeepLink = groups->GetGroupsTabOrder().last();
        check(isSelected(newerDeepLink), "newer deeplink selects immediately while earlier URL is queued");
        fixture.release();
        waitFor([&] { return finished >= beforeQueue + 3 && Subscription::updater()->IsIdle(); }, "all queued jobs finish");
        pump();
        check(isSelected(created.last()) && created.last() != newerDeepLink, "queued URL selects when its group is eventually created");
        std::printf("QUEUE_ORDER newer_deeplink=%d eventually_created_url=%d selected=%d\n", newerDeepLink, created.last(), selected());
        check(!settings->core_running && settings->started_id < 0 && !settings->spmode_vpn && !settings->spmode_system_proxy, "no core, VPN, or system proxy activated during tests");
        check(created.size() == 9, "creation notifications emitted once per confirmed UI subscription plus two queued-event cases");
        std::printf("PROMPTS url=%d deeplink=%d reports=%d HTTP=%d\n", inputDialogs, deepLinkDialogs, reports, fixture.requests);
    }
    std::printf("RESULT assertions=%d failures=%d mode=%s\n", assertions, failures, headless ? "headless" : "gui");
    std::fflush(stdout);
    // The application's log consumer is intentionally process-lifetime. Avoid
    // running GUI shutdown code that owns OS proxy/core lifecycle in this harness.
    std::_Exit(failures ? 1 : 0);
}
