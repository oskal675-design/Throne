// Exercises actual profile widgets, repositories, imports and refresh with disposable state.
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QItemSelectionModel>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <atomic>
#include "include/configs/sub/GroupUpdater.hpp"
#include "include/database/GroupsRepo.h"
#include "include/database/ProfilesRepo.h"
#include "include/ui/utils/ProfilesTableFilterHeader.h"
#include "include/ui/utils/ProfilesTableView.h"
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
static void finish() {
    std::printf("RESULT assertions=%d failures=%d\n", checks, failures); std::fflush(stdout);
    // The normal app owns permanent worker loops; the Python runner removes all temporary state.
    std::_Exit(failures ? 1 : 0);
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

static std::shared_ptr<Configs::Profile> addProfile(int gid, int port, const QString& name) {
    auto profile = Configs::ProfilesRepo::NewProfile("socks");
    if (!profile->outbound->ParseFromLink(QStringLiteral("socks://127.0.0.1:%1#%2").arg(port).arg(name)) ||
        !Configs::dataManager->profilesRepo->AddProfile(profile, gid)) std::abort();
    return profile;
}
static std::shared_ptr<Configs::Group> newGroup() {
    auto group = Configs::GroupsRepo::NewGroup();
    if (!Configs::dataManager->groupsRepo->AddGroup(group)) std::abort();
    return group;
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
    settings->skip_delete_confirmation = true; settings->sub_clear = false;
    settings->allow_stopping_active_profile = false;
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    auto& repo = Configs::dataManager->profilesRepo;
    auto& groups = Configs::dataManager->groupsRepo;
    auto group = newGroup(), other = newGroup();
    auto alpha = addProfile(group->id, 1080, "Alpha"), beta = addProfile(group->id, 1081, "Beta"),
         unicode = addProfile(group->id, 1082, QString::fromUtf8("日本 東京")), outside = addProfile(other->id, 1083, "Other");
    settings->current_group = group->id;
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
    window->show_group(group->id);
    window->resize(900, 650); window->show(); QApplication::processEvents();
    auto* table = window->findChild<ProfilesTableView*>("profilesTableView");
    auto* add = window->findChild<QAction*>("actionAddToFavorites");
    auto* remove = window->findChild<QAction*>("actionRemoveFromFavorites");
    auto* only = window->findChild<QToolButton*>("btnFavoritesOnly");
    auto* menu = window->findChild<QMenu*>("menu_server");
    check(table && menu, "actual profile table and menu exist");
    check(add && remove && only, "favorite actions and filter are available");
    if (!table || !menu || !add || !remove || !only) finish();
#ifndef FAVORITES_BASELINE_PROBE
    auto* proxy = qobject_cast<ProfilesFilterProxyModel*>(table->model());
    auto* model = qobject_cast<ProfilesTableModel*>(proxy->sourceModel());
    auto* header = qobject_cast<ProfilesTableFilterHeader*>(table->horizontalHeader());
    auto visible = [&] {
        QList<int> ids;
        for (int row=0; row<table->model()->rowCount(); ++row)
            ids.append(table->model()->index(row,0).data(ProfilesTableModel::ProfileIdRole).toInt());
        return ids;
    };
    auto select = [&](const QList<int>& ids) {
        table->clearSelection();
        for (int row=0; row<table->model()->rowCount(); ++row) {
            const auto index=table->model()->index(row,0);
            if (ids.contains(index.data(ProfilesTableModel::ProfileIdRole).toInt()))
                table->selectionModel()->select(index,QItemSelectionModel::Select|QItemSelectionModel::Rows);
        }
        QMetaObject::invokeMethod(menu,"aboutToShow",Qt::DirectConnection);
    };
    const auto originalOrder = group->Profiles();
    const auto originalLink = alpha->outbound->ExportJsonLink();
    const auto originalIdentity = alpha->outbound->ExportIdentity();
    const auto originalJson = alpha->outbound->ExportToJson();
    const auto originalConfig = alpha->outbound->Build().object;
    check(!only->isChecked() && visible()==originalOrder, "favorites view starts off and preserves current order");
    select({}); check(!add->isEnabled()&&!remove->isEnabled(), "empty selection disables both mutations");
    select({alpha->id,unicode->id});
    check(add->isEnabled()&&!remove->isEnabled(), "ordinary selection offers adding favorites");
    add->trigger();
    check(alpha->IsFavorite()&&unicode->IsFavorite()&&!beta->IsFavorite()&&!outside->IsFavorite(), "multi-selection changes exactly selected profiles");
    check(group->Profiles()==originalOrder, "adding favorites does not pin or reorder profiles");
    check(model->index(model->indexOfProfile(alpha->id),ProfilesTableModel::ColName).data().toString()==QString::fromUtf8("★ Alpha"), "favorite star is displayed beside raw name");
    check(model->filterKeyAt(model->indexOfProfile(alpha->id))->name=="Alpha", "filter index retains raw name without decoration");
    check(alpha->outbound->ExportJsonLink()==originalLink && alpha->outbound->ExportIdentity()==originalIdentity &&
          alpha->outbound->ExportToJson()==originalJson && alpha->outbound->Build().object==originalConfig,
          "favorites do not alter share links, identity, JSON or core outbound configuration");
    select({alpha->id,beta->id}); check(add->isEnabled()&&remove->isEnabled(), "mixed selection has explicit add and remove actions");
    only->click();
    check(visible()==QList<int>{alpha->id,unicode->id} && proxy->hasActiveFilter(), "favorite filter keeps current-group matching order");
    table->rowsSwapped(0,1);
    check(group->Profiles()==originalOrder, "manual drag cannot reorder a favorites-filtered list");
    header->setFiltersVisible(true);
    auto filterEdits=header->findChildren<QLineEdit*>();
    auto* nameFilter=filterEdits.at(2);
    nameFilter->setText("ALPHA");
    check(waitFor([&]{return visible()==QList<int>{alpha->id};}), "favorite filter composes with case-insensitive name filter");
    nameFilter->setText(QString::fromUtf8("東京"));
    check(waitFor([&]{return visible()==QList<int>{unicode->id};}), "favorite filter composes with Unicode names");
    nameFilter->setText("missing");
    check(waitFor([&]{return visible().isEmpty();}), "combined filters can show no matches");
    nameFilter->clear();
    check(waitFor([&]{return visible()==QList<int>{alpha->id,unicode->id};}), "clearing text filter retains favorites-only view");
    select({alpha->id,unicode->id}); remove->trigger();
    check(visible().isEmpty() && !alpha->IsFavorite()&&!unicode->IsFavorite(), "removal captures every selected ID before rows disappear");
    check(group->Profiles()==originalOrder, "filtering and unfavoriting preserve manual order");
    only->click(); select({beta->id}); add->trigger();
    check(beta->IsFavorite()&&!alpha->IsFavorite(), "selection maps filtered proxy rows to correct profile IDs");
    only->click(); window->show_group(other->id);
    check(visible().isEmpty()&&only->isChecked(), "favorites-only follows current group without a global tab");
    only->click(); select({outside->id}); add->trigger(); only->click();
    check(visible()==QList<int>{outside->id}, "second group's own favorite is displayed");
    window->show_group(group->id);
    check(visible()==QList<int>{beta->id}, "changing back restores first group's favorites");

    // Existing group order can contain duplicate slots for one stored profile.
    { QMutexLocker locker(&group->mutex); group->profiles.append(beta->id); }
    groups->Save(group); window->show_group(group->id);
    check(visible()==QList<int>{beta->id,beta->id}, "favorites view includes existing duplicate slots");
    select({beta->id}); remove->trigger();
    check(visible().isEmpty()&&!beta->IsFavorite(), "unfavoriting removes every duplicate slot from the filtered view");
    only->click(); select({beta->id}); add->trigger(); only->click();
    { QMutexLocker locker(&group->mutex); group->profiles=originalOrder; }
    groups->Save(group); window->show_group(group->id);

    // A refused SQL mutation must leave the visible star and selection intact.
    auto& db=Configs::dataManager->getDatabase();
    db.execThrow("CREATE TRIGGER reject_favorite BEFORE UPDATE OF favorite ON profiles BEGIN SELECT RAISE(ABORT,'synthetic toggle failure'); END");
    select({beta->id}); bool warning=false;
    QTimer closer; closer.setInterval(10);
    QObject::connect(&closer,&QTimer::timeout,[&] {
        if (auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) { warning=true; box->accept(); }
    });
    closer.start(); remove->trigger(); closer.stop();
    check(warning&&beta->IsFavorite()&&visible()==QList<int>{beta->id}, "failed toggle warns and retains visible favorite");
    db.execThrow("DROP TRIGGER reject_favorite");
    remove->trigger(); check(!beta->IsFavorite()&&visible().isEmpty(), "toggle retry succeeds after failure is removed");

    only->click(); select({alpha->id}); add->trigger();
    const auto beforeDrag=group->Profiles();
    table->rowsSwapped(0,2);
    check(group->Profiles()!=beforeDrag&&alpha->IsFavorite(), "unfiltered manual reorder still works and keeps favorites");
    { QMutexLocker locker(&group->mutex); group->profiles=beforeDrag; }
    groups->Save(group); window->show_group(group->id);
    // Edit and move use the same existing-row Save/membership contract as the editor.
    alpha->outbound->name="Edited"; repo->Save(alpha);
    check(alpha->IsFavorite()&&repo->GetProfile(alpha->id)->IsFavorite(), "editing keeps favorite metadata");
    group->RemoveProfile(alpha->id); other->AddProfile(alpha->id); alpha->gid=other->id;
    groups->Save(group); groups->Save(other); repo->Save(alpha);
    check(alpha->IsFavorite()&&repo->GetProfile(alpha->id)->gid==other->id, "moving preserves ID-bound favorite");
    window->show_group(other->id); select({alpha->id});
    auto* clone=window->findChild<QAction*>("menu_clone");
    auto beforeClone=other->Profiles(); bool cloneConfirmed=false;
    QTimer cloneConfirmation; cloneConfirmation.setInterval(10);
    QObject::connect(&cloneConfirmation,&QTimer::timeout,[&] {
        if (auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            cloneConfirmed=true; box->button(QMessageBox::Yes)->click();
        }
    });
    cloneConfirmation.start(); clone->trigger(); cloneConfirmation.stop();
    check(cloneConfirmed, "actual clone confirmation is accepted for the synthetic fixture");
    check(waitFor([&]{return Subscription::updater()->IsIdle()&&other->Profiles().size()==beforeClone.size()+1;}), "actual clone action completes");
    auto copy=repo->GetProfile(other->Profiles().last());
    check(copy&&copy->id!=alpha->id&&!copy->IsFavorite()&&alpha->IsFavorite(), "cloned profile is a new unfavorited ID");
    check(copy->outbound->ExportIdentity()==alpha->outbound->ExportIdentity(), "favorite does not alter cloned identity");
    select({alpha->id}); auto* del=window->findChild<QAction*>("menu_delete"); del->trigger();
    check(!repo->GetProfile(alpha->id)&&!other->HasProfile(alpha->id), "favorite is deleted by ordinary GUI delete");

    Fixture fixture; auto subscribed=newGroup(); subscribed->url=fixture.url(); subscribed->sub_options.keep_working=false;
    fixture.body="socks://127.0.0.2:1080#Before\n";
    auto outcome=refresh(subscribed->id);
    check(outcome.ok()&&subscribed->Profiles().size()==1, "actual subscription import creates synthetic profile");
    auto retained=repo->GetProfile(subscribed->Profiles().first()); const int retainedId=retained->id;
    check(!retained->IsFavorite()&&repo->SetFavorites({retainedId},true), "new subscription ID starts ordinary and can be favorited");
    outcome=refresh(subscribed->id);
    check(outcome.ok()&&subscribed->Profiles()==QList<int>{retainedId}&&retained->IsFavorite(), "unchanged refresh retains favorite and ID");
    fixture.body="socks://127.0.0.2:1080#After\n"; outcome=refresh(subscribed->id);
    check(outcome.ok()&&outcome.updated==1&&subscribed->Profiles()==QList<int>{retainedId}&&retained->IsFavorite(), "identity-matched content refresh retains favorite");
    fixture.body+="socks://127.0.0.3:1080#New\n"; outcome=refresh(subscribed->id);
    check(outcome.ok()&&outcome.added==1&&!repo->GetProfile(subscribed->Profiles().last())->IsFavorite(), "genuinely new subscription identity is not automatically favorited");
    settings->sub_clear=true; outcome=refresh(subscribed->id); settings->sub_clear=false;
    check(outcome.ok()&&!subscribed->Profiles().contains(retainedId)&&!repo->GetProfile(retainedId), "clear-before-refresh recreates IDs under existing semantics");
    bool recreatedOrdinary=true; for(auto p:repo->GetProfileBatch(subscribed->Profiles())) recreatedOrdinary &= !p->IsFavorite();
    check(recreatedOrdinary, "recreated subscription IDs start unfavorited");
    check(!settings->core_running&&settings->started_id<0&&!settings->spmode_vpn&&!settings->spmode_system_proxy, "no core or VPN/proxy mode started");
#endif
    finish();
}
