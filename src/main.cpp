#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSurfaceFormat>
#include <QUrl>

int main(int argc, char *argv[])
{
    // Qt Multimedia's gstreamer backend is flaky with arbitrary formats on
    // Linux; force ffmpeg. Must be set before QGuiApplication is constructed.
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");

    // Don't inherit the desktop's QQC2 style (e.g. KDE Breeze), which
    // expects platform theme integration this app doesn't provide and
    // throws null-object errors from its own delegates otherwise.
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    // Real window transparency/compositor blur needs an alpha-enabled
    // surface; this is a global default and must be set before
    // QGuiApplication exists, or the first window's surface can't carry it.
    QSurfaceFormat surfaceFormat;
    surfaceFormat.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(surfaceFormat);

    QGuiApplication app(argc, argv);
    // Required for QSettings (used by AppSettings.qml) to know where to
    // persist to; otherwise it fails to initialize and nothing survives a
    // restart.
    QCoreApplication::setOrganizationName("suopTranscriber");
    QCoreApplication::setApplicationName("suopTranscriber");

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    // Optional `suopTranscriber <vault-dir>` pre-opens a vault instead of
    // requiring the Open Vault dialog on every launch.
    const QUrl initialVaultPath = argc > 1 ? QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])) : QUrl();
    engine.rootContext()->setContextProperty("initialVaultPath", initialVaultPath);

    engine.loadFromModule("SuopTranscriber", "Main");

    return app.exec();
}
