#include "RelabelTask.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

#include "../LabelingEngine.h"
#include "../TranscriptionController.h"
#include "EngineCache.h"

RelabelTask::RelabelTask(QUrl file, QUrl vault, bool restrictTagsToExisting, double tagReuseBias, double wordiness,
                         bool skipIfAlreadyLabeled, QObject *parent)
    : Task(parent)
    , m_restrictTagsToExisting(restrictTagsToExisting)
    , m_tagReuseBias(tagReuseBias)
    , m_wordiness(wordiness)
    , m_skipIfAlreadyLabeled(skipIfAlreadyLabeled)
{
    fileUrl = std::move(file);
    vaultUrl = std::move(vault);
}

QString RelabelTask::displayName() const
{
    return QStringLiteral("Regenerate label: %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
}

QJsonObject RelabelTask::toJson() const
{
    return {{QStringLiteral("restrictTagsToExisting"), m_restrictTagsToExisting},
            {QStringLiteral("tagReuseBias"), m_tagReuseBias},
            {QStringLiteral("wordiness"), m_wordiness}};
}

QVariantMap RelabelTask::toVariantMap() const
{
    return {};
}

ModelPrerequisites RelabelTask::prerequisites() const
{
    return {{ModelDownloadTarget::LabelModel, QString()}};
}

void RelabelTask::run(TaskContext &ctx)
{
    // Mirrors TranscribeTask's eviction of the label engine, inverted: this
    // task needs only the label engine, so free whisper's GPU-resident model
    // now rather than let it sit warm through a labeling pass that never
    // touches it. Diarization is never actually resident here either —
    // evicted defensively in case that ever changes.
    ctx.engines.evictAsr();
    ctx.engines.evictDiar();

    const QString filePath = fileUrl.toLocalFile();
    const QString mdPath = sidecarPath(filePath);
    if (!QFileInfo::exists(mdPath))
        throw TaskError(QStringLiteral("%1 has not been transcribed yet").arg(filePath));

    QFile mdFile(mdPath);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
        throw TaskError(QStringLiteral("could not open %1").arg(mdPath));
    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---"))
        throw TaskError(QStringLiteral("%1 has no frontmatter block").arg(mdPath));

    // Pull out "speakers:"/"label:"/"tags:" while skipping the rest of the
    // frontmatter — speakers feeds plainTextFromTranscriptBody()'s known-
    // speaker allowlist; label/tags feed the skipIfAlreadyLabeled check.
    QStringList knownSpeakers;
    QString existingLabel;
    QString existingTags;
    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QString key = line.left(colon).trimmed();
        if (key == QStringLiteral("speakers"))
            knownSpeakers = parseSpeakersList(line.mid(colon + 1));
        else if (key == QStringLiteral("label"))
            existingLabel = line.mid(colon + 1).trimmed();
        else if (key == QStringLiteral("tags"))
            existingTags = line.mid(colon + 1).trimmed();
    }
    const QString rawBody = in.readAll();
    const QString transcriptText = plainTextFromTranscriptBody(rawBody, knownSpeakers);
    mdFile.close();

    // This is the automatic post-transcribe follow-up, not an explicit user
    // request — don't clobber label/tags/prelude a previous run (or a manual
    // edit) already produced.
    if (m_skipIfAlreadyLabeled
        && (!existingLabel.isEmpty() || !existingTags.isEmpty() || rawBody.contains(kPreludeEndMarker)))
        return; // success, nothing to do

    if (transcriptText.trimmed().isEmpty())
        throw TaskError(QStringLiteral("%1 has no transcript text to label").arg(mdPath));

    emit phase(QStringLiteral("labeling"));

    QString error;
    LabelingEngine *labelingEngine = ctx.engines.ensureLabel(&error);
    if (!labelingEngine)
        throw TaskError(error);

    // existingTags is ALWAYS collected (cheap — vaults are small), not just
    // when restrictTagsToExisting is set — tagReuseBias needs a vocabulary
    // to bias toward even in unrestricted mode. nullptr = no cache: this
    // runs on the worker thread (must not touch the GUI thread's cache) and
    // only once per task.
    const QStringList existingTagsVocab = collectVaultTags(vaultUrl.toLocalFile(), nullptr);
    const int wordCount =
        transcriptText.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size();
    const double effectiveWordiness = TranscriptionController::effectiveWordinessForLength(m_wordiness, wordCount);

    QString genError;
    const auto result = labelingEngine->label(transcriptText, existingTagsVocab, m_restrictTagsToExisting,
                                               m_tagReuseBias, effectiveWordiness, &genError);
    if (!result)
        throw TaskError(QStringLiteral("auto-label generation failed: %1").arg(genError));

    writeLabelingFields(mdPath, *result);
}
