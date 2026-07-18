#pragma once

#include <QString>
#include <QList>

// Which model a download targets. Its own header (rather than nested in
// DownloadTask) so Task.h can declare prerequisites() — "the models this kind
// needs on disk before it can run" — without including DownloadTask.h, which
// includes Task.h right back.
enum class ModelDownloadTarget { WhisperModel, LabelModel, VadModel, SegmentationModel, EmbeddingModel };

// One model a Task needs present before it can run. The queue turns each into
// its own DownloadTask inserted AHEAD of the requesting task (deduped — see
// TranscriptionController::maybeQueueDownloadTask()). modelId is only
// meaningful for WhisperModel/SegmentationModel/EmbeddingModel; LabelModel and
// VadModel are each a single fixed file.
struct ModelPrerequisite {
    ModelDownloadTarget target = ModelDownloadTarget::WhisperModel;
    QString modelId;
};
using ModelPrerequisites = QList<ModelPrerequisite>;
