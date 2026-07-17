#include "ModelManager.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>

#include <csignal>
#include <unistd.h>

namespace ModelManager {

namespace {
// Parakeet ONNX bundle (in-process via sherpa-onnx — see ParakeetEngine).
// k2-fsa's int8 export on the sherpa-onnx `asr-models` release: a .tar.bz2
// whose top dir holds encoder/decoder/joiner .int8.onnx + tokens.txt. We
// extract it (strip the archive's top dir) into <models>/<kParakeetBundleDirName>/.
constexpr auto kParakeetBundleDirName = "parakeet-tdt-0.6b-v2";
constexpr auto kParakeetArchiveUrl =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/"
    "sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8.tar.bz2";
constexpr auto kParakeetEncoderFile = "encoder.int8.onnx";
constexpr auto kParakeetDecoderFile = "decoder.int8.onnx";
constexpr auto kParakeetJoinerFile = "joiner.int8.onnx";
constexpr auto kParakeetTokensFile = "tokens.txt";
} // namespace

const std::vector<ModelInfo> &availableModels()
{
    static const std::vector<ModelInfo> models = {
        {"tiny.en", "ggml-tiny.en.bin", "Tiny (English) — ~75 MB, fastest"},
        {"base.en", "ggml-base.en.bin", "Base (English) — ~148 MB, balanced"},
        {"small.en", "ggml-small.en.bin", "Small (English) — ~488 MB, more accurate"},
        {"medium.en", "ggml-medium.en.bin", "Medium (English) — ~1.5 GB, most accurate"},
        // No English-only "large" ggml model exists upstream — large-v3 is
        // multilingual-only, so (unlike the four entries above) this one
        // must not claim "(English)".
        {"large-v3", "ggml-large-v3.bin", "Large (multilingual, most accurate) — ~3.1 GB"},
        // In-process NeMo Parakeet-TDT via sherpa-onnx (see ParakeetEngine).
        // fileName is the extracted bundle DIRECTORY (not a single ggml file) —
        // modelFileName()/isModelDownloaded()/ensureModel() special-case this id.
        {kParakeetModelId, kParakeetBundleDirName,
         "Parakeet TDT 0.6B v2 (English, NVIDIA) — ~630 MB, fast + accurate"},
    };
    return models;
}

QString modelFileName(const QString &modelId)
{
    for (const ModelInfo &info : availableModels()) {
        if (info.id == modelId)
            return info.fileName;
    }
    return {};
}

namespace {

QDir modelsDirFor(const QUrl &vaultPath)
{
    return QDir(vaultPath.toLocalFile() + "/.transcriber/models");
}

// The single model file this app downloads into the vault for a given
// out-of-process engine, or empty if the engine's model is managed externally
// by its own runtime. Parakeet is deliberately absent: NeMo's from_pretrained()
// fetches and caches its own weights (the HF cache), so Parakeet's availability
// is gated by its dependencies being installed (see EngineRegistry's deps
// check), not by an app-managed file. This table is where a future engine that
// DOES ship an app-downloaded model file would register one.
QString engineModelFileName(const QString &engineId)
{
    Q_UNUSED(engineId);
    return QString();
}

}

bool isParakeetModelId(const QString &modelId)
{
    return modelId == QLatin1String(kParakeetModelId);
}

ParakeetModelPaths parakeetModelPaths(const QUrl &vaultPath)
{
    const QDir bundle(modelsDirFor(vaultPath).filePath(QString::fromLatin1(kParakeetBundleDirName)));
    ParakeetModelPaths paths;
    paths.encoder = bundle.filePath(QString::fromLatin1(kParakeetEncoderFile));
    paths.decoder = bundle.filePath(QString::fromLatin1(kParakeetDecoderFile));
    paths.joiner = bundle.filePath(QString::fromLatin1(kParakeetJoinerFile));
    paths.tokens = bundle.filePath(QString::fromLatin1(kParakeetTokensFile));
    return paths;
}

bool ParakeetModelPaths::isComplete() const
{
    return QFileInfo::exists(encoder) && QFileInfo::exists(decoder) && QFileInfo::exists(joiner)
        && QFileInfo::exists(tokens);
}

bool isModelDownloaded(const QUrl &vaultPath, const QString &modelId)
{
    // The Parakeet bundle is a directory of ONNX files, not a single file —
    // "downloaded" means all four are present (a half-extracted dir is not).
    if (isParakeetModelId(modelId))
        return parakeetModelPaths(vaultPath).isComplete();
    const QString fileName = modelFileName(modelId);
    return !fileName.isEmpty() && QFileInfo::exists(modelsDirFor(vaultPath).filePath(fileName));
}

bool isLabelModelDownloaded(const QUrl &vaultPath)
{
    return QFileInfo::exists(modelsDirFor(vaultPath).filePath(QString(kLabelModelFileName)));
}

bool areDiarizationModelsDownloaded(const QUrl &vaultPath)
{
    const QDir dir = modelsDirFor(vaultPath);
    return QFileInfo::exists(dir.filePath(QString(kDiarizationSegmentationFileName)))
        && QFileInfo::exists(dir.filePath(QString(kDiarizationEmbeddingFileName)));
}

bool isEngineModelPresent(const QUrl &vaultPath, const QString &engineId)
{
    const QString fileName = engineModelFileName(engineId);
    // No app-managed file for this engine ⇒ its model is externally managed
    // (e.g. NeMo/HF cache), so it never gates availability here.
    if (fileName.isEmpty())
        return true;
    return QFileInfo::exists(modelsDirFor(vaultPath).filePath(fileName));
}

bool isVadModelDownloaded(const QUrl &vaultPath)
{
    return QFileInfo::exists(modelsDirFor(vaultPath).filePath(QString(kVadModelFileName)));
}

namespace {

// Probes Content-Length via a HEAD request, without downloading anything —
// best-effort only: 0 if the server doesn't report a length or the probe
// fails (caller then shows indeterminate progress instead of a percentage).
qint64 probeContentLength(const QString &url)
{
    QProcess curl;
    curl.start("curl", {"-sIL", url});
    if (!curl.waitForFinished(10000))
        return 0;
    const QString headers = QString::fromUtf8(curl.readAllStandardOutput());
    qint64 lastLength = 0;
    for (const QString &line : headers.split(QStringLiteral("\r\n"), Qt::SkipEmptyParts)) {
        if (line.startsWith(QStringLiteral("content-length:"), Qt::CaseInsensitive)) {
            bool ok = false;
            const qint64 value = line.mid(line.indexOf(':') + 1).trimmed().toLongLong(&ok);
            if (ok)
                lastLength = value; // a redirect chain reports several — the LAST one is the real download's
        }
    }
    return lastLength;
}

// Shared by ensureModel()/ensureLabelModel(): download url to
// <vault>/.transcriber/models/<fileName> via curl to a .part file, then
// rename into place. Returns the final path, or empty on failure (errorOut
// set if given). Assumes modelsDir already exists.
//
// Wave N (task system): event-driven rather than one blocking
// waitForFinished(-1) — polls every 100ms instead, which both reports real
// byte progress (against an upfront HEAD probe of Content-Length) and gives
// a cooperative checkpoint for pause (SIGSTOP the curl process, SIGCONT to
// resume — Linux-only, per CLAUDE.md's "Linux (CachyOS) primary") and cancel
// (kill the process, remove the partial file). Still fully synchronous from
// the CALLER's point of view (this runs on TranscriptionWorker's own
// dedicated thread, never the GUI thread — blocking there is fine, same as
// every other job).
QString downloadToModelsDir(const QDir &modelsDir, const QString &fileName, const QString &url, QString *errorOut,
                             const DownloadProgressCallback &onProgress = {}, const DownloadControl &control = {})
{
    const QString modelPath = modelsDir.filePath(fileName);
    const QString tmpPath = modelPath + ".part";
    QFile::remove(tmpPath); // stale partial from a previously cancelled attempt

    const qint64 totalBytes = probeContentLength(url);

    QProcess curl;
    curl.start("curl", {"-L", "--fail", "-o", tmpPath, url});
    if (!curl.waitForStarted(10000)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not start curl");
        return {};
    }

    bool wasPaused = false;
    bool cancelled = false;
    while (!curl.waitForFinished(100)) {
        if (control.cancelled && control.cancelled()) {
            cancelled = true;
            curl.kill();
            curl.waitForFinished(2000);
            break;
        }
        const bool shouldPause = control.paused && control.paused();
        if (shouldPause && !wasPaused) {
            ::kill(static_cast<pid_t>(curl.processId()), SIGSTOP);
            wasPaused = true;
        } else if (!shouldPause && wasPaused) {
            ::kill(static_cast<pid_t>(curl.processId()), SIGCONT);
            wasPaused = false;
        }
        if (onProgress && !wasPaused)
            onProgress(QFileInfo(tmpPath).size(), totalBytes);
    }

    if (cancelled) {
        if (errorOut)
            *errorOut = QStringLiteral("Cancelled");
        QFile::remove(tmpPath);
        return {};
    }

    if (curl.exitStatus() != QProcess::NormalExit || curl.exitCode() != 0) {
        if (errorOut)
            *errorOut = QStringLiteral("model download failed: %1").arg(QString::fromUtf8(curl.readAllStandardError()));
        QFile::remove(tmpPath);
        return {};
    }

    if (onProgress) {
        const qint64 finalSize = QFileInfo(tmpPath).size();
        onProgress(finalSize, totalBytes > 0 ? totalBytes : finalSize);
    }
    QFile::rename(tmpPath, modelPath);
    return modelPath;
}

// sherpa-onnx's pyannote-segmentation-3.0 loader reads custom ONNX metadata
// (e.g. "sample_rate") that sherpa-onnx's OWN export script bakes in —
// confirmed empirically that a generic community ONNX export of the same
// model (e.g. onnx-community/pyannote-segmentation-3.0) lacks this metadata
// and crashes the process at load time, not just fails gracefully. So this
// model MUST come from sherpa-onnx's own packaged release, which ships as a
// .tar.bz2 archive (model.onnx alongside export scripts/README/an int8
// variant) rather than a bare .onnx file — downloaded then the one needed
// file is extracted straight into modelsDir under our own local name, and
// the archive plus its siblings are discarded.
QString downloadAndExtractFromTarBz2(const QDir &modelsDir, const QString &localFileName, const QString &archiveUrl,
                                      const QString &pathInsideArchive, QString *errorOut,
                                      const DownloadProgressCallback &onProgress = {},
                                      const DownloadControl &control = {})
{
    const QString archiveFileName = QStringLiteral("%1.tar.bz2").arg(localFileName);
    const QString archivePath =
        downloadToModelsDir(modelsDir, archiveFileName, archiveUrl, errorOut, onProgress, control);
    if (archivePath.isEmpty())
        return {};

    const QString modelPath = modelsDir.filePath(localFileName);
    QProcess tar;
    tar.setStandardOutputFile(modelPath);
    tar.start("tar", {"-xjf", archivePath, "-O", pathInsideArchive});
    tar.waitForFinished(-1);
    QFile::remove(archivePath);

    if (tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0 || !QFileInfo::exists(modelPath)) {
        if (errorOut)
            *errorOut = QStringLiteral("model archive extraction failed: %1")
                            .arg(QString::fromUtf8(tar.readAllStandardError()));
        QFile::remove(modelPath);
        return {};
    }
    return modelPath;
}

// Downloads a .tar.bz2 and extracts the WHOLE archive into
// <modelsDir>/<destDirName>/, stripping the archive's own top-level directory
// (--strip-components=1) so files land directly under destDirName regardless of
// what the archive names its root. Verifies every requiredFile exists
// afterward. Unlike downloadAndExtractFromTarBz2() (one file via `tar -O`), this
// is for multi-file model bundles (the Parakeet ONNX encoder/decoder/joiner +
// tokens). Returns the dest dir, or empty on failure. Idempotent: returns
// immediately if the bundle is already complete.
QString downloadAndExtractBundle(const QDir &modelsDir, const QString &destDirName, const QString &archiveUrl,
                                  const QStringList &requiredFiles, QString *errorOut,
                                  const DownloadProgressCallback &onProgress = {},
                                  const DownloadControl &control = {})
{
    const QDir destDir(modelsDir.filePath(destDirName));
    const auto allPresent = [&] {
        for (const QString &f : requiredFiles) {
            if (!QFileInfo::exists(destDir.filePath(f)))
                return false;
        }
        return true;
    };
    if (allPresent())
        return destDir.path();

    if (!modelsDir.mkpath(destDirName)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(destDir.path());
        return {};
    }

    const QString archiveFileName = QStringLiteral("%1.tar.bz2").arg(destDirName);
    const QString archivePath =
        downloadToModelsDir(modelsDir, archiveFileName, archiveUrl, errorOut, onProgress, control);
    if (archivePath.isEmpty())
        return {}; // download failed/cancelled — errorOut already set

    QProcess tar;
    tar.start("tar", {"-xjf", archivePath, "-C", destDir.path(), "--strip-components=1"});
    tar.waitForFinished(-1);
    QFile::remove(archivePath);

    if (tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0 || !allPresent()) {
        if (errorOut)
            *errorOut = QStringLiteral("model bundle extraction failed: %1")
                            .arg(QString::fromUtf8(tar.readAllStandardError()));
        return {};
    }
    return destDir.path();
}

}

QString ensureModel(const QUrl &vaultPath, const QString &modelId, QString *errorOut, bool *usedFallback,
                     const DownloadProgressCallback &onProgress, const DownloadControl &control)
{
    if (usedFallback)
        *usedFallback = false;

    // Parakeet is a multi-file ONNX bundle, not a single ggml file — download/
    // extract it and return the bundle DIRECTORY (the caller resolves the four
    // files via parakeetModelPaths()). Never falls back to a whisper default:
    // an explicit Parakeet request must fail loudly if the bundle can't be had.
    if (isParakeetModelId(modelId)) {
        return downloadAndExtractBundle(
            modelsDirFor(vaultPath), QString::fromLatin1(kParakeetBundleDirName),
            QString::fromLatin1(kParakeetArchiveUrl),
            {QString::fromLatin1(kParakeetEncoderFile), QString::fromLatin1(kParakeetDecoderFile),
             QString::fromLatin1(kParakeetJoinerFile), QString::fromLatin1(kParakeetTokensFile)},
            errorOut, onProgress, control);
    }

    const QDir modelsDir = modelsDirFor(vaultPath);
    QString modelName = modelFileName(modelId);
    if (modelName.isEmpty()) {
        // modelId isn't in availableModels() — never silently pretend the
        // requested model was used; the caller must be told.
        qWarning() << "ModelManager: unrecognized model id" << modelId << "— falling back to"
                   << kDefaultModelId;
        modelName = modelFileName(QString::fromUtf8(kDefaultModelId));
        if (usedFallback)
            *usedFallback = true;
    }
    const QString modelPath = modelsDir.filePath(modelName);

    if (QFileInfo::exists(modelPath))
        return modelPath;

    if (!modelsDir.mkpath(".")) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(modelsDir.path());
        return {};
    }

    const QString repoUrl = QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/%1");
    return downloadToModelsDir(modelsDir, modelName, repoUrl.arg(modelName), errorOut, onProgress, control);
}

QString ensureLabelModel(const QUrl &vaultPath, QString *errorOut, const DownloadProgressCallback &onProgress,
                          const DownloadControl &control)
{
    const QDir modelsDir = modelsDirFor(vaultPath);
    const QString modelPath = modelsDir.filePath(QString(kLabelModelFileName));

    if (QFileInfo::exists(modelPath))
        return modelPath;

    if (!modelsDir.mkpath(".")) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(modelsDir.path());
        return {};
    }

    const QString url = QStringLiteral("https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/resolve/main/%1")
                             .arg(kLabelModelFileName);
    return downloadToModelsDir(modelsDir, QString(kLabelModelFileName), url, errorOut, onProgress, control);
}

QString ensureVadModel(const QUrl &vaultPath, QString *errorOut, const DownloadProgressCallback &onProgress,
                        const DownloadControl &control)
{
    const QDir modelsDir = modelsDirFor(vaultPath);
    const QString modelPath = modelsDir.filePath(QString(kVadModelFileName));

    if (QFileInfo::exists(modelPath))
        return modelPath;

    if (!modelsDir.mkpath(".")) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(modelsDir.path());
        return {};
    }

    const QString url = QStringLiteral("https://huggingface.co/ggml-org/whisper-vad/resolve/main/%1")
                             .arg(kVadModelFileName);
    return downloadToModelsDir(modelsDir, QString(kVadModelFileName), url, errorOut, onProgress, control);
}

bool ensureDiarizationModels(const QUrl &vaultPath, QString *segmentationPathOut, QString *embeddingPathOut,
                              QString *errorOut, const DownloadProgressCallback &onProgress,
                              const DownloadControl &control)
{
    const QDir modelsDir = modelsDirFor(vaultPath);

    const QString segmentationPath = modelsDir.filePath(QString(kDiarizationSegmentationFileName));
    const QString embeddingPath = modelsDir.filePath(QString(kDiarizationEmbeddingFileName));

    if (QFileInfo::exists(segmentationPath) && QFileInfo::exists(embeddingPath)) {
        if (segmentationPathOut)
            *segmentationPathOut = segmentationPath;
        if (embeddingPathOut)
            *embeddingPathOut = embeddingPath;
        return true;
    }

    if (!modelsDir.mkpath(".")) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(modelsDir.path());
        return false;
    }

    // Remote file names/layouts differ from the local names above (upstream
    // repos ship several variants each; these are the specific ones this
    // app uses).
    if (!QFileInfo::exists(segmentationPath)) {
        const QString archiveUrl = QStringLiteral(
            "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-segmentation-models/"
            "sherpa-onnx-pyannote-segmentation-3-0.tar.bz2");
        // See kDiarizationSegmentationFileName's doc comment (ModelManager.h)
        // for why this is the int8-quantized export, not model.onnx.
        if (downloadAndExtractFromTarBz2(modelsDir, QString(kDiarizationSegmentationFileName), archiveUrl,
                                          QStringLiteral("sherpa-onnx-pyannote-segmentation-3-0/model.int8.onnx"),
                                          errorOut, onProgress, control)
                .isEmpty())
            return false;
    }
    if (!QFileInfo::exists(embeddingPath)) {
        // See kDiarizationEmbeddingFileName's doc comment (ModelManager.h)
        // for why this is WeSpeaker's ResNet293-LM rather than CAM++.
        const QString url = QStringLiteral(
            "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
            "wespeaker_en_voxceleb_resnet293_LM.onnx");
        if (downloadToModelsDir(modelsDir, QString(kDiarizationEmbeddingFileName), url, errorOut, onProgress, control)
                .isEmpty())
            return false;
    }

    if (segmentationPathOut)
        *segmentationPathOut = segmentationPath;
    if (embeddingPathOut)
        *embeddingPathOut = embeddingPath;
    return true;
}

}
