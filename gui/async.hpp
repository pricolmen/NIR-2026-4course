#pragma once
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <optional>
#include <exception>

namespace telemetry::gui {
template<class T> struct Outcome { std::optional<T> value; QString error; };
// Work captures values only; the callback runs on the owner's Qt event loop.
template<class T, class Work, class Done>
void background(QObject* owner, Work work, Done done) {
    auto* watcher = new QFutureWatcher<Outcome<T>>(owner);
    QObject::connect(watcher,&QFutureWatcher<Outcome<T>>::finished,owner,[watcher,done] {
        const auto result = watcher->result();
        watcher->deleteLater();
        done(result);
    });
    watcher->setFuture(QtConcurrent::run([work] {
        try { return Outcome<T>{work(),{}}; }
        catch (const std::exception& error) { return Outcome<T>{{},QString::fromUtf8(error.what())}; }
        catch (...) { return Outcome<T>{{},"Неизвестная ошибка чтения файлов"}; }
    }));
}
} // namespace telemetry::gui
