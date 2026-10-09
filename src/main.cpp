#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QFontDatabase>
#include <QIcon>
#include <QQuickWindow>
#include <QTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include "library.h"
#include "thumbnailprovider.h"
#include "tmdb.h"

// Dev hook QTFLIX_SELFTEST=progress (run it twice, headless): the first run sets progress on the first title,
// checks the models and the file written by flushProgress(), then quits with a newer position that only the
// shutdown path writes; the second run checks that position was restored and clears it again.
static void selfTestProgress(Library &lib)
{
    const QString path = lib.allTitles()->get(0).value(QStringLiteral("path")).toString();
    const QString id = lib.titleIdForPath(path);
    auto fail = [](const char *what) { qWarning("[selftest] FAILED: %s", what); QCoreApplication::exit(2); };
    if (path.isEmpty())
        return fail("library is empty");
    if (lib.position(path) == 43000) {
        lib.clearProgress(path);
        qInfo("[selftest] progress restored after restart: OK (cleared again)");
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }
    lib.setProgress(path, 42000, 100000);
    if (!lib.continueWatching()->ids().contains(id) || lib.title(id).value(QStringLiteral("progress")).toDouble() != 0.42)
        return fail("models not updated");
    lib.flushProgress();
    QTimer::singleShot(500, qApp, [&lib, path, fail] {
        QFile f(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/watchstate.json"));
        const QJsonObject o = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
        if (o.value(QStringLiteral("progress")).toObject().value(path).toObject().value(QStringLiteral("positionMs")).toInt() != 42000)
            return fail("flushProgress() did not write watchstate.json");
        lib.setProgress(path, 43000, 100000); // not flushed: written by the shutdown path
        qInfo("[selftest] progress saved by flushProgress(): OK; quitting with an unsaved position");
        QCoreApplication::quit();
    });
}

int main(int argc, char *argv[])
{
    QGuiApplication::setApplicationName("qtflix");
    QGuiApplication::setOrganizationName("qtflix");
    QGuiApplication::setApplicationDisplayName("QtFlix");
    QGuiApplication app(argc, argv);
    QGuiApplication::setDesktopFileName("qtflix"); // matches packaging/qtflix.desktop (taskbar icon on Wayland)
    QGuiApplication::setWindowIcon(QIcon(":/assets/mark.svg"));
    QQuickStyle::setStyle("Basic");

    Library library(nullptr);
    Tmdb tmdb(&library);                            // declared before the engine so it outlives QML
    auto *thumbs = new ThumbnailProvider(&library); // engine takes ownership
    // Thumbnail warm-up skips titles TMDB will give artwork; re-evaluated whenever TMDB's answers may change.
    library.setWarmUpSkip([&tmdb](const QString &id) { return tmdb.expectsArtwork(id); });
    QObject::connect(&tmdb, &Tmdb::idle, &library, &Library::requeueWarmUp);
    QObject::connect(&tmdb, &Tmdb::apiKeyChanged, &library, &Library::requeueWarmUp);

    QQmlApplicationEngine engine;
    engine.addImportPath("qrc:/"); // module is embedded at :/QtFlix (RESOURCE_PREFIX /), so it loads outside build/ too
    engine.addImageProvider("thumbs", thumbs);
    // Library and Tmdb are QML_SINGLETONs in module QtFlix (see their headers); instances created above.
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    // Dev hooks: QTFLIX_ROOT=<QmlTypeName> loads another root component (e.g. a test harness),
    // QTFLIX_SCREENSHOT=/path.png grabs the first window after QTFLIX_SCREENSHOT_DELAY ms (default 4000) and quits.
    const QString root = qEnvironmentVariable("QTFLIX_ROOT", "Main");
    engine.loadFromModule("QtFlix", root);
    library.rescan();
    if (qEnvironmentVariable("QTFLIX_SELFTEST") == QLatin1String("progress"))
        QObject::connect(&library, &Library::libraryChanged, &app, [&library] { selfTestProgress(library); },
                         Qt::SingleShotConnection);

    const QString shot = qEnvironmentVariable("QTFLIX_SCREENSHOT");
    if (!shot.isEmpty()) {
        const int delay = qEnvironmentVariableIntValue("QTFLIX_SCREENSHOT_DELAY") > 0
                              ? qEnvironmentVariableIntValue("QTFLIX_SCREENSHOT_DELAY") : 4000;
        QTimer::singleShot(delay, &app, [&engine, shot]() {
            for (QObject *o : engine.rootObjects())
                if (auto *w = qobject_cast<QQuickWindow *>(o)) { w->grabWindow().save(shot); break; }
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
