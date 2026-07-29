#include "ChaptersTask.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <cmath>

#include "../LabelingEngine.h"
#include "../TranscriptionController.h"
#include "EngineCache.h"

// See chaptersGenerateProgress()'s doc comment in ChaptersTask.h. ~320 output
// tokens is a typical chapter-array generation for one full 16k-char window;
// used only as the ramp's time constant, so being off by 2x just makes the
// ramp reach ~86% instead of ~63% of the window's span before it snaps.
static constexpr double kTypicalChapterOutputTokens = 320.0;

double chaptersGenerateProgress(int w, int windowCount, int tokens)
{
    if (windowCount <= 0)
        return 0.0;
    const double span = kChaptersGenerateBand / windowCount;
    const double base = span * w;
    const double ramp = 1.0 - std::exp(-static_cast<double>(qMax(0, tokens)) / kTypicalChapterOutputTokens);
    return qMin(kChaptersGenerateBand, base + span * qMin(0.97, ramp));
}

// See this function's doc comment in ChaptersTask.h.
QList<Chapter> withOpeningChapter(const QList<Chapter> &chapters, const QString &title)
{
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty())
        return chapters;
    if (!chapters.isEmpty() && chapters.first().afterSegmentIndex < 0)
        return chapters; // already opens at segment 0

    QList<Chapter> result;
    result.reserve(chapters.size() + 1);
    result.append({-1, trimmed});
    result.append(chapters);
    return result;
}

ChaptersTask::ChaptersTask(QUrl file, QUrl vault, double density, QObject *parent)
    : Task(parent)
    , m_density(density)
{
    fileUrl = std::move(file);
    vaultUrl = std::move(vault);
}

QString ChaptersTask::displayName() const
{
    return QStringLiteral("Generate chapters: %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
}

QJsonObject ChaptersTask::toJson() const
{
    return {{QStringLiteral("chapterDensity"), m_density}};
}

QVariantMap ChaptersTask::toVariantMap() const
{
    return {};
}

ModelPrerequisites ChaptersTask::prerequisites() const
{
    return {{ModelDownloadTarget::LabelModel, QString()}};
}

void ChaptersTask::run(TaskContext &ctx)
{
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

    QStringList knownSpeakers;
    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        const int colon = line.indexOf(':');
        if (colon >= 0 && line.left(colon).trimmed() == QStringLiteral("speakers"))
            knownSpeakers = parseSpeakersList(line.mid(colon + 1));
    }
    const QString body = in.readAll();
    mdFile.close();

    // Fresh parse of THIS task's own read of the file — index i's position
    // in this list IS the index LabelingEngine::generateChapters() expects
    // and returns "afterSegment" against.
    const QList<ParsedSegment> segments = parsedSegmentsFromTranscriptBody(body, knownSpeakers);
    if (segments.isEmpty())
        throw TaskError(QStringLiteral("%1 has no transcript text to generate chapters from").arg(mdPath));

    QString error;
    LabelingEngine *labelingEngine = ctx.engines.ensureLabel(&error);
    if (!labelingEngine)
        throw TaskError(error);

    QStringList segmentTexts;
    segmentTexts.reserve(segments.size());
    for (const ParsedSegment &segment : segments)
        segmentTexts.append(segment.text);

    // generateChapters() silently truncates any single call whose formatted
    // input exceeds LabelingEngine::kMaxChapterInputChars — split into
    // multiple windows, each kept under that budget, offsetting each
    // window's returned afterSegmentIndex by its starting index before
    // merging.
    const QList<int> windowStarts = computeChapterWindows(segmentTexts);

    // Chapter generation is a per-window sequence of LLM calls with a known
    // window count up front — but on a long file that is only ~5 windows,
    // and the merge pass afterwards is a further chapters-1 calls. Reporting
    // one unit per window (plus one for everything after) made a 100-minute
    // file advance in 20% jumps with a long dead stretch at the end. So the
    // bar is banded instead: generation owns [0, kChaptersGenerateBand),
    // subdivided per window AND within a window by tokens decoded; the merge
    // pass owns up to kChaptersMergeBandEnd, subdivided per comparison.
    emit phase(QStringLiteral("chaptering"));
    emit progress(0.0);
    const int windowCount = windowStarts.size();

    QList<Chapter> chapters;
    for (int w = 0; w < windowStarts.size(); ++w) {
        // Cooperative cancel/pause checkpoint between windows — an in-flight
        // single generateChapters() call can't be interrupted, but checking
        // here means a cancel takes effect promptly between windows rather
        // than only after the entire task.
        if (ctx.checkpoint())
            throw TaskError(QStringLiteral("Cancelled"));

        const int startIdx = windowStarts.at(w);
        const int endIdx = (w + 1 < windowStarts.size()) ? windowStarts.at(w + 1) : segmentTexts.size();
        const QStringList windowTexts = segmentTexts.mid(startIdx, endIdx - startIdx);

        QString genError;
        const auto windowChapters =
            labelingEngine->generateChapters(windowTexts, m_density, &genError, [this, w, windowCount](int tokens) {
                emit progress(chaptersGenerateProgress(w, windowCount, tokens));
            });
        if (!windowChapters)
            throw TaskError(QStringLiteral("chapter generation failed: %1").arg(genError));
        // Snap to this window's upper edge — the ramp deliberately never
        // reaches it on its own.
        emit progress(kChaptersGenerateBand * (w + 1) / windowCount);
        // afterSegmentIndex is 0-relative to THIS window's own windowTexts
        // slice — offset by startIdx so it's relative to the full file's
        // segments list instead, as if one single call had produced it.
        for (const Chapter &chapter : *windowChapters)
            chapters.append({chapter.afterSegmentIndex + startIdx, chapter.title});
    }

    // The LLM only ever returns which SEGMENT INDEX a chapter starts after
    // — never trusted as a timestamp directly. Sanitized against this
    // task's own freshly-parsed `segments`, before anything touches disk.
    // Out-of-range or duplicate target indices (and an empty title) are
    // silently dropped, not a task failure. Sorted by afterSegmentIndex
    // ascending: the merge pass below treats list order AS adjacency.
    QList<Chapter> sanitized;
    QSet<int> seenTargets;
    for (const Chapter &chapter : std::as_const(chapters)) {
        const int targetIndex = chapter.afterSegmentIndex + 1;
        const QString title = chapter.title.trimmed();
        if (targetIndex < 0 || targetIndex >= segments.size() || title.isEmpty())
            continue;
        if (seenTargets.contains(targetIndex))
            continue;
        seenTargets.insert(targetIndex);
        sanitized.append({chapter.afterSegmentIndex, title});
    }
    std::sort(sanitized.begin(), sanitized.end(),
              [](const Chapter &a, const Chapter &b) { return a.afterSegmentIndex < b.afterSegmentIndex; });

    // One more cancel/pause checkpoint before the merge pass (itself a run
    // of LLM calls).
    if (ctx.checkpoint())
        throw TaskError(QStringLiteral("Cancelled"));

    // Small instruct models over-propose boundaries — collapse over-eager
    // adjacent chapters with a second, narrower pass. A refinement on an
    // already-valid result, not a prerequisite: a failure here falls back
    // to the unmerged-but-sanitized chapters instead of failing the task.
    emit phase(QStringLiteral("merging-chapters"));
    QList<Chapter> finalChapters = sanitized;
    QString mergeError;
    if (const auto merged = labelingEngine->mergeSimilarChapters(
            sanitized, segmentTexts, m_density, &mergeError, [this](int done, int total) {
                if (total <= 0)
                    return;
                emit progress(kChaptersGenerateBand
                              + (kChaptersMergeBandEnd - kChaptersGenerateBand) * done / total);
            }))
        finalChapters = *merged;
    else
        qWarning() << "ChaptersTask: chapter-merge pass failed, keeping unmerged chapters:" << mergeError;

    // generateChapters() is told to SKIP the very first chapter (it "always
    // starts at segment 0 implicitly"), so nothing above ever names the
    // transcript's opening stretch and the file's first minutes sit under no
    // heading at all. Synthesize that chapter here so a transcript ALWAYS
    // opens under one: it spans segment 0 up to the first real boundary — the
    // WHOLE file when the model proposed no boundaries, which is exactly the
    // single-topic case that should still get one chapter — and is titled by
    // its own narrow pass over precisely that stretch. Non-fatal on failure,
    // same policy as the merge pass above: shipping the boundaries we already
    // have beats failing the whole task over one title.
    if (ctx.checkpoint())
        throw TaskError(QStringLiteral("Cancelled"));
    const int openingEnd = finalChapters.isEmpty() ? segments.size() : finalChapters.first().afterSegmentIndex + 1;
    QString openingError;
    if (const auto openingTitle = labelingEngine->openingChapterTitle(segmentTexts.mid(0, openingEnd), &openingError))
        finalChapters = withOpeningChapter(finalChapters, *openingTitle);
    else
        qWarning() << "ChaptersTask: opening-chapter title pass failed, leaving the opening stretch untitled:"
                   << openingError;

    // afterSegmentIndex -1 (the synthesized opening chapter) resolves to
    // segments.at(0).t0 through this same expression — no special case.
    QList<ResolvedChapter> resolved;
    resolved.reserve(finalChapters.size());
    for (const Chapter &chapter : std::as_const(finalChapters))
        resolved.append({segments.at(chapter.afterSegmentIndex + 1).t0, chapter.title});

    QString writeError;
    if (!writeChaptersField(mdPath, resolved, &writeError))
        throw TaskError(writeError);
    emit progress(1.0);
}
