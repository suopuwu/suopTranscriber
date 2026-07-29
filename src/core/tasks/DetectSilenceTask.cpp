#include "DetectSilenceTask.h"

#include <QFileInfo>

#include <utility>

#include "../AudioDecoder.h"
#include "../MdFrontmatter.h"
#include "../ModelManager.h"
#include "../TranscriptionController.h"
#include "../VadChunker.h"
#include "EngineCache.h"

DetectSilenceTask::DetectSilenceTask(QUrl file, QUrl vault, QObject *parent)
    : Task(parent)
{
    fileUrl = std::move(file);
    vaultUrl = std::move(vault);
}

QString DetectSilenceTask::displayName() const
{
    return QStringLiteral("Detect silences: %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
}

ModelPrerequisites DetectSilenceTask::prerequisites() const
{
    return {{ModelDownloadTarget::VadModel, QString()}};
}

std::vector<std::pair<double, double>> DetectSilenceTask::selectSilences(
    const QString &chunkingError, const std::vector<std::pair<double, double>> &speechSpans,
    const QList<ParsedSegment> &segments, double totalSeconds)
{
    // See the doc comment on the declaration for the empty-speechSpans
    // rationale.
    if (chunkingError.isEmpty() && !speechSpans.empty())
        return VadChunker::complementSpans(speechSpans, totalSeconds);

    // VAD unavailable/failed/empty despite the prerequisite — fall back to
    // the SAME transcript-gap heuristic TranscribeTask's inline capture uses
    // when VAD didn't run: treat consecutive segments as touching spans (the
    // file format stores no per-segment end time by design, so an
    // already-written .md can only ever recover a
    // LEADING silence gap before the first segment this way, never an
    // internal one; still better than leaving the key absent, matching
    // TranscribeTask's "so the key is always present" contract).
    std::vector<std::pair<double, double>> fallbackSpans;
    fallbackSpans.reserve(segments.size());
    for (int i = 0; i < segments.size(); ++i) {
        const double spanEnd = (i + 1 < segments.size()) ? segments.at(i + 1).t0 : totalSeconds;
        fallbackSpans.emplace_back(segments.at(i).t0, spanEnd);
    }
    return VadChunker::complementSpans(fallbackSpans, totalSeconds);
}

void DetectSilenceTask::run(TaskContext &ctx)
{
    // Needs no resident heavy model of its own (VAD loads/frees inline per
    // call — see VadChunker::computeWindows()) — the default engineNeeds()
    // (needs nothing) is correct here, so TaskRunner's dispatch-time
    // residency policy already freed whatever a previous task left resident.
    const QString filePath = fileUrl.toLocalFile();
    const QString mdPath = sidecarPath(filePath);
    if (!QFileInfo::exists(mdPath))
        throw TaskError(QStringLiteral("%1 has not been transcribed yet").arg(filePath));

    QStringList frontmatterLines;
    QString body;
    QString parseError;
    if (!MdFrontmatter::read(mdPath, &frontmatterLines, &body, &parseError))
        throw TaskError(parseError);

    // Only needed for the transcript-gap fallback below (parsedSegmentsFrom
    // TranscriptBody() needs the file's own "speakers:" allowlist to tell a
    // real "[Speaker N]" tag from ordinary bracketed text — see its doc
    // comment); read now, cheaply, alongside the rest of the frontmatter.
    QStringList knownSpeakers;
    for (const QString &line : std::as_const(frontmatterLines)) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon >= 0 && line.left(colon).trimmed() == QStringLiteral("speakers"))
            knownSpeakers = parseSpeakersList(line.mid(colon + 1));
    }

    // Decode the sibling audio (same 16k mono pass whisper/VAD use) —
    // pause/cancel honored between chunks, same as DiarizeTask.
    emit phase(QStringLiteral("decoding"));
    bool decodePausedSignaled = false;
    AudioDecoder::DecodeControl decodeControl;
    decodeControl.paused = [&ctx, &decodePausedSignaled] {
        const bool p = ctx.pauseRequested();
        if (p != decodePausedSignaled) {
            decodePausedSignaled = p;
            ctx.reportPaused(p);
        }
        return p;
    };
    decodeControl.cancelled = [&ctx] { return ctx.cancelRequested(); };
    QString decodeError;
    auto pcm = AudioDecoder::decodeTo16kMono(filePath, &decodeError, decodeControl);
    if (!pcm)
        throw TaskError(decodeError.isEmpty() ? QStringLiteral("audio decode failed") : decodeError);

    if (ctx.checkpoint())
        throw TaskError(QStringLiteral("Cancelled"));

    const double totalSeconds = static_cast<double>(pcm->size()) / 16000.0;

    emit phase(QStringLiteral("detecting-silence"));
    // VAD primary (Decision A, this feature): the model is a declared
    // prerequisite (see prerequisites() above), so by the time run() reaches
    // here the queue has already ensured it's on disk — ensureVadModel()
    // below is expected to just resolve the already-downloaded file, not
    // trigger a fresh network fetch.
    QString ensureError;
    const QString vadModelPath = ModelManager::ensureVadModel(&ensureError);

    QString chunkingError;
    std::vector<std::pair<double, double>> speechSpans;
    if (!vadModelPath.isEmpty())
        VadChunker::computeWindows(*pcm, vadModelPath, &chunkingError, &speechSpans);
    const QList<ParsedSegment> segments = parsedSegmentsFromTranscriptBody(body, knownSpeakers);
    const std::vector<std::pair<double, double>> silences =
        selectSilences(chunkingError, speechSpans, segments, totalSeconds);

    if (ctx.checkpoint())
        throw TaskError(QStringLiteral("Cancelled"));

    // Only this one key changes — segments/speakers/chapters/label are left
    // completely untouched (upsertKey() rewrites just this line; body is
    // passed straight through unmodified).
    MdFrontmatter::upsertKey(&frontmatterLines, QStringLiteral("silences"), formatSilencesValue(silences));

    QString writeError;
    if (!writeFrontmatterBlock(mdPath, frontmatterLines, body, &writeError))
        throw TaskError(writeError);
}
