#include "ModelManager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace ModelManager {

const std::vector<ModelInfo> &availableModels()
{
    static const std::vector<ModelInfo> models = {
        {"tiny.en", "ggml-tiny.en.bin", "Tiny (English) — ~75 MB, fastest"},
        {"base.en", "ggml-base.en.bin", "Base (English) — ~148 MB, balanced"},
        {"small.en", "ggml-small.en.bin", "Small (English) — ~488 MB, more accurate"},
        {"medium.en", "ggml-medium.en.bin", "Medium (English) — ~1.5 GB, most accurate"},
    };
    return models;
}

namespace {

QString fileNameForModelId(const QString &modelId)
{
    for (const ModelInfo &info : availableModels()) {
        if (info.id == modelId)
            return info.fileName;
    }
    return {};
}

// Shared by ensureModel()/ensureLabelModel(): download url to
// <vault>/.transcriber/models/<fileName> via curl to a .part file, then
// rename into place. Returns the final path, or empty on failure (errorOut
// set if given). Assumes modelsDir already exists.
QString downloadToModelsDir(const QDir &modelsDir, const QString &fileName, const QString &url, QString *errorOut)
{
    const QString modelPath = modelsDir.filePath(fileName);
    const QString tmpPath = modelPath + ".part";

    QProcess curl;
    curl.start("curl", {"-L", "--fail", "-o", tmpPath, url});
    curl.waitForFinished(-1);

    if (curl.exitStatus() != QProcess::NormalExit || curl.exitCode() != 0) {
        if (errorOut)
            *errorOut = QStringLiteral("model download failed: %1").arg(QString::fromUtf8(curl.readAllStandardError()));
        QFile::remove(tmpPath);
        return {};
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
                                      const QString &pathInsideArchive, QString *errorOut)
{
    const QString archiveFileName = QStringLiteral("%1.tar.bz2").arg(localFileName);
    const QString archivePath = downloadToModelsDir(modelsDir, archiveFileName, archiveUrl, errorOut);
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

}

QString ensureModel(const QUrl &vaultPath, const QString &modelId, QString *errorOut, bool *usedFallback)
{
    if (usedFallback)
        *usedFallback = false;

    const QDir modelsDir(vaultPath.toLocalFile() + "/.transcriber/models");
    QString modelName = fileNameForModelId(modelId);
    if (modelName.isEmpty()) {
        // modelId isn't in availableModels() — never silently pretend the
        // requested model was used; the caller must be told.
        qWarning() << "ModelManager: unrecognized model id" << modelId << "— falling back to"
                   << kDefaultModelId;
        modelName = fileNameForModelId(kDefaultModelId);
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
    return downloadToModelsDir(modelsDir, modelName, repoUrl.arg(modelName), errorOut);
}

QString ensureLabelModel(const QUrl &vaultPath, QString *errorOut)
{
    const QDir modelsDir(vaultPath.toLocalFile() + "/.transcriber/models");
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
    return downloadToModelsDir(modelsDir, QString(kLabelModelFileName), url, errorOut);
}

bool ensureDiarizationModels(const QUrl &vaultPath, QString *segmentationPathOut, QString *embeddingPathOut,
                              QString *errorOut)
{
    const QDir modelsDir(vaultPath.toLocalFile() + "/.transcriber/models");

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
                                          errorOut)
                .isEmpty())
            return false;
    }
    if (!QFileInfo::exists(embeddingPath)) {
        // See kDiarizationEmbeddingFileName's doc comment (ModelManager.h)
        // for why this is WeSpeaker's ResNet293-LM rather than CAM++.
        const QString url = QStringLiteral(
            "https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/"
            "wespeaker_en_voxceleb_resnet293_LM.onnx");
        if (downloadToModelsDir(modelsDir, QString(kDiarizationEmbeddingFileName), url, errorOut).isEmpty())
            return false;
    }

    if (segmentationPathOut)
        *segmentationPathOut = segmentationPath;
    if (embeddingPathOut)
        *embeddingPathOut = embeddingPath;
    return true;
}

}
