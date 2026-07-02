#include "ModelManager.h"

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

}

QString ensureModel(const QUrl &vaultPath, const QString &modelId, bool speakerDetection, QString *errorOut)
{
    const QDir modelsDir(vaultPath.toLocalFile() + "/.transcriber/models");
    QString modelName = speakerDetection ? QString(kTdrzModelName) : fileNameForModelId(modelId);
    if (modelName.isEmpty())
        modelName = fileNameForModelId(kDefaultModelId);
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
    const QString url = repoUrl.arg(modelName);
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
