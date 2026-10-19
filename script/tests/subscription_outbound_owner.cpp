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
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include "include/configs/generate.h"
#include "include/configs/sub/GroupUpdater.hpp"
#include "include/database/DatabaseManager.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/global/Configs.hpp"
#include "include/global/DeviceDetailsHelper.hpp"
#include "include/global/VpnCredentialOverride.hpp"

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

static std::shared_ptr<Configs::Profile> vpnProfile(const QString &type, int version) {
    auto profile = Configs::ProfilesRepo::NewProfile(type);
    QJsonObject data{{"type", type}, {"tag", type + QString::number(version)},
                     {"username", "synthetic-stored-user" + QString::number(version > 1 ? 2 : 0)},
                     {"password", "synthetic-stored-password" + QString::number(version > 1 ? 2 : 0)}};
    if (type == "openconnect") data["server"] = "https://127.0.0.2";
    else {
        data["server"] = "127.0.0.3";
        data["server_port"] = 1194;
        // Only a syntactically valid test pin. No VPN connection is made.
        data["tls"] = QJsonObject{{"peer_fingerprint", QJsonArray{QString(64, QLatin1Char('a'))}}};
    }
    if (!profile->outbound->ParseFromJson(data)) std::abort();
    return profile;
}
static QByteArray subscription(int version) {
    QByteArray body;
    for (const auto &type : {QString("openconnect"), QString("openvpn")})
        body += vpnProfile(type, version)->outbound->ExportJsonLink().toUtf8() + '\n';
    return body;
}
static int lastId() {
    auto query = Configs::dataManager->getDatabase().queryThrow("SELECT profile_last_id FROM entity_ids");
    if (!query->executeStep()) std::abort();
    return query->getColumn(0).getInt();
}
static QJsonObject storedOutbound(int id) {
    auto query = Configs::dataManager->getDatabase().queryThrow("SELECT outbound_json FROM profiles WHERE id = ?", id);
    if (!query->executeStep()) std::abort();
    return QJsonDocument::fromJson(query->getColumn(0).getText()).object();
}
static void checkCredentials(const std::shared_ptr<Configs::Profile> &profile, const Configs::VpnCredentials &credentials) {
    const auto built = profile->outbound->Build();
    check(built.error.isEmpty(), "actual outbound config builder succeeds");
    check(built.object["username"] == credentials.username && built.object["password"] == credentials.password,
          "outbound builder resolves credentials for retained owner");
    const auto config = Configs::BuildSingBoxConfig(profile);
    check(config->error.isEmpty(), "full application config generation succeeds without starting a core");
    bool found = false;
    for (const auto &value : config->coreConfig["endpoints"].toArray()) {
        const auto endpoint = value.toObject();
        if (config->vpnEndpointProfiles.value(endpoint["tag"].toString(), -1) != profile->id) continue;
        found = endpoint["username"] == credentials.username && endpoint["password"] == credentials.password;
    }
    check(found, "generated endpoint credentials match its mapped profile ID");
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
    Fixture fixture;
    auto group = Configs::GroupsRepo::NewGroup();
    group->url = fixture.url(); group->sub_options.keep_working = false;
    check(Configs::dataManager->groupsRepo->AddGroup(group), "create isolated subscription group");
    fixture.body = subscription(0);
    auto outcome = refresh(group->id);
    check(outcome.ok() && outcome.added == 2 && outcome.updated == 0, "production subscription parser imports both VPN types");
    const auto ids = group->Profiles();
    if (ids.size() != 2) std::abort();
    // The GUI and engine retain profile entities; hold these across refresh rather than reload them.
    auto retained = repo->GetProfileBatch(ids);
    check(retained.size() == 2 && retained[0]->type == "openconnect" && retained[1]->type == "openvpn",
          "native VPN profile types were imported");
    for (const auto &profile : retained) {
        check(profile->outbound->profile_id == profile->id, "initial insert stamps correct owner");
        Configs::VpnCredentials credentials{profile->type + "-synthetic-session-user", profile->type + "-synthetic-session-password"};
        Configs::SetVpnCredentialOverride(profile->id, credentials);
        checkCredentials(profile, credentials);
    }
    for (int version : {1, 2}) {
        const int previousLastId = lastId();
        fixture.body = subscription(version);
        outcome = refresh(group->id);
        check(outcome.ok() && outcome.updated == 2 && outcome.added == 0 && outcome.removed == 0,
              "changed subscription content updates both retained identities");
        check(group->Profiles() == ids, "refresh preserves profile IDs and order");
        for (const auto &profile : retained) {
            check(repo->GetProfile(profile->id) == profile, "cached profile entity is retained");
            check(profile->outbound->profile_id == profile->id, "replacement outbound owns retained profile ID");
            check(profile->name == profile->type + QString::number(version), "changed profile content is installed");
            Configs::VpnCredentials credentials{profile->type + "-synthetic-session-user", profile->type + "-synthetic-session-password"};
            checkCredentials(profile, credentials);
            // Authentication retries may first set the override after the refresh completed.
            const Configs::VpnCredentials retry{profile->type + "-synthetic-retry-user", profile->type + "-synthetic-retry-password"};
            Configs::SetVpnCredentialOverride(profile->id, retry);
            checkCredentials(profile, retry);
            Configs::SetVpnCredentialOverride(profile->id, credentials);
            const auto stored = storedOutbound(profile->id);
            check(stored["password"] == "synthetic-stored-password" + QString::number(version > 1 ? 2 : 0),
                  "database retains subscription credentials rather than session override");
            check(!stored.contains("profile_id") && !profile->outbound->ExportToJson().contains("profile_id"),
                  "runtime owner ID remains outside serialized outbound");
        }
        const int allocated = lastId();
        check(allocated == previousLastId + 2, "refresh used temporary rows for replacement content");
        bool removed = true;
        for (int id = previousLastId + 1; id <= allocated; ++id) removed &= repo->GetProfile(id) == nullptr;
        check(removed, "temporary imported profiles are deleted after reconciliation");
        outcome = refresh(group->id);
        check(outcome.ok() && outcome.added == 0 && outcome.updated == 0 && outcome.removed == 0,
              "unchanged subscription reuses existing profiles");
        check(group->Profiles() == ids && lastId() == allocated, "unchanged refresh allocates no temporary IDs");
    }
    fixture.body += "socks://127.0.0.4:1080#New\n";
    outcome = refresh(group->id);
    check(outcome.ok() && outcome.added == 1 && outcome.updated == 0, "new identity follows normal insertion path");
    const auto added = repo->GetProfile(group->Profiles().last());
    check(added && added->id != ids[0] && added->id != ids[1] && added->outbound->profile_id == added->id,
          "new profile owns its own independent ID");
    for (const auto &profile : retained) {
        Configs::ClearVpnCredentialOverride(profile->id);
        checkCredentials(profile, {"synthetic-stored-user2", "synthetic-stored-password2"});
    }
    std::weak_ptr<Configs::Profile> expired = retained.first();
    retained.clear();
    check(expired.expired(), "releasing cached entities permits an actual database reload");
    for (int id : ids) {
        auto reloaded = repo->GetProfile(id);
        check(reloaded && reloaded->outbound->profile_id == id, "database reload also preserves ownership");
        checkCredentials(reloaded, {"synthetic-stored-user2", "synthetic-stored-password2"});
    }
    check(!settings->core_running && settings->started_id < 0 && !settings->spmode_vpn && !settings->spmode_system_proxy,
          "no core or proxy modes started");
    std::printf("RESULT assertions=%d failures=%d\n", assertions, failures); std::fflush(stdout);
    std::_Exit(failures ? 1 : 0);
}
