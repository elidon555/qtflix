#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QFontDatabase>
#include <QIcon>
#include <QSurfaceFormat>
#include <QQuickWindow>
#include <QTimer>

#include "library.h"
#include "thumbnailprovider.h"
#include "tmdb.h"

int main(int argc, char *argv[])
{
    QGuiApplication::setApplicationName("qtflix");
    QGuiApplication::setOrganizationName("qtflix");
    QGuiApplication::setApplicationDisplayName("QtFlix");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");

    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setSamples(4);
    QSurfaceFormat::setDefaultFormat(fmt);

    Library library(nullptr);
    Tmdb tmdb(&library);                            // declared before the engine so it outlives QML
    auto *thumbs = new ThumbnailProvider(&library); // engine takes ownership

    QQmlApplicationEngine engine;
    engine.addImageProvider("thumbs", thumbs);
    // Library and Tmdb are QML_SINGLETONs in module QtFlix (see their headers); instances created above.
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    // Dev hooks: QTFLIX_ROOT=<QmlTypeName> loads another root component (e.g. a test harness),
    // QTFLIX_SCREENSHOT=/path.png grabs the first window after QTFLIX_SCREENSHOT_DELAY ms (default 4000) and quits.
    const QString root = qEnvironmentVariable("QTFLIX_ROOT", "Main");
    engine.loadFromModule("QtFlix", root);
    library.rescan();

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
