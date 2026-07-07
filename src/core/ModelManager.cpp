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

}

QString ensureModel(const QUrl &vaultPath, const QString &modelId, bool speakerDetection, QString *errorOut,
                     bool *usedFallback)
{
    if (usedFallback)
        *usedFallback = false;

    const QDir modelsDir(vaultPath.toLocalFile() + "/.transcriber/models");
    QString modelName = speakerDetection ? QString(kTdrzModelName) : fileNameForModelId(modelId);
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

    // tinydiarize's -tdrz fine-tune is published in a different HF repo than
    // ggerganov's standard models.
    const QString repoUrl = speakerDetection
        ? QStringLiteral("https://huggingface.co/akashmjn/tinydiarize-whisper.cpp/resolve/main/%1")
        : QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/%1");
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

}
