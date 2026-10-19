// Real MainWindow/header/model regression. No EngineHost::init(), core or user configuration.
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTreeView>
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
#include "include/ui/utils/ConnectionsFilterHeader.h"
#include "include/ui/utils/ConnectionsTreeFilterProxyModel.h"
#include "include/ui/utils/ConnectionsTreeModel.h"

using Model = ConnectionsTreeModel;
static int checks = 0, failures = 0;
static void check(bool ok, const char *label) {
    ++checks; if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); std::fflush(stdout);
}
static void settle(int ms = 30) {
    QElapsedTimer timer; timer.start();
    do { QApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1); } while (timer.elapsed() < ms);
}
[[noreturn]] static void finish() {
    std::printf("RESULT assertions=%d failures=%d\n", checks, failures); std::fflush(stdout);
    std::_Exit(failures ? 1 : 0);
}
static void mouse(QWidget *widget, QEvent::Type type, const QPoint &pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(pos), QPointF(widget->mapToGlobal(pos)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}
static void drag(QHeaderView *header, int column, int delta) {
    const QPoint start(header->sectionViewportPosition(column) + header->sectionSize(column) - 1, 8);
    mouse(header->viewport(), QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(header->viewport(), QEvent::MouseMove, start + QPoint(delta, 0), Qt::NoButton, Qt::LeftButton);
    mouse(header->viewport(), QEvent::MouseButtonRelease, start + QPoint(delta, 0), Qt::LeftButton, Qt::NoButton);
    settle();
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
    settings->inbound_address = "127.0.0.1";
    for (const auto &list : Configs::dataManager->ipListsRepo->GetAllIpLists()) {
        list->auto_update = false; Configs::dataManager->ipListsRepo->SaveHeader(list);
    }
    settings->Save();
    API::defaultClient = new API::Client;
    LogThread = new QThread; LogThread->start();
    Engine::RuntimeOptions options; options.systemProxy = false; options.resume = false;
    options.blockingPrivilegeCheck = false; options.exitCountryLookup = false;
    Engine::EngineHost::create(options);
    auto *window = new MainWindow;
    auto *view = window->findChild<QTreeView *>("connections");
    auto *tabs = window->findChild<QTabWidget *>("stats_widget");
    check(view && tabs, "actual connection widgets are present");
    if (!view || !tabs) finish();
    auto *header = qobject_cast<ConnectionsFilterHeader *>(view->header());
    auto *proxy = qobject_cast<ConnectionsTreeFilterProxyModel *>(view->model());
    auto *model = proxy ? proxy->treeModel() : nullptr;
    check(header && model, "actual connection header and model are present");
    if (!header || !model) finish();
    tabs->setCurrentWidget(window->findChild<QWidget *>("connections_tab"));
    window->resize(1250, 700); window->show(); settle();

    const QString longName = QString::fromUtf8("Long outbound 👨‍👩‍👧‍👦 é ") + QString(70, QChar('x'));
    Stats::ConnectionMetadata first{};
    first.id = "one"; first.outbound = "proxy-one"; first.outboundName = longName;
    first.process = "first-process"; first.dest = "example.test:443";
    first.network = "tcp"; first.protocol = "tls"; first.sourceDisplay = "192.0.2.20";
    first.upload = 123; first.download = 456;
    auto second = first; second.id = "two"; second.process = "second-process";
    second.outbound = "proxy-two"; second.outboundName = "Short outbound";
    QList<Stats::ConnectionMetadata> connections{first, second};
    window->UpdateConnectionList(connections); view->expandAll(); settle();
    check(header->sectionResizeMode(Model::ColOutbound) == QHeaderView::Fixed, "default keeps automatic outbound width");
    check(header->sectionResizeMode(Model::ColTarget) == QHeaderView::Stretch, "default keeps stretched Target");
    check(view->isColumnHidden(Model::ColSource), "loopback keeps Source hidden");
    check(model->index(0, Model::ColOutbound).data().toString() != longName, "automatic mode keeps existing short display name");
    check(model->index(0, Model::ColOutbound).data(Qt::ToolTipRole).toString() == longName, "automatic mode retains full tooltip");

    auto withMenu = [&](int column, const std::function<void(QMenu *)> &run) {
        QTimer::singleShot(0, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) std::abort();
            run(menu); if (menu->isVisible()) menu->close();
        });
        const QPoint point(header->sectionViewportPosition(column) + 5, 5);
        QMetaObject::invokeMethod(header, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, point));
        settle();
    };
    bool available = false;
    withMenu(Model::ColTraffic, [&](QMenu *menu) {
        auto *action = menu->findChild<QAction *>("actionManualConnectionWidths");
        available = action != nullptr;
        check(available, "header offers Manual Column Widths");
    });
    if (!available) finish(); // Unchanged probe intentionally fails on exact dev.
#ifdef THRONE_WIDTHS_PROBE_ONLY
    finish();
#else
    auto action = [&](int column, const char *name) {
        withMenu(column, [&](QMenu *menu) {
            auto *item = menu->findChild<QAction *>(name);
            check(item != nullptr, "requested width action is available");
            if (item) item->trigger();
        });
    };
    check(!settings->connection_manual_widths, "manual mode defaults off in settings");
    for (int col : {Model::ColTarget, Model::ColProtocol, Model::ColOutbound, Model::ColTraffic, Model::ColSpeed}) {
        withMenu(col, [&](QMenu *menu) {
            check(menu->findChild<QAction *>("actionManualConnectionWidths") && menu->findChild<QAction *>("actionResetConnectionWidths"), "width actions are offered on every visible header");
            if (col == Model::ColTraffic || col == Model::ColSpeed) {
                int choices = 0; for (auto *item : menu->actions()) if (item->data().isValid()) ++choices;
                check(choices == 3, "existing traffic/speed sort choices survive");
            }
        });
    }
    action(Model::ColOutbound, "actionManualConnectionWidths");
    check(header->manualColumnWidths() && settings->connection_manual_widths, "menu enables and persists manual mode");
    check(model->index(0, Model::ColOutbound).data().toString() == longName, "manual process row displays complete Unicode outbound name");
    check(model->index(0, Model::ColOutbound, model->index(0, 0)).data().toString() == longName, "manual connection row displays complete outbound name");
    check(view->isColumnHidden(Model::ColSource), "manual mode does not reveal Source");
    for (int col : {Model::ColProtocol, Model::ColOutbound, Model::ColTraffic}) {
        const int before = header->sectionSize(col); drag(header, col, 35);
        check(header->sectionSize(col) == before + 35, "middle column resizes through actual mouse drag");
    }
    QHash<int, int> widths;
    for (int col : {Model::ColProtocol, Model::ColOutbound, Model::ColTraffic}) widths.insert(col, header->sectionSize(col));
    auto stable = [&] {
        for (auto it = widths.cbegin(); it != widths.cend(); ++it) if (header->sectionSize(it.key()) != it.value()) return false;
        return true;
    };
    first.download = 900000000000LL; first.downloadSpeed = 900000000000LL;
    connections[0] = first;
    window->UpdateConnectionList(connections); settle();
    check(stable(), "connection poll and growing traffic digits preserve manual widths");
    QMetaObject::invokeMethod(header, "sectionClicked", Qt::DirectConnection, Q_ARG(int, int(Model::ColOutbound)));
    window->UpdateConnectionList(connections); settle();
    check(stable() && settings->connection_sort == Stats::ByOutbound, "real sorting retains manual widths");
    withMenu(Model::ColTraffic, [&](QMenu *menu) {
        for (auto *item : menu->actions()) if (item->data().isValid() && item->data().toInt() == Stats::ByDownload) {
            menu->setActiveAction(item);
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(menu, &enter);
            break;
        }
    });
    check(stable() && settings->connection_sort == Stats::ByDownload, "traffic context menu still applies its chosen sort");
    view->collapseAll(); settle(); view->expandAll(); settle();
    check(stable(), "collapse and expansion retain manual widths");
    window->resize(1450, 740); settle(); window->resize(1050, 650); settle();
    check(stable(), "window resizing preserves manual middle widths");
    header->setFiltersVisible(true); settle();
    auto edits = header->findChildren<QLineEdit *>();
    QLineEdit *outboundFilter = nullptr;
    for (auto *edit : edits) if (edit->isVisible() && edit->x() == header->sectionViewportPosition(Model::ColOutbound) + 2) outboundFilter = edit;
    check(outboundFilter != nullptr, "outbound filter follows its manually sized column");
    if (outboundFilter) {
        outboundFilter->setText("Short outbound"); settle(100);
        check(proxy->rowCount() == 1, "real filter still selects matching process");
        check(stable(), "filtering retains manual widths");
        outboundFilter->clear(); settle(100);
    }
    check(proxy->rowCount() == 2, "clearing filter restores both processes");
    header->resizeSection(Model::ColOutbound, 1600); settle();
    view->horizontalScrollBar()->setValue(view->horizontalScrollBar()->maximum()); settle();
    check(view->horizontalScrollBar()->value() > 0, "wide manual columns support horizontal scrolling");
    check(outboundFilter && outboundFilter->x() == header->sectionViewportPosition(Model::ColOutbound) + 2, "filter remains aligned after horizontal scroll");
    if (outboundFilter) check(outboundFilter->width() == header->sectionSize(Model::ColOutbound) - 4, "filter width matches manual section");
    view->horizontalScrollBar()->setValue(0); header->resizeSection(Model::ColOutbound, widths[Model::ColOutbound]); settle();

    // Only synthetic settings change: EngineHost is never initialized and no listener is opened.
    settings->inbound_address = "192.0.2.1"; window->refresh_status(); settle();
    check(!view->isColumnHidden(Model::ColSource), "LAN configuration still reveals Source");
    QLineEdit *sourceFilter = nullptr;
    for (auto *edit : edits) if (edit->isVisible() && edit->x() == header->sectionViewportPosition(Model::ColSource) + 2) sourceFilter = edit;
    if (sourceFilter) { sourceFilter->setText("192.0.2.20"); settle(100); }
    const int sourceBefore = header->sectionSize(Model::ColSource); drag(header, Model::ColSource, 43);
    const int sourceWidth = header->sectionSize(Model::ColSource);
    check(sourceWidth == sourceBefore + 43, "Source resizes through actual mouse drag");
    QMetaObject::invokeMethod(header, "sectionClicked", Qt::DirectConnection, Q_ARG(int, int(Model::ColSource)));
    settings->inbound_address = "127.0.0.1"; window->refresh_status(); settle();
    check(view->isColumnHidden(Model::ColSource) && settings->connection_sort == Stats::Default, "hiding Source still clears its unreachable sort");
    check(sourceFilter && sourceFilter->text().isEmpty(), "hiding Source still clears its unreachable filter");
    settings->inbound_address = "192.0.2.1"; window->refresh_status(); settle();
    check(header->sectionSize(Model::ColSource) == sourceWidth, "Source retains its width across hide/show");
    header->setFiltersVisible(false); settle(250);
    const QString saved = settings->connection_column_widths;
    Configs::SettingsRepo reread(Configs::dataManager->getDatabase());
    check(reread.connection_manual_widths && reread.connection_column_widths == saved, "debounced database save round-trips mode and named widths");
    const auto savedObject = QJsonDocument::fromJson(saved.toUtf8()).object();
    check(savedObject.size() == 4 && !savedObject.contains("target") && !savedObject.contains("speed"), "saved layout contains only four logical middle-column widths");
    action(Model::ColProtocol, "actionManualConnectionWidths");
    check(!header->manualColumnWidths() && header->sectionResizeMode(Model::ColOutbound) == QHeaderView::Fixed, "unticking manual restores automatic modes");
    check(model->index(0, Model::ColOutbound).data().toString() != longName, "automatic mode restores shortened outbound display");
    action(Model::ColProtocol, "actionManualConnectionWidths");
    check(stable() && header->sectionSize(Model::ColSource) == sourceWidth, "reenabling manual restores remembered widths");

    // Recreate the real MainWindow from saved settings, retaining isolated DB and uninitialized host.
    settings->connection_manual_widths = reread.connection_manual_widths;
    settings->connection_column_widths = reread.connection_column_widths;
    settings->inbound_address = "127.0.0.1"; settings->Save();
    window->hide();
    auto *restoredWindow = new MainWindow;
    auto *restoredView = restoredWindow->findChild<QTreeView *>("connections");
    auto *restored = qobject_cast<ConnectionsFilterHeader *>(restoredView->header());
    restoredWindow->findChild<QTabWidget *>("stats_widget")->setCurrentWidget(restoredWindow->findChild<QWidget *>("connections_tab"));
    restoredWindow->resize(1250, 700); restoredWindow->show(); settle();
    check(restored->manualColumnWidths() && restored->sectionSize(Model::ColOutbound) == widths[Model::ColOutbound], "recreated window restores manual mode and width");
    check(restoredView->isColumnHidden(Model::ColSource), "restored widths do not override Source visibility");
    restoredWindow->UpdateConnectionList(connections); settle();
    auto *restoredProxy = qobject_cast<ConnectionsTreeFilterProxyModel *>(restoredView->model());
    check(restoredProxy->treeModel()->index(0, Model::ColOutbound).data().toString() == longName, "restored manual mode displays full names on the next poll");
    restoredWindow->hide(); window->show(); window->refresh_status(); settle();

    action(Model::ColTraffic, "actionResetConnectionWidths");
    check(!header->manualColumnWidths() && !settings->connection_manual_widths, "Reset returns to automatic sizing");
    check(QJsonDocument::fromJson(settings->connection_column_widths.toUtf8()).object().isEmpty(), "Reset forgets saved manual widths");
    check(header->sectionResizeMode(Model::ColTraffic) == QHeaderView::ResizeToContents && header->sectionResizeMode(Model::ColOutbound) == QHeaderView::Fixed, "Reset restores original resize policies");
    check(header->sectionSize(Model::ColOutbound) < 1600, "automatic reset removes oversized manual section");
    header->restoreColumnWidths(QStringLiteral("{\"source\":-5,\"protocol\":12.5,\"outbound\":99999999,\"traffic\":\"bad\",\"target\":120}"));
    check(QJsonDocument::fromJson(header->savedColumnWidths().toUtf8()).object().isEmpty(), "negative fractional oversized wrong-type and unknown widths are ignored");
    header->restoreColumnWidths(QStringLiteral("not json"));
    check(QJsonDocument::fromJson(header->savedColumnWidths().toUtf8()).object().isEmpty(), "malformed saved layout is ignored");
    header->restoreColumnWidths(QStringLiteral("{\"protocol\":1,\"outbound\":333,\"future\":444}"));
    header->setManualColumnWidths(true); settle();
    check(header->sectionSize(Model::ColOutbound) == 333, "valid named width survives invalid siblings");
    check(header->sectionSize(Model::ColProtocol) >= header->minimumSectionSize(), "too-small width is clamped to Qt minimum");
    check(view->isColumnHidden(Model::ColSource), "invalid/restored settings never change Source visibility");

    // Double-clicking a resize handle asks Qt to fit even unusually long content.
    auto enormous = first; enormous.outboundName = QString(5000, QChar('W'));
    window->UpdateConnectionList({enormous}); view->expandAll(); settle();
    view->horizontalScrollBar()->setValue(0); settle();
    const QPoint handle(header->sectionViewportPosition(Model::ColOutbound) + header->sectionSize(Model::ColOutbound) - 1, 8);
    mouse(header->viewport(), QEvent::MouseButtonDblClick, handle, Qt::LeftButton, Qt::LeftButton);
    mouse(header->viewport(), QEvent::MouseButtonRelease, handle, Qt::LeftButton, Qt::NoButton); settle();
    check(header->sectionSize(Model::ColOutbound) == 16384, "actual double-click of huge outbound is bounded");
    const auto bounded = header->savedColumnWidths();
    check(QJsonDocument::fromJson(bounded.toUtf8()).object().value("outbound").toInt() == 16384, "live and saved oversized widths use the same bound");
    header->setManualColumnWidths(false); header->restoreColumnWidths(bounded); header->setManualColumnWidths(true); settle();
    check(header->sectionSize(Model::ColOutbound) == 16384, "bounded double-click width survives restore");

    header->resetColumnWidths();
    enormous.protocol = QString(5000, QChar('W'));
    window->UpdateConnectionList({enormous}); view->expandAll(); settle();
    check(header->sectionSize(Model::ColProtocol) > 16384, "automatic huge-content fixture exercises manual-mode seeding");
    header->setManualColumnWidths(true); settle();
    check(header->sectionSize(Model::ColProtocol) == 16384, "automatic-to-manual seed is bounded");
    check(QJsonDocument::fromJson(header->savedColumnWidths().toUtf8()).object().value("protocol").toInt() == 16384, "bounded initial width is also saved consistently");
    header->resetColumnWidths(); window->UpdateConnectionList(connections); settle();
    header->setManualColumnWidths(true); settle();
    const auto screenshotDirectory = qEnvironmentVariable("THRONE_TEST_SCREENSHOT_DIR");
    if (!screenshotDirectory.isEmpty()) {
        window->resize(1800, 740); header->resizeSection(Model::ColOutbound, 1050);
        header->resizeSection(Model::ColProtocol, 120); header->resizeSection(Model::ColTraffic, 210); settle();
        window->grab().save(screenshotDirectory + "/manual-widths.png");
    }
    header->resetColumnWidths(); settle();
    check(!header->manualColumnWidths(), "repeated reset remains usable");
    if (!screenshotDirectory.isEmpty()) window->grab().save(screenshotDirectory + "/automatic-widths.png");
    finish();
#endif
}
