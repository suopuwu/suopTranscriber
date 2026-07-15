#include "TranscriptionController.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QTemporaryFile>
#include <QTextStream>
#include <QTimer>
#include <QVector>

#include <csignal>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <optional>

#include "AudioDecoder.h"
#include "DiarizationEngine.h"
#include "LabelingEngine.h"
#include "ModelManager.h"
#include "Segment.h"
#include "VadChunker.h"
#include "WhisperCppEngine.h"

namespace {

// Compact H:MM:SS.fff — leading zero-valued units (hours, then minutes) are
// dropped entirely rather than shown as "0:"/"00:", and the fractional
// part's trailing zeros are trimmed (dropped entirely for a whole-second
// value) — e.g. 7.25 -> "7.25", 67.0 -> "1:07", 3725.25 -> "1:02:05.25".
// No precision is lost (this only removes redundant characters, unlike the
// GUTTER display's separate 1-decimal rounding in MainPanel.qml).
QString formatTimestamp(double seconds)
{
    const qint64 totalMs = static_cast<qint64>(seconds * 1000.0 + 0.5);
    const qint64 h = totalMs / 3600000;
    const qint64 m = (totalMs / 60000) % 60;
    const qint64 s = (totalMs / 1000) % 60;
    const qint64 ms = totalMs % 1000;

    QString msPart;
    if (ms > 0) {
        msPart = QStringLiteral(".") + QString::number(ms).rightJustified(3, QChar('0'));
        while (msPart.endsWith(QChar('0')))
            msPart.chop(1);
    }

    if (h > 0) {
        return QStringLiteral("%1:%2:%3%4")
            .arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')).arg(msPart);
    }
    if (m > 0) {
        return QStringLiteral("%1:%2%3").arg(m).arg(s, 2, 10, QChar('0')).arg(msPart);
    }
    return QStringLiteral("%1%2").arg(s).arg(msPart);
}

// Item P7: derived from the audio file's FULL name (fi.fileName(), extension
// included), not just its stripped stem (fi.completeBaseName()) — two audio
// files sharing a stem with different extensions (e.g. "meeting.wav" +
// "meeting.mp3") must get distinct sidecars ("meeting.wav.md" /
// "meeting.mp3.md"), never collide on one "meeting.md". Every sidecar-path
// call site in this file and in VaultTreeModel.cpp (sidecarMdPath()) must
// stay consistent with this same convention. Deliberate, confirmed
// trade-off (2026-07-11): this DOES orphan a pre-existing vault's sidecars
// that were written under the old completeBaseName()-based convention
// before this change — accepted by the user as a one-time rename they'll
// do themselves, in exchange for the stem-collision safety this buys going
// forward. Do not revert without checking with the user first.
// Self-referential for a path that's already a .md (an orphaned sidecar with
// no matching audio) — mirrors VaultTreeModel::sidecarMdPath()'s isOrphanedMd
// case. Without this, an orphaned .md's own path got ".md" appended AGAIN
// (fi.fileName() keeps the existing ".md"), producing a nonexistent
// "foo.wav.md.md" and silently empty readMetadata()/readTranscriptBody()
// results — this is why orphaned entries fell back to showing their filename
// instead of their stored label.
QString sidecarPath(const QString &audioFilePath)
{
    const QFileInfo fi(audioFilePath);
    if (fi.suffix().compare(QStringLiteral("md"), Qt::CaseInsensitive) == 0)
        return fi.absoluteFilePath();
    return fi.absolutePath() + "/" + fi.fileName() + ".md";
}

// Minimum spacing between TranscriptionWorker::segmentWritten() emissions
// (see run()'s onSegment) — a chatty stretch of audio can produce several
// whisper segments within milliseconds of each other, and each live-update
// tick costs QML a full file re-read/re-parse/re-render (see
// TranscriptionController::liveUpdateTick's doc comment), so bursts are
// coalesced down to this rate rather than firing one tick per segment.
constexpr qint64 kLiveUpdateThrottleMs = 200;

// whisper.cpp occasionally hallucinates a bracketed non-speech annotation
// (e.g. "[APPLAUSE]", "[MUSIC]", "[BLANK_AUDIO]") as a segment's ENTIRE
// text — same shape segmentLinePattern()'s doc comment already guards
// against on the parsing side. Nobody is actually speaking during one of
// these, so it must never get a diarized speaker label: labelForSegment()
// below always force-assigns SOME turn (even the merely-nearest one when
// none genuinely overlaps, see its own doc comment), which previously went
// unnoticed only because speaker detection defaulted off. With detection
// now on by default, these annotations were getting labeled like real
// speech — and since a run of them can each land nearest a DIFFERENT real
// speaker, that also broke SegmentLineWriter::tryMergeCycle()'s repeat
// collapsing (which requires matching speakerLabel AND text to fold
// duplicates), turning what should be one collapsed "(×N)" line back into
// many individually-mislabeled repeats.
bool isNonSpeechAnnotation(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^\\[[^\\]]+\\]$"));
    return pattern.match(text).hasMatch();
}

// Assigns a whisper segment [t0,t1] the diarized turn it overlaps most with
// (by duration) — turns are few (typically a handful per file), so a linear
// scan per segment is cheap. Picks the LEAST-negative "overlap" (i.e.
// nearest turn) even when no turn genuinely overlaps at all (a segment
// falling entirely in a gap diarization didn't attribute to anyone), so
// every segment gets a label whenever diarization produced at least one
// turn. Empty turns list (diarization found nothing, or was skipped) yields
// an empty label — no tag written (see formatSegmentLine).
QString labelForSegment(const std::vector<DiarizedTurn> &turns, double t0, double t1)
{
    if (turns.empty())
        return {};
    double bestOverlap = -std::numeric_limits<double>::infinity();
    int bestSpeaker = 0;
    for (const DiarizedTurn &turn : turns) {
        const double overlap = std::min(t1, turn.end) - std::max(t0, turn.start);
        if (overlap > bestOverlap) {
            bestOverlap = overlap;
            bestSpeaker = turn.speaker;
        }
    }
    return QStringLiteral("Speaker %1").arg(bestSpeaker + 1);
}

// DiarizationEngine renumbers cluster ids 0-based in first-appearance order
// (see its own doc comment) — the number of distinct speakers is simply one
// past the highest id seen, with no turns at all meaning zero speakers.
int distinctSpeakerCount(const std::vector<DiarizedTurn> &turns)
{
    int maxSpeaker = -1;
    for (const DiarizedTurn &turn : turns)
        maxSpeaker = std::max(maxSpeaker, turn.speaker);
    return maxSpeaker + 1;
}

// Inverse of formatTimestamp(): splits on ':' rather than matching fixed
// digit widths, since the compact format's unit count/width varies
// ("7.25", "1:07", "1:02:05.25" are 1/2/3 colon-separated parts
// respectively) — much simpler than a digit-counting regex per shape.
double parseCompactTimestamp(const QString &raw)
{
    const QStringList parts = raw.split(QLatin1Char(':'));
    bool ok = true;
    double result = 0.0;
    if (parts.size() >= 3) {
        bool okH = false, okM = false, okS = false;
        result = parts[0].toDouble(&okH) * 3600.0 + parts[1].toDouble(&okM) * 60.0 + parts[2].toDouble(&okS);
        ok = okH && okM && okS;
    } else if (parts.size() == 2) {
        bool okM = false, okS = false;
        result = parts[0].toDouble(&okM) * 60.0 + parts[1].toDouble(&okS);
        ok = okM && okS;
    } else {
        result = parts[0].toDouble(&ok);
    }
    // Item O32: a malformed/non-numeric timestamp used to silently fall back
    // to 0.0 with no diagnostic — a corrupted .md file's mis-sorted/
    // mis-highlighted segment was otherwise undiagnosable. The 0.0 fallback
    // itself is unchanged (graceful degradation is still correct here), this
    // just stops it from being silent.
    if (!ok)
        qWarning() << "parseCompactTimestamp: could not parse timestamp" << raw;
    return result;
}

// Splits a frontmatter "speakers: A, B, C" value into trimmed, non-empty
// names — same comma-splitting shape as collectVaultTags()' "tags:"
// handling. An empty/missing "speakers:" key (speaker detection was off, or
// this predates that key) yields an empty list.
QStringList parseSpeakersList(const QString &raw)
{
    QStringList result;
    for (const QString &name : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty())
            result.append(trimmed);
    }
    return result;
}

// Matches SegmentLineWriter's exact output: "[H:MM:SS.fff] [Speaker N] text"
// (compact timestamp — see formatTimestamp) with an optional trailing
// " (×N)" repeat indicator and/or " [SPEAKER_TURN]" marker. MultilineOption
// lets ^/$ match at each line boundary so globalMatch() can walk every
// segment line in one pass over the whole (multi-line) body, giving
// capturedStart(3)/capturedLength(3) as exact offsets into that same body
// string — what QML needs to span-highlight the right substring.
//
// knownSpeakers (the file's frontmatter "speakers:" allowlist, see
// writeFrontmatter()'s doc comment) is REQUIRED, not cosmetic: whisper.cpp
// occasionally hallucinates bracketed non-speech annotations (e.g.
// "[BLANK_AUDIO]", "[MUSIC PLAYING]") that sit in exactly the position a
// real "[Speaker N]" tag would — an earlier version of this pattern matched
// ANY `[^\]]+` there, so a hallucinated annotation got misread as a brand
// new "speaker" (and, once SegmentLineWriter started merging repeated
// identical lines, that misreading became highly visible: dozens of
// hallucinated repeats collapsing into one bogus "speaker said X (×32)").
// The speaker-tag group here is instead an ALTERNATION of the file's own
// known names, built fresh per call — only an EXACT, literal match against
// a name actually listed in frontmatter is ever treated as a speaker tag;
// anything else "[...]"-shaped is left as ordinary text. An empty
// knownSpeakers list (speaker detection was off, or a legacy file predating
// the "speakers:" key) makes the tag group permanently unmatchable via the
// "(?!)" sentinel (a zero-width assertion that always fails) — every
// bracketed run is just text, never a fake speaker.
//
// Group map: 1 the whole timestamp expression (parsed via
// parseCompactTimestamp, not fixed-width digit groups — the compact format's
// shape varies), 2 the speaker tag's literal text (Item 11, Wave E —
// optional, only present when it matches one of knownSpeakers exactly), 3
// text, 4 the repeat count digits from a trailing " (×N)" marker
// (SegmentLineWriter's identical-consecutive-line merge — optional,
// absent/1 means the line wasn't a merge), 5 legacy trailing
// " [SPEAKER_TURN]" marker (older files only). This group SHAPE (which
// numbered group means what) never changes regardless of knownSpeakers'
// content, only what group 2 is allowed to match — callers can rely on
// fixed group indices either way. No effort to parse the OLDEST fixed-width
// "HH:MM:SS.mmm" format precisely by shape — the flexible `[\d:.]+` capture
// happens to still parse it correctly (splits into the same 3 colon-parts),
// so old single-timestamp files keep working for free. An optional, ignored
// "- <timestamp>" end-range suffix is tolerated too, for the oldest
// range-format files — full backward compatibility isn't a goal, but there's
// no reason to gratuitously break already-transcribed files when tolerating
// them costs almost nothing here.
QRegularExpression segmentLinePattern(const QStringList &knownSpeakers)
{
    QString tagAlternation = QStringLiteral("(?!)");
    if (!knownSpeakers.isEmpty()) {
        QStringList escaped;
        escaped.reserve(knownSpeakers.size());
        for (const QString &name : knownSpeakers)
            escaped.append(QRegularExpression::escape(name));
        tagAlternation = escaped.join(QLatin1Char('|'));
    }
    return QRegularExpression(
        QStringLiteral("^\\[([\\d:.]+)(?: - [\\d:.]+)?\\]"
                        "(?: \\[(%1)\\])? (.*?)(?: \\(×(\\d+)\\))?( \\[SPEAKER_TURN\\])?$")
            .arg(tagAlternation),
        QRegularExpression::MultilineOption);
}

QString jobStatusToString(TranscriptionController::JobStatus status)
{
    switch (status) {
    case TranscriptionController::JobStatus::None:
        return QString();
    case TranscriptionController::JobStatus::Queued:
        return QStringLiteral("queued");
    case TranscriptionController::JobStatus::Active:
        return QStringLiteral("active");
    case TranscriptionController::JobStatus::Paused:
        return QStringLiteral("paused");
    case TranscriptionController::JobStatus::Done:
        return QStringLiteral("done");
    case TranscriptionController::JobStatus::Failed:
        return QStringLiteral("failed");
    }
    return QString();
}

// Item P6: priority order rebuildJobStatusIndex() uses to pick ONE status
// when multiple m_jobs entries share a path (e.g. a Done history entry plus
// a freshly re-queued run) — "in progress" beats "finished", and among
// finished entries Failed beats Done since a failure is the more actionable
// one for the file tree to surface.
int jobStatusPriority(TranscriptionController::JobStatus status)
{
    switch (status) {
    case TranscriptionController::JobStatus::Active:
        return 5;
    case TranscriptionController::JobStatus::Paused:
        return 4;
    case TranscriptionController::JobStatus::Queued:
        return 3;
    case TranscriptionController::JobStatus::Failed:
        return 2;
    case TranscriptionController::JobStatus::Done:
        return 1;
    case TranscriptionController::JobStatus::None:
        return 0;
    }
    return 0;
}

// Written immediately when a job starts, before whisper_full runs, so a
// killed/stopped run still leaves a file recording which settings it was
// transcribed with (see Milestone 4 Task 3 — durability, not full resume).
//
// modelFallback/modelRequested are only written when ModelManager had to
// substitute its default model for an unrecognized requestedModelId (see
// ModelManager::ensureModel's usedFallback out-param) — the frontmatter is
// this app's single source of truth, so a fallback must be visible there,
// not just logged. `model:` always names the model file that actually ran;
// these two extra keys exist so the file never reads as if the requested
// model were honored when it wasn't.
//
// `speakers:` (comma-separated, only written when speakerDetection is on) is
// the ALLOWLIST of names segmentLinePattern() is willing to recognize as a
// leading "[tag]" speaker marker in the body — see that function's doc
// comment for why an open-ended `[^\]]+` match is unsafe (whisper.cpp
// hallucinates bracketed non-speech annotations like "[BLANK_AUDIO]" that
// must NOT be misread as a speaker). speakerLabels is the REAL distinct
// label set DiarizationEngine's clustering found — known up front (before
// whisper.cpp even runs, see TranscriptionWorker::run()), since diarization
// is a separate pass over the whole file's audio that completes first, so
// (unlike the old alternating-heuristic era) there's no need to write a
// hardcoded superset or splice this in after the fact. renameSpeaker() keeps
// this key in sync when a user renames a speaker (see renameSpeakerInFile()).
//
// speakerDetectionError is set when speaker detection was requested but the
// diarization models couldn't be downloaded/loaded — speakerDetection then
// reads false (the transcription still ran, ASR-only) rather than
// misrepresenting a feature that didn't actually run (see ModelManager's
// usedFallback/modelFallback for the same never-misrepresent principle).
//
// vad/vadError (this feature): whether whisper.cpp's built-in Silero VAD
// gating actually ran (see WhisperCppEngine::setVadModelPath() and
// VadChunker.h) — unlike speechnorm (unconditional, no failure mode, so it
// gets no frontmatter field at all), VAD depends on a small model download
// that can fail (network/disk), and ASR must still proceed without it. Same
// never-misrepresent contract as speakerDetection/speakerDetectionError.
void writeFrontmatter(QFile &mdFile, const QString &modelUsed, bool speakerDetection, bool modelFallback,
                      const QString &requestedModelId, const QStringList &speakerLabels,
                      const QString &speakerDetectionError, bool vad, const QString &vadError)
{
    QTextStream out(&mdFile);
    out << "---\n";
    out << "model: " << modelUsed << '\n';
    out << "speakerDetection: " << (speakerDetection ? "true" : "false") << '\n';
    if (speakerDetection && !speakerLabels.isEmpty())
        out << "speakers: " << speakerLabels.join(QStringLiteral(", ")) << '\n';
    if (!speakerDetectionError.isEmpty())
        out << "speakerDetectionError: " << speakerDetectionError << '\n';
    out << "vad: " << (vad ? "true" : "false") << '\n';
    if (!vadError.isEmpty())
        out << "vadError: " << vadError << '\n';
    out << "transcribedAt: " << QDateTime::currentDateTime().toString(Qt::ISODate) << '\n';
    if (modelFallback) {
        out << "modelRequested: " << requestedModelId << '\n';
        out << "modelFallback: true\n";
    }
    out << "---\n\n";
    out.flush();
    mdFile.flush();
}

// Formats one segment line, e.g. "[1:07] [Speaker 1] hello there (×3)" —
// the trailing " (×N)" repeat indicator (see SegmentLineWriter) is only
// included for N > 1. speakerLabel empty = no leading tag written.
QString formatSegmentLine(double t0, const QString &speakerLabel, const QString &text, int repeatCount)
{
    QString line = QLatin1Char('[') + formatTimestamp(t0) + QLatin1Char(']');
    if (!speakerLabel.isEmpty())
        line += QStringLiteral(" [%1]").arg(speakerLabel);
    line += QLatin1Char(' ') + text;
    if (repeatCount > 1)
        line += QStringLiteral(" (×%1)").arg(repeatCount);
    return line;
}

// Fixed sentinel marking the end of the prelude region at the very start of
// a transcript's BODY — frontmatter can't hold real newlines, so the
// multi-paragraph, editable prelude (Feature 2, upgrading the old read-only
// single-line "summary") lives in the body instead, immediately after
// frontmatter's closing "---". An EXPLICIT marker, not implicit "first line
// matching segmentLinePattern()" detection: that pattern's timestamp group
// ([\d:.]+) is loose enough that a hand-typed numbered list in the prelude
// (e.g. "[1] First point") would otherwise collide and get misdetected as a
// real segment.
const QString kPreludeEndMarker = QStringLiteral("<!-- prelude end -->");

// Everything from `body`'s start through kPreludeEndMarker (and its
// immediately-following blank line, if present) is the prelude region —
// strip it, leaving just the segment-line content. `body` unchanged if the
// marker isn't present (a fresh or pre-Feature-2 file has nothing to strip).
// Shared by every consumer that scans for segment lines
// (segmentLinePattern().globalMatch()) — without this, a prelude paragraph
// that happens to start a line with bracket-shaped text (e.g. a hand-typed
// "[1] First point") risks being misdetected as a real segment, since
// nothing else marks where the prelude ends from that scan's point of view.
QString stripPreludeIfPresent(const QString &body)
{
    const int markerIdx = body.indexOf(kPreludeEndMarker);
    if (markerIdx < 0)
        return body;
    int afterMarker = markerIdx + kPreludeEndMarker.size();
    if (afterMarker < body.size() && body.at(afterMarker) == QLatin1Char('\n'))
        ++afterMarker; // end of the marker's own line
    if (afterMarker < body.size() && body.at(afterMarker) == QLatin1Char('\n'))
        ++afterMarker; // its immediately-following blank line, if present
    return body.mid(afterMarker);
}

// Replaces whatever prelude currently precedes kPreludeEndMarker in `body`
// (or, if the marker isn't present yet — a fresh or pre-Feature-2 file —
// the whole of `body`) with newPrelude, re-inserting the marker. Shared by
// writeLabelingFields() (auto regeneration) and
// TranscriptionController::setPrelude() (manual edit) — identical splice
// shape either way. Idempotent by construction: splicing the SAME
// newPrelude into this function's own prior output reproduces byte-identical
// spacing, so repeated regeneration with unchanged text never accumulates
// blank lines. An empty newPrelude still gets the marker written (rather
// than omitting the whole block) — that way readPrelude() always reliably
// reads back "" instead of having to distinguish "empty prelude" from "no
// marker written yet, treat the whole body as unmarked content".
QString spliceBodyPrelude(const QString &body, const QString &newPrelude)
{
    const QString remaining = stripPreludeIfPresent(body);
    const QString trimmed = newPrelude.trimmed();
    if (trimmed.isEmpty())
        return kPreludeEndMarker + QStringLiteral("\n\n") + remaining;
    return trimmed + QStringLiteral("\n\n") + kPreludeEndMarker + QStringLiteral("\n\n") + remaining;
}

// AI auto-labeling's second pass runs AFTER the primary frontmatter+segments
// have already been written and mdFile closed (needs the full transcript
// text, which isn't known until transcription completes) — reopens the same
// .md and splices in label/prelude/tags: label/tags into frontmatter,
// prelude into the body via spliceBodyPrelude() above. Splice-or-append (not
// append-only) for the frontmatter keys: a fresh transcribe job's
// frontmatter never has these keys yet (so this falls through to append),
// but "regenerate auto-title" (relabel, see TranscriptionController::
// relabel()) reruns this against a .md that already has them — must replace
// in place, not duplicate. A failure here IS surfaced as its own job's
// failure (see TranscriptionController::relabel()) — auto-labeling now
// always runs as a separate, visible follow-up task (see transcribe()'s
// autoLabel doc), not a silent bonus pass baked into the transcription job
// itself. Newlines are stripped from label/tags (not prelude) since those
// are a flat line-based format — a raw '\n' inside a JSON string (from a
// "\n" escape) would otherwise corrupt the frontmatter block; the prelude's
// real newlines are exactly the point of moving it to the body.
void writeLabelingFields(const QString &mdPath, const LabelResult &result)
{
    QFile mdFile(mdPath);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "auto-label: could not reopen" << mdPath << "to write label/prelude/tags";
        return;
    }
    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---")) {
        qWarning() << "auto-label:" << mdPath << "has no frontmatter block, skipping";
        return;
    }
    QStringList frontmatterLines;
    QString line;
    bool closed = false;
    while (!(line = in.readLine()).isNull()) {
        if (line == QStringLiteral("---")) {
            closed = true;
            break;
        }
        frontmatterLines.append(line);
    }
    if (!closed) {
        qWarning() << "auto-label:" << mdPath << "has a malformed frontmatter block, skipping";
        return;
    }
    const QString body = in.readAll();
    mdFile.close();

    auto sanitize = [](QString s) { return s.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' ')); };
    auto spliceOrAppend = [&frontmatterLines](const QString &key, const QString &value) {
        for (QString &fmLine : frontmatterLines) {
            const int colon = fmLine.indexOf(':');
            if (colon >= 0 && fmLine.left(colon).trimmed() == key) {
                fmLine = key + QStringLiteral(": ") + value;
                return;
            }
        }
        frontmatterLines.append(key + QStringLiteral(": ") + value);
    };
    spliceOrAppend(QStringLiteral("label"), sanitize(result.label));
    spliceOrAppend(QStringLiteral("tags"), sanitize(result.tags));
    const QString newBody = spliceBodyPrelude(body, result.prelude);

    if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "auto-label: could not reopen" << mdPath << "for writing";
        return;
    }
    QTextStream out(&mdFile);
    out << "---\n";
    for (const QString &fmLine : frontmatterLines)
        out << fmLine << '\n';
    out << "---\n";
    out << newBody;
    out.flush();
    mdFile.flush();
}

// Plain prose for the labeling prompt (see relabel()'s slot below): strips
// the "[start - end]"/" [SPEAKER_TURN]" formatting back out of an existing
// transcript body, leaving just the spoken text — same shape as the text
// TranscriptionWorker::run() accumulates live during a fresh transcription.
// knownSpeakers is the file's own frontmatter "speakers:" allowlist (see
// segmentLinePattern()'s doc comment) — the caller reads it alongside the
// body since both come from the same frontmatter block. stripPreludeIfPresent
// first: the prelude is prose ABOUT the transcript, not spoken content, and
// feeding a stale one back into the labeling prompt would bias the very
// regeneration meant to replace it.
QString plainTextFromTranscriptBody(const QString &body, const QStringList &knownSpeakers)
{
    QStringList pieces;
    QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatch(stripPreludeIfPresent(body));
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString text = m.captured(3).trimmed();
        if (!text.isEmpty())
            pieces.append(text);
    }
    return pieces.join(QLatin1Char(' '));
}

// Feature 3a ("generate chapters"): one segment's parsed identity from a
// transcript body — this struct's position in the QList returned by
// parsedSegmentsFromTranscriptBody() below IS its index (the same indexing
// LabelingEngine::generateChapters() expects and returns "afterSegment"
// against).
struct ParsedSegment {
    double t0 = 0.0;
    QString text;
};

// A lightweight sibling to plainTextFromTranscriptBody() above — walks the
// same segmentLinePattern().globalMatch() but keeps each segment's t0
// alongside its text instead of discarding everything but the joined
// string. Chapter generation needs both: text is what's actually shown to
// the model (indexed), t0 is what a chapter's LLM-returned index gets
// resolved to before anything touches disk (this app never trusts the LLM
// to echo a timestamp back directly — see
// LabelingEngine::generateChapters()'s doc comment).
QList<ParsedSegment> parsedSegmentsFromTranscriptBody(const QString &body, const QStringList &knownSpeakers)
{
    QList<ParsedSegment> segments;
    QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatch(stripPreludeIfPresent(body));
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString text = m.captured(3).trimmed();
        if (text.isEmpty())
            continue;
        ParsedSegment segment;
        segment.t0 = parseCompactTimestamp(m.captured(1));
        segment.text = text;
        segments.append(segment);
    }
    return segments;
}

// Feature 3a: "## [timestamp] title" marker line format — see
// spliceBodyChapters()'s doc comment (TranscriptionController.h) for why
// this is collision-free with segmentLinePattern() by construction. Group 1:
// the timestamp text (parsed via parseCompactTimestamp, same helper/format
// every other timestamp in this file uses); group 2: the title text.
QRegularExpression chapterMarkerPattern()
{
    return QRegularExpression(QStringLiteral("^## \\[([\\d:.]+)\\] (.*)$"), QRegularExpression::MultilineOption);
}

// Item 14 "auto-categorize" mode: the existing tag vocabulary for a vault,
// scanned fresh per labeling job (vaults are small; no cache invalidation
// to get wrong). Reads every .md's `tags:` frontmatter line directly rather
// than going through readMetadata()'s full parse, since that's all that's
// needed here. Returns a de-duplicated, sorted list of individual tags.
QStringList collectVaultTags(const QString &vaultLocalPath)
{
    QSet<QString> tags;
    QDirIterator it(vaultLocalPath, {QStringLiteral("*.md")}, QDir::Files,
                     QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile mdFile(it.next());
        if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QTextStream in(&mdFile);
        if (in.readLine() != QStringLiteral("---"))
            continue;
        QString line;
        while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
            const int colon = line.indexOf(':');
            if (colon < 0 || line.left(colon).trimmed() != QStringLiteral("tags"))
                continue;
            const QString value = line.mid(colon + 1).trimmed();
            for (const QString &tag : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString trimmed = tag.trimmed();
                if (!trimmed.isEmpty())
                    tags.insert(trimmed);
            }
        }
    }
    QStringList sorted = tags.values();
    sorted.sort(Qt::CaseInsensitive);
    return sorted;
}

// Shared prologue for renameSpeakerInFile/mergeSpeakersInFile/
// syncSpeakersFrontmatterToBody: opens path, splits it into frontmatter
// lines + body, locates the "speakers:" line (if any, via
// writeFrontmatter()'s/segmentLinePattern()'s doc comments) and its parsed
// allowlist, and collects every distinct speaker tag actually present in
// the body (first-appearance order). *speakersLineIndex is -1 if the file
// has no "speakers:" line. Fails (returns false, *errorOut set) on any
// read/format problem — callers must bail out immediately in that case.
bool parseSpeakerTaggedFile(const QString &path, QStringList *frontmatterLines, QString *body,
                             int *speakersLineIndex, QStringList *knownSpeakers, QStringList *distinctTags,
                             QString *errorOut)
{
    QFile mdFile(path);
    if (!mdFile.exists()) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has not been transcribed yet").arg(QFileInfo(path).fileName());
        return false;
    }
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for reading").arg(path);
        return false;
    }

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---")) {
        mdFile.close();
        if (errorOut)
            *errorOut = QStringLiteral("%1 has no frontmatter block").arg(mdFile.fileName());
        return false;
    }
    QString line;
    bool closed = false;
    while (!(line = in.readLine()).isNull()) {
        if (line == QStringLiteral("---")) {
            closed = true;
            break;
        }
        frontmatterLines->append(line);
    }
    if (!closed) {
        mdFile.close();
        if (errorOut)
            *errorOut = QStringLiteral("%1 has a malformed frontmatter block").arg(mdFile.fileName());
        return false;
    }
    *body = in.readAll();
    mdFile.close();

    *speakersLineIndex = -1;
    for (int i = 0; i < frontmatterLines->size(); ++i) {
        const int colon = frontmatterLines->at(i).indexOf(':');
        if (colon >= 0 && frontmatterLines->at(i).left(colon).trimmed() == QStringLiteral("speakers")) {
            *speakersLineIndex = i;
            *knownSpeakers = parseSpeakersList(frontmatterLines->at(i).mid(colon + 1));
            break;
        }
    }

    QRegularExpressionMatchIterator it = segmentLinePattern(*knownSpeakers).globalMatch(*body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedLength(2) == 0)
            continue;
        const QString tag = m.captured(2);
        if (!distinctTags->contains(tag))
            distinctTags->append(tag);
    }
    return true;
}

// Truncates mdFile and writes frontmatterLines + body back out in the
// standard "---\n<lines>\n---\n<body>" shape shared by every frontmatter
// rewrite (rename/merge/sync). mdFile must already be open for reading
// having been closed by parseSpeakerTaggedFile() above (or otherwise not
// open) — this reopens it WriteOnly|Truncate itself.
bool writeFrontmatterBlock(const QString &path, const QStringList &frontmatterLines, const QString &body,
                           QString *errorOut)
{
    QFile mdFile(path);
    if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(mdFile.fileName());
        return false;
    }
    QTextStream out(&mdFile);
    out << "---\n";
    for (const QString &fmLine : frontmatterLines)
        out << fmLine << '\n';
    out << "---\n";
    out << body;
    out.flush();
    mdFile.flush();
    mdFile.close();
    return true;
}

// Feature 3a: reopens mdPath, leaves frontmatter completely untouched
// (chapters live purely in the body — no frontmatter key involved, unlike
// label/tags), and splices `chapters` into the body via spliceBodyChapters()
// — which itself strips any existing marker set first, so regeneration
// (a later "generate chapters" call on the same file) replaces the old set
// wholesale rather than accumulating duplicates, same policy as Feature 2's
// prelude. A failure here IS surfaced as the job's own failure (see
// TranscriptionWorker::generateChaptersJob()).
bool writeChaptersField(const QString &mdPath, const QList<ResolvedChapter> &chapters, QString *errorOut)
{
    QFile mdFile(mdPath);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not reopen %1 to write chapters").arg(mdPath);
        return false;
    }
    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---")) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has no frontmatter block").arg(mdPath);
        return false;
    }
    QStringList frontmatterLines;
    QString line;
    bool closed = false;
    while (!(line = in.readLine()).isNull()) {
        if (line == QStringLiteral("---")) {
            closed = true;
            break;
        }
        frontmatterLines.append(line);
    }
    if (!closed) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has a malformed frontmatter block").arg(mdPath);
        return false;
    }
    const QString body = in.readAll();
    mdFile.close();

    return writeFrontmatterBlock(mdPath, frontmatterLines, spliceBodyChapters(body, chapters), errorOut);
}

// Rewrites every "[oldName]" segment-line speaker tag in path's transcript
// BODY to "[newName]" — the tag text itself IS the speaker's display name
// (see segmentLinePattern()'s doc comment), so this is the ONLY thing a
// rename needs to touch; frontmatter is read back out unchanged. Runs on
// TranscriptionWorker's background thread (see
// TranscriptionController::renameSpeaker()).
//
// Rejects (returns false, *errorOut set) if oldName isn't actually present
// as a tag anywhere in the file, or if newName (trimmed, case-insensitively)
// already names a DIFFERENT speaker present in the file — callers must not
// silently merge two distinct speakers into one by typing an existing name
// (see mergeSpeakersInFile() below for the deliberate, explicit way to do
// that). A no-op rename (newName already equals oldName) succeeds without
// writing.
bool renameSpeakerInFile(const QString &path, const QString &oldName, const QString &newName, QString *errorOut)
{
    QStringList frontmatterLines;
    QString body;
    int speakersLineIndex = -1;
    QStringList knownSpeakers;
    QStringList distinctTags;
    if (!parseSpeakerTaggedFile(path, &frontmatterLines, &body, &speakersLineIndex, &knownSpeakers, &distinctTags,
                                 errorOut))
        return false;

    if (!distinctTags.contains(oldName)) {
        if (errorOut)
            *errorOut = QStringLiteral("speaker \"%1\" was not found in this transcript").arg(oldName);
        return false;
    }

    QString sanitizedName = newName;
    sanitizedName.replace(QLatin1Char('\n'), QLatin1Char(' '));
    sanitizedName.replace(QLatin1Char('['), QLatin1Char(' '));
    sanitizedName.replace(QLatin1Char(']'), QLatin1Char(' '));
    sanitizedName = sanitizedName.trimmed();
    if (sanitizedName.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("speaker name cannot be empty");
        return false;
    }
    if (sanitizedName == oldName)
        return true; // no-op: already named that

    for (const QString &tag : std::as_const(distinctTags)) {
        if (tag != oldName && tag.compare(sanitizedName, Qt::CaseInsensitive) == 0) {
            if (errorOut)
                *errorOut = QStringLiteral("a speaker named \"%1\" already exists in this transcript").arg(sanitizedName);
            return false;
        }
    }

    // Re-match oldName's tags to splice by exact offset (capturedStart/
    // capturedLength) rather than a blind string-replace, which could
    // corrupt spoken TEXT that happens to contain the same characters.
    // Splice from the END of the body backward so earlier matches' offsets
    // stay valid as later ones are replaced.
    QVector<QRegularExpressionMatch> taggedMatches;
    QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedLength(2) > 0 && m.captured(2) == oldName)
            taggedMatches.append(m);
    }
    for (auto matchIt = taggedMatches.crbegin(); matchIt != taggedMatches.crend(); ++matchIt)
        body.replace(matchIt->capturedStart(2), matchIt->capturedLength(2), sanitizedName);

    // Keep the frontmatter allowlist in sync with the rename — otherwise a
    // SECOND rename of this same speaker (matched by their now-current name)
    // would fail segmentLinePattern()'s allowlist check even though the body
    // tag itself was just updated above.
    if (speakersLineIndex >= 0) {
        QStringList updatedSpeakers = knownSpeakers;
        const int idx = updatedSpeakers.indexOf(oldName);
        if (idx >= 0)
            updatedSpeakers[idx] = sanitizedName;
        frontmatterLines[speakersLineIndex] = QStringLiteral("speakers: ") + updatedSpeakers.join(QStringLiteral(", "));
    }

    return writeFrontmatterBlock(path, frontmatterLines, body, errorOut);
}

// Relabels every "[sourceName]" body tag to "[targetName]" and drops
// sourceName from the "speakers:" allowlist (targetName is already listed
// there and is left untouched) — the deliberate, explicit way to collapse
// two over-split diarized speakers back into one (see MainPanel.qml's
// speaker-pill drag-to-merge UI), unlike renameSpeakerInFile() above, which
// REJECTS colliding with an existing name specifically to prevent an
// accidental merge via a typo. No-op (returns true) if sourceName ==
// targetName. Fails if either name isn't an existing distinct tag in the
// body.
bool mergeSpeakersInFile(const QString &path, const QString &sourceName, const QString &targetName,
                          QString *errorOut)
{
    if (sourceName == targetName)
        return true; // no-op: merging a speaker into itself

    QStringList frontmatterLines;
    QString body;
    int speakersLineIndex = -1;
    QStringList knownSpeakers;
    QStringList distinctTags;
    if (!parseSpeakerTaggedFile(path, &frontmatterLines, &body, &speakersLineIndex, &knownSpeakers, &distinctTags,
                                 errorOut))
        return false;

    if (!distinctTags.contains(sourceName)) {
        if (errorOut)
            *errorOut = QStringLiteral("speaker \"%1\" was not found in this transcript").arg(sourceName);
        return false;
    }
    if (!distinctTags.contains(targetName)) {
        if (errorOut)
            *errorOut = QStringLiteral("speaker \"%1\" was not found in this transcript").arg(targetName);
        return false;
    }

    QVector<QRegularExpressionMatch> sourceMatches;
    QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedLength(2) > 0 && m.captured(2) == sourceName)
            sourceMatches.append(m);
    }
    for (auto matchIt = sourceMatches.crbegin(); matchIt != sourceMatches.crend(); ++matchIt)
        body.replace(matchIt->capturedStart(2), matchIt->capturedLength(2), targetName);

    if (speakersLineIndex >= 0) {
        QStringList updatedSpeakers = knownSpeakers;
        updatedSpeakers.removeAll(sourceName);
        frontmatterLines[speakersLineIndex] = QStringLiteral("speakers: ") + updatedSpeakers.join(QStringLiteral(", "));
    }

    return writeFrontmatterBlock(path, frontmatterLines, body, errorOut);
}

// See distinctSpeakerCount()'s doc comment: the "speakers:" frontmatter
// list is computed from DiarizationEngine's raw cluster count BEFORE any
// whisper segment exists, so a cluster that survived DiarizationEngine's
// own minor-speaker merge (cleared that merge's relative-duration bar) can
// still lose labelForSegment()'s per-segment overlap comparison for EVERY
// segment on a short/noisy clip — never actually tagging a single body
// line, yet still listed in frontmatter as a phantom entry nothing (not
// even the speaker-pill merge UI, which only shows tags that appear in the
// body) can ever clean up. Called once, at the very end of run(), once
// every segment line has actually been written — only then is "which tags
// actually got used" knowable. Rewrites the "speakers:" line to exactly the
// tags present in the body (first-appearance order), dropping the line
// entirely if none are, or does nothing if it already matches.
void syncSpeakersFrontmatterToBody(const QString &path)
{
    QStringList frontmatterLines;
    QString body;
    int speakersLineIndex = -1;
    QStringList knownSpeakers;
    QStringList distinctTags;
    QString error;
    if (!parseSpeakerTaggedFile(path, &frontmatterLines, &body, &speakersLineIndex, &knownSpeakers, &distinctTags,
                                 &error)) {
        qWarning() << "syncSpeakersFrontmatterToBody: failed to read" << path << ":" << error;
        return;
    }
    if (speakersLineIndex < 0 || knownSpeakers == distinctTags)
        return; // nothing to sync: no speakers: line, or it already matches

    if (distinctTags.isEmpty())
        frontmatterLines.removeAt(speakersLineIndex);
    else
        frontmatterLines[speakersLineIndex] = QStringLiteral("speakers: ") + distinctTags.join(QStringLiteral(", "));

    if (!writeFrontmatterBlock(path, frontmatterLines, body, &error))
        qWarning() << "syncSpeakersFrontmatterToBody: failed to write" << path << ":" << error;
}

// findCombineGroups()'s outputPath is built from (ruleName, capturedKey), not
// a filename component that's guaranteed filesystem-safe (a regex capture
// group can pull out literally anything) — collapse everything except
// alphanumerics/-/_ to '-' so the derived name is always a valid single path
// segment on every filesystem this app targets.
QString sanitizeForFilename(const QString &raw)
{
    QString out = raw;
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_-]+"));
    out.replace(unsafe, QStringLiteral("-"));
    while (out.startsWith(QChar('-')))
        out.remove(0, 1);
    while (out.endsWith(QChar('-')))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("combined") : out;
}

// findCombineGroups()'s "pending" status: a group is otherwise "ready" but
// its newest member was modified more recently than this many seconds ago —
// the recorder may still be actively writing that file, and concatenating a
// growing/not-yet-closed file risks truncation or a corrupt merge.
constexpr qint64 kCombineStabilityWindowSecs = 5;

// One rule ({name, pattern, enabled, sortGroupIndex} map) matched against a
// single filename (not a full path — QFileInfo::fileName()), search
// semantics (QRegularExpression, unanchored unless the pattern itself has
// ^/$). On a match, returns the "real string" key: capture groups 1..N
// concatenated in order, EXCLUDING the rule's sort differentiator group if
// it has one (empty key if the pattern has no OTHER capture groups) — see
// TranscriptionController::findCombineGroups()'s doc comment for the full
// semantics. Shared by findCombineGroups() (which tries every rule against
// every candidate path) and maybeRedirectForCombine() (which only needs to
// try every rule against ONE already-known filename). `capturedValuesOut`,
// if given, is filled with the SAME (non-sort) capture groups
// un-concatenated — only findCombineGroups() needs the individual values
// (for the Settings preview header showing exactly which differentiator
// values grouped these files); maybeRedirectForCombine() only ever needs the
// flattened key. `sortValueOut`, if given, receives the sort differentiator
// group's own captured text (untouched/nullopt if the rule has none
// configured, or the configured index is out of range for this match) —
// only findCombineGroups() uses this, to order a group's members by that
// value instead of the mtime default.
std::optional<QString> matchRuleCapturedKey(const QVariantMap &rule, const QString &fileName,
                                             QStringList *capturedValuesOut = nullptr,
                                             std::optional<QString> *sortValueOut = nullptr)
{
    if (!rule.value(QStringLiteral("enabled"), true).toBool())
        return std::nullopt;
    const QString pattern = rule.value(QStringLiteral("pattern")).toString();
    if (pattern.isEmpty())
        return std::nullopt;
    const QRegularExpression re(pattern);
    if (!re.isValid())
        return std::nullopt;
    const QRegularExpressionMatch m = re.match(fileName);
    if (!m.hasMatch())
        return std::nullopt;
    const int sortGroupIndex = rule.value(QStringLiteral("sortGroupIndex"), -1).toInt();
    QString key;
    for (int i = 1; i <= m.lastCapturedIndex(); ++i) {
        const QString captured = m.captured(i);
        if (i == sortGroupIndex) {
            if (sortValueOut)
                *sortValueOut = captured;
            continue; // the sort differentiator is explicitly NOT a grouping one
        }
        key += captured;
        if (capturedValuesOut)
            capturedValuesOut->append(captured);
    }
    return key;
}

// Deterministic merged-output filename for a (ruleName, capturedKey) pair,
// resolved against a directory + extension — see
// TranscriptionController::findCombineGroups()'s outputPath doc comment for
// why it's derived from the rule+key rather than any single member's
// filename.
QString combineOutputPathFor(const QString &ruleName, const QString &key, const QString &dir, const QString &ext)
{
    QString name = sanitizeForFilename(ruleName);
    if (!key.isEmpty())
        name += QStringLiteral("-") + sanitizeForFilename(key);
    return dir + QStringLiteral("/") + name + QStringLiteral(".combined.") + ext;
}

}

SegmentLineWriter::SegmentLineWriter(QFile &mdFile, int maxCycleLength)
    : m_mdFile(mdFile), m_maxCycleLength(std::max(1, maxCycleLength))
{
}

void SegmentLineWriter::append(const Segment &segment, const QString &speakerLabel)
{
    const QString text = QString::fromStdString(segment.text).trimmed();
    if (text.isEmpty())
        return;

    LineRecord record;
    record.fileOffset = m_mdFile.pos();
    record.speakerLabel = speakerLabel;
    record.text = text;
    record.t0 = segment.t0;
    record.repeatCount = 1;
    writeRecord(record);
    m_tail.append(record);

    tryMergeCycle();

    // Only the last 2*maxCycleLength entries can ever matter to a future
    // check (see tryMergeCycle()) — drop anything older. Safe to just
    // forget them: their CONTENT is fixed and already durably on disk, and
    // whether they'd match something later never changes once written.
    const int maxKeep = 2 * m_maxCycleLength;
    if (m_tail.size() > maxKeep)
        m_tail.remove(0, m_tail.size() - maxKeep);
}

void SegmentLineWriter::writeRecord(const LineRecord &record)
{
    QTextStream out(&m_mdFile);
    out << formatSegmentLine(record.t0, record.speakerLabel, record.text, record.repeatCount) << '\n';
    out.flush();
    m_mdFile.flush();
}

// Checks, smallest period first, whether the last `period` lines (for each
// period from 1 to m_maxCycleLength) are an EXACT repeat — same speaker AND
// same text, ignoring each line's current repeatCount — of the `period`
// lines immediately before them. On the first match found, collapses the
// duplicate copy: truncates the file back to the start of the earlier
// copy's oldest line and rewrites just that one copy with every line's
// repeatCount bumped by the (usually 1, but itself possibly already >1 if
// this cycle has matched before) duplicate copy's own count. Smallest
// period wins so a plain single-line repeat (period 1) is preferred over
// reading it as a degenerate case of a longer period.
void SegmentLineWriter::tryMergeCycle()
{
    for (int period = 1; period <= m_maxCycleLength; ++period) {
        if (m_tail.size() < 2 * period)
            continue;

        bool matches = true;
        for (int k = 0; k < period && matches; ++k) {
            const LineRecord &recent = m_tail[m_tail.size() - 1 - k];
            const LineRecord &prior = m_tail[m_tail.size() - 1 - period - k];
            matches = recent.speakerLabel == prior.speakerLabel && recent.text == prior.text;
        }
        if (!matches)
            continue;

        const int firstIndex = m_tail.size() - 2 * period;
        QVector<LineRecord> merged;
        merged.reserve(period);
        for (int i = 0; i < period; ++i) {
            LineRecord updated = m_tail[firstIndex + i];
            updated.repeatCount += m_tail[firstIndex + period + i].repeatCount;
            merged.append(updated);
        }

        m_mdFile.resize(merged.first().fileOffset);
        m_mdFile.seek(merged.first().fileOffset);
        for (LineRecord &rec : merged) {
            rec.fileOffset = m_mdFile.pos();
            writeRecord(rec);
        }

        m_tail.remove(firstIndex, m_tail.size() - firstIndex);
        m_tail.append(merged);
        return;
    }
}

// See this function's doc comment in TranscriptionController.h.
QString spliceBodyChapters(const QString &body, const QList<ResolvedChapter> &chapters)
{
    QString result = body;

    // Strip every existing marker line first — offsets computed below are
    // against this already-stripped string, so a chapter's target segment
    // line is found at its TRUE (post-strip) position, not shifted by
    // whatever marker set a previous "generate chapters" run left behind.
    QVector<QRegularExpressionMatch> existingMarkers;
    QRegularExpressionMatchIterator stripIt = chapterMarkerPattern().globalMatch(result);
    while (stripIt.hasNext())
        existingMarkers.append(stripIt.next());
    for (auto matchIt = existingMarkers.crbegin(); matchIt != existingMarkers.crend(); ++matchIt) {
        int start = matchIt->capturedStart(0);
        int length = matchIt->capturedLength(0);
        if (start + length < result.size() && result.at(start + length) == QLatin1Char('\n'))
            ++length; // swallow the marker's own trailing newline too
        result.remove(start, length);
    }

    // Locate every target offset UP FRONT, against the one unmodified
    // (post-strip) string — only then sort descending and insert, so each
    // insertion never invalidates an offset already computed for another
    // chapter still pending (see this function's doc comment in the header).
    struct PendingInsert {
        int offset;
        QString marker;
    };
    QList<PendingInsert> pending;
    pending.reserve(chapters.size());
    for (const ResolvedChapter &chapter : chapters) {
        const QString anchor = QStringLiteral("[") + formatTimestamp(chapter.t0) + QStringLiteral("]");
        const QRegularExpression lineStart(QStringLiteral("^") + QRegularExpression::escape(anchor),
                                            QRegularExpression::MultilineOption);
        const QRegularExpressionMatch m = lineStart.match(result);
        if (!m.hasMatch())
            continue; // target segment not found — defensive, shouldn't happen for a caller-resolved t0
        pending.append({static_cast<int>(m.capturedStart(0)),
                         QStringLiteral("## [") + formatTimestamp(chapter.t0) + QStringLiteral("] ") + chapter.title
                             + QLatin1Char('\n')});
    }
    std::sort(pending.begin(), pending.end(),
              [](const PendingInsert &a, const PendingInsert &b) { return a.offset > b.offset; });
    for (const PendingInsert &p : std::as_const(pending))
        result.insert(p.offset, p.marker);

    return result;
}

// Lives on TranscriptionController's dedicated QThread; never touches the
// GUI thread. Owns the warm WhisperCppEngine so the model stays resident
// across transcribe() calls.
class TranscriptionWorker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Wave N (task system): plain (non-slot) methods called DIRECTLY from
    // the controller thread — safe despite the cross-thread call because
    // they only touch std::atomic<bool>s, never anything else this object
    // owns. Deliberately NOT routed through QMetaObject::invokeMethod: that
    // would queue behind whatever job is currently running on this worker's
    // event loop, which is exactly the moment a pause/cancel request needs
    // to take effect — these have to land immediately, not wait in line.
    void requestPause() { m_pauseRequested.store(true); }
    void requestResume() { m_pauseRequested.store(false); }
    void requestCancel() { m_cancelRequested.store(true); }
    // Called by TranscriptionController::startJob() right before dispatching
    // the next job, so a cancel/pause left over from whatever the PREVIOUS
    // job did doesn't leak into this one.
    void resetControl()
    {
        m_pauseRequested.store(false);
        m_cancelRequested.store(false);
    }
    bool isCancelRequested() const { return m_cancelRequested.load(); }

public slots:
    void run(const QString &filePath, const QString &vaultPath, const QString &modelId, bool speakerDetection)
    {
        // GPU memory hygiene: transcribe and label jobs never run
        // concurrently (this worker runs at most one job at a time), so
        // there's no reason to keep the label engine's GPU-resident weights
        // + KV-cache buffer around while a transcription runs — evicting it
        // here (unconditionally, not just under memory pressure) keeps this
        // worker's steady-state GPU footprint to whichever ONE big model
        // (whisper or label) its most recent job actually needed, instead of
        // silently accumulating every model ever used this session. Costs a
        // reload (a few seconds) the next time a labeling job runs, which is
        // deliberately accepted in exchange for not exhausting VRAM — see
        // LabelingEngine's CPU-fallback retry for when that reload itself
        // hits GPU memory pressure.
        m_labelingEngine.reset();

        // 2026-07-10 (rolling-average phase weighting): wall-clock timer for
        // whichever phase is currently running, restarted (recording the
        // PREVIOUS phase's duration via phaseDurationMs) at each transition
        // below — see TranscriptionController::recordPhaseDuration()'s doc
        // comment. Deliberately only recorded right as a phase completes
        // NORMALLY (never on a cancel/failure return in between), so an
        // aborted phase's artificially-short elapsed time never pollutes the
        // average.
        QElapsedTimer phaseTimer;
        phaseTimer.start();

        // "loading": whisper.cpp has no progress hook for model load itself
        // (whisper_init_from_file_with_params is one blocking call) — this
        // phase covers ensureModel()'s (rare, first-run) download plus the
        // model load, both otherwise invisible. Bigger models (medium.en,
        // ~1.5GB) can take a real, previously-unexplained-looking delay here.
        emit phase(QStringLiteral("loading"));

        QString error;
        bool modelFallback = false;
        const QString modelPath =
            ModelManager::ensureModel(QUrl::fromLocalFile(vaultPath), modelId, &error, &modelFallback);
        if (modelPath.isEmpty()) {
            emit finished(false, error.isEmpty() ? QStringLiteral("model unavailable") : error);
            return;
        }

        if (!m_engine || m_engine->isValid() == false || m_modelPath != modelPath) {
            m_engine = std::make_unique<WhisperCppEngine>(modelPath);
            m_modelPath = modelPath;
        }
        if (!m_engine->isValid()) {
            emit finished(false, QStringLiteral("failed to load model at %1").arg(modelPath));
            return;
        }
        // Item O35: whichever backend whisper.cpp just picked for this load
        // (see WhisperCppEngine::activeBackendName()'s doc comment) — fired
        // here rather than after the later diarization-eviction reload below
        // since QML only needs to know once per job, not on every reload.
        emit backendNameChanged(m_engine->activeBackendName());

        // A cancel (or pause, which resolves to cancel — see
        // TranscriptionController::pauseActive()'s doc comment) requested
        // DURING "loading" can't be caught mid-model-load (no callback hook
        // exists there — see the "loading" phase comment above), but must
        // not then ALSO be ignored through the entire audio decode below
        // (which can be a real, multi-second delay of its own for a long
        // file) — check the instant loading finishes, before starting
        // anything else.
        if (isCancelRequested()) {
            emit finished(false, QStringLiteral("Cancelled"));
            return;
        }
        emit phaseDurationMs(QStringLiteral("loading"), phaseTimer.restart());

        // "decoding": unlike "loading" above, ffmpeg runs as a real
        // subprocess (see AudioDecoder.h) — genuinely pausable (SIGSTOP the
        // process) and instantly cancellable (kill it), same mechanism as
        // ModelManager's downloads. Pause here behaves exactly like pausing
        // during transcribing/diarization (activePaused, real cooperative
        // resume), not the restart-based Paused status "loading" needs.
        emit phase(QStringLiteral("decoding"));
        bool decodePausedSignaled = false;
        AudioDecoder::DecodeControl decodeControl;
        decodeControl.paused = [this, &decodePausedSignaled] {
            const bool p = m_pauseRequested.load();
            if (p != decodePausedSignaled) {
                decodePausedSignaled = p;
                emit paused(p);
            }
            return p;
        };
        decodeControl.cancelled = [this] { return m_cancelRequested.load(); };
        QString decodeError;
        auto pcm = AudioDecoder::decodeTo16kMono(filePath, &decodeError, decodeControl);
        if (!pcm) {
            emit finished(false, decodeError.isEmpty() ? QStringLiteral("audio decode failed") : decodeError);
            return;
        }

        // Speaker diarization (see DESIGN.md "Speaker diarization") is a
        // full pass over the WHOLE file's audio, run BEFORE whisper
        // transcription starts — unlike tdrz, it's independent of which
        // whisper model is selected. Running it first means the real
        // speaker-label set is already known by the time frontmatter is
        // written below (no post-hoc splice needed), and each whisper
        // segment can be labeled via a simple time-overlap lookup as it
        // streams in. A failure here NEVER blocks the transcription — ASR
        // still runs, frontmatter honestly records speakerDetection: false
        // + why (see writeFrontmatter()'s doc comment).
        std::vector<DiarizedTurn> diarizedTurns;
        bool effectiveSpeakerDetection = false;
        QString speakerDetectionError;
        if (speakerDetection) {
            // GPU memory hygiene: whisper's GPU-resident weights + KV cache
            // have no reason to sit warm through diarization — it's a
            // separate pass that doesn't touch m_engine at all (see below),
            // and leaving them resident means whisper.cpp's own CUDA
            // allocator and onnxruntime's CUDA allocator (DiarizationEngine)
            // are both live on the same device at once. Two independent
            // allocators fragmenting one device's address space is exactly
            // what let a real "Failed to allocate memory for requested
            // buffer" BFCArena error through even with plenty of aggregate
            // free VRAM (see patches/sherpa-onnx-batch-segmentation.patch's
            // item 5, a companion fix for the same failure). Reloaded right
            // after diarization finishes below — costs one extra model load
            // (a few seconds), the same trade already accepted for
            // m_labelingEngine at the top of this job.
            m_engine.reset();

            // "detecting-speakers": REAL progress, unlike "loading" above —
            // DiarizationEngine forwards sherpa-onnx's own chunk-processed
            // callback (see its doc comment).
            emit phase(QStringLiteral("detecting-speakers"));
            QString segPath, embedPath, diarizeError;
            if (ModelManager::ensureDiarizationModels(QUrl::fromLocalFile(vaultPath), &segPath, &embedPath,
                                                       &diarizeError)) {
                if (!m_diarizationEngine || !m_diarizationEngine->isValid() || m_diarizationSegPath != segPath
                    || m_diarizationEmbedPath != embedPath) {
                    m_diarizationEngine = std::make_unique<DiarizationEngine>(segPath, embedPath);
                    m_diarizationSegPath = segPath;
                    m_diarizationEmbedPath = embedPath;
                }
                if (m_diarizationEngine->isValid()) {
                    // Item 6 (Wave N): progress() is PHASE-relative (this
                    // sub-phase's own 0->1) — TranscriptionController blends
                    // it into overallProgress using this phase's rolling-
                    // average share of the whole job (see
                    // TranscriptionController::recomputeOverallProgress()'s
                    // doc comment), not a fixed half.
                    //
                    // Sub-phase text: DiarizationEngine's fraction blends
                    // sherpa-onnx's own internal segmentation/label-
                    // conversion/embedding split (see the vendored patch's
                    // kSegmentationProgressShare + kLabelConversionProgressShare
                    // = 0.30 boundary) into one 0->1 number — 0.30 here is
                    // that exact same boundary, not an approximation, so
                    // the label switches right when the real work actually
                    // moves from "found where speech is" to "whose voice is
                    // it". Only emitted on an actual sub-phase change (not
                    // every callback tick) to avoid redundant phase signals.
                    //
                    // Pause blocks right here (this callback fires often
                    // enough — see DiarizationEngine's doc comment). Cancel
                    // is NOT actionable mid-pass (no native sherpa-onnx abort
                    // hook this wave — see the Wave N plan's Decisions): the
                    // return value is deliberately ignored, and a cancel
                    // requested during diarization takes effect right after
                    // diarize() returns, below, instead of instantly.
                    QString detectingSubPhase;
                    diarizedTurns = m_diarizationEngine->diarize(*pcm, [this, &detectingSubPhase](double fraction) {
                        waitWhilePausedOrCancelled();
                        const QString subPhase = fraction < 0.30 ? QStringLiteral("detecting-speakers-segmenting")
                                                                  : QStringLiteral("detecting-speakers-identifying");
                        if (subPhase != detectingSubPhase) {
                            detectingSubPhase = subPhase;
                            emit phase(subPhase);
                        }
                        emit progress(fraction);
                    });
                    effectiveSpeakerDetection = true;
                } else {
                    speakerDetectionError = QStringLiteral("failed to load speaker diarization models");
                }
            } else {
                speakerDetectionError =
                    diarizeError.isEmpty() ? QStringLiteral("speaker diarization models unavailable") : diarizeError;
            }
        }
        // Diarization only contributes a rolling-average timing sample when
        // it actually ran successfully — a failed/unavailable-models attempt
        // is fast and not representative of a real diarization pass (see
        // speakerDetectionError above), so recording it would drag the
        // average down for every future job's bar proportions. Restarted
        // unconditionally either way — this is also "transcribing"'s start
        // baseline when speaker detection wasn't requested at all.
        const qint64 detectingSpeakersMs = phaseTimer.restart();
        if (effectiveSpeakerDetection)
            emit phaseDurationMs(QStringLiteral("detecting-speakers"), detectingSpeakersMs);
        // Diarization is a one-shot pass at the START of this job (above) —
        // nothing from here to the end of run() touches it again, so free
        // its GPU/CPU resources now rather than leaving them resident for no
        // reason through the whole (often much longer) transcription phase
        // that follows. Cheap to reload next time (~35MB of small onnx
        // models), unlike the big whisper/label models above, so this is
        // evicted unconditionally on every job rather than only on a
        // model-path change.
        m_diarizationEngine.reset();

        // Reload whisper now that diarization's GPU footprint is gone (see
        // the eviction above) — a no-op if speaker detection wasn't
        // requested, since m_engine was never evicted in that case.
        if (!m_engine || !m_engine->isValid() || m_modelPath != modelPath) {
            m_engine = std::make_unique<WhisperCppEngine>(modelPath);
            m_modelPath = modelPath;
        }
        if (!m_engine->isValid()) {
            emit finished(false, QStringLiteral("failed to load model at %1").arg(modelPath));
            return;
        }

        // Wave N (task system): a cancel requested DURING diarization (which
        // has no mid-pass abort hook — see the diarize() lambda's doc
        // comment above) takes effect here instead, right before any file
        // I/O happens — also doubles as a normal pause checkpoint between
        // phases for a job that wasn't paused during diarization at all.
        if (waitWhilePausedOrCancelled()) {
            emit finished(false, QStringLiteral("Cancelled"));
            return;
        }

        // VAD (this feature — DESIGN.md "Durable / resumable transcription",
        // chunking half; see VadChunker.h): one small model does double duty —
        // (a) whisper.cpp's own per-window silence gating (set on m_engine
        // below) and (b) finding VAD-aligned window boundaries so a long file
        // is transcribed as several bounded whisper_full() calls instead of
        // one unbounded one (reduces long-context hallucination drift). A
        // download/load failure degrades gracefully to today's single
        // whole-file window with no gating — never blocks the job — recorded
        // honestly in frontmatter below, same contract as speaker detection's
        // speakerDetectionError.
        QString vadError;
        const QString vadModelPath = ModelManager::ensureVadModel(QUrl::fromLocalFile(vaultPath), &vadError);
        std::vector<VadChunker::Window> windows;
        bool vadActive = false;
        if (!vadModelPath.isEmpty()) {
            QString chunkingError;
            windows = VadChunker::computeWindows(*pcm, vadModelPath, &chunkingError);
            if (chunkingError.isEmpty())
                vadActive = true;
            else
                vadError = chunkingError;
        }
        if (windows.empty())
            windows.push_back({0.0, static_cast<double>(pcm->size()) / 16000.0});
        // Reset on every job (not just a failure this time) — m_engine is a
        // persistent member reused across jobs/files, so a PREVIOUS job's
        // successful VAD path must not silently leak into this one if this
        // job's VAD failed.
        m_engine->setVadModelPath(vadActive ? vadModelPath : QString());

        QFile mdFile(sidecarPath(filePath));
        if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit finished(false, QStringLiteral("could not open %1 for writing").arg(mdFile.fileName()));
            return;
        }

        QStringList speakerLabels;
        for (int i = 1; i <= distinctSpeakerCount(diarizedTurns); ++i)
            speakerLabels.append(QStringLiteral("Speaker %1").arg(i));

        writeFrontmatter(mdFile, QFileInfo(modelPath).fileName(), effectiveSpeakerDetection, modelFallback, modelId,
                          speakerLabels, speakerDetectionError, vadActive, vadError);
        SegmentLineWriter lineWriter(mdFile);

        // "transcribing": whisper's own per-segment progress, blended across
        // ALL windows into one cumulative 0->1 (see secondsDoneInPriorWindows
        // below) — each window's own done/total would otherwise snap the bar
        // back to 0% at every window boundary. Item 6 (Wave N):
        // progress() is this phase's OWN 0->1 regardless of whether
        // diarization ran before it — TranscriptionController blends it into
        // overallProgress using this phase's rolling-average share (see
        // recomputeOverallProgress()'s doc comment), not a fixed half.
        emit phase(QStringLiteral("transcribing"));
        emit progress(0.0);
        const double totalFileSeconds = static_cast<double>(pcm->size()) / 16000.0;
        double secondsDoneInPriorWindows = 0.0;

        // Drives TranscriptionController::liveUpdateTick (see its doc
        // comment) — throttled independently of whisper's own progress
        // ticks, which fire on every internal decode-loop iteration
        // (including retries) rather than on real new content. liveTimer
        // starts already kLiveUpdateThrottleMs in the past so the FIRST
        // segment's tick fires immediately instead of waiting out the
        // throttle window.
        QElapsedTimer liveTimer;
        liveTimer.start();
        qint64 lastLiveTickMs = -kLiveUpdateThrottleMs;

        // Wave N (task system): whisper.cpp checks this deep inside its own
        // encode/decode loops (see WhisperCppEngine.cpp's onAbortCheck) —
        // frequent enough that blocking here for a pause is near-instant,
        // and returning true from waitWhilePausedOrCancelled() stops
        // whisper_full() mid-file rather than only at its next natural
        // boundary, unlike diarization above.
        const AbortCallback onAbort = [this] { return waitWhilePausedOrCancelled(); };

        for (const VadChunker::Window &window : windows) {
            // Between-window pause/cancel checkpoint — a single-window file
            // (today's pre-chunking behavior) never had this gap; mid-window
            // pause/cancel is still handled by onAbort above, unchanged.
            if (waitWhilePausedOrCancelled())
                break;

            const auto startSample = static_cast<std::size_t>(window.startSeconds * 16000.0);
            const auto endSample =
                std::min(pcm->size(), static_cast<std::size_t>(window.endSeconds * 16000.0));
            if (endSample <= startSample)
                continue;

            AudioChunk chunk;
            chunk.pcm16kMono.assign(pcm->begin() + static_cast<std::ptrdiff_t>(startSample),
                                     pcm->begin() + static_cast<std::ptrdiff_t>(endSample));
            chunk.offsetSeconds = window.startSeconds;
            const double windowSeconds = static_cast<double>(chunk.pcm16kMono.size()) / 16000.0;

            ProgressCallback onProgress = [this, &secondsDoneInPriorWindows, totalFileSeconds](double done, double) {
                const double fraction =
                    totalFileSeconds > 0 ? (secondsDoneInPriorWindows + done) / totalFileSeconds : 0.0;
                emit progress(fraction);
            };
            SegmentCallback onSegment = [&lineWriter, &diarizedTurns, effectiveSpeakerDetection, &liveTimer,
                                          &lastLiveTickMs, &chunk, this](const Segment &rawSegment) {
                // Chunk offset merge is the app's job (see
                // WhisperCppEngine.cpp/DESIGN.md "Engine architecture") — this
                // is now a REAL merge for multi-window files, not the no-op
                // it was when every job was a single offsetSeconds=0.0 chunk.
                Segment segment = rawSegment;
                segment.t0 += chunk.offsetSeconds;
                segment.t1 += chunk.offsetSeconds;
                // See isNonSpeechAnnotation()'s doc comment: a hallucinated
                // "[APPLAUSE]"/"[MUSIC]"/etc. segment never gets a speaker
                // label, regardless of what diarization found nearby.
                const bool isSpeech = !isNonSpeechAnnotation(QString::fromStdString(segment.text).trimmed());
                const QString label = (effectiveSpeakerDetection && isSpeech)
                    ? labelForSegment(diarizedTurns, segment.t0, segment.t1) : QString();
                lineWriter.append(segment, label);

                const qint64 nowMs = liveTimer.elapsed();
                if (nowMs - lastLiveTickMs >= kLiveUpdateThrottleMs) {
                    lastLiveTickMs = nowMs;
                    emit segmentWritten();
                }
            };

            m_engine->transcribe(chunk, onProgress, onSegment, onAbort);
            secondsDoneInPriorWindows += windowSeconds;

            if (isCancelRequested())
                break;
        }
        mdFile.close();

        // See syncSpeakersFrontmatterToBody()'s doc comment — the
        // "speakers:" list written above (before any segment existed) can
        // list a cluster that never actually won a single body tag. Only
        // knowable now that every segment line has actually been written.
        if (effectiveSpeakerDetection)
            syncSpeakersFrontmatterToBody(sidecarPath(filePath));

        // AI auto-labeling (opt-in, see DESIGN.md "Model management") is no
        // longer run as a bonus pass here — TranscriptionController::
        // onWorkerFinished() queues it as its own separate, visible follow-up
        // job (see transcribe()'s autoLabel doc comment) once this job
        // reports success.

        // A cancel mid-decode stops whisper_full() early (see onAbort above)
        // but everything transcribed up to that point is still flushed and
        // valid (SegmentLineWriter's per-append durability contract, same as
        // a kill mid-run) — reported as Failed/"Cancelled" rather than
        // Done, matching the Tasks list's honesty contract (this was
        // deliberately stopped, not a silent success).
        if (isCancelRequested()) {
            emit finished(false, QStringLiteral("Cancelled"));
            return;
        }
        emit phaseDurationMs(QStringLiteral("transcribing"), phaseTimer.elapsed());
        emit finished(true, {});
    }

    // "Regenerate auto-title" (item 9): re-runs JUST the labeling pass on an
    // already-transcribed file — no audio decode, no whisper, no frontmatter
    // reset. Unlike the autoLabel branch above, a failure here IS the job's
    // result (there's nothing else for this job to accomplish).
    //
    // skipIfAlreadyLabeled (item P21): only ever true for the automatic
    // follow-up pass transcribe(autoLabel=true) queues right after its own
    // transcribe job (see Job::skipIfAlreadyLabeled's doc comment) — false
    // for the user's explicit "Regenerate label and prelude" button
    // (relabel()), which must always force-regenerate. When true, an
    // existing non-empty label/tags/prelude means this file was already
    // labeled by a previous run (or a manual edit) and re-transcribing
    // shouldn't clobber it — reports success with no LLM call rather than
    // failure, since "nothing to do" isn't an error.
    void relabelJob(const QString &filePath, const QString &vaultPath, bool restrictTagsToExisting,
                     double tagReuseBias, double wordiness, bool skipIfAlreadyLabeled = false)
    {
        // Mirrors run()'s eviction of the label engine (see its doc
        // comment): this job needs only the label engine, so free whisper's
        // GPU-resident model now rather than let it sit warm through a
        // labeling pass that never touches it. m_diarizationEngine is
        // already evicted at the end of every run() job, so it's never
        // actually resident here — reset defensively anyway in case that
        // ever changes.
        m_engine.reset();
        m_diarizationEngine.reset();

        const QString mdPath = sidecarPath(filePath);
        if (!QFileInfo::exists(mdPath)) {
            emit finished(false, QStringLiteral("%1 has not been transcribed yet").arg(filePath));
            return;
        }

        QFile mdFile(mdPath);
        if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            emit finished(false, QStringLiteral("could not open %1").arg(mdPath));
            return;
        }
        QTextStream in(&mdFile);
        if (in.readLine() != QStringLiteral("---")) {
            emit finished(false, QStringLiteral("%1 has no frontmatter block").arg(mdPath));
            return;
        }
        // Pull out "speakers:"/"label:"/"tags:" while skipping the rest of
        // the frontmatter — speakers feeds plainTextFromTranscriptBody()'s
        // known-speaker allowlist (see segmentLinePattern()'s doc comment);
        // label/tags feed the skipIfAlreadyLabeled check below (item P21).
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

        // Item P21: this is the automatic post-transcribe follow-up, not an
        // explicit user request — don't clobber label/tags/prelude a
        // previous run (or a manual edit) already produced.
        if (skipIfAlreadyLabeled
            && (!existingLabel.isEmpty() || !existingTags.isEmpty() || rawBody.contains(kPreludeEndMarker))) {
            emit finished(true, {});
            return;
        }

        if (transcriptText.trimmed().isEmpty()) {
            emit finished(false, QStringLiteral("%1 has no transcript text to label").arg(mdPath));
            return;
        }

        QString error;
        if (!runLabelingPass(vaultPath, transcriptText, mdPath, restrictTagsToExisting, tagReuseBias, wordiness,
                              &error)) {
            emit finished(false, error);
            return;
        }
        emit finished(true, {});
    }

    // Feature 3a ("generate chapters"): a second, separate LLM pass sharing
    // the SAME resident label engine as relabelJob()/runLabelingPass() above
    // (see LabelingEngine::generateChapters()'s doc comment) — never a
    // second LabelingEngine instance. Mirrors relabelJob()'s shape closely:
    // same eviction, same "reopen the .md, read frontmatter+body, no audio
    // decode" approach, same "a failure here IS the job's result" contract.
    void generateChaptersJob(const QString &filePath, const QString &vaultPath)
    {
        m_engine.reset();
        m_diarizationEngine.reset();

        const QString mdPath = sidecarPath(filePath);
        if (!QFileInfo::exists(mdPath)) {
            emit finished(false, QStringLiteral("%1 has not been transcribed yet").arg(filePath));
            return;
        }

        QFile mdFile(mdPath);
        if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            emit finished(false, QStringLiteral("could not open %1").arg(mdPath));
            return;
        }
        QTextStream in(&mdFile);
        if (in.readLine() != QStringLiteral("---")) {
            emit finished(false, QStringLiteral("%1 has no frontmatter block").arg(mdPath));
            return;
        }
        QStringList knownSpeakers;
        QString line;
        while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
            const int colon = line.indexOf(':');
            if (colon >= 0 && line.left(colon).trimmed() == QStringLiteral("speakers"))
                knownSpeakers = parseSpeakersList(line.mid(colon + 1));
        }
        const QString body = in.readAll();
        mdFile.close();

        // Fresh parse of THIS job's own read of the file — the exact (index,
        // t0, text) tuples chapter generation needs (see
        // parsedSegmentsFromTranscriptBody()'s doc comment): index i's
        // position in this list IS the index LabelingEngine::
        // generateChapters() expects and returns "afterSegment" against.
        const QList<ParsedSegment> segments = parsedSegmentsFromTranscriptBody(body, knownSpeakers);
        if (segments.isEmpty()) {
            emit finished(false, QStringLiteral("%1 has no transcript text to generate chapters from").arg(mdPath));
            return;
        }

        QString labelError;
        const QString labelModelPath = ModelManager::ensureLabelModel(QUrl::fromLocalFile(vaultPath), &labelError);
        if (labelModelPath.isEmpty()) {
            emit finished(false, QStringLiteral("auto-label model unavailable: %1").arg(labelError));
            return;
        }
        if (!m_labelingEngine || !m_labelingEngine->isValid() || m_labelModelPath != labelModelPath) {
            m_labelingEngine = std::make_unique<LabelingEngine>(labelModelPath);
            m_labelModelPath = labelModelPath;
        }
        if (!m_labelingEngine->isValid()) {
            emit finished(false, QStringLiteral("failed to load labeling model at %1").arg(labelModelPath));
            return;
        }

        QStringList segmentTexts;
        segmentTexts.reserve(segments.size());
        for (const ParsedSegment &segment : segments)
            segmentTexts.append(segment.text);

        // Item P34: generateChapters() silently truncates (with a qWarning)
        // any single call whose formatted "INDEX: text\n" input exceeds
        // LabelingEngine::kMaxChapterInputChars — for a long file, one call
        // over the whole transcript meant everything past ~20 minutes of
        // speech never got proposed at all. Fix: split into multiple
        // windows, each kept under that budget, call generateChapters()
        // once per window, and offset each window's returned
        // afterSegmentIndex (0-relative to THAT window's own segmentTexts
        // slice) by the window's starting index in the full segments list
        // before merging. A window's cost estimate mirrors generateChapters(
        // )'s own "index: text\n" formatting exactly (digit count of i, ": ",
        // text, '\n') so the window boundary lands at the same place the
        // single-call truncation would have.
        QList<int> windowStarts; // segment index each window begins at
        {
            int windowStart = 0;
            qsizetype windowChars = 0;
            for (int i = 0; i < segmentTexts.size(); ++i) {
                const qsizetype segChars =
                    QString::number(i - windowStart).size() + 2 + segmentTexts.at(i).size() + 1;
                if (i > windowStart && windowChars + segChars > LabelingEngine::kMaxChapterInputChars) {
                    windowStarts.append(windowStart);
                    windowStart = i;
                    windowChars = 0;
                }
                windowChars += segChars;
            }
            windowStarts.append(windowStart);
        }
        // Don't let a pathologically small trailing window (e.g. one
        // segment left over) waste a whole extra LLM call on almost nothing
        // — fold it into the previous window if it's under a quarter of the
        // full transcript's average window size. Only meaningful once
        // there's more than one window.
        if (windowStarts.size() > 1) {
            const qsizetype lastWindowSegCount = segmentTexts.size() - windowStarts.last();
            const qsizetype avgWindowSegCount = segmentTexts.size() / windowStarts.size();
            if (lastWindowSegCount < avgWindowSegCount / 4)
                windowStarts.removeLast();
        }

        QList<Chapter> chapters;
        for (int w = 0; w < windowStarts.size(); ++w) {
            const int startIdx = windowStarts.at(w);
            const int endIdx = (w + 1 < windowStarts.size()) ? windowStarts.at(w + 1) : segmentTexts.size();
            const QStringList windowTexts = segmentTexts.mid(startIdx, endIdx - startIdx);

            QString genError;
            const auto windowChapters = m_labelingEngine->generateChapters(windowTexts, &genError);
            if (!windowChapters) {
                emit finished(false, QStringLiteral("chapter generation failed: %1").arg(genError));
                return;
            }
            // afterSegmentIndex is 0-relative to THIS window's own
            // windowTexts slice (see generateChapters()'s doc comment) —
            // offset by startIdx so it's relative to the full file's
            // segments list instead, exactly as if one single call had
            // produced it (startIdx is 0 for the first window, a no-op
            // offset there). The existing bounds/dedup/empty-title check
            // below (against the full `segments` list) still applies
            // uniformly across every window's contributions.
            for (const Chapter &chapter : *windowChapters)
                chapters.append({chapter.afterSegmentIndex + startIdx, chapter.title});
        }

        // See LabelingEngine::generateChapters()'s doc comment: the LLM only
        // ever returns which SEGMENT INDEX a chapter starts after — never
        // trusted as a timestamp directly. Sanitized here, against this
        // job's own freshly-parsed `segments`, before anything touches disk
        // (including the merge pass below, which needs valid indices to
        // pull excerpt text). Out-of-range or duplicate target indices (and
        // an empty title, which would otherwise render as a bare "## [t] "
        // marker line) are silently dropped, not treated as a job failure —
        // the grammar only constrains JSON shape, never the semantic
        // validity of the integer (a small model can still propose e.g.
        // afterSegment=99999 on a 50-segment transcript). Sorted by
        // afterSegmentIndex ascending: window offsets keep this roughly
        // sorted already, but mergeSimilarChapters() below treats list order
        // AS adjacency, so this makes that assumption actually hold.
        QList<Chapter> sanitized;
        QSet<int> seenTargets;
        for (const Chapter &chapter : chapters) {
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

        // Small instruct models over-propose boundaries in generateChapters()
        // itself (see kChapterSystemPrompt's doc comment) — collapse
        // over-eager adjacent chapters back down with a second, narrower
        // pass rather than trying to prompt-tune that away. This is a
        // refinement on an already-valid result, not a prerequisite for one:
        // a failure here (e.g. a transient generation error) falls back to
        // the unmerged-but-sanitized chapters instead of failing the whole
        // job.
        QList<Chapter> finalChapters = sanitized;
        QString mergeError;
        if (const auto merged = m_labelingEngine->mergeSimilarChapters(sanitized, segmentTexts, &mergeError))
            finalChapters = *merged;
        else
            qWarning() << "generateChaptersJob: chapter-merge pass failed, keeping unmerged chapters:" << mergeError;

        QList<ResolvedChapter> resolved;
        resolved.reserve(finalChapters.size());
        for (const Chapter &chapter : finalChapters)
            resolved.append({segments.at(chapter.afterSegmentIndex + 1).t0, chapter.title});

        QString writeError;
        if (!writeChaptersField(mdPath, resolved, &writeError)) {
            emit finished(false, writeError);
            return;
        }
        emit finished(true, {});
    }

    // Queued through the same job pipeline as run()/relabelJob() above (see
    // TranscriptionController::renameSpeaker()'s doc comment) — emits BOTH
    // the generic finished() (so this participates in busy/progress/the
    // jobs() list exactly like any other task) AND the specific
    // speakerRenamed() (so the caller learns which speaker's rename this
    // result belongs to, which finished() alone can't carry).
    void renameSpeaker(const QString &audioPath, const QString &oldName, const QString &newName)
    {
        QString error;
        const bool ok = renameSpeakerInFile(sidecarPath(audioPath), oldName, newName, &error);
        // Reported back against audioPath (not the .md sidecar) — matches
        // finished()'s convention of identifying a file by its ORIGINAL
        // (audio) fileUrl, which is what QML's Connections target compares
        // against.
        emit speakerRenamed(audioPath, oldName, ok, error);
        emit finished(ok, error);
    }

    // Mirrors renameSpeaker() immediately above, just calling
    // mergeSpeakersInFile() instead — see
    // TranscriptionController::mergeSpeakers()'s doc comment.
    void mergeSpeakers(const QString &audioPath, const QString &sourceName, const QString &targetName)
    {
        QString error;
        const bool ok = mergeSpeakersInFile(sidecarPath(audioPath), sourceName, targetName, &error);
        emit speakersMerged(audioPath, sourceName, targetName, ok, error);
        emit finished(ok, error);
    }

    // Wave N (task system, item 5): a model download as its OWN job — no
    // engine touched, nothing evicted, just bytes fetched via ModelManager.
    // target is "model" (uses modelId)/"label"/"diarization", matching
    // TranscriptionController::DownloadTarget one-to-one (passed as a plain
    // QString rather than the enum itself to cross the thread boundary
    // without needing a registered Qt metatype for a controller-private
    // enum).
    void download(const QString &vaultPath, const QString &target, const QString &modelId)
    {
        const QUrl vaultUrl = QUrl::fromLocalFile(vaultPath);
        const ModelManager::DownloadProgressCallback onProgress = [this](qint64 done, qint64 total) {
            emit downloadProgress(done, total);
        };
        ModelManager::DownloadControl control;
        control.paused = [this] { return m_pauseRequested.load(); };
        control.cancelled = [this] { return m_cancelRequested.load(); };

        QString error;
        bool ok = false;
        if (target == QStringLiteral("model")) {
            ok = !ModelManager::ensureModel(vaultUrl, modelId, &error, nullptr, onProgress, control).isEmpty();
        } else if (target == QStringLiteral("label")) {
            ok = !ModelManager::ensureLabelModel(vaultUrl, &error, onProgress, control).isEmpty();
        } else if (target == QStringLiteral("diarization")) {
            QString segPath, embedPath;
            ok = ModelManager::ensureDiarizationModels(vaultUrl, &segPath, &embedPath, &error, onProgress, control);
        }
        emit finished(ok, error);
    }

    // Split-recording combine (see
    // TranscriptionController::findCombineGroups()'s doc comment):
    // concatenates sourcePaths (already in combine order per the caller) into
    // outputPath via ffmpeg's concat demuxer with stream copy (lossless,
    // fast — these are same-format parts from the same recorder), then
    // moves (or deletes, per deleteOriginals) each source into a sibling
    // .combined/ dir so the next vault scan doesn't re-detect the same
    // group. Never touches m_engine/m_diarizationEngine/m_labelingEngine —
    // unlike run()/download(), this job doesn't use any of them, so there's
    // nothing to evict/reload here.
    void combineJob(const QStringList &sourcePaths, const QString &outputPath, bool deleteOriginals)
    {
        emit phase(QStringLiteral("combining"));

        QTemporaryFile listFile;
        if (!listFile.open()) {
            emit finished(false, QStringLiteral("could not create ffmpeg concat list"));
            return;
        }
        qint64 totalInputBytes = 0;
        for (const QString &path : sourcePaths) {
            // ffmpeg concat demuxer syntax: single-quoted paths, any literal
            // single quote escaped as '\''.
            QString escaped = path;
            escaped.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
            listFile.write(QStringLiteral("file '%1'\n").arg(escaped).toUtf8());
            totalInputBytes += QFileInfo(path).size();
        }
        listFile.close();

        QProcess ffmpeg;
        ffmpeg.start(QStringLiteral("ffmpeg"),
                      {QStringLiteral("-f"), QStringLiteral("concat"), QStringLiteral("-safe"), QStringLiteral("0"),
                       QStringLiteral("-i"), listFile.fileName(), QStringLiteral("-c"), QStringLiteral("copy"),
                       QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                       QStringLiteral("error"), outputPath});
        if (!ffmpeg.waitForStarted()) {
            emit finished(false, QStringLiteral("failed to start ffmpeg"));
            return;
        }

        // Same 100ms poll/pause/cancel shape as AudioDecoder::decodeTo16kMono
        // — SIGSTOP/SIGCONT for pause, kill for cancel. Progress is
        // approximate (output file size vs. summed input size — stream-copy
        // throughput tracks output size closely for PCM/WAV), matching this
        // app's existing "poll a size on disk" precedent (ModelManager's
        // curl downloads) rather than adding a second, ffmpeg-`-progress`-
        // based progress-parsing mechanism just for this one job kind.
        bool wasPaused = false;
        bool cancelled = false;
        while (ffmpeg.state() != QProcess::NotRunning) {
            if (isCancelRequested()) {
                cancelled = true;
                ffmpeg.kill();
                ffmpeg.waitForFinished(2000);
                break;
            }
            const bool shouldPause = m_pauseRequested.load();
            if (shouldPause && !wasPaused) {
                ::kill(static_cast<pid_t>(ffmpeg.processId()), SIGSTOP);
                wasPaused = true;
                emit paused(true);
            } else if (!shouldPause && wasPaused) {
                ::kill(static_cast<pid_t>(ffmpeg.processId()), SIGCONT);
                wasPaused = false;
                emit paused(false);
            }
            ffmpeg.waitForReadyRead(100);
            if (totalInputBytes > 0) {
                const qint64 outBytes = QFileInfo(outputPath).size();
                emit progress(std::clamp(static_cast<double>(outBytes) / static_cast<double>(totalInputBytes), 0.0, 1.0));
            }
        }

        if (cancelled) {
            QFile::remove(outputPath);
            emit finished(false, QStringLiteral("Cancelled"));
            return;
        }

        if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
            const QString err = QString::fromUtf8(ffmpeg.readAllStandardError());
            QFile::remove(outputPath);
            emit finished(false, err.isEmpty() ? QStringLiteral("ffmpeg concat failed") : err);
            return;
        }

        emit progress(1.0);

        // Move (or delete) every source now that the merge succeeded — a
        // sibling .combined/ dir next to each one (hidden dotfolder, same
        // convention as .transcriber/ — naturally invisible to
        // VaultTreeModel::rebuild()'s scan, which never passes QDir::Hidden)
        // so the next scan doesn't re-detect this exact group.
        for (const QString &path : sourcePaths) {
            if (deleteOriginals) {
                QFile::remove(path);
                continue;
            }
            const QFileInfo info(path);
            QDir combinedDir(info.absolutePath() + QStringLiteral("/.combined"));
            if (!combinedDir.exists())
                combinedDir.mkpath(QStringLiteral("."));
            QFile::rename(path, combinedDir.absoluteFilePath(info.fileName()));
        }

        emit finished(true, QString());
    }

    // GPU memory hygiene, the idle case: run()/relabelJob() already evict
    // whichever engine THIS job doesn't need (see their doc comments), but
    // that leaves whichever engine the MOST RECENT job DID use warm
    // indefinitely — fine while more jobs are queued (that's the whole
    // point of staying warm), pointless once the queue is empty and nothing
    // is going to reuse it. TranscriptionController::onWorkerFinished()
    // calls this exactly when the job queue has just drained to empty, so
    // idle GPU footprint goes back to ~0 instead of sitting at whatever the
    // last job happened to need. Evicting all three unconditionally (rather
    // than tracking which one is actually still resident) keeps this simple
    // — resetting an already-null unique_ptr is a no-op.
    void evictIdle()
    {
        m_engine.reset();
        m_diarizationEngine.reset();
        m_labelingEngine.reset();
    }

signals:
    void progress(double fraction);
    // 2026-07-10 (rolling-average phase weighting): fired once a named phase
    // (one of TranscriptionController::kTimedPhases) completes NORMALLY —
    // never on a cancel/failure mid-phase, see run()'s emit sites — so
    // TranscriptionController can feed it into that phase's RollingAverage
    // (recordPhaseDuration()) and re-derive overallProgress's per-phase
    // widths from real timing data instead of a fixed guess. Replaced the
    // old overallProgress signal this worker used to emit directly (a
    // hardcoded diarization=first-half/transcription=second-half blend) —
    // the controller now computes that blend itself, since it's the one
    // that owns the rolling-average data this needs.
    void phaseDurationMs(const QString &phase, qint64 durationMs);
    // See TranscriptionController::liveUpdateTick's doc comment — emitted
    // (throttled, see run()'s liveTimer) each time a real new segment line
    // is written to the .md sidecar during an in-progress transcription.
    void segmentWritten();
    // See TranscriptionController::currentPhase's doc comment — emitted at
    // each phase transition within run() (loading/detecting-speakers/
    // transcribing); never emitted by relabelJob()/renameSpeaker()/
    // mergeSpeakers() (none of these have phases worth naming).
    void phase(const QString &phase);
    // Item O35: fired once right after a model successfully (re)loads in
    // run() — see WhisperCppEngine::activeBackendName()'s doc comment for
    // where the value itself comes from. Never emitted by relabelJob()/
    // renameSpeaker()/mergeSpeakers()/download() (none of them touch
    // WhisperCppEngine).
    void backendNameChanged(const QString &name);
    // Wave N (task system): fired on a real pause/resume transition (never
    // on every poll — see waitWhilePausedOrCancelled()'s "only on change"
    // discipline).
    void paused(bool isPaused);
    void finished(bool ok, const QString &error);
    // Wave N (task system, item 5): download-job progress — deliberately a
    // SEPARATE signal from progress()/phase() above (which are transcribe-
    // job-specific concepts: whisper's segment progress, diarization/
    // loading/transcribing phase names) rather than overloading them, since
    // a download job has neither segments nor those phases, just bytes.
    void downloadProgress(qint64 bytesDone, qint64 bytesTotal);
    void speakerRenamed(const QString &path, const QString &oldName, bool ok, const QString &error);
    void speakersMerged(const QString &path, const QString &sourceName, const QString &targetName, bool ok,
                         const QString &error);

private:
    // Used by relabelJob() — both a manual "regenerate label" request and an
    // automatic post-transcription follow-up (see
    // TranscriptionController::onWorkerFinished()) go through the SAME
    // relabelJob() call now, run() no longer has its own bonus-pass branch.
    // Resolves/warms the label model, generates, and splices the result into
    // mdPath's frontmatter. Returns false + *errorOut on failure.
    bool runLabelingPass(const QString &vaultPath, const QString &transcriptText, const QString &mdPath,
                         bool restrictTagsToExisting, double tagReuseBias, double wordiness,
                         QString *errorOut = nullptr)
    {
        QString labelError;
        const QString labelModelPath = ModelManager::ensureLabelModel(QUrl::fromLocalFile(vaultPath), &labelError);
        if (labelModelPath.isEmpty()) {
            const QString msg = QStringLiteral("auto-label model unavailable: %1").arg(labelError);
            if (errorOut)
                *errorOut = msg;
            else
                qWarning() << msg;
            return false;
        }
        if (!m_labelingEngine || !m_labelingEngine->isValid() || m_labelModelPath != labelModelPath) {
            m_labelingEngine = std::make_unique<LabelingEngine>(labelModelPath);
            m_labelModelPath = labelModelPath;
        }
        if (!m_labelingEngine->isValid()) {
            const QString msg = QStringLiteral("failed to load labeling model at %1").arg(labelModelPath);
            if (errorOut)
                *errorOut = msg;
            else
                qWarning() << msg;
            return false;
        }
        // Item 15 (Wave E): existingTags is now ALWAYS collected (cheap —
        // vaults are small), not just when restrictTagsToExisting is set —
        // tagReuseBias needs a vocabulary to bias toward even in
        // unrestricted mode, whereas before this scan only ran for the hard-
        // restrict case.
        const QStringList existingTags = collectVaultTags(vaultPath);
        // Item P29: a short transcript shouldn't get a disproportionately
        // long prelude just because the user's global wordiness slider is
        // set high — cap the EFFECTIVE wordiness by transcript length (word
        // count is a cheap, good-enough proxy for "how much is there to
        // summarize"). Thresholds are deliberately coarse steps, not a
        // precise formula — this only needs to stop the "3-sentence
        // transcript, 5-paragraph prelude" case, not fine-tune prose length.
        const int wordCount = transcriptText.split(QRegularExpression(QStringLiteral("\\s+")),
                                                     Qt::SkipEmptyParts)
                                   .size();
        double effectiveWordiness = wordiness;
        if (wordCount < 50)
            effectiveWordiness = std::min(wordiness, 0.15);
        else if (wordCount < 150)
            effectiveWordiness = std::min(wordiness, 0.35);
        QString genError;
        const auto result = m_labelingEngine->label(transcriptText, existingTags, restrictTagsToExisting,
                                                     tagReuseBias, effectiveWordiness, &genError);
        if (!result) {
            const QString msg = QStringLiteral("auto-label generation failed: %1").arg(genError);
            if (errorOut)
                *errorOut = msg;
            else
                qWarning() << msg;
            return false;
        }
        writeLabelingFields(mdPath, *result);
        return true;
    }

    std::unique_ptr<WhisperCppEngine> m_engine;
    QString m_modelPath;
    std::unique_ptr<LabelingEngine> m_labelingEngine;
    QString m_labelModelPath;
    std::unique_ptr<DiarizationEngine> m_diarizationEngine;
    QString m_diarizationSegPath;
    QString m_diarizationEmbedPath;

    std::atomic<bool> m_pauseRequested{false};
    std::atomic<bool> m_cancelRequested{false};
    // Tracks whether the LAST waitWhilePausedOrCancelled() call actually
    // blocked, so paused(true)/paused(false) fire only on a real transition
    // — same "only on change" discipline as phase() (see run()'s doc
    // comment), not once per poll.
    bool m_pausedSignaled = false;

    // Wave N (task system): the ONE interruption point every cooperative
    // checkpoint in this file funnels through — whisper.cpp's
    // abort_callback (via WhisperCppEngine's onAbort) and the diarization
    // progress callback below both call this on every tick. Blocks (waking
    // every 100ms to re-check) while paused and not cancelled; returns true
    // the instant cancellation should take effect (whether requested before
    // this call, or while blocked inside it).
    bool waitWhilePausedOrCancelled()
    {
        while (m_pauseRequested.load() && !m_cancelRequested.load()) {
            if (!m_pausedSignaled) {
                m_pausedSignaled = true;
                emit paused(true);
            }
            QThread::msleep(100);
        }
        if (m_pausedSignaled) {
            m_pausedSignaled = false;
            emit paused(false);
        }
        return m_cancelRequested.load();
    }
};

TranscriptionController::TranscriptionController(QObject *parent)
    : QObject(parent)
{
    loadPhaseTimings();
    // See recomputeOverallProgress()'s doc comment — the only way
    // "loading"/"decoding" (no real progress callback) ever advance
    // overallProgress mid-phase. Only ever running while m_phase is one of
    // those two — started/stopped from the phase() handler below.
    m_indeterminatePhaseTimer = new QTimer(this);
    m_indeterminatePhaseTimer->setInterval(200);
    connect(m_indeterminatePhaseTimer, &QTimer::timeout, this, &TranscriptionController::recomputeOverallProgress);

    m_worker = new TranscriptionWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &TranscriptionWorker::progress, this, [this](double fraction) {
        m_progress = fraction;
        emit progressChanged();
        // A combine job's "combining" phase isn't one of
        // recomputeOverallProgress()'s fixed transcribe-job phase buckets
        // (loading/decoding/[detecting-speakers]/transcribing) — mirrors
        // the downloadProgress connection below's direct assignment, since
        // ffmpeg concat progress is likewise a single real fraction with no
        // sub-phases to blend.
        if (m_phase == QStringLiteral("combining")) {
            m_overallProgress = fraction;
            emit overallProgressChanged();
        } else {
            recomputeOverallProgress();
        }
    });
    // 2026-07-10 (rolling-average phase weighting): feeds this phase's
    // just-finished duration into its RollingAverage — see
    // recordPhaseDuration()'s doc comment. Replaces the old direct
    // TranscriptionWorker::overallProgress connection (the controller now
    // derives overallProgress itself, from m_phaseTimings + m_phase +
    // m_progress, via recomputeOverallProgress()).
    connect(m_worker, &TranscriptionWorker::phaseDurationMs, this, &TranscriptionController::recordPhaseDuration);
    // Wave N (task system, item 5): a download job has no "phase" of its
    // own in the transcribe-job sense, but reusing m_phase/m_progress here
    // (rather than adding parallel QML-facing properties) means BottomBar's
    // existing phase-label + percentage + progress-bar bindings work for a
    // download job with no changes beyond a new phaseLabel() case for
    // "downloading" (see BottomBar.qml). Guarded so currentPhaseChanged only
    // fires on a real transition, not on every ~100ms poll tick.
    connect(m_worker, &TranscriptionWorker::downloadProgress, this, [this](qint64 bytesDone, qint64 bytesTotal) {
        if (m_phase != QStringLiteral("downloading")) {
            m_phase = QStringLiteral("downloading");
            emit currentPhaseChanged();
        }
        const double fraction =
            bytesTotal > 0 ? static_cast<double>(bytesDone) / static_cast<double>(bytesTotal) : 0.0;
        m_progress = fraction;
        m_overallProgress = fraction;
        emit progressChanged();
        emit overallProgressChanged();
    });
    connect(m_worker, &TranscriptionWorker::paused, this, [this](bool isPaused) {
        m_activePaused = isPaused;
        if (isPaused)
            m_activeAccumulatedMs += m_activeSegmentTimer.elapsed();
        else
            m_activeSegmentTimer.restart();
        emit activePausedChanged();
    });
    connect(this, &TranscriptionController::queueChanged, this, &TranscriptionController::persistQueue);
    connect(this, &TranscriptionController::queueChanged, this, &TranscriptionController::rebuildJobStatusIndex);
    connect(m_worker, &TranscriptionWorker::segmentWritten, this, [this] {
        m_liveUpdateTick++;
        emit liveUpdateTickChanged();
    });
    connect(m_worker, &TranscriptionWorker::phase, this, [this](const QString &phase) {
        m_phase = phase;
        m_phaseStartMs = activeElapsedMs();
        emit currentPhaseChanged();
        // See recomputeOverallProgress()'s doc comment — only "loading" and
        // "decoding" need the timer (no real progress callback of their
        // own); every other phase (including the detecting-speakers-*
        // sub-phases) already advances overallProgress via the progress()
        // connection above.
        if (phase == QStringLiteral("loading") || phase == QStringLiteral("decoding"))
            m_indeterminatePhaseTimer->start();
        else
            m_indeterminatePhaseTimer->stop();
        recomputeOverallProgress();
    });
    connect(m_worker, &TranscriptionWorker::backendNameChanged, this, [this](const QString &name) {
        if (m_activeBackendName != name) {
            m_activeBackendName = name;
            emit activeBackendNameChanged();
        }
    });
    connect(m_worker, &TranscriptionWorker::finished, this, &TranscriptionController::onWorkerFinished);
    connect(m_worker, &TranscriptionWorker::speakerRenamed, this,
            [this](const QString &path, const QString &oldName, bool ok, const QString &error) {
                emit speakerRenamed(QUrl::fromLocalFile(path), oldName, ok, error);
            });
    connect(m_worker, &TranscriptionWorker::speakersMerged, this,
            [this](const QString &path, const QString &sourceName, const QString &targetName, bool ok,
                   const QString &error) {
                emit speakersMerged(QUrl::fromLocalFile(path), sourceName, targetName, ok, error);
            });
    m_thread.start();
}

TranscriptionController::~TranscriptionController()
{
    // Bug report (2026-07-09): "transcription was still running after
    // closing the GUI... it should never run in the background". Root
    // cause: m_thread.wait() below blocks the calling (GUI) thread until
    // the worker thread's current call returns — but nothing was ever
    // telling that call to stop, so closing the app while a job was
    // Active made the WHOLE APPLICATION SHUTDOWN silently hang/block until
    // whatever whisper.cpp/sherpa-onnx call was in flight happened to
    // finish naturally (which, for a long recording, could be minutes),
    // during which the transcription kept running exactly as if nothing
    // had been closed at all. Requesting cancel first lets it unblock at
    // the very next checkpoint the active job's phase supports (the same
    // ones the pause/cancel UI feature already relies on — see
    // waitWhilePausedOrCancelled()'s doc comment for which phases those
    // are), so shutdown is prompt instead of an indefinite silent hang.
    // requestResume() wakes it first in case it was sitting paused, which
    // would otherwise block forever waiting for a pause that will never be
    // lifted by anyone once this destructor is running.
    if (m_worker) {
        m_worker->requestResume();
        m_worker->requestCancel();
    }
    m_thread.quit();
    m_thread.wait();
}

const QStringList TranscriptionController::kTimedPhases = {
    QStringLiteral("loading"),
    QStringLiteral("decoding"),
    QStringLiteral("detecting-speakers"),
    QStringLiteral("transcribing"),
};

// DiarizationEngine's progress callback emits phase() as
// "detecting-speakers-segmenting"/"detecting-speakers-identifying" (see
// run()'s doc comment on the 0.30 sub-phase boundary) — both belong to the
// single "detecting-speakers" bucket for timing/weighting purposes. Every
// other phase name is already its own bucket. This is the BUCKET identity
// only (used to compare against recomputeOverallProgress()'s `active` list
// and to look up defaultPhaseDurationMs()) — NOT the m_phaseTimings storage
// key, see canonicalPhaseKey() below for that.
static QString phaseBucket(const QString &phase)
{
    if (phase.startsWith(QStringLiteral("detecting-speakers")))
        return QStringLiteral("detecting-speakers");
    return phase;
}

// 2026-07-10 (item P3): m_phaseTimings' actual storage key — folds modelId
// (and, only when relevant, an explicit "diar" marker for speakerDetection)
// onto the phase bucket name. A large model's "loading"/"transcribing" takes
// materially longer than a tiny one, and diarization adds real per-job
// overhead — sharing one rolling average across every model/config produced
// meaningless bar proportions and ETAs the instant a user switched models.
// modelId is deliberately part of the key for EVERY phase (not just
// "loading"/"transcribing" where it obviously matters) — keeping one
// consistent key shape is simpler than special-casing which phases care.
static QString canonicalPhaseKey(const QString &phase, const QString &modelId, bool speakerDetection)
{
    QString key = phaseBucket(phase) + QLatin1Char('|') + modelId;
    if (speakerDetection)
        key += QStringLiteral("|diar");
    return key;
}

// modelId of whichever job is currently Active, or empty if none — the
// per-config timing key (canonicalPhaseKey()) needs this at each of the three
// points below; job.modelId (set in transcribe()) and m_activeSpeakerDetection
// (set in startJob()) are both already available here since m_activeIndex
// only clears in onWorkerFinished(), which runs strictly after every
// phase()/phaseDurationMs() signal for that job.
QString TranscriptionController::activeModelIdForTiming() const
{
    if (m_activeIndex >= 0 && m_activeIndex < m_jobs.size())
        return m_jobs[m_activeIndex].modelId;
    return QString();
}

void TranscriptionController::loadPhaseTimings()
{
    // 2026-07-10 (item P3): keys are now composite (phase|modelId[|diar]),
    // so there's no fixed upfront list to load by name — enumerate whatever
    // keys are actually persisted instead. childKeys() naturally covers every
    // model/config combination ever recorded on this machine.
    QSettings settings;
    settings.beginGroup(QStringLiteral("phaseTimings"));
    for (const QString &key : settings.childKeys()) {
        const QVariantList stored = settings.value(key).toList();
        QVector<double> samples;
        samples.reserve(stored.size());
        for (const QVariant &v : stored)
            samples.append(v.toDouble());
        RollingAverage avg;
        avg.setSamples(samples);
        m_phaseTimings.insert(key, avg);
    }
    settings.endGroup();
}

void TranscriptionController::savePhaseTiming(const QString &key)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("phaseTimings"));
    QVariantList list;
    for (double v : m_phaseTimings.value(key).samples())
        list.append(v);
    settings.setValue(key, list);
    settings.endGroup();
}

void TranscriptionController::recordPhaseDuration(const QString &phase, qint64 durationMs)
{
    const QString key = canonicalPhaseKey(phase, activeModelIdForTiming(), m_activeSpeakerDetection);
    m_phaseTimings[key].addSample(static_cast<double>(durationMs));
    savePhaseTiming(key);
    recomputeOverallProgress();
}

// See this method's header doc comment. "detecting-speakers"/"transcribing"
// share the same estimate so their DEFAULT split is exactly 50/50 (the fixed
// split this whole feature replaced) until real per-machine/per-model timing
// data overrides it; "loading"/"decoding" get their own modest estimates
// instead of being folded into that 50/50.
double TranscriptionController::defaultPhaseDurationMs(const QString &phase)
{
    if (phase == QStringLiteral("loading"))
        return 2000.0;
    if (phase == QStringLiteral("decoding"))
        return 1500.0;
    return 10000.0; // "detecting-speakers" / "transcribing"
}

double TranscriptionController::phaseRelativeSyntheticProgress() const
{
    if (m_phase != QStringLiteral("loading") && m_phase != QStringLiteral("decoding"))
        return m_progress;
    const QString key = canonicalPhaseKey(m_phase, activeModelIdForTiming(), m_activeSpeakerDetection);
    const double avg = m_phaseTimings.value(key).average(defaultPhaseDurationMs(m_phase));
    if (avg <= 0.0)
        return 0.0;
    const qint64 elapsedInPhase = activeElapsedMs() - m_phaseStartMs;
    return qMin(0.95, static_cast<double>(elapsedInPhase) / avg);
}

void TranscriptionController::recomputeOverallProgress()
{
    // Empty (idle/relabel/rename/merge) or "downloading"/"combining" (both
    // get their own direct m_overallProgress assignment — see the
    // downloadProgress and progress() connections in the constructor)
    // aren't covered by this phase-weighting scheme.
    if (m_phase.isEmpty() || m_phase == QStringLiteral("downloading") || m_phase == QStringLiteral("combining"))
        return;

    QStringList active = {QStringLiteral("loading"), QStringLiteral("decoding")};
    if (m_activeSpeakerDetection)
        active.append(QStringLiteral("detecting-speakers"));
    active.append(QStringLiteral("transcribing"));

    const QString modelId = activeModelIdForTiming();

    double totalWeight = 0.0;
    QMap<QString, double> weight;
    for (const QString &p : std::as_const(active)) {
        const double w = m_phaseTimings.value(canonicalPhaseKey(p, modelId, m_activeSpeakerDetection))
                              .average(defaultPhaseDurationMs(p));
        weight[p] = w;
        totalWeight += w;
    }
    if (totalWeight <= 0.0)
        totalWeight = 1.0;

    const QString currentBucket = phaseBucket(m_phase);
    double offset = 0.0;
    for (const QString &p : std::as_const(active)) {
        const double share = weight[p] / totalWeight;
        if (p == currentBucket) {
            m_overallProgress = offset + phaseRelativeSyntheticProgress() * share;
            emit overallProgressChanged();
            return;
        }
        offset += share;
    }
}

bool TranscriptionController::isQueued(const QUrl &fileUrl) const
{
    for (const Job &job : m_jobs) {
        if (job.fileUrl == fileUrl
            && (job.status == JobStatus::Queued || job.status == JobStatus::Active
                || job.status == JobStatus::Paused))
            return true;
    }
    return false;
}

void TranscriptionController::rebuildJobStatusIndex()
{
    QHash<QString, JobStatus> rebuilt;
    for (const Job &job : m_jobs) {
        if (job.fileUrl.isEmpty())
            continue; // download job — names no single vault file
        const QString path = job.fileUrl.toLocalFile();
        const auto it = rebuilt.constFind(path);
        if (it == rebuilt.constEnd() || jobStatusPriority(job.status) > jobStatusPriority(it.value()))
            rebuilt[path] = job.status;
    }
    if (rebuilt != m_jobStatusByPath) {
        m_jobStatusByPath = rebuilt;
        emit jobStatusesChanged();
    }
}

int TranscriptionController::jobStatusForPath(const QString &path) const
{
    const auto it = m_jobStatusByPath.constFind(path);
    return it == m_jobStatusByPath.constEnd() ? static_cast<int>(JobStatus::None) : static_cast<int>(it.value());
}

bool TranscriptionController::matchesExistingTranscription(const QUrl &fileUrl, const QString &modelId,
                                                            bool speakerDetection) const
{
    const QVariantMap meta = readMetadata(fileUrl);
    if (meta.isEmpty())
        return false; // not transcribed yet — nothing to match

    if (meta.value(QStringLiteral("speakerDetection")).toBool() != speakerDetection)
        return false;

    // Same model-resolution logic as ModelManager::ensureModel(), minus the
    // disk/network side effects — look up modelId's file name, falling back
    // to the default model's for an unrecognized id (mirroring ensureModel's
    // own fallback so this comparison never falsely reports "differs" for
    // the same fallback substitution that actually ran). Diarization no
    // longer affects which whisper model file is expected (it's a separate
    // pass, independent of modelId — see DiarizationEngine).
    QString requestedFileName;
    QString defaultFileName;
    for (const ModelManager::ModelInfo &info : ModelManager::availableModels()) {
        if (info.id == modelId)
            requestedFileName = info.fileName;
        if (info.id == QString::fromUtf8(ModelManager::kDefaultModelId))
            defaultFileName = info.fileName;
    }
    const QString expectedFileName = requestedFileName.isEmpty() ? defaultFileName : requestedFileName;

    return meta.value(QStringLiteral("model")).toString() == expectedFileName;
}

void TranscriptionController::transcribe(const QUrl &fileUrl, const QUrl &vaultUrl, const QString &modelId,
                                          bool speakerDetection, bool autoLabel, bool restrictTagsToExisting,
                                          double tagReuseBias, double wordiness)
{
    // Done/Failed entries for the same fileUrl deliberately do NOT block
    // re-queueing (see class doc) — only Queued/Active does.
    if (isQueued(fileUrl))
        return;

    // Item O15: a freshly requested job jumps ahead of whatever's already
    // Queued — see frontOfQueueInsertIndex()'s doc comment. startIndex
    // remembers where the FIRST newly-added entry (a download job, if one
    // was queued, otherwise this transcribe job itself) landed, so
    // autostart-if-idle below starts the right one; insertAt then walks
    // forward as each subsequent entry (download(s), the transcribe job
    // itself, autoLabel's follow-up relabel job) is inserted right after the
    // previous one, keeping this whole cluster contiguous at the front of
    // the queue rather than scattered.
    int insertAt = frontOfQueueInsertIndex();
    const int startIndex = insertAt;

    // Item 5 (Wave N): the model(s) this job needs, queued as their OWN
    // job(s) ahead of it if not already on disk — see
    // maybeQueueDownloadJob()'s doc comment.
    if (maybeQueueDownloadJob(DownloadTarget::WhisperModel, vaultUrl, modelId, insertAt) >= 0)
        ++insertAt;
    if (speakerDetection
        && maybeQueueDownloadJob(DownloadTarget::DiarizationModels, vaultUrl, QString(), insertAt) >= 0)
        ++insertAt;

    Job job;
    job.id = m_nextJobId++;
    job.fileUrl = fileUrl;
    job.vaultUrl = vaultUrl;
    job.modelId = modelId;
    job.speakerDetection = speakerDetection;
    job.autoLabel = autoLabel;
    job.restrictTagsToExisting = restrictTagsToExisting;
    job.tagReuseBias = tagReuseBias;
    job.wordiness = wordiness;
    job.status = JobStatus::Queued;
    m_jobs.insert(insertAt, job);
    ++insertAt;

    // autoLabel's follow-up label pass is queued as its OWN job right NOW —
    // both entries show up in the Tasks list together immediately, rather
    // than the label task only appearing once transcription finishes. It
    // naturally waits its turn behind the job above (only one job runs at a
    // time — see startJob()/onWorkerFinished()'s queue advancement) and, if
    // that job fails, this one still runs next and fails fast on its own
    // (relabelJob() reports "not transcribed yet" — there's no .md yet) —
    // it is NOT silently skipped just because its upstream job failed, so
    // the Tasks list always reflects exactly what was requested.
    if (autoLabel) {
        if (maybeQueueDownloadJob(DownloadTarget::LabelModel, vaultUrl, QString(), insertAt) >= 0)
            ++insertAt;
        Job labelJob;
        labelJob.id = m_nextJobId++;
        labelJob.fileUrl = fileUrl;
        labelJob.vaultUrl = vaultUrl;
        labelJob.relabelOnly = true;
        labelJob.skipIfAlreadyLabeled = true;
        labelJob.restrictTagsToExisting = restrictTagsToExisting;
        labelJob.tagReuseBias = tagReuseBias;
        labelJob.wordiness = wordiness;
        labelJob.status = JobStatus::Queued;
        m_jobs.insert(insertAt, labelJob);
    }

    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(startIndex);
}

void TranscriptionController::transcribeAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                             const QString &modelId, bool speakerDetection, bool autoLabel,
                                             bool restrictTagsToExisting, double tagReuseBias, double wordiness)
{
    for (const QVariant &v : fileUrls)
        transcribe(v.toUrl(), vaultUrl, modelId, speakerDetection, autoLabel, restrictTagsToExisting, tagReuseBias,
                   wordiness);
}

void TranscriptionController::relabel(const QUrl &fileUrl, const QUrl &vaultUrl, bool restrictTagsToExisting,
                                       double tagReuseBias, double wordiness)
{
    // Same dedup rule as transcribe(): a Queued/Active entry (whether a full
    // transcribe or a relabel-only job) blocks a duplicate request.
    if (isQueued(fileUrl))
        return;

    // Item O15: see transcribe()'s identical use of frontOfQueueInsertIndex()
    // for why this jumps ahead of whatever's already Queued.
    int insertAt = frontOfQueueInsertIndex();
    const int startIndex = insertAt;
    if (maybeQueueDownloadJob(DownloadTarget::LabelModel, vaultUrl, QString(), insertAt) >= 0)
        ++insertAt;

    Job job;
    job.id = m_nextJobId++;
    job.fileUrl = fileUrl;
    job.vaultUrl = vaultUrl;
    job.relabelOnly = true;
    job.restrictTagsToExisting = restrictTagsToExisting;
    job.tagReuseBias = tagReuseBias;
    job.wordiness = wordiness;
    job.status = JobStatus::Queued;
    m_jobs.insert(insertAt, job);
    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(startIndex);
}

void TranscriptionController::relabelAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                          bool restrictTagsToExisting, double tagReuseBias, double wordiness)
{
    for (const QVariant &v : fileUrls)
        relabel(v.toUrl(), vaultUrl, restrictTagsToExisting, tagReuseBias, wordiness);
}

void TranscriptionController::generateChapters(const QUrl &fileUrl, const QUrl &vaultUrl)
{
    // Same dedup rule as relabel() — see its doc comment.
    if (isQueued(fileUrl))
        return;

    // Item O15: see transcribe()'s identical use of frontOfQueueInsertIndex()
    // for why this jumps ahead of whatever's already Queued.
    int insertAt = frontOfQueueInsertIndex();
    const int startIndex = insertAt;
    // Chapters use the same label model as relabel() — see
    // LabelingEngine::generateChapters()'s doc comment.
    if (maybeQueueDownloadJob(DownloadTarget::LabelModel, vaultUrl, QString(), insertAt) >= 0)
        ++insertAt;

    Job job;
    job.id = m_nextJobId++;
    job.fileUrl = fileUrl;
    job.vaultUrl = vaultUrl;
    job.generateChaptersOnly = true;
    job.status = JobStatus::Queued;
    m_jobs.insert(insertAt, job);
    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(startIndex);
}

void TranscriptionController::generateChaptersAll(const QVariantList &fileUrls, const QUrl &vaultUrl)
{
    for (const QVariant &v : fileUrls)
        generateChapters(v.toUrl(), vaultUrl);
}

void TranscriptionController::startJob(int index)
{
    // Safety-net re-check, before any state mutation below — see
    // maybeRedirectForCombine()'s doc comment. A true return means it
    // already recursed onto whatever should run instead (a newly-inserted
    // combine job); this call contributes nothing further.
    if (maybeRedirectForCombine(index))
        return;

    m_activeIndex = index;
    Job &job = m_jobs[index];
    job.status = JobStatus::Active;
    job.error.clear();

    // Wave N (task system): a pause/cancel left over from whatever the
    // PREVIOUS job did must never leak into this one (see resetControl()'s
    // doc comment). Elapsed-time tracking (activeElapsedMs()) restarts from
    // zero for every new Active job.
    m_worker->resetControl();
    m_activePaused = false;
    emit activePausedChanged();
    m_activeAccumulatedMs = 0;
    m_activeSegmentTimer.start();
    m_overallProgress = 0.0;
    emit overallProgressChanged();
    // See recomputeOverallProgress()'s doc comment — only "transcribe" jobs
    // ever set this true (via job.speakerDetection below); every other kind
    // never emits phase() at all, so it's simply never read for them.
    m_activeSpeakerDetection = job.speakerDetection;
    m_phaseStartMs = 0;
    m_indeterminatePhaseTimer->stop();

    m_busy = true;
    emit busyChanged();
    m_progress = 0.0;
    emit progressChanged();
    // Cleared here (not just at the end of the previous job) so a
    // relabel/rename job — neither of which ever emits phase() — doesn't
    // inherit a stale "transcribing" (or whatever) left over from an
    // earlier transcribe job.
    m_phase.clear();
    emit currentPhaseChanged();
    m_currentFile = job.fileUrl;
    emit currentFileChanged();
    emit queueChanged();

    if (job.renameSpeakerOnly) {
        QMetaObject::invokeMethod(m_worker, "renameSpeaker", Qt::QueuedConnection,
                                   Q_ARG(QString, job.fileUrl.toLocalFile()), Q_ARG(QString, job.oldSpeakerName),
                                   Q_ARG(QString, job.newSpeakerName));
    } else if (job.mergeSpeakersOnly) {
        QMetaObject::invokeMethod(m_worker, "mergeSpeakers", Qt::QueuedConnection,
                                   Q_ARG(QString, job.fileUrl.toLocalFile()), Q_ARG(QString, job.sourceSpeakerName),
                                   Q_ARG(QString, job.targetSpeakerName));
    } else if (job.relabelOnly) {
        QMetaObject::invokeMethod(m_worker, "relabelJob", Qt::QueuedConnection,
                                   Q_ARG(QString, job.fileUrl.toLocalFile()),
                                   Q_ARG(QString, job.vaultUrl.toLocalFile()),
                                   Q_ARG(bool, job.restrictTagsToExisting), Q_ARG(double, job.tagReuseBias),
                                   Q_ARG(double, job.wordiness), Q_ARG(bool, job.skipIfAlreadyLabeled));
    } else if (job.generateChaptersOnly) {
        QMetaObject::invokeMethod(m_worker, "generateChaptersJob", Qt::QueuedConnection,
                                   Q_ARG(QString, job.fileUrl.toLocalFile()),
                                   Q_ARG(QString, job.vaultUrl.toLocalFile()));
    } else if (job.combineOnly) {
        QMetaObject::invokeMethod(m_worker, "combineJob", Qt::QueuedConnection,
                                   Q_ARG(QStringList, job.combineSourcePaths),
                                   Q_ARG(QString, job.fileUrl.toLocalFile()),
                                   Q_ARG(bool, job.deleteOriginalsAfterCombine));
    } else if (job.downloadTarget != DownloadTarget::None) {
        QString target;
        switch (job.downloadTarget) {
        case DownloadTarget::WhisperModel: target = QStringLiteral("model"); break;
        case DownloadTarget::LabelModel: target = QStringLiteral("label"); break;
        case DownloadTarget::DiarizationModels: target = QStringLiteral("diarization"); break;
        case DownloadTarget::None: break;
        }
        QMetaObject::invokeMethod(m_worker, "download", Qt::QueuedConnection,
                                   Q_ARG(QString, job.vaultUrl.toLocalFile()), Q_ARG(QString, target),
                                   Q_ARG(QString, job.modelId));
    } else {
        QMetaObject::invokeMethod(m_worker, "run", Qt::QueuedConnection,
                                   Q_ARG(QString, job.fileUrl.toLocalFile()),
                                   Q_ARG(QString, job.vaultUrl.toLocalFile()), Q_ARG(QString, job.modelId),
                                   Q_ARG(bool, job.speakerDetection));
    }
}

void TranscriptionController::onWorkerFinished(bool ok, const QString &error)
{
    m_busy = false;
    emit busyChanged();
    // A job that fails/gets cancelled mid-"loading"/"decoding" never gets
    // another phase() signal to naturally stop this — see recomputeOverallProgress()'s
    // doc comment.
    m_indeterminatePhaseTimer->stop();
    const QUrl finishedFile = m_currentFile;

    // See JobStatus::Paused's doc comment and togglePauseActive()'s
    // "loading" branch — same requestCancel()->finished(false, "Cancelled")
    // mechanism as a real cancel, distinguished only by this flag, which
    // togglePauseActive() sets right before triggering it.
    const bool wasStopping = m_pausingActiveAsRestart;
    const bool pausedAsRestart = m_pausingActiveAsRestart && !ok && error == QStringLiteral("Cancelled");
    m_pausingActiveAsRestart = false;
    if (wasStopping)
        emit activeStoppingChanged();

    if (m_activeIndex >= 0 && m_activeIndex < m_jobs.size()) {
        Job &job = m_jobs[m_activeIndex];
        if (pausedAsRestart) {
            job.status = JobStatus::Paused;
            job.error.clear();
        } else {
            job.status = ok ? JobStatus::Done : JobStatus::Failed;
            job.error = error;
            // Item 7 (Wave N): snapshot active-work elapsed time (excludes
            // any paused stretches — see activeElapsedMs()'s doc comment) as
            // this job's final duration, shown in the Tasks popup's history
            // rows. Left unset (-1) for a Paused job — it isn't finished.
            job.durationMs = activeElapsedMs();
        }
    }
    m_activeIndex = -1;
    // A paused job that gets cancelled resumes (see cancelJob()'s doc
    // comment) right before finishing — reset here too so a stray "Paused"
    // state never lingers once nothing is Active.
    m_activePaused = false;
    emit activePausedChanged();

    if (pausedAsRestart) {
        // Deliberately do NOT advance to the next Queued job — the Paused
        // job sits at the front until the user explicitly resumes it
        // (startNext() prioritizes any Paused job over Queued ones), same
        // as a persisted-but-not-yet-started job on vault open. A sibling
        // relabel job queued alongside this one (autoLabel) correctly stays
        // Queued behind it rather than running against a transcript that
        // was never written.
        m_currentFile = QUrl();
        emit currentFileChanged();
        emit queueChanged();
        QMetaObject::invokeMethod(m_worker, "evictIdle", Qt::QueuedConnection);
        emit finished(finishedFile, ok, error);
        return;
    }

    // A transcribe(autoLabel=true) call already queued its follow-up
    // "relabel" job UP FRONT, right alongside this one (see transcribe()'s
    // doc comment) — so it's just the next Queued entry here, exactly like
    // any other job a user queued ahead of time. No special-casing needed.
    int nextIndex = -1;
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].status == JobStatus::Queued) {
            nextIndex = i;
            break;
        }
    }

    if (nextIndex >= 0) {
        startJob(nextIndex);
    } else {
        m_currentFile = QUrl();
        emit currentFileChanged();
        emit queueChanged();
        // See TranscriptionWorker::evictIdle()'s doc comment — nothing left
        // queued to reuse a warm engine, so release whichever one the job
        // that just finished was using.
        QMetaObject::invokeMethod(m_worker, "evictIdle", Qt::QueuedConnection);
    }

    emit finished(finishedFile, ok, error);
}

void TranscriptionController::dismiss(const QUrl &fileUrl)
{
    // A fileUrl may have multiple entries (re-queueing a Done/Failed job is
    // allowed) — remove only the first Done/Failed match, skipping over any
    // Queued/Active entry for the same url rather than no-op'ing on it.
    for (int i = 0; i < m_jobs.size(); ++i) {
        const Job &job = m_jobs[i];
        if (job.fileUrl == fileUrl && (job.status == JobStatus::Done || job.status == JobStatus::Failed)) {
            m_jobs.removeAt(i);
            if (m_activeIndex > i)
                --m_activeIndex;
            emit queueChanged();
            return;
        }
    }
}

void TranscriptionController::clearFinished()
{
    bool changed = false;
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        if (m_jobs[i].status == JobStatus::Done || m_jobs[i].status == JobStatus::Failed) {
            m_jobs.removeAt(i);
            if (m_activeIndex > i)
                --m_activeIndex;
            changed = true;
        }
    }
    if (changed)
        emit queueChanged();
}

qint64 TranscriptionController::activeElapsedMs() const
{
    if (m_activeIndex < 0)
        return 0;
    return m_activeAccumulatedMs + (m_activePaused ? 0 : m_activeSegmentTimer.elapsed());
}

qint64 TranscriptionController::estimatedRemainingMs() const
{
    if (m_activeIndex < 0 || m_overallProgress <= 0.0)
        return -1;
    const double remainingFraction = (1.0 - m_overallProgress) / m_overallProgress;
    return static_cast<qint64>(activeElapsedMs() * remainingFraction);
}

void TranscriptionController::pauseActive()
{
    if (m_activeIndex < 0 || m_activeIndex >= m_jobs.size())
        return;
    // See this method's header doc comment — only these kinds have a real
    // interruption point. "combine" is the same real, killable/SIGSTOP-able
    // ffmpeg subprocess as "transcribe"'s "decoding" phase (see
    // TranscriptionWorker::combineJob()), just as its ENTIRE job rather than
    // one phase of it.
    const QString kind = jobKind(m_jobs[m_activeIndex]);
    if (kind != QStringLiteral("transcribe") && kind != QStringLiteral("download")
        && kind != QStringLiteral("combine"))
        return;
    // See this method's header doc comment — "loading" is whisper.cpp's
    // in-process, unhookable model-load call (no interruption point at all),
    // unlike "decoding" (a real, killable/SIGSTOP-able ffmpeg subprocess —
    // see AudioDecoder.h) or transcribing/diarization (their own cooperative
    // checkpoints) — queuing a pause during "loading" would only take effect
    // much later, confusingly. Irrelevant to "combine" (never emits a
    // "loading" phase at all).
    if (m_phase == QStringLiteral("loading"))
        return;
    // A soft-pause-as-restart from an earlier "loading" click on THIS SAME
    // job is still in flight (see activeStopping's doc comment) —
    // its cancel flag is already set on the worker and hasn't been observed
    // yet. Layering a fresh pause request on top would only get silently
    // suppressed by that pending cancel (waitWhilePausedOrCancelled()'s wait
    // loop never runs while cancelled), permanently breaking pause for this
    // job. Wait for it to resolve instead of stacking a second request.
    if (m_pausingActiveAsRestart)
        return;
    m_worker->requestPause();
}

void TranscriptionController::resumeActive()
{
    m_worker->requestResume();
}

void TranscriptionController::togglePauseActive()
{
    // See this method's header doc comment for the race it exists to close
    // — mirrors BottomBar.qml's old onTapped branching exactly, just reading
    // m_busy/m_activePaused/m_phase (this object's own state) instead of
    // QML's copies of them.
    if (!m_busy) {
        startNext();
        return;
    }
    if (m_activePaused) {
        resumeActive();
        return;
    }
    // A soft-pause-as-restart is already in flight and hasn't resolved yet
    // (see activeStopping's doc comment) — ignore further clicks rather than
    // stacking a second request on top of the still-pending cancel.
    if (m_pausingActiveAsRestart)
        return;
    // "loading" has no cooperative checkpoint to truly suspend AT (see
    // pauseActive()'s doc comment) — it's normally short, so instead of a
    // real pause this kills the current attempt and presents it to the user
    // as Paused (see JobStatus::Paused's doc comment), not Failed.
    // onWorkerFinished() reads this flag to tell that apart from an actual
    // cancel/failure using the exact same underlying mechanism. "decoding"
    // no longer needs this — it gets a real pause via pauseActive() below.
    if (m_phase == QStringLiteral("loading")) {
        m_pausingActiveAsRestart = true;
        emit activeStoppingChanged();
        cancelActive();
        return;
    }
    pauseActive();
}

void TranscriptionController::cancelActive()
{
    if (m_activeIndex < 0 || m_activeIndex >= m_jobs.size())
        return;
    cancelJob(m_jobs[m_activeIndex].id);
}

void TranscriptionController::cancelJob(int id)
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        Job &job = m_jobs[i];
        if (job.id != id)
            continue;
        // Paused (see JobStatus::Paused's doc comment) has nothing running
        // to interrupt — same removal as Queued.
        if (job.status == JobStatus::Queued || job.status == JobStatus::Paused) {
            m_jobs.removeAt(i);
            if (m_activeIndex > i)
                --m_activeIndex;
            emit queueChanged();
            return;
        }
        if (job.status == JobStatus::Active) {
            // requestResume() wakes it if currently blocked in a pause wait
            // — otherwise a paused-then-cancelled job would sit paused
            // forever instead of ever reaching the cancel check (see
            // waitWhilePausedOrCancelled()'s doc comment). See cancelJob()'s
            // header doc comment for which kinds this actually interrupts
            // immediately vs. at the next checkpoint.
            m_worker->requestCancel();
            m_worker->requestResume();
        }
        return;
    }
}

void TranscriptionController::cancelAll()
{
    bool changed = false;
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        if (m_jobs[i].status == JobStatus::Queued || m_jobs[i].status == JobStatus::Paused) {
            m_jobs.removeAt(i);
            if (m_activeIndex > i)
                --m_activeIndex;
            changed = true;
        }
    }
    if (changed)
        emit queueChanged();
    if (m_activeIndex >= 0) {
        m_worker->requestCancel();
        m_worker->requestResume();
    }
}

void TranscriptionController::moveQueuedJob(int id, int newIndex)
{
    int fromRealIndex = -1;
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].id == id && m_jobs[i].status == JobStatus::Queued) {
            fromRealIndex = i;
            break;
        }
    }
    if (fromRealIndex < 0)
        return;

    // Real (m_jobs) indices of every Queued entry, in current order — the
    // ordering moveQueuedJob()'s newIndex counts within (see its header doc
    // comment: Active/Done/Failed rows are never part of this ordering).
    QVector<int> queuedIndices;
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].status == JobStatus::Queued)
            queuedIndices.append(i);
    }
    const int fromQueuedPos = queuedIndices.indexOf(fromRealIndex);
    const int clampedNewIndex = std::clamp(newIndex, 0, static_cast<int>(queuedIndices.size()) - 1);
    if (clampedNewIndex == fromQueuedPos)
        return;

    const Job movedJob = m_jobs[fromRealIndex];
    m_jobs.removeAt(fromRealIndex);
    if (m_activeIndex > fromRealIndex)
        --m_activeIndex;

    // Recompute the remaining Queued real-indices after the removal shifted
    // everything past fromRealIndex down by one, then reinsert movedJob
    // right at the target Queued slot's (now-current) real index — or at
    // the very end of m_jobs if newIndex names the last Queued slot.
    queuedIndices.removeAt(fromQueuedPos);
    for (int &idx : queuedIndices) {
        if (idx > fromRealIndex)
            --idx;
    }
    const int insertRealIndex = clampedNewIndex >= queuedIndices.size() ? m_jobs.size() : queuedIndices[clampedNewIndex];
    m_jobs.insert(insertRealIndex, movedJob);
    if (m_activeIndex >= insertRealIndex)
        ++m_activeIndex;
    emit queueChanged();
}

bool TranscriptionController::isDownloadQueued(DownloadTarget target, const QUrl &vaultUrl,
                                                const QString &modelId) const
{
    for (const Job &job : m_jobs) {
        if (job.downloadTarget == target && job.vaultUrl == vaultUrl
            && (target != DownloadTarget::WhisperModel || job.modelId == modelId)
            && (job.status == JobStatus::Queued || job.status == JobStatus::Active))
            return true;
    }
    return false;
}

int TranscriptionController::maybeQueueDownloadJob(DownloadTarget target, const QUrl &vaultUrl,
                                                    const QString &modelId, int insertIndex)
{
    bool alreadyOnDisk = false;
    switch (target) {
    case DownloadTarget::WhisperModel:
        // An unrecognized modelId has no real file to pre-download — there
        // is nothing sensible to queue (ensureModel()'s own fallback-to-
        // default logic, run inline when the transcribe job actually
        // starts, is what actually resolves this case; see ModelManager's
        // class doc comment). Queuing a phantom download job here would
        // silently redirect to the default model under a job label that
        // still says the unrecognized id.
        if (ModelManager::modelFileName(modelId).isEmpty())
            return -1;
        alreadyOnDisk = ModelManager::isModelDownloaded(vaultUrl, modelId);
        break;
    case DownloadTarget::LabelModel:
        alreadyOnDisk = ModelManager::isLabelModelDownloaded(vaultUrl);
        break;
    case DownloadTarget::DiarizationModels:
        alreadyOnDisk = ModelManager::areDiarizationModelsDownloaded(vaultUrl);
        break;
    case DownloadTarget::None:
        return -1;
    }
    if (alreadyOnDisk || isDownloadQueued(target, vaultUrl, modelId))
        return -1;

    Job job;
    job.id = m_nextJobId++;
    job.vaultUrl = vaultUrl;
    job.modelId = modelId; // only meaningful for WhisperModel
    job.downloadTarget = target;
    job.status = JobStatus::Queued;
    const int at = (insertIndex < 0) ? m_jobs.size() : std::clamp(insertIndex, 0, static_cast<int>(m_jobs.size()));
    m_jobs.insert(at, job);
    return job.id;
}

// See this method's header doc comment.
int TranscriptionController::frontOfQueueInsertIndex() const
{
    if (m_activeIndex >= 0)
        return m_activeIndex + 1;
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].status == JobStatus::Queued)
            return i;
    }
    return m_jobs.size();
}

// See this method's header doc comment for the full matching/grouping/status
// semantics.
QList<TranscriptionController::CombineGroup>
TranscriptionController::findCombineGroups(const QVariantList &rules, const QStringList &audioPaths)
{
    struct Candidate {
        QString path;
        QString ruleName;
        QString key;
        // Same capture groups as `key`, un-concatenated — every member of a
        // group shares identical values here by construction (that's what
        // makes them a group), so the group only needs one member's copy.
        QStringList capturedValues;
        // The rule's designated sort differentiator's captured text, if the
        // rule has one AND it participated in this match — see the sort
        // logic below for how this overrides the mtime default. A rule with
        // no sort differentiator configured (the common case) leaves every
        // candidate's sortValue empty (std::nullopt).
        std::optional<QString> sortValue;
        // Default combine-order signal, and always the stability-window
        // signal below regardless of sort order: deliberately mtime, NOT
        // file creation/birth time. Creation time would seem more semantically
        // "correct" (it's when the recorder started this part), but it
        // doesn't survive a plain file copy on Linux — cp/rsync/a
        // cross-filesystem mv all stamp a fresh birth time on the
        // destination, while mtime is preserved by every standard copy tool.
        // Since this app doesn't control (and shouldn't assume anything
        // about) how audio lands in the vault — direct recording onto a
        // mounted vault path vs. copied off an SD card vs. synced some other
        // way — mtime is the only signal that's reliably correct across all
        // of them.
        QDateTime mtime;
    };

    QList<Candidate> candidates;
    for (const QVariant &ruleVar : rules) {
        const QVariantMap rule = ruleVar.toMap();
        const QString ruleName = rule.value(QStringLiteral("name")).toString();
        for (const QString &path : audioPaths) {
            const QFileInfo info(path);
            QStringList capturedValues;
            std::optional<QString> sortValue;
            if (const auto key = matchRuleCapturedKey(rule, info.fileName(), &capturedValues, &sortValue))
                candidates.append(Candidate{path, ruleName, *key, capturedValues, sortValue, info.lastModified()});
        }
    }

    QMap<QPair<QString, QString>, QList<Candidate>> buckets;
    for (const Candidate &c : candidates)
        buckets[qMakePair(c.ruleName, c.key)].append(c);

    const QDateTime now = QDateTime::currentDateTime();
    QList<CombineGroup> groups;
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it) {
        QList<Candidate> members = it.value();

        // Combine order: the rule's sort differentiator (numeric-aware if
        // every member's captured value parses as a number, else plain
        // string comparison), if configured and every member actually
        // captured a value for it; otherwise mtime ascending (see
        // Candidate::mtime's doc comment for why mtime, not creation time).
        // "Numeric" is decided once for the whole group, not per pair — a
        // comparator that's numeric for some pairs and lexicographic for
        // others isn't a strict weak ordering, which is undefined behavior
        // for std::sort.
        const bool allHaveSortValue = std::all_of(members.begin(), members.end(),
                                                   [](const Candidate &c) { return c.sortValue.has_value(); });
        if (allHaveSortValue) {
            const bool allNumeric = std::all_of(members.begin(), members.end(), [](const Candidate &c) {
                bool ok = false;
                c.sortValue->toDouble(&ok);
                return ok;
            });
            if (allNumeric) {
                std::sort(members.begin(), members.end(), [](const Candidate &a, const Candidate &b) {
                    return a.sortValue->toDouble() < b.sortValue->toDouble();
                });
            } else {
                std::sort(members.begin(), members.end(),
                          [](const Candidate &a, const Candidate &b) { return *a.sortValue < *b.sortValue; });
            }
        } else {
            std::sort(members.begin(), members.end(),
                      [](const Candidate &a, const Candidate &b) { return a.mtime < b.mtime; });
        }

        CombineGroup group;
        group.ruleName = it.key().first;
        group.key = it.key().second;
        group.capturedValues = members.first().capturedValues;
        for (const Candidate &c : members)
            group.files << c.path;

        const QFileInfo firstInfo(members.first().path);
        group.outputPath =
            combineOutputPathFor(group.ruleName, group.key, firstInfo.absolutePath(), firstInfo.suffix());

        if (members.size() < 2) {
            group.status = QStringLiteral("singleton");
        } else if (QFileInfo::exists(group.outputPath)) {
            group.status = QStringLiteral("alreadyCombined");
        } else {
            bool anyTranscribed = false;
            for (const Candidate &c : members) {
                if (QFileInfo::exists(c.path + QStringLiteral(".md"))) {
                    anyTranscribed = true;
                    break;
                }
            }
            // Independent of whatever order `members` ended up sorted into
            // above (recording-order sort ≠ mtime order once a sort
            // differentiator is configured) — this check is genuinely about
            // "is the recorder still actively writing the newest part," so
            // it always needs the TRUE newest mtime, not just members.last().
            const auto newest = std::max_element(members.begin(), members.end(),
                                                  [](const Candidate &a, const Candidate &b) { return a.mtime < b.mtime; });
            if (anyTranscribed)
                group.status = QStringLiteral("alreadyTranscribed");
            else if (newest->mtime.secsTo(now) < kCombineStabilityWindowSecs)
                group.status = QStringLiteral("pending");
            else
                group.status = QStringLiteral("ready");
        }
        groups.append(group);
    }

    // Deterministic ordering — callers (tests, the Settings preview) don't
    // have to guess QMap's iteration order.
    std::sort(groups.begin(), groups.end(), [](const CombineGroup &a, const CombineGroup &b) {
        if (a.ruleName != b.ruleName)
            return a.ruleName < b.ruleName;
        return a.key < b.key;
    });
    return groups;
}

QVariantList TranscriptionController::detectCombineGroups(const QVariantList &rules,
                                                            const QStringList &audioPaths) const
{
    QVariantList result;
    for (const CombineGroup &group : findCombineGroups(rules, audioPaths)) {
        QVariantMap entry;
        entry[QStringLiteral("ruleName")] = group.ruleName;
        entry[QStringLiteral("key")] = group.key;
        entry[QStringLiteral("capturedValues")] = group.capturedValues;
        entry[QStringLiteral("files")] = group.files;
        entry[QStringLiteral("outputPath")] = group.outputPath;
        entry[QStringLiteral("status")] = group.status;
        result << entry;
    }
    return result;
}

QString TranscriptionController::regexValidationError(const QString &pattern) const
{
    if (pattern.isEmpty())
        return QString();
    const QRegularExpression re(pattern);
    return re.isValid() ? QString() : re.errorString();
}

QStringList TranscriptionController::differentiatorNonParticipatingFiles(const QString &pattern, int groupIndex,
                                                                           const QStringList &audioPaths) const
{
    QStringList offending;
    const QRegularExpression re(pattern);
    if (!re.isValid() || groupIndex < 1 || groupIndex > re.captureCount())
        return offending;
    for (const QString &path : audioPaths) {
        const QFileInfo info(path);
        const QRegularExpressionMatch m = re.match(info.fileName());
        if (m.hasMatch() && m.capturedStart(groupIndex) < 0)
            offending << info.fileName();
    }
    return offending;
}

bool TranscriptionController::isCombineQueued(const QString &outputPath) const
{
    const QUrl outputUrl = QUrl::fromLocalFile(outputPath);
    for (const Job &job : m_jobs) {
        if (job.combineOnly && job.fileUrl == outputUrl
            && (job.status == JobStatus::Queued || job.status == JobStatus::Active))
            return true;
    }
    return false;
}

void TranscriptionController::enqueueCombineJob(const CombineGroup &group, const QUrl &vaultUrl,
                                                 bool deleteOriginals, int insertIndex)
{
    Job job;
    job.id = m_nextJobId++;
    job.fileUrl = QUrl::fromLocalFile(group.outputPath);
    job.vaultUrl = vaultUrl;
    job.combineOnly = true;
    job.combineSourcePaths = group.files;
    job.deleteOriginalsAfterCombine = deleteOriginals;
    job.status = JobStatus::Queued;
    const int at = (insertIndex < 0) ? frontOfQueueInsertIndex() : insertIndex;
    m_jobs.insert(std::clamp(at, 0, static_cast<int>(m_jobs.size())), job);
}

// See this method's header doc comment.
bool TranscriptionController::maybeRedirectForCombine(int index)
{
    Job &job = m_jobs[index];
    if (job.combineOnly || job.relabelOnly || job.generateChaptersOnly || job.renameSpeakerOnly
        || job.mergeSpeakersOnly || job.downloadTarget != DownloadTarget::None || job.fileUrl.isEmpty())
        return false;

    const QFileInfo info(job.fileUrl.toLocalFile());

    // Outcome 1: an earlier manual runCombineScan() run may already have
    // merged this exact file's group (and moved the original fragment into
    // .combined/) since this job was queued. The merged output's path is
    // recomputable from this filename alone (see findCombineGroups()'s
    // outputPath doc comment), so check for it directly.
    for (const QVariant &ruleVar : m_combineRules) {
        const QVariantMap rule = ruleVar.toMap();
        const auto key = matchRuleCapturedKey(rule, info.fileName());
        if (!key)
            continue;
        const QString outputPath = combineOutputPathFor(rule.value(QStringLiteral("name")).toString(), *key,
                                                          info.absolutePath(), info.suffix());
        if (QFileInfo::exists(outputPath))
            job.fileUrl = QUrl::fromLocalFile(outputPath);
        break; // only the first matching rule is used, mirroring findCombineGroups()
    }

    // Outcome 2: this file is a member of a currently-"ready" group per the
    // last runCombineScan() scan that hasn't been merged yet — insert the
    // combine job ahead of this one and redirect, same insert-ahead shape
    // maybeQueueDownloadJob() already uses for model downloads.
    const QString filePath = job.fileUrl.toLocalFile();
    for (const CombineGroup &group : findCombineGroups(m_combineRules, m_combineAudioPaths)) {
        if (group.status != QStringLiteral("ready") || !group.files.contains(filePath))
            continue;
        job.fileUrl = QUrl::fromLocalFile(group.outputPath);
        if (!isCombineQueued(group.outputPath))
            enqueueCombineJob(group, job.vaultUrl, m_combineDeleteOriginals, index);
        emit queueChanged();
        // Recurses onto the freshly-inserted combine job at this same index
        // (safe — see this method's header doc comment). If a combine job
        // for this group was somehow already queued elsewhere (defensive
        // only: this app dispatches one job at a time via front-to-back
        // scans, and a front-inserted combine job is always reached before
        // a transcribe job queued behind it — see enqueueCombineJob()'s
        // insertIndex), nothing was inserted at `index` here, so this
        // recurses onto the SAME transcribe job; its redirected fileUrl
        // still won't exist yet and it fails cleanly via the normal
        // "file not found"-shaped decode error rather than looping.
        startJob(index);
        return true;
    }
    return false;
}

void TranscriptionController::runCombineScan(const QVariantList &rules, const QStringList &audioPaths,
                                               const QUrl &vaultUrl, bool deleteOriginals)
{
    // Cached for maybeRedirectForCombine()'s per-job safety check — see its
    // doc comment and the m_combineRules/m_combineAudioPaths field comments.
    m_combineRules = rules;
    m_combineAudioPaths = audioPaths;
    m_combineDeleteOriginals = deleteOriginals;

    bool inserted = false;
    for (const CombineGroup &group : findCombineGroups(rules, audioPaths)) {
        if (group.status != QStringLiteral("ready") || isCombineQueued(group.outputPath))
            continue;
        enqueueCombineJob(group, vaultUrl, deleteOriginals);
        inserted = true;
    }
    if (!inserted)
        return;
    emit queueChanged();
    // Live trigger (like transcribe()), not a persisted-queue replay — auto-
    // starting when idle matches transcribe()'s own behavior, unlike
    // loadPersistedQueue()'s deliberately-not-auto-started restored jobs
    // (see startNext()'s doc comment).
    if (m_activeIndex < 0)
        startNext();
}

// Sidebar's per-row "combine this group" hover button (item 51): same shape
// as runCombineScan() above but filtered to the ONE group whose outputPath
// matches — a deliberate single-group action, not "scan and enqueue
// everything ready" the ribbon button performs. Silently no-ops if that
// group is no longer "ready" (e.g. a concurrent edit/scan already changed
// its status) or already queued — the sidebar only shows this button for a
// row it just computed as "ready" via detectCombineGroups(), but that
// snapshot can go stale between hover and click just as easily as
// runCombineScan()'s own dedup check already accounts for.
void TranscriptionController::combineGroup(const QVariantList &rules, const QStringList &audioPaths,
                                            const QString &outputPath, const QUrl &vaultUrl, bool deleteOriginals)
{
    m_combineRules = rules;
    m_combineAudioPaths = audioPaths;
    m_combineDeleteOriginals = deleteOriginals;

    for (const CombineGroup &group : findCombineGroups(rules, audioPaths)) {
        if (group.outputPath != outputPath)
            continue;
        if (group.status != QStringLiteral("ready") || isCombineQueued(group.outputPath))
            return;
        enqueueCombineJob(group, vaultUrl, deleteOriginals);
        emit queueChanged();
        if (m_activeIndex < 0)
            startNext();
        return;
    }
}

QString TranscriptionController::tasksJsonPath(const QUrl &vaultUrl)
{
    return vaultUrl.toLocalFile() + QStringLiteral("/.transcriber/tasks.json");
}

void TranscriptionController::persistQueue() const
{
    if (m_vaultUrl.toString().isEmpty())
        return;

    QJsonArray array;
    for (const Job &job : m_jobs) {
        // Only Queued survives a restart — an Active OR Paused job is
        // persisted AS Queued (no true mid-file resume exists yet, see
        // loadPersistedQueue()'s doc comment); Done/Failed history is
        // already session-only by design (see class doc comment) and isn't
        // persisted here either.
        if (job.status != JobStatus::Queued && job.status != JobStatus::Active && job.status != JobStatus::Paused)
            continue;

        QJsonObject obj;
        obj["kind"] = jobKind(job);
        obj["fileUrl"] = job.fileUrl.toString();
        obj["vaultUrl"] = job.vaultUrl.toString();
        obj["modelId"] = job.modelId;
        obj["speakerDetection"] = job.speakerDetection;
        obj["restrictTagsToExisting"] = job.restrictTagsToExisting;
        obj["tagReuseBias"] = job.tagReuseBias;
        obj["wordiness"] = job.wordiness;
        obj["oldSpeakerName"] = job.oldSpeakerName;
        obj["newSpeakerName"] = job.newSpeakerName;
        obj["sourceSpeakerName"] = job.sourceSpeakerName;
        obj["targetSpeakerName"] = job.targetSpeakerName;
        obj["combineSourcePaths"] = QJsonArray::fromStringList(job.combineSourcePaths);
        obj["deleteOriginalsAfterCombine"] = job.deleteOriginalsAfterCombine;
        switch (job.downloadTarget) {
        case DownloadTarget::WhisperModel: obj["downloadTarget"] = QStringLiteral("model"); break;
        case DownloadTarget::LabelModel: obj["downloadTarget"] = QStringLiteral("label"); break;
        case DownloadTarget::DiarizationModels: obj["downloadTarget"] = QStringLiteral("diarization"); break;
        case DownloadTarget::None: break;
        }
        array.append(obj);
    }

    QFile file(tasksJsonPath(m_vaultUrl));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
}

void TranscriptionController::loadPersistedQueue(const QUrl &vaultUrl)
{
    m_vaultUrl = vaultUrl;

    QFile file(tasksJsonPath(vaultUrl));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return;

    // Replayed in file order, which is the order persistQueue() wrote
    // m_jobs in — a download job a transcribe/relabel entry depends on was
    // always written just BEFORE it (see transcribe()/relabel()'s
    // maybeQueueDownloadJob() calls), so replaying in order queues each
    // download ahead of its dependent job here too, and that dependent
    // job's OWN maybeQueueDownloadJob() call sees it already queued
    // (isDownloadQueued()) and doesn't duplicate it.
    //
    // Every non-download entry goes through enqueueReplayedJob() — NOT the
    // public transcribe()/relabel()/renameSpeaker()/mergeSpeakers()
    // functions — precisely so a "transcribe" entry immediately followed by
    // a "relabel" entry for the SAME fileUrl (an autoLabel job) replays
    // both: calling relabel() directly here would see the transcribe entry
    // already Queued/Active for that fileUrl and silently reject it via its
    // own isQueued() dedup guard (see enqueueReplayedJob()'s doc comment).
    //
    // Deliberately does NOT auto-start anything (see loadPersistedQueue()'s
    // doc comment) — every restored entry, download jobs included, just
    // sits Queued until the user resumes via startNext().
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject obj = v.toObject();
        const QString kind = obj.value("kind").toString();

        if (kind == QStringLiteral("download")) {
            const QString target = obj.value("downloadTarget").toString();
            DownloadTarget dt = DownloadTarget::None;
            if (target == QStringLiteral("model"))
                dt = DownloadTarget::WhisperModel;
            else if (target == QStringLiteral("label"))
                dt = DownloadTarget::LabelModel;
            else if (target == QStringLiteral("diarization"))
                dt = DownloadTarget::DiarizationModels;
            const QUrl jobVaultUrl(obj.value("vaultUrl").toString());
            if (maybeQueueDownloadJob(dt, jobVaultUrl, obj.value("modelId").toString()) >= 0)
                emit queueChanged();
        } else {
            enqueueReplayedJob(kind, obj);
        }
    }
}

void TranscriptionController::enqueueReplayedJob(const QString &kind, const QJsonObject &obj)
{
    const QUrl fileUrl(obj.value("fileUrl").toString());
    const QUrl jobVaultUrl(obj.value("vaultUrl").toString());

    if (kind == QStringLiteral("relabel") || kind == QStringLiteral("chapters")) {
        // Chapters use the same label model as relabel — see
        // LabelingEngine::generateChapters()'s doc comment.
        maybeQueueDownloadJob(DownloadTarget::LabelModel, jobVaultUrl, QString());
    } else if (kind != QStringLiteral("rename") && kind != QStringLiteral("merge")
               && kind != QStringLiteral("combine")) {
        // "transcribe"
        const QString modelId = obj.value("modelId").toString();
        maybeQueueDownloadJob(DownloadTarget::WhisperModel, jobVaultUrl, modelId);
        if (obj.value("speakerDetection").toBool())
            maybeQueueDownloadJob(DownloadTarget::DiarizationModels, jobVaultUrl, QString());
    }

    Job job;
    job.id = m_nextJobId++;
    job.fileUrl = fileUrl;
    job.vaultUrl = jobVaultUrl;
    if (kind == QStringLiteral("relabel")) {
        job.relabelOnly = true;
        job.restrictTagsToExisting = obj.value("restrictTagsToExisting").toBool();
        job.tagReuseBias = obj.value("tagReuseBias").toDouble(0.5);
        job.wordiness = obj.value("wordiness").toDouble(0.5);
    } else if (kind == QStringLiteral("rename")) {
        job.renameSpeakerOnly = true;
        job.oldSpeakerName = obj.value("oldSpeakerName").toString();
        job.newSpeakerName = obj.value("newSpeakerName").toString();
    } else if (kind == QStringLiteral("merge")) {
        job.mergeSpeakersOnly = true;
        job.sourceSpeakerName = obj.value("sourceSpeakerName").toString();
        job.targetSpeakerName = obj.value("targetSpeakerName").toString();
    } else if (kind == QStringLiteral("chapters")) {
        job.generateChaptersOnly = true;
    } else if (kind == QStringLiteral("combine")) {
        job.combineOnly = true;
        for (const QJsonValue &v : obj.value("combineSourcePaths").toArray())
            job.combineSourcePaths << v.toString();
        job.deleteOriginalsAfterCombine = obj.value("deleteOriginalsAfterCombine").toBool();
    } else {
        job.modelId = obj.value("modelId").toString();
        job.speakerDetection = obj.value("speakerDetection").toBool();
    }
    job.status = JobStatus::Queued;
    m_jobs.push_back(job);
    emit queueChanged();
}

void TranscriptionController::startNext()
{
    if (m_activeIndex >= 0)
        return;
    // A Paused job (see JobStatus::Paused's doc comment) is what the user
    // just clicked "resume" on — it takes priority over unrelated Queued
    // jobs regardless of list position; there's at most one at a time.
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].status == JobStatus::Paused) {
            startJob(i);
            return;
        }
    }
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].status == JobStatus::Queued) {
            startJob(i);
            return;
        }
    }
}

void TranscriptionController::terminateApplicationImmediately() const
{
    // See this method's header doc comment for why this is a hard process
    // exit rather than a graceful QCoreApplication::quit() — ::_exit()
    // (POSIX, unistd.h — NOT std::exit()/std::quick_exit(), which still run
    // atexit/global-destructor cleanup) skips every remaining C++
    // destructor (including this object's own, and the worker QThread's)
    // and every bit of Qt event-loop teardown, ending the process instantly
    // regardless of what the worker thread happens to be doing at the time.
    ::_exit(0);
}

QVariantMap TranscriptionController::readMetadata(const QUrl &fileUrl) const
{
    // Item P23: see m_transcriptCacheMutex's doc comment — this function is
    // now also called from loadTranscriptAsync()'s background thread.
    QMutexLocker locker(&m_transcriptCacheMutex);

    const QString mdPath = sidecarPath(fileUrl.toLocalFile());
    const QFileInfo fi(mdPath);
    if (!fi.exists())
        return QVariantMap();

    // Item O19: a cheap stat guard — frontmatter is written once up front
    // and only touched again at a job's very end (see
    // syncSpeakersFrontmatterToBody()/writeLabelingFields()), so on the
    // common "nothing changed since last call" tick this skips the file
    // open/read/parse entirely. See m_frontmatterCache's doc comment.
    const qint64 size = fi.size();
    const QDateTime mtime = fi.lastModified();
    if (m_frontmatterCache.valid && m_frontmatterCache.mdPath == mdPath && m_frontmatterCache.size == size
        && m_frontmatterCache.mtime == mtime)
        return m_frontmatterCache.data;

    QVariantMap result;
    QFile mdFile(mdPath);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return result;

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---"))
        return result;

    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        if (value == QStringLiteral("true"))
            result[key] = true;
        else if (value == QStringLiteral("false"))
            result[key] = false;
        else
            result[key] = value;
    }

    m_frontmatterCache.mdPath = mdPath;
    m_frontmatterCache.size = size;
    m_frontmatterCache.mtime = mtime;
    m_frontmatterCache.data = result;
    m_frontmatterCache.valid = true;
    return result;
}

bool TranscriptionController::setLabel(const QUrl &fileUrl, const QString &label, QString *errorOut) const
{
    const QFileInfo fi(fileUrl.toLocalFile());
    const QString path = sidecarPath(fileUrl.toLocalFile());

    QFile mdFile(path);
    if (!mdFile.exists()) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has not been transcribed yet").arg(fi.fileName());
        return false;
    }

    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for reading").arg(path);
        return false;
    }

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---")) {
        mdFile.close();
        if (errorOut)
            *errorOut = QStringLiteral("%1 has no frontmatter block").arg(mdFile.fileName());
        return false;
    }

    // Mirror readMetadata()'s frontmatter-scanning shape, but keep the raw
    // lines (not parsed key/value pairs) so we can rewrite the block without
    // reformatting keys we don't care about.
    QStringList frontmatterLines;
    QString line;
    bool closed = false;
    while (!(line = in.readLine()).isNull()) {
        if (line == QStringLiteral("---")) {
            closed = true;
            break;
        }
        frontmatterLines.append(line);
    }
    if (!closed) {
        mdFile.close();
        if (errorOut)
            *errorOut = QStringLiteral("%1 has a malformed frontmatter block").arg(mdFile.fileName());
        return false;
    }

    // Everything after the closing "---" line, byte-for-byte — including
    // its leading blank line (writeFrontmatter() emits "---\n\n" before the
    // first segment line) — so the transcript body is left untouched.
    const QString body = in.readAll();
    mdFile.close();

    bool replaced = false;
    for (QString &fmLine : frontmatterLines) {
        const int colon = fmLine.indexOf(':');
        if (colon >= 0 && fmLine.left(colon).trimmed() == QStringLiteral("label")) {
            fmLine = QStringLiteral("label: ") + label;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        frontmatterLines.append(QStringLiteral("label: ") + label);

    if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(mdFile.fileName());
        return false;
    }
    QTextStream out(&mdFile);
    out << "---\n";
    for (const QString &fmLine : frontmatterLines)
        out << fmLine << '\n';
    out << "---\n";
    out << body;
    out.flush();
    mdFile.flush();
    mdFile.close();
    // Item P19: this manual (non-queued) rewrite otherwise leaves the
    // sidebar's cached per-row metadata stale until an unrelated job
    // finishes or a full VaultTreeModel rebuild happens — see finished()'s
    // doc comment.
    emit finished(fileUrl, true, QString());
    return true;
}

bool TranscriptionController::setTags(const QUrl &fileUrl, const QStringList &tags, QString *errorOut) const
{
    const QFileInfo fi(fileUrl.toLocalFile());
    const QString path = sidecarPath(fileUrl.toLocalFile());

    QFile mdFile(path);
    if (!mdFile.exists()) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has not been transcribed yet").arg(fi.fileName());
        return false;
    }

    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for reading").arg(path);
        return false;
    }

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---")) {
        mdFile.close();
        if (errorOut)
            *errorOut = QStringLiteral("%1 has no frontmatter block").arg(mdFile.fileName());
        return false;
    }

    // Mirror setLabel()'s frontmatter-scanning shape.
    QStringList frontmatterLines;
    QString line;
    bool closed = false;
    while (!(line = in.readLine()).isNull()) {
        if (line == QStringLiteral("---")) {
            closed = true;
            break;
        }
        frontmatterLines.append(line);
    }
    if (!closed) {
        mdFile.close();
        if (errorOut)
            *errorOut = QStringLiteral("%1 has a malformed frontmatter block").arg(mdFile.fileName());
        return false;
    }

    const QString body = in.readAll();
    mdFile.close();

    const QString joined = tags.join(QStringLiteral(", "));
    bool replaced = false;
    for (QString &fmLine : frontmatterLines) {
        const int colon = fmLine.indexOf(':');
        if (colon >= 0 && fmLine.left(colon).trimmed() == QStringLiteral("tags")) {
            fmLine = QStringLiteral("tags: ") + joined;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        frontmatterLines.append(QStringLiteral("tags: ") + joined);

    if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(mdFile.fileName());
        return false;
    }
    QTextStream out(&mdFile);
    out << "---\n";
    for (const QString &fmLine : frontmatterLines)
        out << fmLine << '\n';
    out << "---\n";
    out << body;
    out.flush();
    mdFile.flush();
    mdFile.close();
    // Item P19-style notification: sidebar's tag-derived display (if any)
    // should refresh the same way a label edit does.
    emit finished(fileUrl, true, QString());
    return true;
}

// Manual-edit counterpart for segment text (MainPanel's always-editable
// transcriptArea) — reuses parseSpeakerTaggedFile()/writeFrontmatterBlock()
// (originally written for renameSpeakerInFile()/mergeSpeakersInFile() above)
// since a single-segment text replace needs exactly the same "locate every
// segment line via segmentLinePattern(knownSpeakers), splice by captured
// offset, rewrite the whole file" shape — just targeting the TEXT capture
// group (3) per requested t0 instead of the speaker group (2).
bool TranscriptionController::setSentenceTexts(const QUrl &fileUrl, const QVariantList &edits, QString *errorOut) const
{
    const QString path = sidecarPath(fileUrl.toLocalFile());
    QStringList frontmatterLines;
    QString body;
    int speakersLineIndex = -1;
    QStringList knownSpeakers;
    QStringList distinctTags;
    if (!parseSpeakerTaggedFile(path, &frontmatterLines, &body, &speakersLineIndex, &knownSpeakers, &distinctTags,
                                 errorOut))
        return false;

    QVector<QRegularExpressionMatch> matches;
    QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatch(body);
    while (it.hasNext())
        matches.append(it.next());

    struct PendingEdit {
        qsizetype start;
        qsizetype length;
        QString text;
    };
    QVector<PendingEdit> pending;
    pending.reserve(edits.size());

    for (const QVariant &editVar : edits) {
        const QVariantMap edit = editVar.toMap();
        const double t0 = edit.value(QStringLiteral("t0")).toDouble();
        QString newText = edit.value(QStringLiteral("text")).toString();
        // A segment is exactly one body line by construction (see
        // MainPanel.qml's displayData doc comment) — a stray newline here
        // would corrupt that invariant, so collapse defensively rather than
        // trusting the caller (QML already blocks Return/strips pasted
        // newlines, but this is the actual safety boundary).
        newText.replace(QLatin1Char('\n'), QLatin1Char(' '));

        bool found = false;
        for (const QRegularExpressionMatch &m : std::as_const(matches)) {
            if (std::abs(parseCompactTimestamp(m.captured(1)) - t0) < 0.001) {
                pending.append({m.capturedStart(3), m.capturedLength(3), newText});
                found = true;
                break;
            }
        }
        if (!found) {
            if (errorOut)
                *errorOut = QStringLiteral("no segment found at timestamp %1").arg(formatTimestamp(t0));
            return false;
        }
    }

    // Splice from the END of the body backward so earlier edits' offsets
    // stay valid as later ones are replaced (same technique as
    // renameSpeakerInFile()/mergeSpeakersInFile() above).
    std::sort(pending.begin(), pending.end(),
              [](const PendingEdit &a, const PendingEdit &b) { return a.start > b.start; });
    for (const PendingEdit &edit : std::as_const(pending))
        body.replace(edit.start, edit.length, edit.text);

    if (!writeFrontmatterBlock(path, frontmatterLines, body, errorOut))
        return false;
    emit finished(fileUrl, true, QString());
    return true;
}

QString TranscriptionController::readPrelude(const QUrl &fileUrl) const
{
    // Builds on readTranscriptBody() rather than re-skipping frontmatter by
    // hand — that call is already cached behind a size+mtime check (see its
    // own doc comment), so this stays cheap on repeated calls, and correctly
    // invalidates when only the body (not frontmatter) changes, which is
    // exactly the case a manual prelude edit or a relabel produces.
    const QString body = readTranscriptBody(fileUrl);
    const int markerIdx = body.indexOf(kPreludeEndMarker);
    if (markerIdx < 0)
        return QString();
    return body.left(markerIdx).trimmed();
}

bool TranscriptionController::setPrelude(const QUrl &fileUrl, const QString &prelude, QString *errorOut) const
{
    const QFileInfo fi(fileUrl.toLocalFile());
    const QString path = sidecarPath(fileUrl.toLocalFile());

    QFile mdFile(path);
    if (!mdFile.exists()) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has not been transcribed yet").arg(fi.fileName());
        return false;
    }

    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for reading").arg(path);
        return false;
    }

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---")) {
        mdFile.close();
        if (errorOut)
            *errorOut = QStringLiteral("%1 has no frontmatter block").arg(mdFile.fileName());
        return false;
    }

    // Mirror setLabel()'s frontmatter-scanning shape, but this method never
    // touches a frontmatter key — the raw lines are only carried through
    // untouched to the rewrite below.
    QStringList frontmatterLines;
    QString line;
    bool closed = false;
    while (!(line = in.readLine()).isNull()) {
        if (line == QStringLiteral("---")) {
            closed = true;
            break;
        }
        frontmatterLines.append(line);
    }
    if (!closed) {
        mdFile.close();
        if (errorOut)
            *errorOut = QStringLiteral("%1 has a malformed frontmatter block").arg(mdFile.fileName());
        return false;
    }

    const QString body = in.readAll();
    mdFile.close();

    const QString newBody = spliceBodyPrelude(body, prelude);

    if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(mdFile.fileName());
        return false;
    }
    QTextStream out(&mdFile);
    out << "---\n";
    for (const QString &fmLine : frontmatterLines)
        out << fmLine << '\n';
    out << "---\n";
    out << newBody;
    out.flush();
    mdFile.flush();
    mdFile.close();
    // Item P19: see setLabel()'s matching emit for why.
    emit finished(fileUrl, true, QString());
    return true;
}

QVariantList TranscriptionController::readChapters(const QUrl &fileUrl) const
{
    // stripPreludeIfPresent isn't load-bearing here — "## " can never
    // collide with the prelude's own kPreludeEndMarker sentinel or with
    // segmentLinePattern()'s "^\[" anchor, unlike the prelude's own
    // numbered-list false-positive risk (see stripPreludeIfPresent()'s doc
    // comment). Kept anyway purely for consistency with every other body
    // scanner in this file (parseSentences(), plainTextFromTranscriptBody())
    // — belt-and-suspenders, not correctness-required.
    const QString body = stripPreludeIfPresent(readTranscriptBody(fileUrl));

    QVariantList chapters;
    QRegularExpressionMatchIterator it = chapterMarkerPattern().globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QVariantMap chapter;
        chapter["t0"] = parseCompactTimestamp(m.captured(1));
        chapter["title"] = m.captured(2);
        chapters.append(chapter);
    }
    return chapters;
}

void TranscriptionController::renameSpeaker(const QUrl &fileUrl, const QString &oldName, const QString &newName)
{
    // Same dedup rule as transcribe()/relabel() — but unlike those, a caller
    // here is actively waiting on speakerRenamed() for a definitive result
    // (see MainPanel.qml's pendingSpeakerRenames), so silently dropping the
    // request would leave its optimistic UI stuck forever. Report it instead.
    if (isQueued(fileUrl)) {
        emit speakerRenamed(fileUrl, oldName, false,
                             QStringLiteral("another task is already running for this file"));
        return;
    }

    Job job;
    job.id = m_nextJobId++;
    job.fileUrl = fileUrl;
    job.renameSpeakerOnly = true;
    job.oldSpeakerName = oldName;
    job.newSpeakerName = newName;
    job.status = JobStatus::Queued;
    m_jobs.push_back(job);
    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(m_jobs.size() - 1);
}

void TranscriptionController::mergeSpeakers(const QUrl &fileUrl, const QString &sourceName, const QString &targetName)
{
    // Same dedup rule as renameSpeaker() immediately above, for the same
    // reason — see its doc comment.
    if (isQueued(fileUrl)) {
        emit speakersMerged(fileUrl, sourceName, targetName, false,
                             QStringLiteral("another task is already running for this file"));
        return;
    }

    Job job;
    job.id = m_nextJobId++;
    job.fileUrl = fileUrl;
    job.mergeSpeakersOnly = true;
    job.sourceSpeakerName = sourceName;
    job.targetSpeakerName = targetName;
    job.status = JobStatus::Queued;
    m_jobs.push_back(job);
    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(m_jobs.size() - 1);
}

QString TranscriptionController::readTranscriptBody(const QUrl &fileUrl) const
{
    // Item P23: see m_transcriptCacheMutex's doc comment.
    QMutexLocker locker(&m_transcriptCacheMutex);

    const QString mdPath = sidecarPath(fileUrl.toLocalFile());
    const QFileInfo fi(mdPath);
    if (!fi.exists())
        return QString();

    // Item O19: see m_bodyCache's doc comment — during an ACTIVE
    // transcription this is invoked on every live-update tick while the
    // file is constantly growing, so this stat-based guard alone rarely
    // skips work THEN, but it does for every other caller polling a file
    // that isn't currently changing (e.g. browsing while nothing is
    // transcribing) — a real full-read-avoided win, not just a
    // theoretical one.
    const qint64 size = fi.size();
    const QDateTime mtime = fi.lastModified();
    if (m_bodyCache.valid && m_bodyCache.mdPath == mdPath && m_bodyCache.size == size && m_bodyCache.mtime == mtime)
        return m_bodyCache.body;

    QFile mdFile(mdPath);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---"))
        return QString();

    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        // skip frontmatter lines
    }
    const QString body = in.readAll();

    m_bodyCache.mdPath = mdPath;
    m_bodyCache.size = size;
    m_bodyCache.mtime = mtime;
    m_bodyCache.body = body;
    m_bodyCache.valid = true;
    return body;
}

QVariantList TranscriptionController::parseSentences(const QUrl &fileUrl) const
{
    // Item P23: see m_transcriptCacheMutex's doc comment — RECURSIVE lock,
    // since readTranscriptBody()/readMetadata() below each lock it again.
    QMutexLocker locker(&m_transcriptCacheMutex);

    const QString mdPath = sidecarPath(fileUrl.toLocalFile());
    // stripPreludeIfPresent first: the prelude is prose ABOUT the transcript,
    // not a segment, and (per stripPreludeIfPresent's doc comment) could
    // otherwise be misdetected as one if it happens to start a line with
    // bracket-shaped text.
    const QString body = stripPreludeIfPresent(readTranscriptBody(fileUrl)); // itself cached — see its doc comment
    // The file's own known-speaker allowlist (see writeFrontmatter()'s and
    // segmentLinePattern()'s doc comments) — read via readMetadata() rather
    // than re-scanning frontmatter lines by hand, since that already parses
    // arbitrary "key: value" frontmatter lines generically (vaults are
    // small, a second open+read costs nothing worth avoiding here) and is
    // itself cached (see readMetadata()'s doc comment).
    const QStringList knownSpeakers =
        parseSpeakersList(readMetadata(fileUrl).value(QStringLiteral("speakers")).toString());

    // Item O19: segments are only ever APPENDED to a live-running
    // transcript (SegmentLineWriter's repeat-cycle merge is the one
    // exception — it rewrites the last couple of lines IN PLACE rather than
    // appending, see its own doc comment) — so once `body`'s first
    // m_sentenceCache.body.size() characters are confirmed to still equal
    // the cached body byte-for-byte, only the NEW trailing slice needs a
    // regex pass; the already-parsed prefix's sentences are reused as-is.
    // Falls back to a full reparse whenever that prefix check fails (a
    // different file, first call, or a merge having rewritten the tail) —
    // this only ever trades a slightly-too-eager full reparse for
    // correctness, never the other way around.
    if (m_sentenceCache.valid && m_sentenceCache.mdPath == mdPath) {
        if (m_sentenceCache.body == body)
            return m_sentenceCache.sentences;
        if (body.startsWith(m_sentenceCache.body)) {
            const qsizetype prefixLen = m_sentenceCache.body.size();
            QVariantList sentences = m_sentenceCache.sentences;
            const QStringView suffix = QStringView(body).mid(prefixLen);
            QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatchView(suffix);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                QVariantMap sentence;
                sentence["t0"] = parseCompactTimestamp(m.captured(1));
                sentence["speaker"] = m.captured(2);
                sentence["text"] = m.captured(3);
                sentence["start"] = static_cast<int>(prefixLen + m.capturedStart(3));
                sentence["length"] = m.capturedLength(3);
                sentence["repeatCount"] = m.capturedLength(4) == 0 ? 1 : m.captured(4).toInt();
                sentences.append(sentence);
            }
            m_sentenceCache.body = body;
            m_sentenceCache.sentences = sentences;
            return sentences;
        }
    }

    // Full (re)parse: no cache yet, a different file, or the cached prefix
    // no longer matches. globalMatch over the whole (multi-line) body, not a
    // per-line split, so capturedStart(3)/capturedLength(3) are exact
    // offsets into this SAME body string — what MainPanel.qml needs to
    // span-highlight the active sentence's substring without re-deriving
    // offsets itself. No "t1": it's no longer stored (see
    // SegmentLineWriter's doc comment — it was always identical to the next
    // segment's t0) and nothing ever consumed it here.
    QVariantList sentences;
    QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QVariantMap sentence;
        sentence["t0"] = parseCompactTimestamp(m.captured(1));
        // Empty when speaker detection wasn't on for this file (group 2
        // didn't participate in the match) — QRegularExpression's captured()
        // returns an empty string in that case, not a crash. The literal tag
        // text itself, never translated through a lookup (see
        // segmentLinePattern()'s doc comment).
        sentence["speaker"] = m.captured(2);
        sentence["text"] = m.captured(3);
        sentence["start"] = m.capturedStart(3);
        sentence["length"] = m.capturedLength(3);
        // Group 4 (repeat count) only participates when SegmentLineWriter
        // merged consecutive identical lines into this one — absent means a
        // normal, unmerged line, i.e. repeatCount 1.
        sentence["repeatCount"] = m.capturedLength(4) == 0 ? 1 : m.captured(4).toInt();
        sentences.append(sentence);
    }

    m_sentenceCache.mdPath = mdPath;
    m_sentenceCache.body = body;
    m_sentenceCache.sentences = sentences;
    m_sentenceCache.valid = true;
    return sentences;
}

void TranscriptionController::loadTranscriptAsync(const QUrl &fileUrl)
{
    // See this function's own doc comment in the header. QThread::create()
    // (Qt::Core, no extra module needed) rather than QtConcurrent — this
    // app has no other use for the Concurrent module, and a plain
    // fire-and-forget QThread per call is simpler than introducing it for
    // one call site. The thread deletes itself (deleteLater on its own
    // finished() signal) once the lambda below returns.
    QThread *thread = QThread::create([this, fileUrl]() {
        // Both calls are cached + mutex-guarded (see
        // m_transcriptCacheMutex's doc comment) — safe to run here
        // concurrently with the GUI thread's own synchronous calls to the
        // same two functions.
        const QString body = readTranscriptBody(fileUrl);
        const QVariantList sentences = parseSentences(fileUrl);
        // Marshal back to the GUI thread — `this` (TranscriptionController)
        // lives there, so Qt::QueuedConnection posts this lambda onto its
        // event loop rather than running it inline on this background
        // thread.
        QMetaObject::invokeMethod(
            this, [this, fileUrl, body, sentences]() { emit transcriptLoaded(fileUrl, body, sentences); },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

int TranscriptionController::activeSentenceIndex(const QVariantList &sentences, double positionSeconds) const
{
    if (sentences.isEmpty())
        return -1;

    // Last index whose t0 <= positionSeconds (std::upper_bound - 1). Sticky
    // through silence gaps: a position between sentence i's t1 and i+1's t0
    // still resolves to i, so the highlight holds rather than blanking out.
    const auto it = std::upper_bound(
        sentences.begin(), sentences.end(), positionSeconds,
        [](double pos, const QVariant &sentence) { return pos < sentence.toMap().value("t0").toDouble(); });
    const int index = int(std::distance(sentences.begin(), it)) - 1;
    return index;
}

QStringList TranscriptionController::existingVaultTags(const QUrl &vaultUrl) const
{
    return collectVaultTags(vaultUrl.toLocalFile());
}

int TranscriptionController::tasksTotal() const
{
    return m_jobs.size();
}

int TranscriptionController::tasksCompleted() const
{
    int count = 0;
    for (const Job &job : m_jobs) {
        if (job.status == JobStatus::Done || job.status == JobStatus::Failed)
            ++count;
    }
    return count;
}

QVariantList TranscriptionController::queuedFileUrls() const
{
    QVariantList list;
    for (const Job &job : m_jobs) {
        if (job.status == JobStatus::Queued)
            list.append(job.fileUrl);
    }
    return list;
}

QString TranscriptionController::jobKind(const Job &job)
{
    if (job.downloadTarget != DownloadTarget::None)
        return QStringLiteral("download");
    if (job.renameSpeakerOnly)
        return QStringLiteral("rename");
    if (job.mergeSpeakersOnly)
        return QStringLiteral("merge");
    if (job.relabelOnly)
        return QStringLiteral("relabel");
    if (job.generateChaptersOnly)
        return QStringLiteral("chapters");
    if (job.combineOnly)
        return QStringLiteral("combine");
    return QStringLiteral("transcribe");
}

QString TranscriptionController::currentJobKind() const
{
    if (m_activeIndex < 0 || m_activeIndex >= m_jobs.size())
        return QString();
    return jobKind(m_jobs[m_activeIndex]);
}

QVariantList TranscriptionController::jobs() const
{
    QVariantList list;
    for (const Job &job : m_jobs) {
        QVariantMap entry;
        entry["id"] = job.id;
        entry["fileUrl"] = job.fileUrl;
        entry["status"] = jobStatusToString(job.status);
        entry["error"] = job.error;
        entry["kind"] = jobKind(job);
        // -1 (QML sees it as a negative number) until Done/Failed — see
        // Job::durationMs's doc comment.
        entry["durationMs"] = job.durationMs;
        // modelId is only meaningful for a "download" kind entry targeting
        // WhisperModel (empty otherwise) — fileUrl is empty for every
        // download job (nothing to open), so the Tasks popup needs SOME
        // name to show; modelId (or a fixed label for the other two
        // download targets, which have no modelId concept) is that name.
        entry["modelId"] = job.modelId;
        switch (job.downloadTarget) {
        case DownloadTarget::WhisperModel: entry["downloadTarget"] = QStringLiteral("model"); break;
        case DownloadTarget::LabelModel: entry["downloadTarget"] = QStringLiteral("label"); break;
        case DownloadTarget::DiarizationModels: entry["downloadTarget"] = QStringLiteral("diarization"); break;
        case DownloadTarget::None: entry["downloadTarget"] = QString(); break;
        }
        list.append(entry);
    }
    return list;
}

QVariantList TranscriptionController::availableModels() const
{
    QVariantList list;
    for (const ModelManager::ModelInfo &info : ModelManager::availableModels()) {
        QVariantMap entry;
        entry["id"] = info.id;
        entry["label"] = info.label;
        // Item O31: whether this tier's .bin is already on disk for the
        // CURRENTLY open vault (m_vaultUrl — set once per vault open by
        // loadPersistedQueue(), empty/false for every entry until then) —
        // lets the model dropdown show which tiers would need a fresh
        // download vs. start immediately.
        entry["downloaded"] = ModelManager::isModelDownloaded(m_vaultUrl, info.id);
        list.append(entry);
    }
    return list;
}

#include "TranscriptionController.moc"
