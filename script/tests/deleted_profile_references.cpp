// Uses the actual repositories, services and config builder with disposable state.
#include <QApplication>
#include <QAction>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QItemSelectionModel>
#include <QNetworkProxy>
#include <QTableView>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <atomic>
#include <functional>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include "include/configs/generate.h"
#include "include/database/DatabaseManager.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/engine/services/ProfileService.h"
#include "include/global/Configs.hpp"
#include "include/global/DeviceDetailsHelper.hpp"
#include "include/api/RPC.h"
#include "include/configs/sub/GroupUpdater.hpp"
#include "include/database/IpListsRepo.h"
#include "include/engine/EngineHost.h"
#include "include/ui/mainwindow.h"
#include "include/ui/utils/ProfilesTableModel.h"
static int checks = 0, failures = 0;
static void check(bool ok, const char *label) {
    ++checks; if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); std::fflush(stdout);
}
static std::shared_ptr<Configs::Group> newGroup() {
    auto group = Configs::GroupsRepo::NewGroup();
    if (!Configs::dataManager->groupsRepo->AddGroup(group)) std::abort();
    return group;
}
static std::shared_ptr<Configs::Profile> addProfile(int gid, int port) {
    auto profile = Configs::ProfilesRepo::NewProfile("socks");
    if (!profile->outbound->ParseFromLink(QStringLiteral("socks://127.0.0.1:%1#Fixture").arg(port)) ||
        !Configs::dataManager->profilesRepo->AddProfile(profile, gid)) std::abort();
    return profile;
}
static bool waitFor(const std::function<bool()> &ready) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return ready();
}
struct Fixture {
    QTcpServer server;
    QByteArray body;
    Fixture() {
        check(server.listen(QHostAddress::LocalHost, 0), "listen on loopback fixture");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                auto request = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request] {
                    request->append(socket->readAll());
                    if (!request->contains("\r\n\r\n") || socket->property("answered").toBool()) return;
                    socket->setProperty("answered", true);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1/sub").arg(server.serverPort()); }
};
static Subscription::UpdateOutcome refresh(int gid) {
    std::atomic<bool> done{false}; Subscription::UpdateOutcome outcome;
    Subscription::updater()->RefreshWithOutcome(gid, Subscription::GroupUpdater::Surface::Log, [&](const auto &value) { outcome = value; done.store(true); });
    if (!waitFor([&] { return done.load() && Subscription::updater()->IsIdle(); })) std::abort();
    return outcome;
}


static QPair<int, int> storedReferences(int gid) {
    auto query = Configs::dataManager->getDatabase().queryThrow("SELECT front_proxy_id, landing_proxy_id FROM groups WHERE id = ?", gid);
    if (!query->executeStep()) std::abort();
    return {query->getColumn(0).getInt(), query->getColumn(1).getInt()};
}
static int storedCount(int gid) {
    auto query = Configs::dataManager->getDatabase().queryThrow("SELECT count(*) FROM profiles WHERE gid = ?", gid);
    if (!query->executeStep()) std::abort();
    return query->getColumn(0).getInt();
}
static void save(const std::shared_ptr<Configs::Group> &group) { Configs::dataManager->groupsRepo->Save(group); }
struct Scenario {
    std::shared_ptr<Configs::Group> source = newGroup(), front = newGroup(), landing = newGroup(), both = newGroup(), unrelated = newGroup();
    std::shared_ptr<Configs::Profile> hop = addProfile(source->id, 1080), keeper = addProfile(source->id, 1083);
    std::shared_ptr<Configs::Profile> own = addProfile(source->id, 1084), first = addProfile(front->id, 1081), second = addProfile(landing->id, 1082);
    Scenario() {
        source->front_proxy_id = hop->id;
        front->front_proxy_id = hop->id; front->landing_proxy_id = keeper->id;
        landing->landing_proxy_id = hop->id;
        both->front_proxy_id = both->landing_proxy_id = hop->id;
        unrelated->front_proxy_id = keeper->id; unrelated->landing_proxy_id = 999999;
        for (const auto &group : {source, front, landing, both, unrelated}) save(group);
    }
    void before() const {
        for (const auto &profile : {own, first, second})
            check(Configs::BuildSingBoxConfig(profile)->error.isEmpty(), "referenced profile builds before deletion");
    }
    void after() const {
        auto &repo = Configs::dataManager->profilesRepo;
        check(!repo->GetProfile(hop->id) && !source->HasProfile(hop->id), "deleted row is absent from repository and source membership");
        check(storedCount(source->id) == 2 && source->Profiles().size() == 2, "source membership matches persisted surviving profiles");
        check(source->front_proxy_id == -1, "owning group also clears its deleted front reference");
        check(front->front_proxy_id == -1 && landing->landing_proxy_id == -1, "cross-group front and landing references are cleared");
        check(both->front_proxy_id == -1 && both->landing_proxy_id == -1, "both fields referencing a removed profile are cleared");
        check(front->landing_proxy_id == keeper->id && unrelated->front_proxy_id == keeper->id && unrelated->landing_proxy_id == 999999,
              "unrelated valid and pre-existing missing references are preserved");
        check(storedReferences(source->id) == qMakePair(-1, -1) && storedReferences(front->id) == qMakePair(-1, keeper->id) &&
              storedReferences(landing->id) == qMakePair(-1, -1) && storedReferences(both->id) == qMakePair(-1, -1),
              "cleared references are persisted across all affected groups");
        Configs::GroupsRepo fresh(Configs::dataManager->getDatabase());
        check(fresh.GetGroup(front->id)->front_proxy_id == -1 && fresh.GetGroup(landing->id)->landing_proxy_id == -1,
              "independent group cache reload sees cleared references");
        for (const auto &profile : {own, first, second})
            check(Configs::BuildSingBoxConfig(profile)->error.isEmpty(), "remaining profile config builds after deletion without opening group editor");
    }
};
int main(int argc, char **argv) {
    const bool gui = argc > 1 && QByteArray(argv[1]) == "--gui";
    QTemporaryDir isolated(QDir::currentPath() + "/fixture-XXXXXX");
    if (!isolated.isValid()) return 2;
    for (const char *key : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) qputenv(key, isolated.path().toUtf8());
    const auto runtime = isolated.path() + "/runtime";
    QDir().mkpath(runtime);
    QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
    std::unique_ptr<QCoreApplication> app;
    if (gui) app = std::make_unique<QApplication>(argc, argv);
    else app = std::make_unique<QCoreApplication>(argc, argv);
    QDir::setCurrent(isolated.path());
    Configs::SetBasePath(isolated.path()); SetDeviceIdStore(isolated.path());
    Configs::initDB((isolated.path() + "/test.db").toStdString());
    auto &repo = Configs::dataManager->profilesRepo;
    auto &groups = Configs::dataManager->groupsRepo;
    auto &settings = Configs::dataManager->settingsRepo;
    settings->sub_send_hwid = false; settings->net_use_proxy = false;
    settings->sub_clear = false; settings->allow_stopping_active_profile = false;
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    if (gui) {
        Scenario scenario; scenario.before();
        settings->current_group = scenario.source->id;
        settings->skip_delete_confirmation = true;
        settings->url_scheme_auto_register = false; settings->file_assoc_auto_register = false;
        settings->sub_auto_update = -30; settings->route_auto_update = -1440;
        settings->remember_enable = false; settings->disable_tray = true; settings->kill_switch = false;
        for (const auto &list : Configs::dataManager->ipListsRepo->GetAllIpLists()) {
            list->auto_update = false; Configs::dataManager->ipListsRepo->SaveHeader(list);
        }
        settings->Save();
        API::defaultClient = new API::Client;
        LogThread = new QThread; LogThread->start();
        Engine::RuntimeOptions options; options.systemProxy = false; options.resume = false;
        options.blockingPrivilegeCheck = false; options.exitCountryLookup = false;
        Engine::EngineHost::create(options); // Deliberately no init(): no core or network modes.
        auto *window = new MainWindow;
        window->show_group(scenario.source->id);
        auto *table = window->findChild<QTableView *>("profilesTableView");
        auto *action = window->findChild<QAction *>("menu_delete");
        check(table && action, "actual window exposes profile table and Delete action");
        if (!table || !action) std::abort();
        bool selected = false;
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            const auto index = table->model()->index(row, 0);
            if (index.data(ProfilesTableModel::ProfileIdRole).toInt() != scenario.hop->id) continue;
            table->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            selected = true; break;
        }
        check(selected, "select synthetic referenced profile in actual GUI");
        action->trigger();
        scenario.after();
    } else {
        for (int path : {0, 1, 2}) {
            std::printf("CASE %s\n", path == 0 ? "API control" : path == 1 ? "repository batch" : "RemoveUnavailable");
            Scenario scenario; scenario.before();
            if (path == 0) {
                int deleted = -1;
                check(!Engine::ProfileService::Delete({scenario.hop->id, scenario.hop->id, 999998}, &deleted).has_value() && deleted == 1,
                      "API delete keeps deduplication and count semantics");
            } else if (path == 1) {
                QList<int> ids{scenario.hop->id, scenario.hop->id, 999998};
                check(repo->BatchDeleteProfiles(ids), "batch deletion succeeds with duplicate and missing requested IDs");
            } else {
                scenario.hop->latency = -1; repo->Save(scenario.hop); settings->current_group = scenario.source->id;
                check(Engine::ProfileService::RemoveUnavailable({scenario.hop->id, scenario.keeper->id}) == 1, "only unavailable profile is removed");
            }
            scenario.after();
        }
        // Simulate only the persisted running-ID guard; no engine or VPN is started.
        {
            Scenario scenario; settings->started_id = scenario.hop->id;
            QList<int> ids{scenario.hop->id, scenario.keeper->id};
            check(repo->BatchDeleteProfiles(ids), "mixed deletion succeeds while excluding running ID");
            check(!ids.contains(scenario.hop->id) && repo->GetProfile(scenario.hop->id) == scenario.hop,
                  "running profile remains stored and cached");
            check(scenario.source->front_proxy_id == scenario.hop->id && scenario.front->front_proxy_id == scenario.hop->id &&
                  scenario.landing->landing_proxy_id == scenario.hop->id && scenario.both->front_proxy_id == scenario.hop->id,
                  "references to excluded running profile remain intact");
            check(scenario.front->landing_proxy_id == -1 && scenario.unrelated->front_proxy_id == -1,
                  "same batch clears only the actually deleted non-running profile");
            check(storedReferences(scenario.front->id) == qMakePair(scenario.hop->id, -1), "running exclusion and successful cleanup both persist");
            settings->started_id = -1;
        }
        // The second SQL delete chunk fails; the first chunk must roll back before any cache cleanup.
        {
            auto source = newGroup(), consumer = newGroup();
            QList<std::shared_ptr<Configs::Profile>> batch;
            for (int i = 0; i < Configs::BATCH_LIMIT_WRITE + 1; ++i) {
                auto profile = Configs::ProfilesRepo::NewProfile("socks");
                profile->outbound->ParseFromLink("socks://127.0.0.1:1080#Batch"); batch << profile;
            }
            check(repo->AddProfileBatch(batch, source->id), "create multi-statement delete fixture");
            consumer->front_proxy_id = batch.first()->id; consumer->landing_proxy_id = batch.last()->id; save(consumer);
            QList<int> ids; for (const auto &profile : batch) ids << profile->id;
            auto &db = Configs::dataManager->getDatabase();
            db.execThrow("CREATE TEMP TRIGGER reject_delete BEFORE DELETE ON profiles WHEN OLD.id = " + std::to_string(batch.last()->id) +
                         " BEGIN SELECT RAISE(ABORT, 'synthetic delete failure'); END");
            check(!repo->BatchDeleteProfiles(ids), "later-chunk SQL failure is reported");
            check(storedCount(source->id) == batch.size() && source->Profiles() == ids, "failed delete rolls back rows and preserves membership");
            check(repo->GetProfile(batch.first()->id) == batch.first() && repo->GetProfile(batch.last()->id) == batch.last(),
                  "failed delete leaves cached entities live");
            check(consumer->front_proxy_id == batch.first()->id && consumer->landing_proxy_id == batch.last()->id &&
                  storedReferences(consumer->id) == qMakePair(batch.first()->id, batch.last()->id), "failed deletion leaves cached and stored references intact");
            db.execThrow("DROP TRIGGER reject_delete");
            check(repo->BatchDeleteProfiles(ids), "retry deletes the whole large batch");
            check(storedCount(source->id) == 0 && source->Profiles().isEmpty(), "successful batch removes every row and membership");
            check(consumer->front_proxy_id == -1 && consumer->landing_proxy_id == -1 && storedReferences(consumer->id) == qMakePair(-1, -1),
                  "single batch cleanup covers references to both SQL chunks");
        }
        // A refresh deletes temporary imported rows but retains the referenced stable profile ID.
        {
            Fixture fixture; auto source = newGroup(), consumer = newGroup();
            auto hop = addProfile(source->id, 1080), main = addProfile(consumer->id, 1081);
            source->url = fixture.url(); source->sub_options.keep_working = false; save(source);
            consumer->front_proxy_id = hop->id; save(consumer);
            fixture.body = "socks://127.0.0.1:1080#Changed\n";
            const auto outcome = refresh(source->id);
            check(outcome.ok() && outcome.updated == 1 && outcome.removed == 0, "actual subscription refresh retains changed profile identity");
            check(repo->GetProfile(hop->id) == hop && hop->name == "Changed", "refresh retains cached referenced entity");
            check(consumer->front_proxy_id == hop->id && storedReferences(consumer->id).first == hop->id,
                  "temporary-row deletion never clears the reused stable ID reference");
            check(Configs::BuildSingBoxConfig(main)->error.isEmpty(), "retained subscription reference still builds");
        }
        // Group-order gaps must not prevent clearing references from surviving groups.
        {
            auto source = newGroup(), consumer = newGroup(); auto hop = addProfile(source->id, 1080);
            consumer->front_proxy_id = hop->id; save(consumer);
            // Delete a distinct empty group first to exercise group order gaps without orphaning fixture rows.
            auto empty = newGroup(); check(groups->DeleteGroup(empty->id), "remove unrelated group before deleting referenced profile");
            QList<int> ids{hop->id}; check(repo->BatchDeleteProfiles(ids), "delete referenced profile after group ordering change");
            check(consumer->front_proxy_id == -1 && storedReferences(consumer->id).first == -1, "surviving group reference is cleared across ordering gaps");
        }
    }
    check(!settings->core_running && settings->started_id < 0 && !settings->spmode_vpn && !settings->spmode_system_proxy, "no core or network modes started");
    std::printf("RESULT assertions=%d failures=%d\n", checks, failures); std::fflush(stdout);
    std::_Exit(failures ? 1 : 0);
}
