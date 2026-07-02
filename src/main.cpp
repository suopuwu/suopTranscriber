#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>

int main(int argc, char *argv[])
{
    // Qt Multimedia's gstreamer backend is flaky with arbitrary formats on
    // Linux; force ffmpeg. Must be set before QGuiApplication is constructed.
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");

    QGuiApplication app(argc, argv);

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
