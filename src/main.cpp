#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSurfaceFormat>
#include <QUrl>

#include <unistd.h>

#include "core/FocusBlurFilter.h"
#include "core/IconTheme.h"
#include "core/SortKey.h"

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
    // dlopen call.
    //
    // glibc's dynamic linker parses LD_LIBRARY_PATH exactly ONCE, during its
    // own startup (before main() runs) — confirmed the hard way (2026-07-07):
    // a plain qputenv() here used to "work" in the sense that it never
    // crashed, but silently never actually put CUDA on sherpa-onnx's search
    // path either, since that startup parse had already happened before this
    // line ever runs. dlopen() calls later in the process's life consult
    // that already-parsed, cached search list — NOT a fresh read of environ
    // — so a change made after exec() is invisible to them. The only way to
    // get a runtime-computed LD_LIBRARY_PATH to actually take effect is to
    // set it and then re-exec this same binary: the new process image's own
    // ld.so startup parse (which hasn't happened yet at that point) picks up
    // the corrected environment from the start. SUOP_LD_LIBRARY_PATH_FIXED
    // is the re-exec guard — set only on the child, so it runs this block
    // exactly once instead of looping forever.
    //
    // Read exePath via /proc/self/exe (not argv[0], which may be relative or
    // a bare name found via PATH) so this resolves correctly regardless of
    // how the binary was invoked; harmless no-op if readlink fails, or if
    // the lib/ directory doesn't exist (e.g. a build without
    // SHERPA_ONNX_ENABLE_GPU, which never populates it) — diarization still
    // works CPU-only either way (see DiarizationEngine's own CPU fallback).
    if (!qEnvironmentVariableIsSet("SUOP_LD_LIBRARY_PATH_FIXED")) {
        char exePath[4096];
        const ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len > 0) {
            exePath[len] = '\0';
            const QString libDir = QFileInfo(QString::fromLocal8Bit(exePath)).absolutePath() + "/lib";
            if (QFileInfo::exists(libDir)) {
                const QByteArray existing = qgetenv("LD_LIBRARY_PATH");
                qputenv("LD_LIBRARY_PATH",
                        existing.isEmpty() ? libDir.toLocal8Bit() : (libDir.toLocal8Bit() + ":" + existing));
                qputenv("SUOP_LD_LIBRARY_PATH_FIXED", "1");
                execv(exePath, argv);
                // execv() only returns on failure (e.g. ENOENT/EACCES) — fall
                // through and keep running under the OLD environment rather
                // than aborting the app outright; CUDA diarization just
                // won't be found later, same as before this fix existed.
            }
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

    // VaultSearchProxyModel.fileSortKeys is a typed SortKeyMap (see
    // src/core/SortKey.h) that TreeOrdering.compute() produces and Sidebar.qml
    // passes straight through to the proxy as an opaque value. Register it so
    // the QML property system can carry and assign it by its metatype.
    qRegisterMetaType<SortKeyMap>("SortKeyMap");

    // App-wide "click outside a focused text field blurs (commits) it". Installed
    // on the application so it observes every press BEFORE any QML grab — the
    // reason a Flickable/ScrollView/MouseArea eating the press no longer defeats
    // it. Lives on the stack for the whole app.exec(); never consumes events.
    FocusBlurFilter focusBlurFilter;
    app.installEventFilter(&focusBlurFilter);

    QQmlApplicationEngine engine;
    // Serves AppIcon.qml's "image://suopicons/..." sources — SVG icons
    // rastered at device pixels and tinted, then cached. The engine takes
    // ownership. Must be added before any QML loads, since the first
    // AppIcon to instantiate resolves its source immediately.
    engine.addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId), new IconImageProvider);
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
