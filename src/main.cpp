#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSurfaceFormat>
#include <QUrl>

#include <unistd.h>

int main(int argc, char *argv[])
{
    // Qt Multimedia's gstreamer backend is flaky with arbitrary formats on
    // Linux; force ffmpeg. Must be set before QGuiApplication is constructed.
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");

    // sherpa-onnx's fetched onnxruntime GPU build dlopen()s
    // libonnxruntime_providers_cuda.so lazily (only when speaker detection
    // actually runs, well after startup) — that library ships with NO
    // RPATH/RUNPATH of its own, so its CUDA 12 dependencies (libcublasLt.so.12
    // etc — see CMakeLists.txt's suop_copy_cuda12_compat_libs comment) are
    // only found if LD_LIBRARY_PATH covers them at the time of that LATER
    // dlopen call. Since glibc's dynamic linker re-reads LD_LIBRARY_PATH per
    // dlopen (unlike the one-time exec-time resolution of this executable's
    // own direct NEEDED entries), setting it here — even after this process
    // has already started — still works for that later, lazy load. Read via
    // /proc/self/exe (not argv[0], which may be relative or a bare name found
    // via PATH) so this resolves correctly regardless of how the binary was
    // invoked; harmless no-op if the directory doesn't exist (e.g. a build
    // without SHERPA_ONNX_ENABLE_GPU, which never populates it).
    {
        char exePath[4096];
        const ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len > 0) {
            exePath[len] = '\0';
            const QString libDir = QFileInfo(QString::fromLocal8Bit(exePath)).absolutePath() + "/lib";
            const QByteArray existing = qgetenv("LD_LIBRARY_PATH");
            qputenv("LD_LIBRARY_PATH",
                    existing.isEmpty() ? libDir.toLocal8Bit() : (libDir.toLocal8Bit() + ":" + existing));
        }
    }

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
