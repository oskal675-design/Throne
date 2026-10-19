// Exercises the actual Remove Duplicates action against a disposable SQLite database.
#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include "include/api/RPC.h"
#include "include/database/DatabaseManager.h"
#include "include/database/GroupsRepo.h"
#include "include/database/IpListsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/engine/EngineHost.h"
#include "include/global/Configs.hpp"
#include "include/global/DeviceDetailsHelper.hpp"
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

static QList<int> storedOrder(int gid) {
    auto q = Configs::dataManager->getDatabase().queryThrow("SELECT profiles_json FROM groups WHERE id = ?", gid);
    if (!q->executeStep()) std::abort();
    QList<int> result;
    for (const auto &value : QJsonDocument::fromJson(q->getColumn(0).getText()).array()) result << value.toInt();
    return result;
}
static int countRows(int gid) {
    auto q = Configs::dataManager->getDatabase().queryThrow("SELECT count(*) FROM profiles WHERE gid = ?", gid);
    if (!q->executeStep()) std::abort();
    return q->getColumn(0).getInt();
}
int main(int argc, char **argv) {
    QTemporaryDir isolated(QDir::currentPath() + "/fixture-XXXXXX");
    if (!isolated.isValid()) return 2;
    for (const char *key : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) qputenv(key, isolated.path().toUtf8());
    const auto runtime = isolated.path() + "/runtime"; QDir().mkpath(runtime);
    QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
    QApplication app(argc, argv); QDir::setCurrent(isolated.path());
    Configs::SetBasePath(isolated.path()); SetDeviceIdStore(isolated.path());
    Configs::initDB((isolated.path() + "/test.db").toStdString());
    auto &settings = Configs::dataManager->settingsRepo;
    settings->skip_delete_confirmation = true;
    settings->url_scheme_auto_register = false; settings->file_assoc_auto_register = false;
    settings->sub_auto_update = -30; settings->route_auto_update = -1440;
    settings->sub_send_hwid = false; settings->net_use_proxy = false;
    settings->remember_enable = false; settings->disable_tray = true; settings->kill_switch = false;
    for (const auto &list : Configs::dataManager->ipListsRepo->GetAllIpLists()) {
        list->auto_update = false; Configs::dataManager->ipListsRepo->SaveHeader(list);
    }
    settings->Save();
    API::defaultClient = new API::Client;
    LogThread = new QThread; LogThread->start();
    Engine::RuntimeOptions options; options.systemProxy = false; options.resume = false;
    options.blockingPrivilegeCheck = false; options.exitCountryLookup = false;
    Engine::EngineHost::create(options); // Never init(): no real core or network modes.
    auto *window = new MainWindow;
    auto *action = window->findChild<QAction *>("menu_delete_repeat");
    auto *table = window->findChild<QTableView *>("profilesTableView");
    check(action && table, "actual GUI exposes Remove Duplicates action and profile table");
    if (!action || !table) std::abort();
    auto &db = Configs::dataManager->getDatabase();
    auto show = [&](const std::shared_ptr<Configs::Group>& group) {
        window->refresh_groups();
        window->show_group(group->id);
    };
    auto visibleOrder = [&] {
        QList<int> ids;
        for (int row = 0; row < table->model()->rowCount(); ++row)
            ids << table->model()->index(row, 0).data(ProfilesTableModel::ProfileIdRole).toInt();
        return ids;
    };
    auto assertOrder = [&](const std::shared_ptr<Configs::Group>& group, const QList<int>& expected) {
        check(group->Profiles() == expected, "cached membership and order match");
        check(storedOrder(group->id) == expected, "persisted membership and order match");
        check(visibleOrder() == expected, "visible membership and order match");
    };
    auto assertColdOrder = [&](const std::shared_ptr<Configs::Group>& group, const QList<int>& expected) {
        Configs::GroupsRepo cold(db);
        check(cold.GetGroup(group->id)->Profiles() == expected, "independent group-cache reload preserves membership");
    };

    // The SQL error must leave both rows listed, so removing the fault permits retry.
    {
        std::puts("CASE first-statement failure and retry");
        auto group = newGroup(); auto first = addProfile(group->id, 1080);
        auto duplicate = addProfile(group->id, 1080); const auto original = group->Profiles();
        show(group);
        db.execThrow("CREATE TEMP TRIGGER reject_duplicate_delete BEFORE DELETE ON profiles BEGIN SELECT RAISE(ABORT, 'synthetic delete failure'); END");
        action->trigger();
        check(countRows(group->id) == 2, "failed DELETE retains both persisted profile rows");
        assertOrder(group, original);
        check(Configs::dataManager->profilesRepo->GetProfile(duplicate->id) == duplicate, "failed DELETE leaves the cached duplicate accessible");
        assertColdOrder(group, original);
        db.execThrow("DROP TRIGGER reject_duplicate_delete");
        action->trigger();
        check(countRows(group->id) == 1, "healthy retry deletes the duplicate row");
        assertOrder(group, {first->id});
        check(!Configs::dataManager->profilesRepo->GetProfile(duplicate->id), "healthy retry forgets deleted duplicate");
        assertColdOrder(group, {first->id});
        action->trigger();
        assertOrder(group, {first->id});
    }

    // Real write contention, without a trigger, must also leave the GUI retryable.
    {
        std::puts("CASE database busy and retry");
        auto group = newGroup(); auto first = addProfile(group->id, 1081);
        addProfile(group->id, 1081); const auto original = group->Profiles(); show(group);
        SQLite::Database other(db.Path(), SQLite::OPEN_READWRITE);
        other.exec("BEGIN IMMEDIATE");
        db.queryThrow("PRAGMA busy_timeout = 1")->executeStep(); // Shorten this disposable fixture's timeout only.
        action->trigger();
        check(countRows(group->id) == 2, "write contention leaves both profile rows intact");
        assertOrder(group, original);
        other.exec("ROLLBACK");
        db.queryThrow("PRAGMA busy_timeout = " + std::to_string(Configs::BUSY_TIMEOUT_MS))->executeStep();
        action->trigger();
        check(countRows(group->id) == 1, "retry after write contention deletes the duplicate");
        assertOrder(group, {first->id});
    }

    // Failure in a later SQL chunk must not commit an earlier deletion or new order.
    {
        std::puts("CASE later-statement failure and retry");
        auto group = newGroup();
        QList<std::shared_ptr<Configs::Profile>> profiles;
        for (int i = 0; i < Configs::BATCH_LIMIT_WRITE + 2; ++i) {
            auto profile = Configs::ProfilesRepo::NewProfile("socks");
            if (!profile->outbound->ParseFromLink("socks://127.0.0.1:1082#Batch")) std::abort();
            profiles << profile;
        }
        if (!Configs::dataManager->profilesRepo->AddProfileBatch(profiles, group->id)) std::abort();
        const auto original = group->Profiles(); show(group);
        db.execThrow("CREATE TEMP TRIGGER reject_later_duplicate BEFORE DELETE ON profiles WHEN OLD.id = " +
                     std::to_string(profiles.last()->id) + " BEGIN SELECT RAISE(ABORT, 'later delete failure'); END");
        action->trigger();
        check(countRows(group->id) == profiles.size(), "later failure rolls back all SQL delete chunks");
        assertOrder(group, original);
        check(Configs::dataManager->profilesRepo->GetProfile(profiles[1]->id) == profiles[1] &&
              Configs::dataManager->profilesRepo->GetProfile(profiles.last()->id) == profiles.last(),
              "rollback preserves cached profiles from both SQL chunks");
        assertColdOrder(group, original);
        db.execThrow("DROP TRIGGER reject_later_duplicate");
        action->trigger();
        check(countRows(group->id) == 1, "healthy retry deletes duplicates across both SQL chunks");
        assertOrder(group, {profiles.first()->id});
        check(!Configs::dataManager->profilesRepo->GetProfile(profiles[1]->id) &&
              !Configs::dataManager->profilesRepo->GetProfile(profiles.last()->id),
              "successful retry forgets profiles from both SQL chunks");
    }

    // Positional duplicates have no row to delete. Keep that existing cleanup path.
    {
        std::puts("CASE repeated slots without SQL deletion");
        auto group = newGroup(); auto first = addProfile(group->id, 1083);
        auto second = addProfile(group->id, 1084);
        group->profiles = {first->id, first->id, second->id, first->id, second->id};
        Configs::dataManager->groupsRepo->Save(group); show(group);
        db.execThrow("CREATE TEMP TRIGGER reject_any_delete BEFORE DELETE ON profiles BEGIN SELECT RAISE(ABORT, 'unexpected delete'); END");
        action->trigger();
        check(countRows(group->id) == 2, "repeated slots retain both distinct profile rows");
        assertOrder(group, {first->id, second->id});
        db.execThrow("DROP TRIGGER reject_any_delete");
    }

    // Retain the first listed duplicate, not necessarily the lowest profile ID.
    {
        std::puts("CASE mixed repeated slots and distinct duplicate IDs");
        auto group = newGroup(); auto older = addProfile(group->id, 1085);
        auto distinct = addProfile(group->id, 1086); auto first = addProfile(group->id, 1085);
        const QList<int> original{first->id, distinct->id, first->id, older->id, older->id};
        group->profiles = original; Configs::dataManager->groupsRepo->Save(group); show(group);
        db.execThrow("CREATE TEMP TRIGGER reject_mixed_delete BEFORE DELETE ON profiles BEGIN SELECT RAISE(ABORT, 'mixed delete failure'); END");
        action->trigger();
        check(countRows(group->id) == 3, "mixed failure retains all profile rows");
        assertOrder(group, original);
        db.execThrow("DROP TRIGGER reject_mixed_delete");
        action->trigger();
        check(countRows(group->id) == 2, "mixed retry removes only the distinct duplicate ID");
        assertOrder(group, {first->id, distinct->id});
        check(Configs::dataManager->profilesRepo->GetProfile(first->id) &&
              Configs::dataManager->profilesRepo->GetProfile(distinct->id) &&
              !Configs::dataManager->profilesRepo->GetProfile(older->id), "first-listed and unrelated profiles survive");
    }

    // The confirmation dialog must still precede all persistent changes.
    {
        std::puts("CASE confirmation and healthy deletion");
        auto group = newGroup(); auto first = addProfile(group->id, 1087);
        addProfile(group->id, 1087); const auto original = group->Profiles(); show(group);
        settings->skip_delete_confirmation = false;
        auto respond = [](QMessageBox::StandardButton button) {
            QTimer::singleShot(0, [button] {
                auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (!dialog) std::abort();
                if (!dialog->button(button)) std::abort();
                dialog->button(button)->click();
            });
        };
        respond(QMessageBox::No); action->trigger();
        check(countRows(group->id) == 2, "declining confirmation keeps both rows");
        assertOrder(group, original);
        respond(QMessageBox::Yes); action->trigger();
        check(countRows(group->id) == 1, "accepting confirmation deletes the duplicate");
        assertOrder(group, {first->id});
        settings->skip_delete_confirmation = true;
    }
    check(!settings->core_running && settings->started_id < 0 && !settings->spmode_vpn && !settings->spmode_system_proxy, "no core or network modes started");
    std::printf("RESULT assertions=%d failures=%d\n", checks, failures); std::fflush(stdout);
    std::_Exit(failures ? 1 : 0);
}
