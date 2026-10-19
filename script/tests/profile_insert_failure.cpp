// Links the actual application objects, replacing only main. All state is disposable.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include "include/configs/sub/GroupUpdater.hpp"
#include "include/database/DatabaseManager.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/global/Configs.hpp"
#include "include/global/DeviceDetailsHelper.hpp"
#include "include/global/Notice.hpp"

static int assertions = 0, failures = 0;
static void check(bool ok, const char *label) {
    ++assertions; if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); std::fflush(stdout);
}
static bool waitFor(const std::function<bool()> &ready) {
    QElapsedTimer timer; timer.start();
    while (!ready() && timer.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return ready();
}
static int countRows(int gid) {
    auto query = Configs::dataManager->getDatabase().queryThrow("SELECT count(*) FROM profiles WHERE gid = ?", gid);
    return query->executeStep() ? query->getColumn(0).getInt() : -1;
}
static QList<int> storedOrder(int gid) {
    auto query = Configs::dataManager->getDatabase().queryThrow("SELECT profiles_json FROM groups WHERE id = ?", gid);
    QList<int> result;
    if (query->executeStep()) for (const auto &id : QJsonDocument::fromJson(query->getColumn(0).getText()).array()) result << id.toInt();
    return result;
}
static std::shared_ptr<Configs::Group> newGroup() {
    auto group = Configs::GroupsRepo::NewGroup();
    check(Configs::dataManager->groupsRepo->AddGroup(group), "create isolated group");
    return group;
}
static std::shared_ptr<Configs::Profile> profile(const QString &name, int port = 1088) {
    auto item = Configs::ProfilesRepo::NewProfile("socks");
    if (!item->outbound->ParseFromLink(QStringLiteral("socks://127.0.0.2:%1#%2").arg(port).arg(name))) std::abort();
    return item;
}
static void rejectReplacement(bool enable) {
    auto &db = Configs::dataManager->getDatabase();
    if (enable) db.execThrow("CREATE TEMP TRIGGER reject_test_profile BEFORE INSERT ON profiles WHEN NEW.name = 'Replacement' BEGIN SELECT RAISE(ABORT, 'test profile insert failure'); END");
    else db.execThrow("DROP TRIGGER reject_test_profile");
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
static Subscription::UpdateOutcome import(int gid, const QString &text) {
    std::atomic<bool> done{false}; Subscription::UpdateOutcome outcome;
    Subscription::updater()->ImportTextTo(gid, text, [&](const auto &value) { outcome = value; done.store(true); });
    if (!waitFor([&] { return done.load() && Subscription::updater()->IsIdle(); })) std::abort();
    return outcome;
}
int main(int argc, char **argv) {
    QTemporaryDir isolated(QDir::currentPath() + "/fixture-XXXXXX");
    if (!isolated.isValid()) return 2;
    qputenv("HOME", isolated.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", isolated.path().toUtf8());
    qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
    qputenv("XDG_CACHE_HOME", isolated.path().toUtf8());
    QCoreApplication app(argc, argv);
    QDir::setCurrent(isolated.path());
    Configs::SetBasePath(isolated.path()); SetDeviceIdStore(isolated.path());
    Configs::initDB((isolated.path() + "/test.db").toStdString());
    auto &settings = Configs::dataManager->settingsRepo;
    settings->sub_send_hwid = false; settings->net_use_proxy = false;
    settings->sub_clear = false; settings->allow_stopping_active_profile = false;
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    auto &repo = Configs::dataManager->profilesRepo;
    auto &db = Configs::dataManager->getDatabase();
    QStringList notices;
    SetNoticeSink([&](const Notice &notice) { notices << notice.text; });

    // SQLite's real SQLITE_FULL path, confined to this disposable database.
    {
        auto group = newGroup();
        const auto scalar = [&](const char *sql) {
            auto q = db.queryThrow(sql);
            if (!q->executeStep()) std::abort();
            return q->getColumn(0).getInt64();
        };
        const auto previousLimit = scalar("PRAGMA max_page_count");
        const auto pages = scalar("PRAGMA page_count");
        { auto q = db.queryThrow("PRAGMA max_page_count = " + std::to_string(pages)); q->executeStep(); }
        auto item = profile("DiskFullFixture");
        item->outbound->name = QString(1024 * 1024, QLatin1Char('x'));
        QList<std::shared_ptr<Configs::Profile>> batch{item};
        check(!repo->AddProfileBatch(batch, group->id), "SQLITE_FULL is reported as failed insertion");
        QCoreApplication::processEvents();
        check(std::any_of(notices.cbegin(), notices.cend(), [](const QString &text) { return text.contains("[sqlite 13/"); }), "database emitted actual SQLITE_FULL error code");
        check(item->id == -1 && countRows(group->id) == 0 && group->Profiles().isEmpty(), "SQLITE_FULL publishes no phantom IDs or membership");
        { auto q = db.queryThrow("PRAGMA max_page_count = " + std::to_string(previousLimit)); q->executeStep(); }
        item->outbound->name = "Recovered";
        check(repo->AddProfileBatch(batch, group->id) && countRows(group->id) == 1, "failed disk-full insert remains retryable");
    }

    for (const int size : {1, 220}) {
        auto group = newGroup();
        QList<std::shared_ptr<Configs::Profile>> batch;
        const int failedIndex = size == 1 ? 0 : 110;
        for (int i = 0; i < size; ++i) batch << profile(i == failedIndex ? "Replacement" : QStringLiteral("Node%1").arg(i), 2000 + i);
        rejectReplacement(true);
        check(!repo->AddProfileBatch(batch, group->id), "failed insert returns false");
        const int expected = size == 1 ? 0 : Configs::BATCH_LIMIT_WRITE / 14;
        check(countRows(group->id) == expected, "only completed SQL chunks reached database");
        check(group->Profiles().size() == expected && storedOrder(group->id) == group->Profiles(), "group lists only saved rows");
        bool idsMatch = true;
        for (int i = 0; i < size; ++i) {
            const auto &item = batch[i];
            if (i < expected) idsMatch &= item->id >= 0 && item->outbound->profile_id == item->id && repo->GetProfile(item->id) == item;
            else idsMatch &= item->id == -1 && item->outbound->profile_id == -1;
        }
        check(idsMatch, "IDs and identity cache expose only saved profiles");
        rejectReplacement(false);
        check(repo->AddProfileBatch(batch, group->id), "retry saves remaining unsaved rows");
        check(countRows(group->id) == size && group->Profiles().size() == size, "retry neither duplicates nor drops rows");
    }

    {
        auto group = newGroup();
        auto item = profile("AllocationFailure");
        QList<std::shared_ptr<Configs::Profile>> batch{item};
        db.execThrow("CREATE TEMP TRIGGER reject_test_id BEFORE UPDATE ON entity_ids BEGIN SELECT RAISE(ABORT, 'test ID allocation failure'); END");
        bool accepted = true, threw = false;
        try { accepted = repo->AddProfileBatch(batch, group->id); } catch (const std::exception&) { threw = true; }
        check(!threw && !accepted, "ID allocation failure returns false without escaping");
        check(item->id == -1 && countRows(group->id) == 0 && group->Profiles().isEmpty(), "ID allocation failure leaves profile unsaved");
        db.execThrow("DROP TRIGGER reject_test_id");
    }

    Fixture fixture;
    for (const int incoming : {1, 220, 501}) {
        auto group = newGroup(); group->url = fixture.url(); group->sub_last_update = 123;
        group->sub_options.keep_working = false;
        Configs::dataManager->groupsRepo->Save(group);
        auto old = profile("Old", 1080); check(repo->AddProfile(old, group->id), "seed previous good profile");
        fixture.body.clear();
        for (int i = 0; i < incoming; ++i) {
            const QString name = i == incoming - 1 ? "Replacement" : QStringLiteral("New%1").arg(i);
            fixture.body += QStringLiteral("socks://127.0.0.2:%1#%2\n").arg(3000 + i).arg(name).toUtf8();
        }
        rejectReplacement(true);
        const auto failed = refresh(group->id);
        check(failed.failure == Subscription::UpdateOutcome::Failure::Database, "refresh reports database failure");
        check(failed.report.hasProblems(), "refresh includes save failure diagnostic");
        const int inserted = incoming == 1 ? 0 : incoming == 220 ? 214 : 500;
        check(failed.added == inserted && failed.updated == 0 && failed.removed == 0, "failed refresh reports actual partial additions only");
        check(repo->GetProfile(old->id) != nullptr && group->HasProfile(old->id), "failed refresh keeps previous good profile");
        check(group->sub_last_update == 123, "failed refresh is not stamped fresh");
        check(countRows(group->id) == group->Profiles().size(), "failed refresh has no phantom membership");
        rejectReplacement(false);
        const auto retry = refresh(group->id);
        check(retry.ok() && countRows(group->id) == incoming, "healthy refresh succeeds after failure");
        check(!group->HasProfile(old->id) && group->sub_last_update > 123, "healthy refresh reconciles and stamps normally");
        const auto previous = group->Profiles();
        const auto unchanged = refresh(group->id);
        check(unchanged.ok() && unchanged.added == 0 && unchanged.updated == 0 && unchanged.removed == 0 && group->Profiles() == previous, "unchanged refresh reuses existing IDs without duplicates");
    }

    // Preserve the explicit destructive option: it still clears first, but a failed insert
    // must be reported and must not advance the subscription's freshness timestamp.
    {
        auto group = newGroup(); group->url = fixture.url(); group->sub_last_update = 456;
        Configs::dataManager->groupsRepo->Save(group);
        auto old = profile("ClearedOld"); check(repo->AddProfile(old, group->id), "seed clear-before-update fixture");
        fixture.body = "socks://127.0.0.2:3000#Replacement\n";
        settings->sub_clear = true;
        rejectReplacement(true);
        const auto outcome = refresh(group->id);
        check(outcome.failure == Subscription::UpdateOutcome::Failure::Database && group->sub_last_update == 456, "clear-before-update reports failure without stamping fresh");
        check(countRows(group->id) == 0 && group->Profiles().isEmpty(), "clear-before-update retains deliberate clearing semantics");
        rejectReplacement(false); settings->sub_clear = false;
    }

    auto group = newGroup(); rejectReplacement(true);
    auto outcome = import(group->id, "socks://127.0.0.2:1088#Replacement");
    check(outcome.failure == Subscription::UpdateOutcome::Failure::Database && outcome.addedIds.isEmpty(), "quiet import reports failed save as database failure");
    rejectReplacement(false);
    outcome = import(group->id, "socks://127.0.0.2:1088#Replacement");
    check(outcome.ok() && outcome.addedIds.size() == 1 && countRows(group->id) == 1, "healthy quiet import succeeds");
    check(!settings->core_running && settings->started_id < 0 && !settings->spmode_vpn && !settings->spmode_system_proxy, "no core or proxy modes started");
    std::printf("RESULT assertions=%d failures=%d\n", assertions, failures); std::fflush(stdout);
    std::_Exit(failures ? 1 : 0);
}
