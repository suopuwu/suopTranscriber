#include "ModelManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
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
        {"tiny.en", "ggml-tiny.en.bin", "Tiny (English) — ~75 MB"},
        {"base.en", "ggml-base.en.bin", "Base (English) — ~148 MB"},
        {"small.en", "ggml-small.en.bin", "Small (English) — ~488 MB"},
        {"medium.en", "ggml-medium.en.bin", "Medium (English) — ~1.5 GB"},
        // No English-only "large" ggml model exists upstream — large-v3 is
        // multilingual-only, so (unlike the four entries above) this one
        // must not claim "(English)".
        {"large-v3", "ggml-large-v3.bin", "Large (multilingual) — ~3.1 GB"},
        // In-process NeMo Parakeet-TDT via sherpa-onnx (see ParakeetEngine).
        // fileName is the extracted bundle DIRECTORY (not a single ggml file) —
        // modelFileName()/isModelDownloaded()/ensureModel() special-case this id.
        {kParakeetModelId, kParakeetBundleDirName,
         "Parakeet TDT 0.6B v2 (English, NVIDIA) — ~630 MB, fast + accurate, "
         "avoids whisper's repetition hallucinations"},
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

QString modelsStoreDir()
{
    const QString override = qEnvironmentVariable("SUOP_MODELS_DIR");
    if (!override.isEmpty())
        return override;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/suopTranscriber/models");
}

QString bundledModelsDir()
{
    const QString override = qEnvironmentVariable("SUOP_BUNDLED_MODELS_DIR");
    if (!override.isEmpty())
        return override;
    return QCoreApplication::applicationDirPath() + QStringLiteral("/models");
}

namespace {

QDir storeDir()
{
    return QDir(modelsStoreDir());
}

QDir bundledDir()
{
    return QDir(bundledModelsDir());
}

// Resolves a single model file to an existing absolute path, searching the
// writable store first, then the read-only bundled dir. Empty if the file is
// in neither (never downloads). This is the store-first-then-bundled rule that
// every presence check / path lookup goes through.
QString resolveModelFile(const QString &fileName)
{
    const QString storePath = storeDir().filePath(fileName);
    if (QFileInfo::exists(storePath))
        return storePath;
    const QString bundledPath = bundledDir().filePath(fileName);
    if (QFileInfo::exists(bundledPath))
        return bundledPath;
    return {};
}

// The single model file this app downloads into the store for a given
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

namespace {
ParakeetModelPaths parakeetPathsIn(const QDir &modelsBase)
{
    const QDir bundle(modelsBase.filePath(QString::fromLatin1(kParakeetBundleDirName)));
    ParakeetModelPaths paths;
    paths.encoder = bundle.filePath(QString::fromLatin1(kParakeetEncoderFile));
    paths.decoder = bundle.filePath(QString::fromLatin1(kParakeetDecoderFile));
    paths.joiner = bundle.filePath(QString::fromLatin1(kParakeetJoinerFile));
    paths.tokens = bundle.filePath(QString::fromLatin1(kParakeetTokensFile));
    return paths;
}
} // namespace

ParakeetModelPaths parakeetModelPaths()
{
    const ParakeetModelPaths store = parakeetPathsIn(storeDir());
    if (store.isComplete())
        return store;
    const ParakeetModelPaths bundled = parakeetPathsIn(bundledDir());
    if (bundled.isComplete())
        return bundled;
    return store; // neither complete — return the writable-store download target
}

bool ParakeetModelPaths::isComplete() const
{
    return QFileInfo::exists(encoder) && QFileInfo::exists(decoder) && QFileInfo::exists(joiner)
        && QFileInfo::exists(tokens);
}

bool isModelDownloaded(const QString &modelId)
{
    // The Parakeet bundle is a directory of ONNX files, not a single file —
    // "downloaded" means all four are present (a half-extracted dir is not).
    if (isParakeetModelId(modelId))
        return parakeetModelPaths().isComplete();
    const QString fileName = modelFileName(modelId);
    return !fileName.isEmpty() && !resolveModelFile(fileName).isEmpty();
}

bool isLabelModelDownloaded()
{
    return !resolveModelFile(QString(kLabelModelFileName)).isEmpty();
}

bool areDiarizationModelsDownloaded()
{
    return !resolveModelFile(QString(kDiarizationSegmentationFileName)).isEmpty()
        && !resolveModelFile(QString(kDiarizationEmbeddingFileName)).isEmpty();
}

const std::vector<DiarizationModelInfo> &availableSegmentationModels()
{
    static const std::vector<DiarizationModelInfo> models = {
        {kDefaultSegmentationModelId, kDiarizationSegmentationFileName, "pyannote 3.0 (default, MIT)",
         "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-segmentation-models/"
         "sherpa-onnx-pyannote-segmentation-3-0.tar.bz2",
         "sherpa-onnx-pyannote-segmentation-3-0/model.int8.onnx", false},
        {"reverb-diarization-v1", "reverb-diarization-v1-int8.onnx", "Reverb v1 (Rev.ai — non-commercial)",
         "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-segmentation-models/"
         "sherpa-onnx-reverb-diarization-v1.tar.bz2",
         "sherpa-onnx-reverb-diarization-v1/model.int8.onnx", true},
    };
    return models;
}

const std::vector<DiarizationModelInfo> &availableEmbeddingModels()
{
    static const std::vector<DiarizationModelInfo> models = {
        {kDefaultEmbeddingModelId, kDiarizationEmbeddingFileName, "WeSpeaker ResNet293-LM (default, ~115 MB)",
         "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
         "wespeaker_en_voxceleb_resnet293_LM.onnx",
         QString(), false},
        {"3dspeaker-campplus-en", "3dspeaker-campplus-en.onnx", "3D-Speaker CAM++ (~28 MB, fast)",
         "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
         "3dspeaker_speech_campplus_sv_en_voxceleb_16k.onnx",
         QString(), false},
        {"nemo-titanet-large", "nemo-titanet-large.onnx", "NVIDIA NeMo TitaNet-large (~100 MB)",
         "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
         "nemo_en_titanet_large.onnx",
         QString(), false},
    };
    return models;
}

namespace {
// Catalog lookup with default-substitution (the default is always the first
// entry). Sets *usedFallback true iff id was unrecognized — the caller records
// it (never silently misrepresent which model ran; see ensureModel()).
const DiarizationModelInfo &resolveDiarizationModel(const std::vector<DiarizationModelInfo> &catalog,
                                                    const QString &id, bool *usedFallback)
{
    if (usedFallback)
        *usedFallback = false;
    for (const DiarizationModelInfo &info : catalog) {
        if (info.id == id)
            return info;
    }
    qWarning() << "ModelManager: unrecognized diarization model id" << id << "— falling back to"
               << catalog.front().id;
    if (usedFallback)
        *usedFallback = true;
    return catalog.front();
}
} // namespace

const DiarizationModelInfo &segmentationModelInfo(const QString &id, bool *usedFallback)
{
    return resolveDiarizationModel(availableSegmentationModels(), id, usedFallback);
}

const DiarizationModelInfo &embeddingModelInfo(const QString &id, bool *usedFallback)
{
    return resolveDiarizationModel(availableEmbeddingModels(), id, usedFallback);
}

bool isSegmentationModelDownloaded(const QString &id)
{
    return !resolveModelFile(segmentationModelInfo(id).fileName).isEmpty();
}

bool isEmbeddingModelDownloaded(const QString &id)
{
    return !resolveModelFile(embeddingModelInfo(id).fileName).isEmpty();
}

bool areDiarizationModelsDownloaded(const QString &segmentationModelId, const QString &embeddingModelId)
{
    return isSegmentationModelDownloaded(segmentationModelId) && isEmbeddingModelDownloaded(embeddingModelId);
}

bool isEngineModelPresent(const QString &engineId)
{
    const QString fileName = engineModelFileName(engineId);
    // No app-managed file for this engine ⇒ its model is externally managed
    // (e.g. NeMo/HF cache), so it never gates availability here.
    if (fileName.isEmpty())
        return true;
    return !resolveModelFile(fileName).isEmpty();
}

bool isVadModelDownloaded()
{
    return !resolveModelFile(QString(kVadModelFileName)).isEmpty();
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
// <modelsDir>/<fileName> via curl to a .part file, then
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

QString ensureModel(const QString &modelId, QString *errorOut, bool *usedFallback,
                     const DownloadProgressCallback &onProgress, const DownloadControl &control)
{
    if (usedFallback)
        *usedFallback = false;

    // Parakeet is a multi-file ONNX bundle, not a single ggml file — return the
    // bundle DIRECTORY (the caller resolves the four files via
    // parakeetModelPaths()). If a complete bundle already exists (store OR
    // bundled), use it as-is; otherwise download/extract into the writable
    // store. Never falls back to a whisper default: an explicit Parakeet
    // request must fail loudly if the bundle can't be had.
    if (isParakeetModelId(modelId)) {
        const ParakeetModelPaths resolved = parakeetModelPaths();
        if (resolved.isComplete())
            return QFileInfo(resolved.encoder).absolutePath(); // the bundle dir
        return downloadAndExtractBundle(
            storeDir(), QString::fromLatin1(kParakeetBundleDirName),
            QString::fromLatin1(kParakeetArchiveUrl),
            {QString::fromLatin1(kParakeetEncoderFile), QString::fromLatin1(kParakeetDecoderFile),
             QString::fromLatin1(kParakeetJoinerFile), QString::fromLatin1(kParakeetTokensFile)},
            errorOut, onProgress, control);
    }

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

    // Already present in the store or shipped in the bundled dir — use it,
    // never re-download (a bundled model is read-only, loaded straight from
    // there).
    const QString existing = resolveModelFile(modelName);
    if (!existing.isEmpty())
        return existing;

    const QDir modelsDir = storeDir();
    if (!modelsDir.mkpath(".")) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(modelsDir.path());
        return {};
    }

    const QString repoUrl = QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/%1");
    return downloadToModelsDir(modelsDir, modelName, repoUrl.arg(modelName), errorOut, onProgress, control);
}

QString ensureLabelModel(QString *errorOut, const DownloadProgressCallback &onProgress,
                          const DownloadControl &control)
{
    const QString existing = resolveModelFile(QString(kLabelModelFileName));
    if (!existing.isEmpty())
        return existing;

    const QDir modelsDir = storeDir();
    if (!modelsDir.mkpath(".")) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(modelsDir.path());
        return {};
    }

    const QString url = QStringLiteral("https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/resolve/main/%1")
                             .arg(kLabelModelFileName);
    return downloadToModelsDir(modelsDir, QString(kLabelModelFileName), url, errorOut, onProgress, control);
}

QString ensureVadModel(QString *errorOut, const DownloadProgressCallback &onProgress,
                        const DownloadControl &control)
{
    const QString existing = resolveModelFile(QString(kVadModelFileName));
    if (!existing.isEmpty())
        return existing;

    const QDir modelsDir = storeDir();
    if (!modelsDir.mkpath(".")) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(modelsDir.path());
        return {};
    }

    const QString url = QStringLiteral("https://huggingface.co/ggml-org/whisper-vad/resolve/main/%1")
                             .arg(kVadModelFileName);
    return downloadToModelsDir(modelsDir, QString(kVadModelFileName), url, errorOut, onProgress, control);
}

namespace {
// Ensure one diarization sub-model (a direct .onnx or a tar.bz2-extracted file)
// is in the store, downloading if missing; returns its path or empty (errorOut
// set). Resolves store-first-then-bundled and never re-downloads. See
// downloadAndExtractFromTarBz2()'s doc comment for why a segmentation model
// MUST come from sherpa-onnx's own archive (baked-in ONNX metadata).
QString ensureDiarizationSubmodel(const DiarizationModelInfo &info, QString *errorOut,
                                  const DownloadProgressCallback &onProgress, const DownloadControl &control)
{
    const QString existing = resolveModelFile(info.fileName);
    if (!existing.isEmpty())
        return existing;

    const QDir modelsDir = storeDir();
    if (!modelsDir.mkpath(".")) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(modelsDir.path());
        return {};
    }
    if (!info.archiveInnerPath.isEmpty())
        return downloadAndExtractFromTarBz2(modelsDir, info.fileName, info.url, info.archiveInnerPath, errorOut,
                                            onProgress, control);
    return downloadToModelsDir(modelsDir, info.fileName, info.url, errorOut, onProgress, control);
}
} // namespace

QString ensureSegmentationModel(const QString &id, QString *errorOut, bool *usedFallback,
                                const DownloadProgressCallback &onProgress, const DownloadControl &control)
{
    return ensureDiarizationSubmodel(segmentationModelInfo(id, usedFallback), errorOut, onProgress, control);
}

QString ensureEmbeddingModel(const QString &id, QString *errorOut, bool *usedFallback,
                             const DownloadProgressCallback &onProgress, const DownloadControl &control)
{
    return ensureDiarizationSubmodel(embeddingModelInfo(id, usedFallback), errorOut, onProgress, control);
}

bool ensureDiarizationModels(const QString &segmentationModelId, const QString &embeddingModelId,
                             QString *segmentationPathOut, QString *embeddingPathOut, QString *errorOut,
                             bool *usedFallback, const DownloadProgressCallback &onProgress,
                             const DownloadControl &control)
{
    if (usedFallback)
        *usedFallback = false;
    bool segFallback = false;
    bool embedFallback = false;
    const QString segPath = ensureSegmentationModel(segmentationModelId, errorOut, &segFallback, onProgress, control);
    if (segPath.isEmpty())
        return false;
    const QString embedPath = ensureEmbeddingModel(embeddingModelId, errorOut, &embedFallback, onProgress, control);
    if (embedPath.isEmpty())
        return false;
    if (usedFallback)
        *usedFallback = segFallback || embedFallback;
    if (segmentationPathOut)
        *segmentationPathOut = segPath;
    if (embeddingPathOut)
        *embeddingPathOut = embedPath;
    return true;
}

namespace {

// The canonical on-disk shape of a manageable model id (whisper tier /
// parakeet / one of the support pseudo-ids). bundleDirName is non-empty only
// for the parakeet bundle; fileNames are the canonical file names (flat in the
// store, or inside the bundle dir for parakeet). valid=false ⇒ unknown id.
struct ModelLayout {
    bool valid = false;
    QString bundleDirName;
    QStringList fileNames;
};

ModelLayout modelLayout(const QString &id)
{
    ModelLayout layout;
    if (isParakeetModelId(id)) {
        layout.valid = true;
        layout.bundleDirName = QString::fromLatin1(kParakeetBundleDirName);
        layout.fileNames = {QString::fromLatin1(kParakeetEncoderFile), QString::fromLatin1(kParakeetDecoderFile),
                            QString::fromLatin1(kParakeetJoinerFile), QString::fromLatin1(kParakeetTokensFile)};
        return layout;
    }
    if (id == QLatin1String(kLabelModelId)) {
        layout.valid = true;
        layout.fileNames = {QString(kLabelModelFileName)};
        return layout;
    }
    if (id == QLatin1String(kVadModelId)) {
        layout.valid = true;
        layout.fileNames = {QString(kVadModelFileName)};
        return layout;
    }
    // Each selectable diarization sub-model is its own single-file manageable
    // id (download/delete/size/import/export), keyed by its catalog id.
    for (const DiarizationModelInfo &info : availableSegmentationModels()) {
        if (info.id == id) {
            layout.valid = true;
            layout.fileNames = {info.fileName};
            return layout;
        }
    }
    for (const DiarizationModelInfo &info : availableEmbeddingModels()) {
        if (info.id == id) {
            layout.valid = true;
            layout.fileNames = {info.fileName};
            return layout;
        }
    }
    const QString fileName = modelFileName(id); // whisper tier (empty ⇒ unknown)
    if (!fileName.isEmpty()) {
        layout.valid = true;
        layout.fileNames = {fileName};
    }
    return layout;
}

} // namespace

qint64 modelSizeOnDisk(const QString &id)
{
    const ModelLayout layout = modelLayout(id);
    if (!layout.valid)
        return 0;
    qint64 total = 0;
    if (!layout.bundleDirName.isEmpty()) {
        const ParakeetModelPaths p = parakeetModelPaths();
        for (const QString &f : {p.encoder, p.decoder, p.joiner, p.tokens}) {
            const QFileInfo fi(f);
            if (fi.exists())
                total += fi.size();
        }
        return total;
    }
    for (const QString &fileName : layout.fileNames) {
        const QString path = resolveModelFile(fileName);
        if (!path.isEmpty())
            total += QFileInfo(path).size();
    }
    return total;
}

bool isModelBundledOnly(const QString &id)
{
    const ModelLayout layout = modelLayout(id);
    if (!layout.valid)
        return false;
    if (!layout.bundleDirName.isEmpty()) {
        const bool present = parakeetModelPaths().isComplete();
        const bool inStore = parakeetPathsIn(storeDir()).isComplete();
        return present && !inStore;
    }
    bool anyResolved = false;
    bool anyInStore = false;
    for (const QString &fileName : layout.fileNames) {
        if (!resolveModelFile(fileName).isEmpty())
            anyResolved = true;
        if (QFileInfo::exists(storeDir().filePath(fileName)))
            anyInStore = true;
    }
    return anyResolved && !anyInStore;
}

bool deleteModel(const QString &id, QString *errorOut)
{
    const ModelLayout layout = modelLayout(id);
    if (!layout.valid) {
        if (errorOut)
            *errorOut = QStringLiteral("unknown model id: %1").arg(id);
        return false;
    }
    const QDir store = storeDir();
    if (!layout.bundleDirName.isEmpty()) {
        QDir bundle(store.filePath(layout.bundleDirName));
        if (!bundle.exists()) {
            if (errorOut)
                *errorOut = QStringLiteral("not present in the model store");
            return false;
        }
        if (!bundle.removeRecursively()) {
            if (errorOut)
                *errorOut = QStringLiteral("could not remove %1").arg(bundle.path());
            return false;
        }
        return true;
    }
    bool removedAny = false;
    for (const QString &fileName : layout.fileNames) {
        const QString path = store.filePath(fileName);
        if (QFileInfo::exists(path)) {
            if (!QFile::remove(path)) {
                if (errorOut)
                    *errorOut = QStringLiteral("could not remove %1").arg(path);
                return false;
            }
            removedAny = true;
        }
    }
    if (!removedAny) {
        if (errorOut)
            *errorOut = QStringLiteral("not present in the model store");
        return false;
    }
    return true;
}

bool importModel(const QString &id, const QString &sourcePath, QString *errorOut)
{
    const ModelLayout layout = modelLayout(id);
    if (!layout.valid) {
        if (errorOut)
            *errorOut = QStringLiteral("unknown model id: %1").arg(id);
        return false;
    }
    const QDir store = storeDir();
    if (!store.mkpath(".")) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(store.path());
        return false;
    }
    const QFileInfo sourceInfo(sourcePath);

    // A parakeet bundle or the diarization pair import from a DIRECTORY holding
    // the canonical files; a single-file model imports from a FILE.
    const bool multiFile = !layout.bundleDirName.isEmpty() || layout.fileNames.size() > 1;
    if (multiFile) {
        if (!sourceInfo.isDir()) {
            if (errorOut)
                *errorOut = QStringLiteral("expected a folder containing: %1").arg(layout.fileNames.join(", "));
            return false;
        }
        const QDir sourceDir(sourcePath);
        for (const QString &fileName : layout.fileNames) {
            if (!QFileInfo::exists(sourceDir.filePath(fileName))) {
                if (errorOut)
                    *errorOut = QStringLiteral("folder is missing %1").arg(fileName);
                return false;
            }
        }
        QDir destDir = store;
        if (!layout.bundleDirName.isEmpty()) {
            if (!store.mkpath(layout.bundleDirName)) {
                if (errorOut)
                    *errorOut = QStringLiteral("could not create %1").arg(store.filePath(layout.bundleDirName));
                return false;
            }
            destDir = QDir(store.filePath(layout.bundleDirName));
        }
        for (const QString &fileName : layout.fileNames) {
            const QString dest = destDir.filePath(fileName);
            QFile::remove(dest);
            if (!QFile::copy(sourceDir.filePath(fileName), dest)) {
                if (errorOut)
                    *errorOut = QStringLiteral("could not copy %1 into the store").arg(fileName);
                return false;
            }
        }
        return true;
    }

    if (!sourceInfo.isFile()) {
        if (errorOut)
            *errorOut = QStringLiteral("expected a file");
        return false;
    }
    const QString dest = store.filePath(layout.fileNames.first());
    QFile::remove(dest);
    if (!QFile::copy(sourcePath, dest)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not copy into the store");
        return false;
    }
    return true;
}

bool exportModel(const QString &id, const QString &destDir, QString *errorOut)
{
    const ModelLayout layout = modelLayout(id);
    if (!layout.valid) {
        if (errorOut)
            *errorOut = QStringLiteral("unknown model id: %1").arg(id);
        return false;
    }
    const QDir dest(destDir);
    if (!dest.exists()) {
        if (errorOut)
            *errorOut = QStringLiteral("destination folder does not exist");
        return false;
    }
    if (!layout.bundleDirName.isEmpty()) {
        const ParakeetModelPaths p = parakeetModelPaths();
        if (!p.isComplete()) {
            if (errorOut)
                *errorOut = QStringLiteral("model is not downloaded");
            return false;
        }
        if (!dest.mkpath(layout.bundleDirName)) {
            if (errorOut)
                *errorOut = QStringLiteral("could not create %1").arg(dest.filePath(layout.bundleDirName));
            return false;
        }
        const QDir outBundle(dest.filePath(layout.bundleDirName));
        const QString sources[4] = {p.encoder, p.decoder, p.joiner, p.tokens};
        for (int i = 0; i < layout.fileNames.size() && i < 4; ++i) {
            const QString out = outBundle.filePath(layout.fileNames.at(i));
            QFile::remove(out);
            if (!QFile::copy(sources[i], out)) {
                if (errorOut)
                    *errorOut = QStringLiteral("could not copy %1").arg(layout.fileNames.at(i));
                return false;
            }
        }
        return true;
    }
    for (const QString &fileName : layout.fileNames) {
        const QString src = resolveModelFile(fileName);
        if (src.isEmpty()) {
            if (errorOut)
                *errorOut = QStringLiteral("model is not downloaded");
            return false;
        }
        const QString out = dest.filePath(fileName);
        QFile::remove(out);
        if (!QFile::copy(src, out)) {
            if (errorOut)
                *errorOut = QStringLiteral("could not copy %1").arg(fileName);
            return false;
        }
    }
    return true;
}

}
