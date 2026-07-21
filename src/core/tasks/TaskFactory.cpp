#include "TaskFactory.h"

#include <QJsonArray>
#include <QUrl>

#include "../ModelManager.h"
#include "ChaptersTask.h"
#include "CombineTask.h"
#include "DetectSilenceTask.h"
#include "DiarizeTask.h"
#include "DownloadTask.h"
#include "MergeSpeakersTask.h"
#include "RelabelTask.h"
#include "RenameSpeakerTask.h"
#include "Task.h"
#include "TranscribeTask.h"

std::unique_ptr<Task> createTaskFromJson(const QString &kind, const QJsonObject &obj)
{
    const QUrl fileUrl(obj.value(QStringLiteral("fileUrl")).toString());
    const QUrl vaultUrl(obj.value(QStringLiteral("vaultUrl")).toString());

    std::unique_ptr<Task> task;

    if (kind == QStringLiteral("transcribe")) {
        task = std::make_unique<TranscribeTask>(fileUrl, vaultUrl, obj.value(QStringLiteral("modelId")).toString());
    } else if (kind == QStringLiteral("relabel")) {
        task = std::make_unique<RelabelTask>(fileUrl, vaultUrl,
                                              obj.value(QStringLiteral("restrictTagsToExisting")).toBool(),
                                              obj.value(QStringLiteral("tagReuseBias")).toDouble(0.5),
                                              obj.value(QStringLiteral("wordiness")).toDouble(0.5),
                                              /*skipIfAlreadyLabeled=*/false);
    } else if (kind == QStringLiteral("chapters")) {
        task = std::make_unique<ChaptersTask>(fileUrl, vaultUrl,
                                               obj.value(QStringLiteral("chapterDensity")).toDouble(0.5));
    } else if (kind == QStringLiteral("diarize")) {
        // Empty ids ⇒ ModelManager defaults (an entry persisted before these
        // became selectable).
        const QString segId = obj.value(QStringLiteral("segmentationModelId")).toString().isEmpty()
                                  ? QString::fromLatin1(ModelManager::kDefaultSegmentationModelId)
                                  : obj.value(QStringLiteral("segmentationModelId")).toString();
        const QString embedId = obj.value(QStringLiteral("embeddingModelId")).toString().isEmpty()
                                    ? QString::fromLatin1(ModelManager::kDefaultEmbeddingModelId)
                                    : obj.value(QStringLiteral("embeddingModelId")).toString();
        task = std::make_unique<DiarizeTask>(fileUrl, vaultUrl, segId, embedId,
                                              obj.value(QStringLiteral("diarizationThreshold")).toDouble(0.5));
    } else if (kind == QStringLiteral("detectSilence")) {
        task = std::make_unique<DetectSilenceTask>(fileUrl, vaultUrl);
    } else if (kind == QStringLiteral("rename")) {
        task = std::make_unique<RenameSpeakerTask>(fileUrl, obj.value(QStringLiteral("oldSpeakerName")).toString(),
                                                    obj.value(QStringLiteral("newSpeakerName")).toString());
    } else if (kind == QStringLiteral("merge")) {
        task = std::make_unique<MergeSpeakersTask>(fileUrl, obj.value(QStringLiteral("sourceSpeakerName")).toString(),
                                                    obj.value(QStringLiteral("targetSpeakerName")).toString());
    } else if (kind == QStringLiteral("combine")) {
        QStringList sources;
        for (const QJsonValue &v : obj.value(QStringLiteral("combineSourcePaths")).toArray())
            sources << v.toString();
        task = std::make_unique<CombineTask>(sources, fileUrl,
                                              obj.value(QStringLiteral("deleteOriginalsAfterCombine")).toBool());
    } else if (kind == QStringLiteral("download")) {
        ModelDownloadTarget target;
        if (!DownloadTask::targetFromString(obj.value(QStringLiteral("downloadTarget")).toString(), &target))
            return nullptr;
        task = std::make_unique<DownloadTask>(target, obj.value(QStringLiteral("modelId")).toString());
    } else {
        return nullptr; // unrecognized kind
    }

    // Defence in depth: the opt-in persistence contract enforced on the READ
    // side too, so a stale/hand-edited queue file can't resurrect a kind that
    // should never have been written (see this function's doc comment).
    if (!task->isPersistable())
        return nullptr;

    task->vaultUrl = vaultUrl;
    return task;
}
