#include "ModelManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace ModelManager {

QString ensureModel(const QUrl &vaultPath, bool speakerDetection, QString *errorOut)
{
    const QDir modelsDir(vaultPath.toLocalFile() + "/.transcriber/models");
    const QString modelName = speakerDetection ? kTdrzModelName : kModelName;
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
