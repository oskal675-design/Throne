#include "include/global/Utils.hpp"

#include <QAbstractEventDispatcher>
#include <QElapsedTimer>
#include <QEvent>
#include <QPointer>
#include <QThread>

#include <array>
#include <atomic>
#include <cstdio>
#include <vector>

// Observe real timer objects through public Qt APIs. Callback suppression alone
// does not catch an orphaned timer after its context has been destroyed.
class TimerApplication : public QCoreApplication {
public:
    using QCoreApplication::QCoreApplication;
    std::vector<QPointer<QObject>> timers;
    bool coarseTimers = true;
    std::atomic_bool observeTimers{true};

    bool notify(QObject *receiver, QEvent *event) override {
        if (observeTimers.load() && event->type() == QEvent::Timer) {
            timers.emplace_back(receiver);
            for (const auto &timer : QAbstractEventDispatcher::instance()->registeredTimers(receiver)) {
                coarseTimers &= timer.timerType == Qt::CoarseTimer;
            }
        }
        return QCoreApplication::notify(receiver, event);
    }

    bool timersDestroyed() const {
        for (const auto &timer : timers) {
            if (timer) return false;
        }
        return true;
    }

    void reset() {
        timers.clear();
        coarseTimers = true;
    }
};

static int failures = 0;
static void check(bool condition, const char *label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

static void pump(int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    } while (elapsed.elapsed() < milliseconds);
}

int main(int argc, char **argv) {
    TimerApplication app(argc, argv);
    QObject context;
    int calls = 0;
    setTimeout([&] { ++calls; }, &context);
    check(calls == 0, "default zero delay is asynchronous");
    pump(20);
    check(calls == 1, "default zero delay calls back exactly once");
    check(app.timersDestroyed(), "default zero delay releases its timer");
    app.reset();

    auto canceled = new QObject;
    calls = 0;
    setTimeout([&] { ++calls; }, canceled, 0);
    delete canceled;
    pump(20);
    check(calls == 0, "destroyed context cancels a zero-delay callback");
    check(app.timersDestroyed(), "canceled zero-delay timer is released");
    app.reset();

    calls = 0;
    setTimeout([&] { ++calls; }, nullptr);
    setTimeout([&] { ++calls; }, nullptr, 20);
    pump(100);
    check(calls == 0, "null contexts never run callbacks");
    check(app.timersDestroyed(), "null contexts leave no orphaned timers");
    app.reset();

    // These are the three delays used by the current production callers.
    constexpr std::array delays{200, 300, 2000};
    std::array<int, delays.size()> counts{};
    std::array<qint64, delays.size()> firedAt{};
    QElapsedTimer elapsed;
    elapsed.start();
    bool correctThread = true;
    for (size_t i = 0; i < delays.size(); ++i) {
        setTimeout([&, i] {
            ++counts[i];
            firedAt[i] = elapsed.elapsed();
            correctThread &= QThread::currentThread() == context.thread();
        }, &context, delays[i]);
    }
    check(counts == std::array<int, delays.size()>{}, "nonzero delays are asynchronous");
    pump(4300);
    check(counts == std::array{1, 1, 1}, "200, 300 and 2000 ms callbacks each run once");
    bool delayed = true;
    for (size_t i = 0; i < delays.size(); ++i) delayed &= firedAt[i] >= delays[i] * 9 / 10;
    check(delayed, "nonzero callbacks respect the requested delay");
    check(correctThread, "same-thread callbacks run on the context thread");
    check(app.timers.size() == delays.size(), "each nonzero timer fires once");
    check(app.coarseTimers, "nonzero timers retain the coarse timer policy");
    check(app.timersDestroyed(), "completed nonzero timers are released");
    app.reset();

    calls = 0;
    for (int delay : delays) {
        canceled = new QObject;
        setTimeout([&] { ++calls; }, canceled, delay);
        delete canceled;
    }
    pump(2300);
    check(calls == 0, "destroyed contexts cancel every production delay");
    check(app.timersDestroyed(), "canceled timers are released after expiry");
    app.reset();

    // Repeated dialog-close cycles must not accumulate one orphan per timeout.
    for (int i = 0; i < 100; ++i) {
        canceled = new QObject;
        setTimeout([&] { ++calls; }, canceled, 20);
        delete canceled;
    }
    pump(100);
    check(calls == 0, "repeated canceled callbacks never run");
    check(app.timersDestroyed(), "repeated cancellation leaves no orphaned timers");
    app.reset();

    auto selfDeleting = new QObject;
    calls = 0;
    setTimeout([&] { ++calls; delete selfDeleting; }, selfDeleting, 20);
    pump(100);
    check(calls == 1, "a callback can destroy its own context");
    check(app.timersDestroyed(), "self-deleting context leaves no timer behind");

    // notify() also receives events on the worker. Do not mutate the main-thread
    // lifetime-observation vector while testing cross-thread callback delivery.
    app.observeTimers.store(false);
    QThread worker;
    auto workerContext = new QObject;
    workerContext->moveToThread(&worker);
    QObject::connect(&worker, &QThread::finished, workerContext, &QObject::deleteLater);
    worker.start();
    std::atomic_int workerCalls{0};
    std::atomic_bool workerAffinity{false};
    setTimeout([&] {
        workerAffinity.store(QThread::currentThread() == &worker);
        ++workerCalls;
    }, workerContext, 20);
    QElapsedTimer workerWait;
    workerWait.start();
    while (workerCalls.load() == 0 && workerWait.elapsed() < 2000) pump(10);
    pump(100);
    worker.quit();
    worker.wait();
    app.observeTimers.store(true);
    check(workerCalls.load() == 1, "cross-thread callback runs exactly once");
    check(workerAffinity.load(), "cross-thread callback runs on the context thread");

    return failures == 0 ? 0 : 1;
}
