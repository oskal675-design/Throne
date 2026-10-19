// Links the actual application objects, replacing only main. No core, services,
// network requests, or user configuration are started. All SQLite state is temporary.
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <stdexcept>

#include "include/database/DatabaseManager.h"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/global/Configs.hpp"
#include "include/global/DeviceDetailsHelper.hpp"
#include "include/global/Notice.hpp"

namespace {
using Configs::Database;
using Configs::Profile;
using Configs::ProfilesRepo;
using ProfilePtr = std::shared_ptr<Profile>;

int assertions = 0;
int failures = 0;

void check(bool ok, const char *label) {
    ++assertions;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}

void require(bool ok, const char *label) {
    check(ok, label);
    if (!ok) throw std::runtime_error(label);
}

bool waitFor(const std::function<bool()> &ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 15000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return ready();
}

qint64 scalar(Database &db, const std::string &sql) {
    auto query = db.queryThrow(sql);
    if (!query->executeStep()) throw std::runtime_error("scalar query returned no row");
    return query->getColumn(0).getInt64();
}

int favorite(Database &db, int id) {
    auto query = db.queryThrow("SELECT favorite FROM profiles WHERE id = ?", id);
    return query->executeStep() ? query->getColumn(0).getInt() : -1;
}

std::string value(Database &db, int id, const char *column) {
    // The column is a test constant, never external input.
    auto query = db.queryThrow(std::string("SELECT ") + column + " FROM profiles WHERE id = ?", id);
    return query->executeStep() ? query->getColumn(0).getString() : std::string();
}

// A separate read connection observes background SaveBatch/SaveTraffic commits
// without using the application's connection concurrently from the test thread.
bool waitValue(Database &db, int id, const char *column, const std::string &expected) {
    SQLite::Database reader(db.Path(), SQLite::OPEN_READONLY, Configs::BUSY_TIMEOUT_MS);
    return waitFor([&] {
        SQLite::Statement query(reader, std::string("SELECT ") + column + " FROM profiles WHERE id = ?");
        query.bind(1, id);
        return query.executeStep() && query.getColumn(0).getString() == expected;
    });
}

bool favoriteSchema(Database &db) {
    auto query = db.queryThrow("PRAGMA table_info(profiles)");
    int matches = 0;
    bool correct = true;
    while (query->executeStep()) {
        if (query->getColumn(1).getString() != "favorite") continue;
        ++matches;
        correct &= query->getColumn(2).getString() == "INTEGER";
        correct &= query->getColumn(3).getInt() == 1;
        correct &= query->getColumn(4).getString() == "0";
    }
    return matches == 1 && correct;
}

bool hasFavoriteColumn(Database &db) {
    auto query = db.queryThrow("PRAGMA table_info(profiles)");
    while (query->executeStep())
        if (query->getColumn(1).getString() == "favorite") return true;
    return false;
}

ProfilePtr profile(const QString &name, int port = 1080) {
    auto item = ProfilesRepo::NewProfile("socks");
    if (!item || !item->outbound->ParseFromLink(
            QStringLiteral("socks://127.0.0.2:%1#%2").arg(port).arg(name)))
        throw std::runtime_error("synthetic SOCKS fixture could not be parsed");
    item->latency = 123;
    item->latency_at = 1700000123;
    item->dl_speed = "12.3 MiB/s";
    item->ul_speed = "4.5 MiB/s";
    item->test_country = "ZZ";
    item->ip_out = "192.0.2.42";
    item->traffic_downlink = (1LL << 35) + 12;
    item->traffic_uplink = (1LL << 34) + 34;
    item->endpoint.mode = Configs::EndpointSource::Mode::Own;
    return item;
}

bool payloadMatches(const ProfilePtr &item, const QString &name) {
    return item && item->type == "socks" && item->name == name && item->outbound->name == name &&
           item->outbound->profile_id == item->id && item->latency == 123 &&
           item->latency_at == 1700000123 && item->dl_speed == "12.3 MiB/s" &&
           item->ul_speed == "4.5 MiB/s" && item->test_country == "ZZ" &&
           item->ip_out == "192.0.2.42" && item->traffic_downlink == (1LL << 35) + 12 &&
           item->traffic_uplink == (1LL << 34) + 34 &&
           item->endpoint.mode == Configs::EndpointSource::Mode::Own;
}

Configs::ProfileInsertRow snapshot(const ProfilePtr &item) {
    Configs::ProfileInsertRow row;
    row.id = item->id;
    row.type = item->type.toStdString();
    row.name = item->outbound->name.toStdString();
    row.gid = item->gid;
    row.latency = item->latency;
    row.latency_at = item->latency_at;
    row.dl_speed = item->dl_speed.toStdString();
    row.ul_speed = item->ul_speed.toStdString();
    row.test_country = item->test_country.toStdString();
    row.ip_out = item->ip_out.toStdString();
    row.outbound_json = QJsonDocument(item->outbound->ExportToJson()).toJson(QJsonDocument::Compact).toStdString();
    row.traffic_dl = item->traffic_downlink;
    row.traffic_up = item->traffic_uplink;
    row.endpoint_json = QJsonDocument(item->endpoint.ToJson()).toJson(QJsonDocument::Compact).toStdString();
    return row;
}

void testLegacyMigration(const QString &root) {
    Database db((root + "/legacy-migration.db").toStdString());
    Configs::GroupsRepo groups(db);
    db.execThrow("INSERT INTO groups(id, name) VALUES (7, 'Legacy fixtures')");
    // Deliberately predates endpoint_json and latency_at too. Production migration
    // must append columns safely rather than relying on the table's physical order.
    db.execThrow(R"(
        CREATE TABLE profiles (
            id INTEGER PRIMARY KEY, type TEXT NOT NULL, name TEXT,
            gid INTEGER NOT NULL DEFAULT 0, latency INTEGER NOT NULL DEFAULT 0,
            dl_speed TEXT, ul_speed TEXT, test_country TEXT, ip_out TEXT,
            outbound_json TEXT NOT NULL, traffic_dl INTEGER NOT NULL DEFAULT 0,
            traffic_up INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY(gid) REFERENCES groups(id) ON DELETE CASCADE)
    )");
    auto item = profile("Legacy");
    const auto json = QJsonDocument(item->outbound->ExportToJson()).toJson(QJsonDocument::Compact).toStdString();
    db.execThrow("INSERT INTO profiles(id,type,name,gid,latency,outbound_json,traffic_dl,traffic_up) "
                 "VALUES (11,'socks','Legacy',7,456,?,?,?)", json,
                 static_cast<long long>(item->traffic_downlink), static_cast<long long>(item->traffic_uplink));
    check(!hasFavoriteColumn(db), "legacy fixture genuinely lacks the favorite column");
    db.execThrow("PRAGMA query_only = ON");
    bool migrationFailed = false;
    try { ProfilesRepo blocked(db); }
    catch (const std::exception &) { migrationFailed = true; }
    check(migrationFailed && !hasFavoriteColumn(db),
          "a denied schema migration cannot masquerade as successful repository construction");
    db.execThrow("PRAGMA query_only = OFF");
    {
        ProfilesRepo repo(db);
        check(favoriteSchema(db), "legacy migration adds exactly one non-null favorite column with default zero");
        auto loaded = repo.GetProfile(11);
        require(loaded != nullptr, "legacy profile remains readable");
        check(!loaded->IsFavorite() && favorite(db, 11) == 0, "legacy rows migrate as non-favorites");
        check(loaded->name == "Legacy" && loaded->gid == 7 && loaded->latency == 456 &&
              loaded->latency_at == 0 && loaded->traffic_downlink == item->traffic_downlink &&
              loaded->traffic_uplink == item->traffic_uplink &&
              loaded->endpoint.mode == Configs::EndpointSource::Mode::Inherit,
              "migration preserves legacy payload and defaults other added columns");
        check(repo.SetFavorites({11}, true) && loaded->IsFavorite() && favorite(db, 11) == 1,
              "migrated row supports committed favorite toggles");
    }
    for (int i = 0; i < 3; ++i) {
        ProfilesRepo reopened(db);
        const auto batch = reopened.GetProfileBatch({11, 999999});
        check(favoriteSchema(db) && batch.size() == 1 && batch.first()->IsFavorite(),
              "repeated schema initialization and cold batch reads retain migrated favorites");
    }
    bool rejectedNull = false;
    try { db.execThrow("UPDATE profiles SET favorite = NULL WHERE id = 11"); }
    catch (const std::exception &) { rejectedNull = true; }
    check(rejectedNull && favorite(db, 11) == 1, "favorite schema rejects NULL without damaging the row");
}

void testReadWriteAndRollback(Database &db, ProfilesRepo &repo, int gid) {
    auto first = profile("First");
    auto second = profile("Second", 1081);
    check(!first->IsFavorite() && !second->IsFavorite(), "new profile objects default to non-favorite");
    require(repo.AddProfile(first, gid), "ordinary single insertion succeeds");
    QList<ProfilePtr> additions{second};
    require(repo.AddProfileBatch(additions, gid), "ordinary batch insertion succeeds");
    additions.clear();
    const int a = first->id;
    const int b = second->id;
    check(favorite(db, a) == 0 && favorite(db, b) == 0, "single and batch inserts use the database favorite default");
    check(repo.SetFavorites({}, true), "empty favorite selection is a successful no-op");
    check(repo.SetFavorites({-1, std::numeric_limits<int>::max()}, true),
          "missing and invalid profile IDs are a successful no-op");
    check(repo.SetFavorites({a, a, -1, std::numeric_limits<int>::max()}, true),
          "duplicate and missing IDs can be mixed with a real favorite toggle");
    check(first->IsFavorite() && !second->IsFavorite() && favorite(db, a) == 1 && favorite(db, b) == 0,
          "favorite toggle changes only selected persistent rows and retained cache entities");
    check(repo.GetProfile(a) == first && repo.GetProfile(b) == second,
          "favorite toggle retains existing identity-map objects");
    check(repo.SetFavorites({a, b, a}, true) && first->IsFavorite() && second->IsFavorite(),
          "mixed existing and new favorites become uniformly favorite");
    check(repo.SetFavorites({a, b, a}, false) && !first->IsFavorite() && !second->IsFavorite(),
          "mixed selection can be explicitly cleared");
    check(repo.SetFavorites({a}, true) && repo.SetFavorites({a}, true), "repeated favorite setting is idempotent");

    // A deferred FK fails COMMIT, after every UPDATE and RETURNING result was
    // successful. This catches publishing cache state before commit succeeds.
    db.execThrow("CREATE TEMP TABLE favorite_commit_parent(id INTEGER PRIMARY KEY)");
    db.execThrow("CREATE TEMP TABLE favorite_commit_guard(parent_id INTEGER REFERENCES "
                 "favorite_commit_parent(id) DEFERRABLE INITIALLY DEFERRED)");
    db.execThrow("CREATE TEMP TRIGGER reject_favorite_commit AFTER UPDATE OF favorite ON profiles "
                 "WHEN NEW.id = " + std::to_string(b) +
                 " BEGIN INSERT INTO favorite_commit_guard(parent_id) VALUES (1); END");
    check(!repo.SetFavorites({a, b}, false), "favorite COMMIT failure is reported after successful row updates");
    check(first->IsFavorite() && !second->IsFavorite() && favorite(db, a) == 1 && favorite(db, b) == 0 &&
          scalar(db, "SELECT COUNT(*) FROM favorite_commit_guard") == 0,
          "COMMIT failure rolls back rows and leaves mixed cached flags unchanged");
    db.execThrow("DROP TRIGGER reject_favorite_commit");
    db.execThrow("DROP TABLE favorite_commit_guard");
    db.execThrow("DROP TABLE favorite_commit_parent");

    std::weak_ptr<Profile> releasedA = first;
    std::weak_ptr<Profile> releasedB = second;
    first.reset();
    second.reset();
    check(releasedA.expired() && releasedB.expired(), "test releases all strong references before cold reads");
    first = repo.GetProfile(a);
    require(first != nullptr, "single cold read returns saved profile");
    check(first->IsFavorite() && payloadMatches(first, "First"),
          "single cold read restores favorite without shifting payload columns");
    first.reset();
    auto cold = repo.GetProfileBatch({b, a, std::numeric_limits<int>::max(), b});
    require(cold.size() == 3, "batch cold read skips missing rows and preserves requested duplicates");
    check(!cold[0]->IsFavorite() && cold[1]->IsFavorite() && cold[0] == cold[2] &&
          payloadMatches(cold[0], "Second") && payloadMatches(cold[1], "First"),
          "batch cold reads preserve order, identity, favorite, and all payload columns");
    first = cold[1];
    second = cold[0];
    cold.clear();

    first->outbound->name = "SingleSaved";
    require(repo.Save(first), "ordinary Save accepts a favorited profile");
    check(value(db, a, "name") == "SingleSaved" && favorite(db, a) == 1 && first->IsFavorite(),
          "ordinary Save updates payload without clearing favorites");
    second->outbound->name = "BatchSaved";
    first->outbound->name = "BatchFavoriteSaved";
    repo.SaveBatch({first, second});
    require(waitValue(db, b, "name", "BatchSaved"), "background SaveBatch persists its last row");
    check(repo.GetProfile(a) == first && repo.GetProfile(b) == second &&
          favorite(db, a) == 1 && favorite(db, b) == 0,
          "ordinary SaveBatch preserves mixed favorite state and cached identity");

    first->traffic_downlink = (1LL << 40) + 100;
    first->traffic_uplink = (1LL << 39) + 200;
    require(repo.SaveTraffic(first), "single traffic save accepts a favorite");
    require(waitValue(db, a, "traffic_dl", std::to_string(first->traffic_downlink)),
            "asynchronous traffic update reaches SQLite");
    check(favorite(db, a) == 1 && first->IsFavorite(), "single traffic save preserves favorite state");
    second->traffic_downlink = (1LL << 38) + 300;
    repo.SaveTrafficBatch({first, second});
    check(value(db, b, "traffic_dl") == std::to_string(second->traffic_downlink) &&
          favorite(db, a) == 1 && favorite(db, b) == 0,
          "batch traffic update preserves mixed favorite state");

    // Snapshot before the toggle, then deterministically replay after it. This is
    // the stale SaveBatch ordering without timing assumptions or a copied SQL path.
    auto stale = snapshot(second);
    stale.name = "StaleBeforeFavorite";
    require(repo.SetFavorites({b}, true), "toggle favorite after the batch row was serialized");
    db.execBatchReplaceProfiles({stale});
    check(value(db, b, "name") == stale.name && favorite(db, b) == 1 && second->IsFavorite(),
          "direct stale-row batch replay cannot undo a later favorite toggle");
    stale = snapshot(second);
    stale.name = "StaleBeforeUnfavorite";
    require(repo.SetFavorites({b}, false), "clear favorite after the batch row was serialized");
    db.execBatchReplaceProfiles({stale});
    check(value(db, b, "name") == stale.name && favorite(db, b) == 0 && !second->IsFavorite(),
          "direct stale-row batch replay cannot undo a later unfavorite toggle");

    auto insertedByReplace = stale;
    insertedByReplace.id = 1000000;
    insertedByReplace.name = "ReplaceNewRow";
    db.execBatchReplaceProfiles({insertedByReplace});
    check(favorite(db, insertedByReplace.id) == 0,
          "batch replacement of a previously absent row defaults to non-favorite");
    db.execThrow("DELETE FROM profiles WHERE id = ?", insertedByReplace.id);

    require(repo.SetFavorites({b}, true), "seed a retained favorite before deletion");
    QList<int> deleted{b};
    require(repo.BatchDeleteProfiles(deleted, false, false), "delete the retained profile without starting services");
    check(repo.SetFavorites({b, b, -1}, false) && favorite(db, b) == -1 && !repo.GetProfile(b),
          "favorite updates skip deleted IDs and cannot resurrect rows");
    check(second->IsFavorite(), "deleted retained objects are not falsely published as updated favorites");
    check(!repo.Save(second), "ordinary Save refuses a tombstoned profile");
    first->outbound->name = "DeletionBarrier";
    repo.SaveBatch({second, first});
    require(waitValue(db, a, "name", "DeletionBarrier"), "batch containing deleted and live rows completes");
    repo.GetProfile(a); // Takes the repo lock after the background save publishes its cache.
    repo.SaveTrafficBatch({second, first});
    check(favorite(db, b) == -1 && !repo.GetProfile(b) && favorite(db, a) == 1,
          "ordinary batch and traffic saves do not resurrect deleted favorites");
}

void testChunkBoundaries(Database &db, ProfilesRepo &repo, int gid) {
    const int count = Configs::BATCH_LIMIT_READ + 5;
    QList<ProfilePtr> items;
    QList<int> ids;
    for (int i = 0; i < count; ++i) items << profile(QStringLiteral("Chunk%1").arg(i), 2000 + i);
    require(repo.AddProfileBatch(items, gid), "seed profiles across insertion, replacement, update, and read chunk boundaries");
    for (const auto &item : items) ids << item->id;
    check(std::all_of(items.cbegin(), items.cend(), [](const auto &item) { return !item->IsFavorite(); }),
          "all rows across insertion chunks start as non-favorites");
    const int rejectedId = ids[Configs::BATCH_LIMIT_WRITE + 1];
    for (const bool target : {true, false}) {
        // Failure is after an entire update chunk could already have succeeded.
        db.execThrow("CREATE TEMP TRIGGER reject_favorite_update BEFORE UPDATE OF favorite ON profiles "
                     "WHEN NEW.id = " + std::to_string(rejectedId) + " AND NEW.favorite = " +
                     std::to_string(target ? 1 : 0) +
                     " BEGIN SELECT RAISE(ABORT, 'synthetic favorite failure'); END");
        check(!repo.SetFavorites(ids, target), "trigger-induced favorite write failure is reported");
        check(std::all_of(items.cbegin(), items.cend(), [target](const auto &item) {
                  return item->IsFavorite() != target;
              }), "failed favorite transaction leaves every retained cache object unchanged");
        const auto changed = scalar(db, "SELECT COUNT(*) FROM profiles WHERE gid = " + std::to_string(gid) +
                                       " AND favorite = " + std::to_string(target ? 1 : 0));
        check(changed == 0, "failed favorite transaction rolls back every earlier SQL chunk");
        db.execThrow("DROP TRIGGER reject_favorite_update");
        require(repo.SetFavorites(ids, target), "favorite batch succeeds after removing the failure trigger");
        check(std::all_of(items.cbegin(), items.cend(), [target](const auto &item) {
                  return item->IsFavorite() == target;
              }), "committed favorite transaction updates every retained cache object");
    }

    QList<int> selected;
    for (int i = 0; i < ids.size(); ++i) if (i % 3 == 0) selected << ids[i];
    require(repo.SetFavorites(selected, true), "seed mixed favorites across all SQL chunk boundaries");
    std::vector<Configs::ProfileInsertRow> staleRows;
    staleRows.reserve(items.size());
    for (const auto &item : items) staleRows.push_back(snapshot(item));
    require(repo.SetFavorites(ids, true), "favorite every row after cross-chunk snapshots were prepared");
    for (auto &row : staleRows) row.name = "ReplayedChunk";
    db.execBatchReplaceProfiles(staleRows);
    check(scalar(db, "SELECT COUNT(*) FROM profiles WHERE gid = " + std::to_string(gid) +
                     " AND favorite = 1 AND name = 'ReplayedChunk'") == count,
          "replacement chunks preserve later favorite commits at and beyond binding boundaries");

    // A different mixed pattern verifies ordinary SaveBatch's real async route.
    require(repo.SetFavorites(ids, false) && repo.SetFavorites(selected, true),
            "restore mixed favorites for ordinary cross-chunk saving");
    for (const auto &item : items) item->outbound->name = "AsyncChunkSaved";
    repo.SaveBatch(items);
    require(waitValue(db, ids.last(), "name", "AsyncChunkSaved"), "asynchronous SaveBatch completes all replacement chunks");
    repo.GetProfile(ids.last());
    check(scalar(db, "SELECT COUNT(*) FROM profiles WHERE gid = " + std::to_string(gid) + " AND favorite = 1") == selected.size(),
          "ordinary SaveBatch preserves the favorite count across every replacement chunk");
    std::vector<std::weak_ptr<Profile>> released;
    released.reserve(items.size());
    for (const auto &item : items) released.emplace_back(item);
    items.clear();
    // SaveBatch's captured QList may be destroyed on its worker thread. One
    // expired element does not prove the remaining cached objects are cold yet.
    require(waitFor([&] {
                return std::all_of(released.cbegin(), released.cend(), [](const auto &item) {
                    return item.expired();
                });
            }), "large fixture releases every cached object before a multi-chunk cold read");
    const auto loaded = repo.GetProfileBatch(ids);
    bool valid = loaded.size() == count;
    for (int i = 0; valid && i < loaded.size(); ++i)
        valid &= loaded[i]->id == ids[i] && loaded[i]->IsFavorite() == (i % 3 == 0) &&
                 payloadMatches(loaded[i], "AsyncChunkSaved");
    check(valid, "cold batch loading preserves order, payload, and mixed favorites beyond the read limit");
}

void testBackups(Database &source, ProfilesRepo &repo, int gid, const QString &root) {
    auto starred = profile("BackupStarred");
    auto ordinary = profile("BackupOrdinary");
    require(repo.AddProfile(starred, gid) && repo.AddProfile(ordinary, gid), "seed backup compatibility fixtures");
    const int a = starred->id;
    const int b = ordinary->id;
    require(repo.SetFavorites({a}, true), "seed a favorite in the backup source");
    source.execThrow("INSERT OR REPLACE INTO settings(key,value) VALUES ('favorite_test_marker','source')");
    const auto path = [&](const char *name) { return (root + "/" + name).toStdString(); };
    const auto full = path("full-current.db");
    const auto old = path("full-legacy.db");
    const auto profilesOnly = path("profiles-only.db");
    const auto settingsOnly = path("settings-only.db");
    Configs::BackupParts profiles;
    profiles.profiles = true;
    Configs::BackupParts settings;
    settings.settings = true;
    source.backupTo(full);
    source.backupTo(old);
    source.backupSelective(profilesOnly, profiles);
    source.backupSelective(settingsOnly, settings);
    {
        Database backup(full);
        ProfilesRepo reader(backup);
        check(favoriteSchema(backup) && reader.GetProfile(a)->IsFavorite() && !reader.GetProfile(b)->IsFavorite(),
              "full WAL-safe backup preserves the favorite schema and mixed values");
    }
    {
        Database backup(profilesOnly);
        ProfilesRepo reader(backup);
        check(reader.GetProfile(a)->IsFavorite() && !reader.GetProfile(b)->IsFavorite() &&
              scalar(backup, "SELECT COUNT(*) FROM settings") == 0,
              "profiles-only backup contains favorite values without unselected settings");
    }
    {
        Database backup(settingsOnly);
        check(scalar(backup, "SELECT COUNT(*) FROM profiles") == 0,
              "settings-only backup excludes all profile and favorite data");
    }
    {
        Database legacy(old);
        legacy.execThrow("ALTER TABLE profiles DROP COLUMN favorite");
        check(!hasFavoriteColumn(legacy), "old-version backup fixture genuinely lacks favorites");
    }
    {
        const auto targetPath = path("restore-current.db");
        source.backupTo(targetPath);
        Database target(targetPath);
        target.execThrow("UPDATE profiles SET favorite = 0");
        target.execThrow("UPDATE settings SET value = 'target' WHERE key = 'favorite_test_marker'");
        check(target.restoreSelective(full, profiles) == 0, "new-to-new selective profile restore succeeds");
        {
            ProfilesRepo reader(target);
            check(reader.GetProfile(a)->IsFavorite() && !reader.GetProfile(b)->IsFavorite(),
                  "new-to-new restore and cold reads preserve mixed favorites");
        }
        check(scalar(target, "SELECT COUNT(*) FROM settings WHERE key = 'favorite_test_marker' AND value = 'target'") == 1,
              "profile restore leaves unselected settings alone");
        target.execThrow("UPDATE profiles SET favorite = 1");
        check(target.restoreSelective(old, profiles) == 0, "old-to-new selective profile restore succeeds");
        {
            ProfilesRepo reader(target);
            check(favoriteSchema(target) && !reader.GetProfile(a)->IsFavorite() && !reader.GetProfile(b)->IsFavorite() &&
                  scalar(target, "SELECT COUNT(*) FROM profiles WHERE favorite <> 0") == 0,
                  "old backup rows default to non-favorite instead of inheriting target flags");
        }
        check(target.restoreSelective(profilesOnly, profiles) == 0 && favorite(target, a) == 1 && favorite(target, b) == 0,
              "profiles-only restore restores mixed favorites after an older backup");
        check(target.restoreSelective(settingsOnly, settings) == 0 && favorite(target, a) == 1 && favorite(target, b) == 0,
              "settings-only restore preserves all existing profile favorite values");
        check(scalar(target, "SELECT COUNT(*) FROM settings WHERE key = 'favorite_test_marker' AND value = 'source'") == 1,
              "settings-only restore actually applies its selected settings");
        check(scalar(target, "SELECT COUNT(*) FROM pragma_foreign_key_check") == 0,
              "cross-version and selective restores leave valid foreign-key references");
    }
    {
        const auto targetPath = path("restore-legacy.db");
        source.backupTo(targetPath);
        Database target(targetPath);
        target.execThrow("ALTER TABLE profiles DROP COLUMN favorite");
        check(target.restoreSelective(full, profiles) == 0 && !hasFavoriteColumn(target),
              "new-to-old restore copies common columns without requiring the unknown favorite column");
        ProfilesRepo migrated(target);
        const auto loaded = migrated.GetProfileBatch({a, b});
        check(favoriteSchema(target) && loaded.size() == 2 &&
              !loaded[0]->IsFavorite() && !loaded[1]->IsFavorite() &&
              payloadMatches(loaded[0], "BackupStarred") && payloadMatches(loaded[1], "BackupOrdinary"),
              "old destination can subsequently migrate with safe defaults and preserved payload");
    }
    {
        Database restored(path("restore-full.db"));
        restored.restoreFrom(full);
        ProfilesRepo reader(restored);
        check(reader.GetProfile(a)->IsFavorite() && !reader.GetProfile(b)->IsFavorite(),
              "full snapshot restore also preserves mixed favorite values");
    }
}
} // namespace

int main(int argc, char **argv) {
    QTemporaryDir isolated(QDir::currentPath() + "/favorite-fixture-XXXXXX");
    if (!isolated.isValid()) return 2;
    qputenv("HOME", isolated.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", isolated.path().toUtf8());
    qputenv("XDG_DATA_HOME", isolated.path().toUtf8());
    qputenv("XDG_CACHE_HOME", isolated.path().toUtf8());
    QCoreApplication app(argc, argv);
    QDir::setCurrent(isolated.path());
    Configs::SetBasePath(isolated.path());
    SetDeviceIdStore(isolated.path());
    SetNoticeSink([](const Notice &) {});
    try {
        Configs::initDB((isolated.path() + "/test.db").toStdString());
        auto &db = Configs::dataManager->getDatabase();
        auto &repo = *Configs::dataManager->profilesRepo;
        auto &settings = Configs::dataManager->settingsRepo;
        settings->net_use_proxy = false;
        settings->sub_send_hwid = false;
        check(favoriteSchema(db), "fresh production database has the non-null default-false favorite schema");
        testLegacyMigration(isolated.path());
        const auto group = [] {
            auto item = Configs::GroupsRepo::NewGroup();
            require(Configs::dataManager->groupsRepo->AddGroup(item), "create an isolated test group");
            return item;
        };
        testReadWriteAndRollback(db, repo, group()->id);
        testChunkBoundaries(db, repo, group()->id);
        testBackups(db, repo, group()->id, isolated.path());
        check(!settings->core_running && settings->started_id < 0 && !settings->spmode_vpn && !settings->spmode_system_proxy,
              "all persistence tests leave core, VPN, and system proxy modes stopped");
    } catch (const std::exception &error) {
        check(false, "test fixture completed without an unexpected exception");
        std::fprintf(stderr, "EXCEPTION %s\n", error.what());
    }
    std::printf("RESULT assertions=%d failures=%d\n", assertions, failures);
    std::fflush(stdout);
    // Application globals intentionally outlive the harness, as in the other
    // production-object tests. The runner owns cleanup of the outer temp folder.
    std::_Exit(failures ? 1 : 0);
}
