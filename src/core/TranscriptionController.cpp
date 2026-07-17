#include "TranscriptionController.h"

#include <QCoreApplication>
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
#include <tuple>

#include "AudioDecoder.h"
#include "DiarizationEngine.h"
#include "LabelingEngine.h"
#include "ModelManager.h"
#include "RegexValidation.h"
#include "Segment.h"
#include "EngineRegistry.h"
#include "GpuProbe.h"
#include "ParakeetEngine.h"
#include "SubprocessEngine.h"
#include "UsbGrabJob.h"
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

// Item 17: a MANUAL chapter marker carries this trailing HTML-comment sentinel
// ("## [t] Title <!-- manual -->"). An HTML comment round-trips cleanly through
// the chapterMarkerPattern (it's captured as part of group 2's title text, then
// stripped for display by chapterDisplayTitle() below) and never renders as
// literal text in a Markdown viewer, yet is preserved verbatim on disk — so
// spliceBodyChapters()'s regeneration can tell a user's manual chapter apart
// from an LLM-generated auto one and strip only the latter.
const QString kManualChapterSentinel = QStringLiteral("<!-- manual -->");

// True if a chapterMarkerPattern group-2 title capture belongs to a manual
// marker (ends with the sentinel).
bool chapterTitleCaptureIsManual(const QString &titleCapture)
{
    return titleCapture.trimmed().endsWith(kManualChapterSentinel);
}

// The DISPLAY title for a chapterMarkerPattern group-2 capture — the sentinel
// (if any) removed and surrounding whitespace trimmed. Plain (auto) captures
// pass through unchanged.
QString chapterDisplayTitle(const QString &titleCapture)
{
    QString title = titleCapture.trimmed();
    if (title.endsWith(kManualChapterSentinel)) {
        title.chop(kManualChapterSentinel.size());
        title = title.trimmed();
    }
    return title;
}

// Builds a marker LINE (no trailing newline) for timestamp tsText + title,
// tagging it manual (sentinel appended) or auto.
QString formatChapterMarker(const QString &tsText, const QString &title, bool manual)
{
    QString line = QStringLiteral("## [") + tsText + QStringLiteral("] ") + title.trimmed();
    if (manual)
        line += QLatin1Char(' ') + kManualChapterSentinel;
    return line;
}

// Item 17 body mutators (used by TranscriptionController::editChapterMarker()).
// A marker is matched by parsed-timestamp closeness rather than exact string
// equality: formatTimestamp∘parseCompactTimestamp is identity on a stored
// marker, but an epsilon compare is robust to any float drift in the double a
// caller round-trips back through readChapters().

// Finds the chapter marker whose timestamp is within ~1ms of ts. Returns
// whether found; fills *out on success.
bool findChapterMarkerAt(const QString &body, double ts, QRegularExpressionMatch *out)
{
    QRegularExpressionMatchIterator it = chapterMarkerPattern().globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (qAbs(parseCompactTimestamp(m.captured(1)) - ts) < 0.0005) {
            *out = m;
            return true;
        }
    }
    return false;
}

// The byte offset at which to insert a chapter marker for ts: right before the
// first SEGMENT line ("[ts] ...") whose t0 >= ts, else at end of body — so a
// manually added chapter lands in sorted (time) order. Chapter markers
// ("## [") never match the "^\[" segment anchor, so they're naturally skipped.
int chapterInsertOffset(const QString &body, double ts)
{
    static const QRegularExpression segLine(QStringLiteral("^\\[([\\d:.]+)\\]"),
                                            QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator it = segLine.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (parseCompactTimestamp(m.captured(1)) >= ts - 0.0005)
            return static_cast<int>(m.capturedStart(0));
    }
    return static_cast<int>(body.size());
}

// Removes match m's whole marker line (and its trailing newline) from *body.
void removeChapterMarkerLine(QString *body, const QRegularExpressionMatch &m)
{
    int start = m.capturedStart(0);
    int length = m.capturedLength(0);
    if (start + length < body->size() && body->at(start + length) == QLatin1Char('\n'))
        ++length;
    body->remove(start, length);
}

// Item 14 "auto-categorize" mode: the existing tag vocabulary for a vault.
// Reads every .md's `tags:` frontmatter line directly rather than going
// through readMetadata()'s full parse, since that's all that's needed here.
// Returns a de-duplicated, sorted list of individual tags.
//
// `cache` (optional, per-file, keyed by absolute path) skips the open+parse
// for any file whose mtime+size is unchanged since the last call; the
// directory walk and its one stat per file still happen every time, which is
// what makes the mtime check possible in the first place. Pass nullptr for a
// guaranteed-fresh scan.
//
// The labeling job passes nullptr: it runs once per job, on the worker
// thread, and must not touch the GUI thread's cache. existingVaultTags()
// passes its cache, because it is called from a QML binding on every
// keystroke — see its doc comment in the header.
QStringList collectVaultTags(const QString &vaultLocalPath,
                             QHash<QString, TranscriptionController::CachedFileTags> *cache)
{
    QSet<QString> tags;
    QDirIterator it(vaultLocalPath, {QStringLiteral("*.md")}, QDir::Files,
                     QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString mdPath = it.next();

        if (cache) {
            const QFileInfo info(mdPath);
            const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
            const qint64 size = info.size();
            const auto cached = cache->constFind(mdPath);
            if (cached != cache->constEnd() && cached->mtimeMs == mtimeMs && cached->size == size) {
                for (const QString &tag : cached->tags)
                    tags.insert(tag);
                continue;
            }
        }

        QFile mdFile(mdPath);
        if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QTextStream in(&mdFile);
        if (in.readLine() != QStringLiteral("---"))
            continue;
        // This file's own tags, collected separately from the vault-wide set
        // so they can be cached per file.
        QStringList fileTags;
        QString line;
        while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
            const int colon = line.indexOf(':');
            if (colon < 0 || line.left(colon).trimmed() != QStringLiteral("tags"))
                continue;
            const QString value = line.mid(colon + 1).trimmed();
            for (const QString &tag : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString trimmed = tag.trimmed();
                if (!trimmed.isEmpty())
                    fileTags.append(trimmed);
            }
        }
        for (const QString &tag : fileTags)
            tags.insert(tag);
        if (cache) {
            // Re-stat rather than reuse the values read above: the file could
            // have been rewritten between that stat and the read just done,
            // which would otherwise cache the OLD mtime against NEW content
            // and never re-read it.
            const QFileInfo info(mdPath);
            cache->insert(mdPath, TranscriptionController::CachedFileTags{
                                      info.lastModified().toMSecsSinceEpoch(), info.size(), fileTags});
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

// See this function's declaration doc comment in TranscriptionController.h.
QString spliceBodySpeakers(const QString &body, const QStringList &knownSpeakers,
                            const std::vector<DiarizedTurn> &turns, double totalSeconds,
                            QStringList *appliedSpeakersOut)
{
    struct SegEdit {
        bool hadTag = false;
        int tagStart = 0;
        int tagLen = 0;
        int textStart = 0;
        double t0 = 0.0;
        QString text;
    };
    QVector<SegEdit> segs;
    QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        SegEdit e;
        e.hadTag = m.capturedLength(2) > 0;
        e.tagStart = m.capturedStart(2);
        e.tagLen = m.capturedLength(2);
        e.textStart = m.capturedStart(3);
        e.t0 = parseCompactTimestamp(m.captured(1));
        e.text = m.captured(3).trimmed();
        segs.append(e);
    }

    // Forward pass: resolve each segment's label (its end == the next
    // segment's start; the last segment runs to the decoded audio length),
    // collecting the distinct applied labels in first-appearance/file order.
    QVector<QString> labels(segs.size());
    QStringList applied;
    for (int i = 0; i < segs.size(); ++i) {
        const double t1 =
            (i + 1 < segs.size()) ? segs[i + 1].t0 : std::max(totalSeconds, segs[i].t0);
        const QString label =
            isNonSpeechAnnotation(segs[i].text) ? QString() : labelForSegment(turns, segs[i].t0, t1);
        labels[i] = label;
        if (!label.isEmpty() && !applied.contains(label))
            applied.append(label);
    }

    // Backward splice so earlier captured offsets stay valid as later ones
    // are rewritten (same technique as renameSpeakerInFile()).
    QString result = body;
    for (int i = segs.size() - 1; i >= 0; --i) {
        const QString &label = labels[i];
        if (label.isEmpty())
            continue; // non-speech line, or no turns — leave untouched
        if (segs[i].hadTag)
            result.replace(segs[i].tagStart, segs[i].tagLen, label);
        else
            result.insert(segs[i].textStart, QStringLiteral("[") + label + QStringLiteral("] "));
    }
    if (appliedSpeakersOut)
        *appliedSpeakersOut = applied;
    return result;
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

    // Item 17: strip only AUTO markers; PRESERVE manual ones (they carry the
    // sentinel — chapterTitleCaptureIsManual()). A user's hand-added/edited
    // chapter survives regeneration; only the previous LLM-generated auto set
    // is replaced. Offsets computed below are against this partially-stripped
    // string, so a chapter's target segment line is found at its TRUE
    // post-strip position. Manual markers left in place don't match
    // segmentLinePattern()'s "^\[" anchor (they start "## "), so they never
    // interfere with the anchor search below.
    QVector<QRegularExpressionMatch> autoMarkers;
    QSet<QString> manualTimestamps; // group-1 timestamp text a manual marker already occupies
    QRegularExpressionMatchIterator stripIt = chapterMarkerPattern().globalMatch(result);
    while (stripIt.hasNext()) {
        const QRegularExpressionMatch m = stripIt.next();
        if (chapterTitleCaptureIsManual(m.captured(2)))
            manualTimestamps.insert(m.captured(1));
        else
            autoMarkers.append(m);
    }
    for (auto matchIt = autoMarkers.crbegin(); matchIt != autoMarkers.crend(); ++matchIt) {
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
    // An auto chapter whose timestamp coincides with a preserved manual
    // marker is skipped — the manual one wins (re-sort by timestamp is
    // automatic: every marker is anchored to its segment line, and segment
    // lines are already in time order).
    struct PendingInsert {
        int offset;
        QString marker;
    };
    QList<PendingInsert> pending;
    pending.reserve(chapters.size());
    for (const ResolvedChapter &chapter : chapters) {
        const QString tsText = formatTimestamp(chapter.t0);
        if (manualTimestamps.contains(tsText))
            continue;
        const QString anchor = QStringLiteral("[") + tsText + QStringLiteral("]");
        const QRegularExpression lineStart(QStringLiteral("^") + QRegularExpression::escape(anchor),
                                            QRegularExpression::MultilineOption);
        const QRegularExpressionMatch m = lineStart.match(result);
        if (!m.hasMatch())
            continue; // target segment not found — defensive, shouldn't happen for a caller-resolved t0
        pending.append({static_cast<int>(m.capturedStart(0)),
                         formatChapterMarker(tsText, chapter.title, /*manual=*/false) + QLatin1Char('\n')});
    }
    std::sort(pending.begin(), pending.end(),
              [](const PendingInsert &a, const PendingInsert &b) { return a.offset > b.offset; });
    for (const PendingInsert &p : std::as_const(pending))
        result.insert(p.offset, p.marker);

    return result;
}

// Cost of one segment's "INDEX: text\n" formatting, matching
// LabelingEngine::generateChapters()'s own formatting exactly (digit count of
// the window-relative index, ": ", the text, '\n') so a window boundary lands
// where that single call's truncation would have.
static qsizetype chapterSegmentFormattedChars(int windowRelativeIndex, const QString &text)
{
    return QString::number(windowRelativeIndex).size() + 2 + text.size() + 1;
}

// See this function's doc comment in TranscriptionController.h.
QList<int> computeChapterWindows(const QStringList &segmentTexts)
{
    QList<int> windowStarts; // segment index each window begins at
    {
        int windowStart = 0;
        qsizetype windowChars = 0;
        for (int i = 0; i < segmentTexts.size(); ++i) {
            const qsizetype segChars = chapterSegmentFormattedChars(i - windowStart, segmentTexts.at(i));
            if (i > windowStart && windowChars + segChars > LabelingEngine::kMaxChapterInputChars) {
                windowStarts.append(windowStart);
                windowStart = i;
                windowChars = 0;
            }
            windowChars += segChars;
        }
        windowStarts.append(windowStart);
    }
    // Don't waste a whole extra LLM call on a pathologically small trailing
    // window (e.g. one segment left over) — fold it into the previous window
    // IF the combined window still fits kMaxChapterInputChars. Item 28 bug:
    // the old code folded unconditionally, producing a merged window that
    // exceeded the budget, which LabelingEngine::generateChapters() then
    // silently truncated back down — dropping exactly the folded-in tail, so
    // the final stretch of a long transcript got zero chapters. Now the fold
    // only happens when it's safe; otherwise the small trailing window stays
    // its own window and its segments still get proposed.
    if (windowStarts.size() > 1) {
        const qsizetype lastWindowSegCount = segmentTexts.size() - windowStarts.last();
        const qsizetype avgWindowSegCount = segmentTexts.size() / windowStarts.size();
        if (lastWindowSegCount < avgWindowSegCount / 4) {
            const int mergedStart = windowStarts.at(windowStarts.size() - 2);
            qsizetype mergedChars = 0;
            for (int i = mergedStart; i < segmentTexts.size(); ++i)
                mergedChars += chapterSegmentFormattedChars(i - mergedStart, segmentTexts.at(i));
            if (mergedChars <= LabelingEngine::kMaxChapterInputChars)
                windowStarts.removeLast();
        }
    }
    return windowStarts;
}

// --- Out-of-process engine discovery (DESIGN.md "Pluggable engines") -------
//
// A transcription model id is either a whisper ggml id ("base.en",
// "large-v3", resolved by ModelManager) or an out-of-process engine, written
// with the literal "engine:" prefix ("engine:parakeet"). These helpers turn
// the latter into a runnable SubprocessEngine, and back the availableEngines()
// capability list. All are free functions (internal linkage) so both the
// worker thread and the controller's availableEngines() can call them — they
// only do filesystem/process probing, no shared mutable state.

constexpr auto kEngineIdPrefix = "engine:";

bool isEngineModelId(const QString &modelId)
{
    return modelId.startsWith(QLatin1String(kEngineIdPrefix));
}

QString engineIdFromModelId(const QString &modelId)
{
    return isEngineModelId(modelId) ? modelId.mid(int(qstrlen(kEngineIdPrefix))) : QString();
}

// The read-only bundled engines dir shipped beside the app. Checked in a few
// install-layout-tolerant spots, with an env override for dev/tests. Empty if
// none exist (then only vault-local engines are discovered).
QString bundledEnginesDir()
{
    const QString env = qEnvironmentVariable("SUOP_ENGINES_DIR");
    if (!env.isEmpty() && QDir(env).exists())
        return QDir(env).absolutePath();
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{appDir + QStringLiteral("/engines"), appDir + QStringLiteral("/../engines"),
                                 appDir + QStringLiteral("/../share/suopTranscriber/engines")};
    for (const QString &candidate : candidates) {
        if (QDir(candidate).exists())
            return QDir(candidate).absolutePath();
    }
    return QString();
}

// Engine root search path: bundled first, then the vault's own
// .transcriber/engines (which overrides bundled by id, per EngineRegistry).
QStringList engineRootDirs(const QString &vaultPath)
{
    QStringList roots;
    const QString bundled = bundledEnginesDir();
    if (!bundled.isEmpty())
        roots << bundled;
    if (!vaultPath.isEmpty())
        roots << QDir(vaultPath).filePath(QStringLiteral(".transcriber/engines"));
    return roots;
}

// "Are this engine's runtime deps installed?" — runs its command with an extra
// --selftest arg (the run.py exits 0 iff its imports resolve, WITHOUT loading a
// model). Best-effort: a missing interpreter / an engine that doesn't support
// --selftest reads as "deps absent", i.e. grayed out with a reason.
bool engineDepsPresent(const EngineDescriptor &desc)
{
    if (desc.manifest.command.isEmpty())
        return false;
    QProcess probe;
    probe.setWorkingDirectory(desc.dir);
    QStringList args = desc.manifest.command.mid(1);
    args << QStringLiteral("--selftest");
    probe.start(desc.manifest.command.first(), args);
    if (!probe.waitForStarted(3000))
        return false;
    if (!probe.waitForFinished(20000)) {
        probe.kill();
        probe.waitForFinished(500);
        return false;
    }
    return probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
}

// Discovers every engine visible to `vaultPath`, capability-checked against
// this machine. GPU is probed once per process (nvidia-smi/vulkaninfo are
// slow-ish and static). Model presence is delegated to ModelManager.
std::vector<EngineDescriptor> discoverEngines(const QString &vaultPath)
{
    static const GpuStatus gpu = GpuProbe::probe();
    return EngineRegistry::discover(
        engineRootDirs(vaultPath), gpu, [](const EngineDescriptor &d) { return engineDepsPresent(d); },
        [](const EngineDescriptor &d) { return ModelManager::isEngineModelPresent(d.id); });
}

// Finds one engine by id for the transcribe path. Returns nullopt (with *error
// set) if it doesn't exist; a found-but-unavailable engine is returned so the
// caller can surface .unavailableReason.
std::optional<EngineDescriptor> resolveEngine(const QString &engineId, const QString &vaultPath, QString *error)
{
    for (const EngineDescriptor &desc : discoverEngines(vaultPath)) {
        if (desc.id == engineId) {
            if (!desc.available && error)
                *error = desc.unavailableReason;
            return desc;
        }
    }
    if (error)
        *error = QStringLiteral("engine \"%1\" not found").arg(engineId);
    return std::nullopt;
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
    void run(const QString &filePath, const QString &vaultPath, const QString &modelId)
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

        // A model id with the "engine:" prefix routes to an out-of-process
        // SubprocessEngine (DESIGN.md "Pluggable engines"); anything else is a
        // whisper ggml tier resolved by ModelManager. Everything downstream —
        // diarization, VAD-windowing (whisper only, see below), the offset
        // merge, frontmatter, labeling — is engine-agnostic because the
        // abstraction is at the Segment level.
        const bool useSubprocessEngine = isEngineModelId(modelId);
        // Parakeet is in-process (sherpa-onnx, like whisper) but a different
        // engine class + a multi-file ONNX bundle. It resolves through
        // ensureModel() (which returns the bundle dir) and uses VAD windowing
        // like whisper — only its engine construction differs.
        const bool useParakeet = ModelManager::isParakeetModelId(modelId);
        QString error;
        bool modelFallback = false;
        QString modelPath;     // ggml file path (whisper) OR the "engine:" id (subprocess) — also the reuse key
        QString modelUsedName; // frontmatter "model" field: ggml file name, or the engine's display name
        EngineDescriptor engineDesc;

        if (useSubprocessEngine) {
            const auto desc = resolveEngine(engineIdFromModelId(modelId), vaultPath, &error);
            if (!desc || !desc->available) {
                emit finished(false, error.isEmpty() ? QStringLiteral("engine unavailable") : error);
                return;
            }
            engineDesc = *desc;
            modelPath = modelId;
            modelUsedName = desc->manifest.name;
        } else {
            modelPath = ModelManager::ensureModel(modelId, &error, &modelFallback);
            if (modelPath.isEmpty()) {
                emit finished(false, error.isEmpty() ? QStringLiteral("model unavailable") : error);
                return;
            }
            modelUsedName = QFileInfo(modelPath).fileName();
        }

        // Item 50 (why "stop" can't be instant here): the whisper model load
        // below is a single blocking whisper.cpp call
        // (whisper_init_from_file_with_params) with NO abort hook — unlike
        // whisper_full()'s abort_callback (see onAbort further down) or ffmpeg
        // decode (a real killable subprocess). There is simply no interruption
        // point to check a pause/cancel flag AT during a load, so pause is a
        // no-op during "loading" (see pauseActive()) and a cancel requested
        // here only takes effect at the next real checkpoint after the load
        // returns. This is a genuine hard limitation of the native call, not
        // something this app can fix cooperatively — documented so it's
        // discoverable rather than looking like a missing feature. (A
        // subprocess engine constructs instantly and validates lazily at spawn
        // time, so none of this applies to it.)
        if (!m_engine || m_engine->isValid() == false || m_modelPath != modelPath) {
            if (useSubprocessEngine)
                m_engine = std::make_unique<SubprocessEngine>(engineDesc.manifest, engineDesc.dir);
            else if (useParakeet) {
                const ModelManager::ParakeetModelPaths p =
                    ModelManager::parakeetModelPaths();
                m_engine = std::make_unique<ParakeetEngine>(p.encoder, p.decoder, p.joiner, p.tokens);
            } else
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

        // Transcribe is pure ASR — speaker diarization is a SEPARATE task now
        // (the standalone diarizeFile() job / "Detect speakers" action, or the
        // diarize step of the runAllJobs() pipeline), never run inline here.
        // A fresh transcript is therefore always written speaker-less; the
        // diarize job later splices "[Speaker N]" tags + "speakers:" into the
        // .md in place (see spliceBodySpeakers()). See DESIGN.md "Speaker
        // diarization".

        // Wave N (task system): a cancel (or pause) requested during "loading"
        // or "decoding" takes effect here, right after the decode finishes and
        // before any file I/O happens — a normal pause/cancel checkpoint
        // between phases.
        if (waitWhilePausedOrCancelled()) {
            emit finished(false, QStringLiteral("Cancelled"));
            return;
        }

        // "detecting-speech": VAD windowing (DESIGN.md "Durable / resumable transcription",
        // chunking half; see VadChunker.h): one small model does double duty —
        // (a) whisper.cpp's own per-window silence gating (set on m_engine
        // below) and (b) finding VAD-aligned window boundaries so a long file
        // is transcribed as several bounded whisper_full() calls instead of
        // one unbounded one (reduces long-context hallucination drift). A
        // download/load failure degrades gracefully to today's single
        // whole-file window with no gating — never blocks the job — recorded
        // honestly in frontmatter below.
        //
        // Given its own visible phase (VAD is a real, sometimes-slow step:
        // one small-model download on first run + one blocking computeWindows()
        // call with no native progress hook — so its bar fraction is
        // synthesized elapsed-vs-average, like "loading"/"decoding", see
        // phaseRelativeSyntheticProgress()). The timer is restarted here so its
        // duration is recorded separately from "transcribing" below.
        emit phase(QStringLiteral("detecting-speech"));
        phaseTimer.restart();
        QString vadError;
        std::vector<VadChunker::Window> windows;
        bool vadActive = false;
        if (useSubprocessEngine) {
            // Out-of-process engines load a heavy model per process and do
            // their own internal segmentation, so we feed the whole file as ONE
            // chunk (one spawn) rather than VAD-windowing it into many spawns
            // (v1 decision — a persistent multi-chunk worker process is a later
            // optimization). whisper.cpp's own VAD gating is a WhisperCppEngine
            // feature and simply doesn't apply here.
            windows.push_back({0.0, static_cast<double>(pcm->size()) / 16000.0});
        } else {
            const QString vadModelPath = ModelManager::ensureVadModel(&vadError);
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
            // job's VAD failed. Guarded to the concrete whisper engine (VAD
            // gating is its feature); a subprocess engine never reaches here.
            if (auto *whisper = dynamic_cast<WhisperCppEngine *>(m_engine.get()))
                whisper->setVadModelPath(vadActive ? vadModelPath : QString());
        }

        // Record the VAD phase's duration for the rolling-average bar
        // weighting (recomputeOverallProgress()) and restart for "transcribing".
        emit phaseDurationMs(QStringLiteral("detecting-speech"), phaseTimer.restart());

        QFile mdFile(sidecarPath(filePath));
        if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit finished(false, QStringLiteral("could not open %1 for writing").arg(mdFile.fileName()));
            return;
        }

        // Pure-ASR transcript: no speaker detection at transcribe time, so
        // frontmatter records speakerDetection: false with an empty speaker
        // list (the standalone diarize job splices real speakers in later).
        writeFrontmatter(mdFile, modelUsedName, /*speakerDetection=*/false, modelFallback, modelId,
                          /*speakerLabels=*/{}, /*speakerDetectionError=*/QString(), vadActive, vadError);
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
            SegmentCallback onSegment = [&lineWriter, &liveTimer,
                                          &lastLiveTickMs, &chunk, this](const Segment &rawSegment) {
                // Chunk offset merge is the app's job (see
                // WhisperCppEngine.cpp/DESIGN.md "Engine architecture") — this
                // is now a REAL merge for multi-window files, not the no-op
                // it was when every job was a single offsetSeconds=0.0 chunk.
                Segment segment = rawSegment;
                segment.t0 += chunk.offsetSeconds;
                segment.t1 += chunk.offsetSeconds;
                // Pure ASR — no speaker label written at transcribe time (the
                // standalone diarize job adds "[Speaker N]" tags later).
                lineWriter.append(segment, QString());

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

        // No transcribe-time speaker detection, so no "speakers:" frontmatter
        // to reconcile against body tags here (that reconciliation lives in the
        // standalone diarize job — see syncSpeakersFrontmatterToBody()).

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

    // Model benchmark (Settings "Benchmark" page): runs each requested model /
    // engine over ONE audio file, in memory, purely for comparison — writes NO
    // .md, touches no queue or vault state, decodes the audio ONCE and reuses
    // it across models. Deliberately a slot on this same worker (not a separate
    // thread) so it serializes with real transcription jobs and never contends
    // with them for the GPU. modelIds is exactly what to run (the Settings page
    // filters to downloaded whisper tiers + available engines — the "only
    // downloaded models" policy lives there, where availableModels()/
    // availableEngines() give it the info). Reports each model's result as it
    // finishes via benchmarkModelDone, and the whole run via benchmarkFinished
    // (which the page persists through VaultTreeModel::writeBenchmarkLastRun).
    void benchmarkJob(const QString &filePath, const QString &vaultPath, const QStringList &modelIds)
    {
        resetControl(); // a benchmark is cancellable via the same atomics jobs use

        QString decodeError;
        auto pcm = AudioDecoder::decodeTo16kMono(filePath, &decodeError);
        if (!pcm) {
            emit benchmarkFailed(decodeError.isEmpty() ? QStringLiteral("audio decode failed") : decodeError);
            return;
        }
        const double audioSeconds = static_cast<double>(pcm->size()) / 16000.0;

        AudioChunk chunk;
        chunk.pcm16kMono = *pcm;
        chunk.offsetSeconds = 0.0; // whole file, single chunk — no offset merge needed for display

        const AbortCallback onAbort = [this] { return isCancelRequested(); };
        QVariantList results;
        QElapsedTimer totalTimer;
        totalTimer.start();

        for (int i = 0; i < modelIds.size(); ++i) {
            if (isCancelRequested())
                break;
            const QString modelId = modelIds.at(i);
            emit benchmarkModelStarted(modelId, i, modelIds.size());

            QVariantMap result = runOneBenchmarkModel(modelId, vaultPath, chunk, audioSeconds, onAbort);
            results.append(result);
            emit benchmarkModelDone(result);
        }

        QVariantMap run;
        run["file"] = filePath;
        run["fileName"] = QFileInfo(filePath).fileName();
        run["audioSeconds"] = audioSeconds;
        run["totalRuntimeMs"] = static_cast<qint64>(totalTimer.elapsed());
        run["cancelled"] = isCancelRequested();
        run["results"] = results;
        emit benchmarkFinished(run);
    }

    // Diarization sweep: runs each {segmentation, embedding, threshold} combo
    // over ONE decode of the file and reports, per combo, how many speakers it
    // found plus timing — the point being to compare which combination actually
    // separates a hard recording. Writes nothing to the vault (same as
    // benchmarkJob). Each combo builds a fresh DiarizationEngine (the threshold
    // is baked in at construction, and a big embedding model shouldn't hold VRAM
    // across combos); cancellation takes effect between combos (diarize() has no
    // mid-pass abort hook — see runDiarizationPass()).
    void diarizationBenchmarkJob(const QString &filePath, const QVariantList &combos)
    {
        resetControl();

        QString decodeError;
        auto pcm = AudioDecoder::decodeTo16kMono(filePath, &decodeError);
        if (!pcm) {
            emit benchmarkFailed(decodeError.isEmpty() ? QStringLiteral("audio decode failed") : decodeError);
            return;
        }
        const double audioSeconds = static_cast<double>(pcm->size()) / 16000.0;

        m_diarizationEngine.reset(); // don't reuse a warm engine across a sweep
        QVariantList results;
        QElapsedTimer totalTimer;
        totalTimer.start();

        for (int i = 0; i < combos.size(); ++i) {
            if (isCancelRequested())
                break;
            const QVariantMap combo = combos.at(i).toMap();
            const QString segId = combo.value("segmentationModelId").toString();
            const QString embedId = combo.value("embeddingModelId").toString();
            const double threshold = combo.value("threshold").toDouble();
            const QString label = combo.value("label").toString();
            emit diarizationBenchmarkComboStarted(label, i, combos.size());

            QVariantMap result;
            result["label"] = label;
            result["segmentationModelId"] = segId;
            result["embeddingModelId"] = embedId;
            result["threshold"] = threshold;

            QString segPath, embedPath, error;
            if (!ModelManager::ensureDiarizationModels(segId, embedId, &segPath, &embedPath, &error)) {
                result["ok"] = false;
                result["error"] = error.isEmpty() ? QStringLiteral("models unavailable") : error;
                results.append(result);
                emit diarizationBenchmarkComboDone(result);
                continue;
            }

            DiarizationEngine engine(segPath, embedPath, static_cast<float>(threshold));
            if (!engine.isValid()) {
                result["ok"] = false;
                result["error"] = QStringLiteral("failed to load models");
                results.append(result);
                emit diarizationBenchmarkComboDone(result);
                continue;
            }

            QElapsedTimer comboTimer;
            comboTimer.start();
            const std::vector<DiarizedTurn> turns = engine.diarize(*pcm);
            const qint64 runtimeMs = comboTimer.elapsed();
            int maxSpeaker = -1;
            for (const DiarizedTurn &turn : turns)
                maxSpeaker = std::max(maxSpeaker, turn.speaker);
            result["ok"] = true;
            result["speakerCount"] = maxSpeaker + 1;
            result["turnCount"] = static_cast<int>(turns.size());
            result["runtimeMs"] = runtimeMs;
            result["realtimeFactor"] = runtimeMs > 0 ? audioSeconds / (runtimeMs / 1000.0) : 0.0;
            results.append(result);
            emit diarizationBenchmarkComboDone(result);
        }
        m_diarizationEngine.reset();

        QVariantMap run;
        run["file"] = filePath;
        run["fileName"] = QFileInfo(filePath).fileName();
        run["audioSeconds"] = audioSeconds;
        run["totalRuntimeMs"] = static_cast<qint64>(totalTimer.elapsed());
        run["cancelled"] = isCancelRequested();
        run["kind"] = QStringLiteral("diarization");
        run["results"] = results;
        emit diarizationBenchmarkFinished(run);
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
    void generateChaptersJob(const QString &filePath, const QString &vaultPath, double density)
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
        const QString labelModelPath = ModelManager::ensureLabelModel(&labelError);
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

        // Item P34 / item 28: generateChapters() silently truncates (with a
        // qWarning) any single call whose formatted "INDEX: text\n" input
        // exceeds LabelingEngine::kMaxChapterInputChars — for a long file, one
        // call over the whole transcript meant everything past ~20 minutes of
        // speech never got proposed at all. Split into multiple windows, each
        // kept under that budget (see computeChapterWindows()), and offset
        // each window's returned afterSegmentIndex (0-relative to THAT
        // window's own slice) by its starting index in the full segments list
        // before merging.
        const QList<int> windowStarts = computeChapterWindows(segmentTexts);

        // Items 26/31: drive the progress bar. Chapter generation is a
        // per-window sequence of LLM calls with a known window count up front,
        // so a determinate fraction (one window == one unit, plus a final unit
        // for the merge/resolve/write tail) advances the bar smoothly instead
        // of it sitting frozen at 0 until the whole job finishes. Treated like
        // "combining" on the controller side (a single fraction assigned
        // straight to overallProgress — see the progress() connection).
        emit phase(QStringLiteral("chaptering"));
        emit progress(0.0);
        const int progressUnits = windowStarts.size() + 1; // +1 for the merge/resolve/write tail

        QList<Chapter> chapters;
        for (int w = 0; w < windowStarts.size(); ++w) {
            // Items 30/50: cooperative cancel/pause checkpoint between windows.
            // An in-flight single generateChapters() LLM call can't be
            // interrupted (no llama.cpp abort hook for a single-shot decode),
            // but checking here means a cancel takes effect promptly between
            // windows rather than only after the ENTIRE job — a big
            // improvement for a long, multi-window transcript.
            if (waitWhilePausedOrCancelled()) {
                emit finished(false, QStringLiteral("Cancelled"));
                return;
            }

            const int startIdx = windowStarts.at(w);
            const int endIdx = (w + 1 < windowStarts.size()) ? windowStarts.at(w + 1) : segmentTexts.size();
            const QStringList windowTexts = segmentTexts.mid(startIdx, endIdx - startIdx);

            QString genError;
            const auto windowChapters = m_labelingEngine->generateChapters(windowTexts, density, &genError);
            if (!windowChapters) {
                emit finished(false, QStringLiteral("chapter generation failed: %1").arg(genError));
                return;
            }
            emit progress(static_cast<double>(w + 1) / progressUnits);
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
        // Items 30/50: one more cancel/pause checkpoint before the merge pass
        // (itself a run of LLM calls) — cancel takes effect here rather than
        // after the whole merge finishes.
        if (waitWhilePausedOrCancelled()) {
            emit finished(false, QStringLiteral("Cancelled"));
            return;
        }

        QList<Chapter> finalChapters = sanitized;
        QString mergeError;
        if (const auto merged = m_labelingEngine->mergeSimilarChapters(sanitized, segmentTexts, density, &mergeError))
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
        emit progress(1.0);
        emit finished(true, {});
    }

    // Item 52: standalone speaker diarization on an already-transcribed file —
    // re-runs (or adds) speaker detection WITHOUT re-transcribing audio. Reads
    // the existing .md's segment lines, decodes the sibling audio, runs the
    // SAME diarization core run()'s inline speakerDetection path uses
    // (runDiarizationPass()), assigns each existing segment its overlapping
    // speaker (labelForSegment() via spliceBodySpeakers()), and writes the
    // "[Speaker N]" tags + updated "speakerDetection: true"/"speakers:"
    // frontmatter back into the .md in place (prelude/chapters/repeat markers
    // preserved). A failure here IS the job's result (like relabelJob()).
    // Cancel takes effect at the post-diarize checkpoint (diarization has no
    // mid-pass abort hook — see runDiarizationPass()).
    void diarizeJob(const QString &filePath, const QString &vaultPath, const QString &segmentationModelId,
                    const QString &embeddingModelId, double threshold)
    {
        // Needs neither the whisper nor the label model — free them now
        // (GPU hygiene, mirrors relabelJob()'s eviction).
        m_engine.reset();
        m_labelingEngine.reset();

        const QString mdPath = sidecarPath(filePath);
        if (!QFileInfo::exists(mdPath)) {
            emit finished(false, QStringLiteral("%1 has not been transcribed yet").arg(filePath));
            return;
        }

        // Read frontmatter + body + the file's existing "speakers:" allowlist
        // up front (before the slow decode) so a malformed/absent file fails
        // fast — same parse the rename/merge paths use.
        QStringList frontmatterLines;
        QString body;
        int speakersLineIndex = -1;
        QStringList knownSpeakers;
        QStringList distinctTags;
        QString parseError;
        if (!parseSpeakerTaggedFile(mdPath, &frontmatterLines, &body, &speakersLineIndex, &knownSpeakers,
                                     &distinctTags, &parseError)) {
            emit finished(false, parseError);
            return;
        }

        // Decode the sibling audio (same 16k mono pass whisper uses) —
        // pause/cancel honored between chunks, same as run()'s decode.
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

        bool effective = false;
        QString diarError;
        const std::vector<DiarizedTurn> turns = runDiarizationPass(
            *pcm, vaultPath, segmentationModelId, embeddingModelId, static_cast<float>(threshold), &effective,
            &diarError);
        // Unlike run() (where a diarization failure degrades to ASR-only),
        // detecting speakers IS this job's only purpose — so a failure here
        // is the job's result.
        if (!effective) {
            emit finished(false, diarError.isEmpty() ? QStringLiteral("speaker detection failed") : diarError);
            return;
        }

        // A cancel requested DURING diarization (no mid-pass abort hook — see
        // runDiarizationPass()) takes effect here, before any file I/O.
        if (waitWhilePausedOrCancelled()) {
            emit finished(false, QStringLiteral("Cancelled"));
            return;
        }

        const double totalSeconds = static_cast<double>(pcm->size()) / 16000.0;
        QStringList appliedSpeakers;
        const QString newBody = spliceBodySpeakers(body, knownSpeakers, turns, totalSeconds, &appliedSpeakers);

        // Update the frontmatter to reflect that detection now ran: set
        // speakerDetection true, replace/insert the "speakers:" allowlist with
        // exactly the labels that actually landed on a body line (mirrors
        // syncSpeakersFrontmatterToBody()), and drop any stale
        // speakerDetectionError from an earlier failed attempt. Every other
        // frontmatter key (model/vad/transcribedAt/label/tags/...) is left
        // untouched.
        const auto setKey = [&frontmatterLines](const QString &key, const QString &value) {
            for (int i = 0; i < frontmatterLines.size(); ++i) {
                const int c = frontmatterLines[i].indexOf(QLatin1Char(':'));
                if (c >= 0 && frontmatterLines[i].left(c).trimmed() == key) {
                    frontmatterLines[i] = key + QStringLiteral(": ") + value;
                    return;
                }
            }
            frontmatterLines.append(key + QStringLiteral(": ") + value);
        };
        const auto removeKey = [&frontmatterLines](const QString &key) {
            for (int i = 0; i < frontmatterLines.size(); ++i) {
                const int c = frontmatterLines[i].indexOf(QLatin1Char(':'));
                if (c >= 0 && frontmatterLines[i].left(c).trimmed() == key) {
                    frontmatterLines.removeAt(i);
                    return;
                }
            }
        };
        setKey(QStringLiteral("speakerDetection"), QStringLiteral("true"));
        removeKey(QStringLiteral("speakerDetectionError"));
        if (appliedSpeakers.isEmpty())
            removeKey(QStringLiteral("speakers"));
        else
            setKey(QStringLiteral("speakers"), appliedSpeakers.join(QStringLiteral(", ")));

        QString writeError;
        if (!writeFrontmatterBlock(mdPath, frontmatterLines, newBody, &writeError)) {
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
    void download(const QString &target, const QString &modelId)
    {
        const ModelManager::DownloadProgressCallback onProgress = [this](qint64 done, qint64 total) {
            emit downloadProgress(done, total);
        };
        ModelManager::DownloadControl control;
        control.paused = [this] { return m_pauseRequested.load(); };
        control.cancelled = [this] { return m_cancelRequested.load(); };

        QString error;
        bool ok = false;
        if (target == QStringLiteral("model")) {
            ok = !ModelManager::ensureModel(modelId, &error, nullptr, onProgress, control).isEmpty();
        } else if (target == QStringLiteral("label")) {
            ok = !ModelManager::ensureLabelModel(&error, onProgress, control).isEmpty();
        } else if (target == QStringLiteral("vad")) {
            ok = !ModelManager::ensureVadModel(&error, onProgress, control).isEmpty();
        } else if (target == QStringLiteral("segmentation")) {
            ok = !ModelManager::ensureSegmentationModel(modelId, &error, nullptr, onProgress, control).isEmpty();
        } else if (target == QStringLiteral("embedding")) {
            ok = !ModelManager::ensureEmbeddingModel(modelId, &error, nullptr, onProgress, control).isEmpty();
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
        // The merged file stands in for the original recording, so it inherits
        // the first part's mtime instead of taking "now" (see the mtime note in
        // findCombineGroups()'s doc comment — mtime is the vault's stand-in for
        // when a recording happened, since birth time doesn't survive a copy).
        // Read before ffmpeg runs, because the sources move into .combined/
        // afterwards. sourcePaths is in combine order, so .first() is the part
        // the recording actually starts with — not necessarily the oldest, if
        // the rule designated a sort differentiator.
        const QDateTime firstPartMtime =
            sourcePaths.isEmpty() ? QDateTime() : QFileInfo(sourcePaths.first()).lastModified();

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

        // Restamp only now that ffmpeg has exited and closed the output —
        // anything that writes the file stamps mtime as "now", so an earlier
        // setFileTime() would just get clobbered.
        if (firstPartMtime.isValid()) {
            QFile out(outputPath);
            if (out.open(QIODevice::ReadWrite))
                out.setFileTime(firstPartMtime, QFileDevice::FileModificationTime);
        }

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

    // Item 37: drag-and-drop / external audio import — copies sourcePath into
    // destPath (the in-vault destination VaultTreeModel::importDestinationFor()
    // already validated on the GUI thread) OFF the GUI thread, so a large file
    // no longer freezes the UI. A chunked read/write loop (rather than a
    // single blocking QFile::copy) is what lets this report byte progress and
    // honor pause/cancel between chunks, same cooperative-checkpoint discipline
    // combineJob()/download() use. Never touches m_engine/m_diarizationEngine/
    // m_labelingEngine — nothing to evict/reload for a plain file copy.
    // One USB grab: the whole recursive scan+copy of one mounted volume under
    // one already-resolved rule. The walk itself is runUsbGrabScan() (shared
    // verbatim with the dry-run preview — see UsbGrabJob.h); everything here is
    // just wiring it to this worker's phase/progress/pause/cancel contract.
    //
    // Reports its counts through usbGrabScanCompleted() rather than stuffing
    // them into finished(ok, error): a grab's result is "copied N, skipped M,
    // and here are the K files that failed", which is not ok/error-shaped —
    // per-file copy errors are non-fatal, so a grab that missed one file is a
    // SUCCEEDED job that has something to report, not a failed one.
    void usbGrabJob(const UsbGrabRule &rule, const QString &mountPath, const QString &destinationPath)
    {
        emit phase(QStringLiteral("grabbing"));

        bool wasPaused = false;
        UsbGrabScanHooks hooks;
        hooks.onProgress = [this](int processed, int total) {
            if (total > 0)
                emit progress(std::clamp(static_cast<double>(processed) / static_cast<double>(total), 0.0, 1.0));
        };
        hooks.shouldAbort = [this, &wasPaused] {
            // Between files, not between bytes: a grab's unit of work is one
            // file, and QFile::copy() is uninterruptible anyway, so a pause or
            // cancel lands at the next file boundary. Mirrors importJob()'s
            // loop — no subprocess to SIGSTOP, so just block this worker here.
            while (m_pauseRequested.load() && !isCancelRequested()) {
                if (!wasPaused) {
                    wasPaused = true;
                    emit paused(true);
                }
                QThread::msleep(100);
            }
            if (wasPaused) {
                wasPaused = false;
                emit paused(false);
            }
            return isCancelRequested();
        };

        const UsbGrabScanResult result = runUsbGrabScan(rule, mountPath, destinationPath, /*dryRun=*/false, hooks);

        if (!result.refusal.isEmpty()) {
            // A refusal means the walk never ran (see usbGrabRefusalReason) —
            // a genuinely failed job, unlike a per-file copy error.
            emit usbGrabScanCompleted(0, 0, QStringList { result.refusal }, false);
            emit finished(false, result.refusal);
            return;
        }

        emit usbGrabScanCompleted(result.filesCopied, result.filesSkipped, result.errors, result.aborted);
        if (result.aborted) {
            emit finished(false, QStringLiteral("Cancelled"));
            return;
        }
        emit progress(1.0);
        emit finished(true, QString());
    }

    void importJob(const QString &sourcePath, const QString &destPath)
    {
        emit phase(QStringLiteral("importing"));

        QFile src(sourcePath);
        if (!src.open(QIODevice::ReadOnly)) {
            emit finished(false, QStringLiteral("could not open source file"));
            return;
        }
        // Collision could have appeared between validation and now — never
        // silently overwrite (same refusal as importDestinationFor()).
        if (QFileInfo::exists(destPath)) {
            emit finished(false, QStringLiteral("a file with that name already exists in the vault"));
            return;
        }
        QFile dst(destPath);
        if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit finished(false, QStringLiteral("could not create destination file"));
            return;
        }

        const qint64 total = src.size();
        qint64 copied = 0;
        constexpr qint64 kChunk = 1 << 20; // 1 MiB
        bool wasPaused = false;
        while (true) {
            if (isCancelRequested()) {
                dst.close();
                dst.remove(); // don't leave a half-written file in the vault
                emit finished(false, QStringLiteral("Cancelled"));
                return;
            }
            if (m_pauseRequested.load()) {
                // No subprocess to SIGSTOP (unlike combineJob/decode) — just
                // block this worker between chunks until resumed/cancelled.
                if (!wasPaused) {
                    wasPaused = true;
                    emit paused(true);
                }
                QThread::msleep(100);
                continue;
            }
            if (wasPaused) {
                wasPaused = false;
                emit paused(false);
            }
            const QByteArray buf = src.read(kChunk);
            if (buf.isEmpty())
                break; // done (or read error — treated as EOF; size check below is implicit)
            if (dst.write(buf) != buf.size()) {
                dst.close();
                dst.remove();
                emit finished(false, QStringLiteral("write failed while copying into the vault"));
                return;
            }
            copied += buf.size();
            if (total > 0)
                emit progress(std::clamp(static_cast<double>(copied) / static_cast<double>(total), 0.0, 1.0));
        }
        dst.flush();
        dst.close();
        emit progress(1.0);
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
    // Emitted by usbGrabJob() immediately before its finished(), carrying the
    // grab's actual result (which finished(ok, error) has no room for — see
    // usbGrabJob()'s doc comment). cancelled means the counts are a partial
    // tally, so the controller must not report the grab as completed.
    void usbGrabScanCompleted(int filesCopied, int filesSkipped, const QStringList &errors, bool cancelled);
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
    // Model-benchmark progress (see benchmarkJob()): started per model (with
    // its index/total for a progress bar), done per model (the finished result
    // row), finished once with the whole run, or failed before any model ran
    // (e.g. audio decode failed).
    void benchmarkModelStarted(const QString &modelId, int index, int total);
    void benchmarkModelDone(const QVariantMap &result);
    void benchmarkFinished(const QVariantMap &run);
    void benchmarkFailed(const QString &error);
    // Diarization sweep (diarizationBenchmarkJob) — mirrors the model-benchmark
    // trio; a decode failure reuses benchmarkFailed.
    void diarizationBenchmarkComboStarted(const QString &label, int index, int total);
    void diarizationBenchmarkComboDone(const QVariantMap &result);
    void diarizationBenchmarkFinished(const QVariantMap &run);

private:
    // Runs one model/engine for benchmarkJob() and returns its result row. Never
    // writes anything; an unavailable/failed model becomes a row with ok=false
    // and a reason rather than aborting the whole benchmark. runtimeMs measures
    // ONLY transcribe() (model load happens before the timer starts, so tiers
    // aren't penalized for cold-load differences the way the file's own
    // transcription would be).
    QVariantMap runOneBenchmarkModel(const QString &modelId, const QString &vaultPath, const AudioChunk &chunk,
                                     double audioSeconds, const AbortCallback &onAbort)
    {
        QVariantMap row;
        row["modelId"] = modelId;

        std::unique_ptr<TranscriptionEngine> engine;
        if (isEngineModelId(modelId)) {
            QString error;
            const auto desc = resolveEngine(engineIdFromModelId(modelId), vaultPath, &error);
            row["kind"] = QStringLiteral("engine");
            row["label"] = desc && desc->manifestValid ? desc->manifest.name : engineIdFromModelId(modelId);
            if (!desc || !desc->available) {
                row["ok"] = false;
                row["error"] = error.isEmpty() ? QStringLiteral("engine unavailable") : error;
                return row;
            }
            engine = std::make_unique<SubprocessEngine>(desc->manifest, desc->dir);
        } else if (ModelManager::isParakeetModelId(modelId)) {
            row["kind"] = QStringLiteral("parakeet");
            row["label"] = modelId;
            const ModelManager::ParakeetModelPaths p =
                ModelManager::parakeetModelPaths();
            if (!p.isComplete()) {
                row["ok"] = false;
                row["error"] = QStringLiteral("model not downloaded");
                return row;
            }
            auto parakeet = std::make_unique<ParakeetEngine>(p.encoder, p.decoder, p.joiner, p.tokens);
            if (!parakeet->isValid()) {
                row["ok"] = false;
                row["error"] = QStringLiteral("failed to load Parakeet model");
                return row;
            }
            engine = std::move(parakeet);
        } else {
            row["kind"] = QStringLiteral("whisper");
            row["label"] = modelId;
            QString error;
            // Only downloaded tiers are passed in, so ensureModel() resolves the
            // existing file without a network fetch.
            const QString modelPath = ModelManager::ensureModel(modelId, &error);
            if (modelPath.isEmpty()) {
                row["ok"] = false;
                row["error"] = error.isEmpty() ? QStringLiteral("model unavailable") : error;
                return row;
            }
            auto whisper = std::make_unique<WhisperCppEngine>(modelPath);
            if (!whisper->isValid()) {
                row["ok"] = false;
                row["error"] = QStringLiteral("failed to load model");
                return row;
            }
            engine = std::move(whisper);
        }

        QElapsedTimer timer;
        timer.start();
        const std::vector<Segment> segments = engine->transcribe(chunk, {}, {}, onAbort);
        const qint64 runtimeMs = timer.elapsed();

        QString text;
        int wordCount = 0;
        for (const Segment &seg : segments) {
            const QString segText = QString::fromStdString(seg.text).trimmed();
            if (segText.isEmpty())
                continue;
            if (!text.isEmpty())
                text += QLatin1Char(' ');
            text += segText;
            wordCount += segText.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size();
        }

        row["ok"] = true;
        row["runtimeMs"] = runtimeMs;
        row["segmentCount"] = static_cast<int>(segments.size());
        row["wordCount"] = wordCount;
        row["charCount"] = text.size();
        // Realtime factor: seconds of audio processed per second of wall clock
        // (>1 = faster than realtime). 0 if it somehow finished instantly.
        row["realtimeFactor"] = runtimeMs > 0 ? audioSeconds / (runtimeMs / 1000.0) : 0.0;
        row["text"] = text;
        return row;
    }

    // Item 52: the diarization core shared by BOTH the inline transcribe path
    // (run(), gated by its speakerDetection toggle) and the standalone
    // diarizeJob(). Ensures + loads the diarization models, emits the
    // "detecting-speakers" phase and its two sub-phases with REAL progress
    // (sherpa-onnx's own chunk-processed callback, split at the 0.30
    // segmentation/embedding boundary — see the vendored patch), runs the
    // whole pass over pcm, then evicts the engine (a one-shot pass — nothing
    // reuses it; cheap ~35MB reload next time). Sets *effectiveOut true iff
    // diarization actually ran; otherwise *errorOut carries why (models
    // unavailable / failed to load) — the never-misrepresent contract
    // writeFrontmatter() records. Pause BLOCKS inside the progress callback
    // (it fires often enough); a cancel is NOT actionable mid-pass — the
    // DiarizationProgressCallback is `void(double)` (see DiarizationEngine.h),
    // so there's no channel to abort sherpa-onnx's current batch, and its
    // return value is deliberately ignored here. A cancel requested during
    // diarization therefore takes effect at the CALLER's post-diarize
    // checkpoint (run()'s waitWhilePausedOrCancelled() before file I/O,
    // diarizeJob()'s equivalent), not instantly — a hard sherpa-onnx
    // limitation, not faked.
    std::vector<DiarizedTurn> runDiarizationPass(const std::vector<float> &pcm, const QString &vaultPath,
                                                 const QString &segmentationModelId, const QString &embeddingModelId,
                                                 float threshold, bool *effectiveOut, QString *errorOut)
    {
        Q_UNUSED(vaultPath);
        *effectiveOut = false;
        errorOut->clear();

        emit phase(QStringLiteral("detecting-speakers"));
        QString segPath, embedPath, diarizeError;
        if (!ModelManager::ensureDiarizationModels(segmentationModelId, embeddingModelId, &segPath, &embedPath,
                                                    &diarizeError)) {
            *errorOut =
                diarizeError.isEmpty() ? QStringLiteral("speaker diarization models unavailable") : diarizeError;
            return {};
        }
        // Rebuild when the resolved paths OR the sensitivity change — the
        // threshold is baked into the sherpa config at construction.
        if (!m_diarizationEngine || !m_diarizationEngine->isValid() || m_diarizationSegPath != segPath
            || m_diarizationEmbedPath != embedPath || m_diarizationThreshold != threshold) {
            m_diarizationEngine = std::make_unique<DiarizationEngine>(segPath, embedPath, threshold);
            m_diarizationSegPath = segPath;
            m_diarizationEmbedPath = embedPath;
            m_diarizationThreshold = threshold;
        }
        if (!m_diarizationEngine->isValid()) {
            *errorOut = QStringLiteral("failed to load speaker diarization models");
            m_diarizationEngine.reset();
            return {};
        }

        QString detectingSubPhase;
        // sherpa-onnx fires this callback once per internal ONNX chunk — the
        // segmentation pass alone runs the model roughly once per ~1s audio
        // hop (see cpuThreadCount()'s doc comment in DiarizationEngine.cpp),
        // so a long recording means hundreds to thousands of calls, easily
        // faster than the GUI thread can drain progress()'s queued
        // connection (progressChanged() + recomputeOverallProgress() +
        // overallProgressChanged() per tick — see the connect() in this
        // controller's ctor). Unthrottled, that backlog starves repaints for
        // the whole diarization pass, LOOKING like a GUI-thread freeze even
        // though the actual inference is correctly running on this worker
        // thread. Throttled to kLiveUpdateThrottleMs, same as run()'s live
        // segment-write path — but the fraction==1.0 tail call always gets
        // through so the bar actually reaches 100%.
        QElapsedTimer progressThrottle;
        progressThrottle.start();
        qint64 lastProgressTickMs = -kLiveUpdateThrottleMs;
        std::vector<DiarizedTurn> turns = m_diarizationEngine->diarize(
            pcm, [this, &detectingSubPhase, &progressThrottle, &lastProgressTickMs](double fraction) {
                waitWhilePausedOrCancelled(); // see this method's doc comment (cancel not actionable mid-pass)
                const QString subPhase = fraction < 0.30 ? QStringLiteral("detecting-speakers-segmenting")
                                                          : QStringLiteral("detecting-speakers-identifying");
                if (subPhase != detectingSubPhase) {
                    detectingSubPhase = subPhase;
                    emit phase(subPhase);
                }
                const qint64 nowMs = progressThrottle.elapsed();
                if (fraction >= 1.0 || nowMs - lastProgressTickMs >= kLiveUpdateThrottleMs) {
                    lastProgressTickMs = nowMs;
                    emit progress(fraction);
                }
            });
        *effectiveOut = true;
        m_diarizationEngine.reset();
        return turns;
    }

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
        const QString labelModelPath = ModelManager::ensureLabelModel(&labelError);
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
        // nullptr = no cache: this runs on the worker thread (must not touch
        // existingVaultTags()' GUI-thread cache) and only once per job.
        const QStringList existingTags = collectVaultTags(vaultPath, nullptr);
        // Item P29 / item 58: the prelude's length should scale WITH the
        // transcript — capped DOWN for a short one (no "3-sentence transcript,
        // 5-paragraph prelude") and scaled UP for a very long one (a 2-3 hour
        // recording earns a fuller prelude tier). Both live in the static
        // helper so the length->tier mapping is unit-testable without a model.
        const int wordCount = transcriptText.split(QRegularExpression(QStringLiteral("\\s+")),
                                                     Qt::SkipEmptyParts)
                                   .size();
        const double effectiveWordiness = TranscriptionController::effectiveWordinessForLength(wordiness, wordCount);
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

    // Base-interface pointer so the in-process WhisperCppEngine and an
    // out-of-process SubprocessEngine are swappable behind transcribe() (see
    // DESIGN.md "Engine architecture"). m_modelPath doubles as the reuse key:
    // the ggml file path for whisper, or the "engine:<id>" id for a subprocess
    // engine — either way, an unchanged key skips a reload.
    std::unique_ptr<TranscriptionEngine> m_engine;
    QString m_modelPath;
    std::unique_ptr<LabelingEngine> m_labelingEngine;
    QString m_labelModelPath;
    std::unique_ptr<DiarizationEngine> m_diarizationEngine;
    QString m_diarizationSegPath;
    QString m_diarizationEmbedPath;
    // Part of the warm-engine cache key alongside the two paths — a changed
    // sensitivity must rebuild the engine (threshold is baked into the sherpa
    // config at construction; see DiarizationEngine::buildConfig).
    float m_diarizationThreshold = 0.5f;

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
    // A usbGrab job hands its whole UsbGrabRule to the worker across a QUEUED
    // connection (see startJob()'s dispatch), which copies the argument through
    // the metatype system — without this the invoke fails at RUNTIME, silently
    // doing nothing, rather than at compile time.
    qRegisterMetaType<UsbGrabRule>();

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
        // A combine job's "combining" phase, and (items 26/31) a chapter
        // job's "chaptering" phase, aren't one of recomputeOverallProgress()'s
        // fixed transcribe-job phase buckets (loading/decoding/[detecting-
        // speakers]/transcribing) — each emits a single already-cumulative
        // fraction, so mirror the downloadProgress connection below's direct
        // assignment rather than trying to blend sub-phases.
        if (m_phase == QStringLiteral("combining") || m_phase == QStringLiteral("chaptering")
            || m_phase == QStringLiteral("importing")) {
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
    connect(m_worker, &TranscriptionWorker::usbGrabScanCompleted, this,
            [this](int filesCopied, int filesSkipped, const QStringList &errors, bool cancelled) {
                // Safe to read m_activeIndex here: the worker emits this
                // immediately BEFORE its finished(), and both arrive queued on
                // this (the GUI) thread in that order — so the grab that just
                // ran is still the active job, and onWorkerFinished() hasn't
                // reset m_activeIndex to -1 yet. Attaching the job id here
                // rather than making the worker carry it keeps the worker
                // ignorant of the queue, like every other signal it emits.
                if (m_activeIndex < 0 || m_activeIndex >= m_jobs.size())
                    return;
                emit usbGrabFinished(m_jobs[m_activeIndex].id, filesCopied, filesSkipped, errors, cancelled);
            });
    // Benchmark signals are plain value pass-throughs (worker emits on the
    // worker thread; the queued connection marshals them to the GUI thread).
    connect(m_worker, &TranscriptionWorker::benchmarkModelStarted, this,
            &TranscriptionController::benchmarkModelStarted);
    connect(m_worker, &TranscriptionWorker::benchmarkModelDone, this,
            &TranscriptionController::benchmarkModelDone);
    connect(m_worker, &TranscriptionWorker::benchmarkFinished, this, &TranscriptionController::benchmarkFinished);
    connect(m_worker, &TranscriptionWorker::benchmarkFailed, this, &TranscriptionController::benchmarkFailed);
    connect(m_worker, &TranscriptionWorker::diarizationBenchmarkComboStarted, this,
            &TranscriptionController::diarizationBenchmarkComboStarted);
    connect(m_worker, &TranscriptionWorker::diarizationBenchmarkComboDone, this,
            &TranscriptionController::diarizationBenchmarkComboDone);
    connect(m_worker, &TranscriptionWorker::diarizationBenchmarkFinished, this,
            &TranscriptionController::diarizationBenchmarkFinished);
    // Drive the progress glow off benchmark progress (item: benchmark tasks
    // affect the glow, without touching m_busy). Each *Started advances the
    // fraction; Finished/Failed clears it. These are separate connections from
    // the pass-through ones above, so QML still gets the raw signals too.
    connect(m_worker, &TranscriptionWorker::benchmarkModelStarted, this,
            [this](const QString &, int index, int total) {
                setBenchmarkGlow(true, total > 0 ? double(index) / total : 0.0);
            });
    connect(m_worker, &TranscriptionWorker::benchmarkFinished, this,
            [this](const QVariantMap &) { setBenchmarkGlow(false, 0.0); });
    connect(m_worker, &TranscriptionWorker::benchmarkFailed, this,
            [this](const QString &) { setBenchmarkGlow(false, 0.0); });
    connect(m_worker, &TranscriptionWorker::diarizationBenchmarkComboStarted, this,
            [this](const QString &, int index, int total) {
                setBenchmarkGlow(true, total > 0 ? double(index) / total : 0.0);
            });
    connect(m_worker, &TranscriptionWorker::diarizationBenchmarkFinished, this,
            [this](const QVariantMap &) { setBenchmarkGlow(false, 0.0); });
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
    QStringLiteral("detecting-speech"),   // VAD windowing (every transcribe job)
    QStringLiteral("detecting-speakers"), // diarization (standalone diarize job)
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
// data overrides it; "loading"/"decoding"/"detecting-speech" (VAD) get their
// own modest estimates instead of being folded into that 50/50.
double TranscriptionController::defaultPhaseDurationMs(const QString &phase)
{
    if (phase == QStringLiteral("loading"))
        return 2000.0;
    if (phase == QStringLiteral("decoding"))
        return 1500.0;
    if (phase == QStringLiteral("detecting-speech"))
        return 1500.0; // VAD windowing — a small model + one computeWindows() pass
    return 10000.0; // "detecting-speakers" / "transcribing"
}

double TranscriptionController::phaseRelativeSyntheticProgress() const
{
    // "loading"/"decoding"/"detecting-speech" have no native per-item progress
    // callback (each is one blocking call), so their within-phase fraction is
    // synthesized from elapsed-vs-historical-average; every other phase reports
    // real progress via m_progress.
    if (m_phase != QStringLiteral("loading") && m_phase != QStringLiteral("decoding")
        && m_phase != QStringLiteral("detecting-speech"))
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
    // Empty (idle/relabel/rename/merge) or "downloading"/"combining"/
    // "chaptering" (each gets its own direct m_overallProgress assignment —
    // see the downloadProgress and progress() connections in the constructor)
    // aren't covered by this phase-weighting scheme.
    if (m_phase.isEmpty() || m_phase == QStringLiteral("downloading") || m_phase == QStringLiteral("combining")
        || m_phase == QStringLiteral("chaptering"))
        return;

    // The ordered phase set for whichever kind of job is Active, so each
    // phase gets its rolling-average-proportioned slice of the 0..1 bar:
    //   - Standalone diarize job (m_activeSpeakerDetection): decode then
    //     diarize — no whisper model load, no ASR.
    //   - Transcribe job (pure ASR): load, decode, VAD speech-detection, ASR.
    QStringList active;
    if (m_activeSpeakerDetection)
        active = {QStringLiteral("decoding"), QStringLiteral("detecting-speakers")};
    else
        active = {QStringLiteral("loading"), QStringLiteral("decoding"),
                  QStringLiteral("detecting-speech"), QStringLiteral("transcribing")};

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

bool TranscriptionController::matchesExistingTranscription(const QUrl &fileUrl, const QString &modelId) const
{
    const QVariantMap meta = readMetadata(fileUrl);
    if (meta.isEmpty())
        return false; // not transcribed yet — nothing to match

    // Speaker detection is a separate task (diarizeFile()), not part of what
    // Transcribe produces, so it's not part of this equality check — only the
    // whisper model that actually ran matters.
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
                                          bool autoLabel, bool restrictTagsToExisting,
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

    Job job;
    job.id = m_nextJobId++;
    job.fileUrl = fileUrl;
    job.vaultUrl = vaultUrl;
    job.modelId = modelId;
    job.autoLabel = autoLabel;
    job.restrictTagsToExisting = restrictTagsToExisting;
    job.tagReuseBias = tagReuseBias;
    job.wordiness = wordiness;
    job.status = JobStatus::Queued;
    m_jobs.insert(insertAt, job);
    ++insertAt;

    // Item 23: a transcribe call ONLY transcribes now. It used to chain a
    // second relabel-only job whenever autoLabel was true, so clicking
    // "Transcribe" silently also labeled — the dedicated "Regenerate label"
    // action is the sole way to label. The autoLabel parameter is kept (QML
    // call sites in Sidebar.qml/MainPanel.qml still pass
    // AppSettings.autoLabelEnabled, and this invokable signature must stay
    // call-compatible with them) but is now a NO-OP.
    // TODO (later wave): drop the autoLabel parameter here and the
    // AppSettings.autoLabelEnabled property + its wiring in the QML call
    // sites, once those files are in scope to edit.
    Q_UNUSED(autoLabel);

    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(startIndex);
}

void TranscriptionController::transcribeAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                             const QString &modelId, bool autoLabel,
                                             bool restrictTagsToExisting, double tagReuseBias, double wordiness)
{
    for (const QVariant &v : fileUrls)
        transcribe(v.toUrl(), vaultUrl, modelId, autoLabel, restrictTagsToExisting, tagReuseBias,
                   wordiness);
}

void TranscriptionController::importAudio(const QUrl &sourceUrl, const QUrl &destUrl, const QUrl &vaultUrl)
{
    // Same Queued/Active dedup rule as transcribe(), keyed on the destination
    // (two drops resolving to the same in-vault file don't double-enqueue).
    if (isQueued(destUrl))
        return;

    // Item O15: jump ahead of whatever's already Queued — see transcribe()'s
    // frontOfQueueInsertIndex() use. No download job to queue first (a copy
    // needs no model), so the import job itself is the front-most new entry.
    const int insertAt = frontOfQueueInsertIndex();

    Job job;
    job.id = m_nextJobId++;
    job.fileUrl = destUrl; // the in-vault destination (see Job::importOnly)
    job.vaultUrl = vaultUrl;
    job.importOnly = true;
    job.importSourcePath = sourceUrl.toLocalFile();
    job.status = JobStatus::Queued;
    m_jobs.insert(insertAt, job);

    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(insertAt);
}

int TranscriptionController::enqueueUsbGrab(const UsbGrabRule &rule, const QString &mountPath, const QUrl &destUrl,
                                             const QUrl &vaultUrl, bool autoTriggered)
{
    // No isQueued(destUrl) dedup here, deliberately — see the header's doc
    // comment: a grab's fileUrl is a destination FOLDER that a device's two
    // volumes legitimately share.
    const int insertAt = frontOfQueueInsertIndex();

    Job job;
    job.id = m_nextJobId++;
    job.fileUrl = destUrl; // the destination folder (see Job::usbGrabOnly)
    job.vaultUrl = vaultUrl;
    job.usbGrabOnly = true;
    job.usbGrabRule = rule;
    job.usbGrabMountPath = mountPath;
    job.usbGrabAuto = autoTriggered;
    job.status = JobStatus::Queued;
    m_jobs.insert(insertAt, job);

    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(insertAt);

    return job.id;
}

void TranscriptionController::diarizeFile(const QUrl &fileUrl, const QUrl &vaultUrl,
                                          const QString &segmentationModelId, const QString &embeddingModelId,
                                          double sensitivity)
{
    // Same dedup rule as transcribe()/relabel(): a Queued/Active entry blocks
    // a duplicate request.
    if (isQueued(fileUrl))
        return;

    // Empty id ⇒ the ModelManager default (keeps the Job's recorded ids
    // concrete for persistence + the download dedup key).
    const QString segId = segmentationModelId.isEmpty()
                              ? QString::fromLatin1(ModelManager::kDefaultSegmentationModelId)
                              : segmentationModelId;
    const QString embedId = embeddingModelId.isEmpty()
                                ? QString::fromLatin1(ModelManager::kDefaultEmbeddingModelId)
                                : embeddingModelId;

    // Item O15: jump ahead of whatever's already Queued — see transcribe()'s
    // frontOfQueueInsertIndex() use. The CHOSEN segmentation + embedding models
    // are each queued as their own download job ahead of this one if not
    // already on disk (same pattern the runAllJobs() diarize step uses).
    int insertAt = frontOfQueueInsertIndex();
    const int startIndex = insertAt;
    if (maybeQueueDownloadJob(DownloadTarget::SegmentationModel, vaultUrl, segId, insertAt) >= 0)
        ++insertAt;
    if (maybeQueueDownloadJob(DownloadTarget::EmbeddingModel, vaultUrl, embedId, insertAt) >= 0)
        ++insertAt;

    Job job;
    job.id = m_nextJobId++;
    job.fileUrl = fileUrl;
    job.vaultUrl = vaultUrl;
    job.diarizeOnly = true;
    job.segmentationModelId = segId;
    job.embeddingModelId = embedId;
    job.diarizationThreshold = sensitivity;
    job.status = JobStatus::Queued;
    m_jobs.insert(insertAt, job);
    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(startIndex);
}

void TranscriptionController::diarizeFilesForPaths(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                                   const QString &segmentationModelId,
                                                   const QString &embeddingModelId, double sensitivity)
{
    for (const QVariant &v : fileUrls)
        diarizeFile(v.toUrl(), vaultUrl, segmentationModelId, embeddingModelId, sensitivity);
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

void TranscriptionController::generateChapters(const QUrl &fileUrl, const QUrl &vaultUrl, double density)
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
    job.chapterDensity = density;
    job.status = JobStatus::Queued;
    m_jobs.insert(insertAt, job);
    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(startIndex);
}

void TranscriptionController::generateChaptersAll(const QVariantList &fileUrls, const QUrl &vaultUrl, double density)
{
    for (const QVariant &v : fileUrls)
        generateChapters(v.toUrl(), vaultUrl, density);
}

void TranscriptionController::runAllJobs(const QUrl &fileUrl, const QUrl &vaultUrl, const QString &modelId,
                                          bool restrictTagsToExisting, double tagReuseBias,
                                          double wordiness, double chapterDensity, const QString &segmentationModelId,
                                          const QString &embeddingModelId, double sensitivity)
{
    // See this method's header doc comment for the full ordering /
    // already-transcribed-skip / dedup contract. Same Queued/Active dedup gate
    // as the individual actions — checked once, up front, so re-entrancy from
    // the inserts below (which make fileUrl "queued") can't matter.
    if (isQueued(fileUrl))
        return;

    // Empty ids ⇒ ModelManager defaults (see diarizeFile()).
    const QString segId = segmentationModelId.isEmpty()
                              ? QString::fromLatin1(ModelManager::kDefaultSegmentationModelId)
                              : segmentationModelId;
    const QString embedId = embeddingModelId.isEmpty()
                                ? QString::fromLatin1(ModelManager::kDefaultEmbeddingModelId)
                                : embeddingModelId;

    // Skip the transcribe step for a file that already has a sibling .md (the
    // friendly, least-surprising "run all" on an already-done file — just
    // re-detect speakers and refresh its label+chapters; see header doc
    // comment). An orphaned .md's sidecarPath() is itself, so this is true for
    // those too.
    const bool alreadyTranscribed = QFileInfo::exists(sidecarPath(fileUrl.toLocalFile()));

    // Item O15 (same as every other enqueue path): jump ahead of whatever's
    // already Queued, but keep this file's jobs CONTIGUOUS and IN ORDER by
    // walking insertAt forward as each is inserted (rather than each new job
    // re-jumping to the very front — which is exactly why this doesn't just
    // call transcribe()/diarizeFile()/relabel()/generateChapters() in sequence).
    int insertAt = frontOfQueueInsertIndex();
    const int startIndex = insertAt;

    // Models this pipeline needs, queued ahead as their own jobs if absent
    // (mirrors transcribe()/diarizeFile()/relabel()). maybeQueueDownloadJob()
    // dedups, so the single LabelModel request here covers BOTH the later label
    // and chapters jobs. The diarization models are needed regardless of
    // alreadyTranscribed (the diarize step always runs); the whisper model only
    // when transcription itself runs.
    if (!alreadyTranscribed) {
        if (maybeQueueDownloadJob(DownloadTarget::WhisperModel, vaultUrl, modelId, insertAt) >= 0)
            ++insertAt;
    }
    if (maybeQueueDownloadJob(DownloadTarget::SegmentationModel, vaultUrl, segId, insertAt) >= 0)
        ++insertAt;
    if (maybeQueueDownloadJob(DownloadTarget::EmbeddingModel, vaultUrl, embedId, insertAt) >= 0)
        ++insertAt;
    if (maybeQueueDownloadJob(DownloadTarget::LabelModel, vaultUrl, QString(), insertAt) >= 0)
        ++insertAt;

    // (1) transcribe (pure ASR) — skipped if already transcribed.
    if (!alreadyTranscribed) {
        Job job;
        job.id = m_nextJobId++;
        job.fileUrl = fileUrl;
        job.vaultUrl = vaultUrl;
        job.modelId = modelId;
        job.restrictTagsToExisting = restrictTagsToExisting;
        job.tagReuseBias = tagReuseBias;
        job.wordiness = wordiness;
        job.status = JobStatus::Queued;
        m_jobs.insert(insertAt, job);
        ++insertAt;
    }

    // (2) detect speakers — reads the .md the transcribe job (if any) just
    // wrote and splices "[Speaker N]" tags in (see diarizeFile()). This is the
    // step that replaced transcribe()'s old inline speakerDetection toggle.
    {
        Job job;
        job.id = m_nextJobId++;
        job.fileUrl = fileUrl;
        job.vaultUrl = vaultUrl;
        job.diarizeOnly = true;
        job.segmentationModelId = segId;
        job.embeddingModelId = embedId;
        job.diarizationThreshold = sensitivity;
        job.status = JobStatus::Queued;
        m_jobs.insert(insertAt, job);
        ++insertAt;
    }

    // (3) label — reads the .md the diarize job just updated.
    {
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
        ++insertAt;
    }

    // (4) chapters — reads the labeled .md.
    {
        Job job;
        job.id = m_nextJobId++;
        job.fileUrl = fileUrl;
        job.vaultUrl = vaultUrl;
        job.generateChaptersOnly = true;
        job.chapterDensity = chapterDensity;
        job.status = JobStatus::Queued;
        m_jobs.insert(insertAt, job);
        ++insertAt;
    }

    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(startIndex);
}

void TranscriptionController::runAllJobsForPaths(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                                  const QString &modelId,
                                                  bool restrictTagsToExisting, double tagReuseBias, double wordiness,
                                                  double chapterDensity, const QString &segmentationModelId,
                                                  const QString &embeddingModelId, double sensitivity)
{
    for (const QVariant &v : fileUrls)
        runAllJobs(v.toUrl(), vaultUrl, modelId, restrictTagsToExisting, tagReuseBias, wordiness, chapterDensity,
                   segmentationModelId, embeddingModelId, sensitivity);
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
    // See recomputeOverallProgress()'s doc comment — true ONLY for the
    // standalone "diarize" job, whose progress bar is weighted around the
    // detecting-speakers phase. A "transcribe" job is pure ASR now (its bar is
    // weighted around loading/decoding/detecting-speech/transcribing instead).
    // Every other kind never emits phase() at all, so this is simply never
    // read for them.
    m_activeSpeakerDetection = job.diarizeOnly;
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
                                   Q_ARG(QString, job.vaultUrl.toLocalFile()), Q_ARG(double, job.chapterDensity));
    } else if (job.diarizeOnly) {
        QMetaObject::invokeMethod(m_worker, "diarizeJob", Qt::QueuedConnection,
                                   Q_ARG(QString, job.fileUrl.toLocalFile()),
                                   Q_ARG(QString, job.vaultUrl.toLocalFile()),
                                   Q_ARG(QString, job.segmentationModelId), Q_ARG(QString, job.embeddingModelId),
                                   Q_ARG(double, job.diarizationThreshold));
    } else if (job.combineOnly) {
        QMetaObject::invokeMethod(m_worker, "combineJob", Qt::QueuedConnection,
                                   Q_ARG(QStringList, job.combineSourcePaths),
                                   Q_ARG(QString, job.fileUrl.toLocalFile()),
                                   Q_ARG(bool, job.deleteOriginalsAfterCombine));
    } else if (job.usbGrabOnly) {
        QMetaObject::invokeMethod(m_worker, "usbGrabJob", Qt::QueuedConnection, Q_ARG(UsbGrabRule, job.usbGrabRule),
                                   Q_ARG(QString, job.usbGrabMountPath),
                                   Q_ARG(QString, job.fileUrl.toLocalFile()));
    } else if (job.importOnly) {
        QMetaObject::invokeMethod(m_worker, "importJob", Qt::QueuedConnection,
                                   Q_ARG(QString, job.importSourcePath),
                                   Q_ARG(QString, job.fileUrl.toLocalFile()));
    } else if (job.downloadTarget != DownloadTarget::None) {
        QString target;
        switch (job.downloadTarget) {
        case DownloadTarget::WhisperModel: target = QStringLiteral("model"); break;
        case DownloadTarget::LabelModel: target = QStringLiteral("label"); break;
        case DownloadTarget::VadModel: target = QStringLiteral("vad"); break;
        case DownloadTarget::SegmentationModel: target = QStringLiteral("segmentation"); break;
        case DownloadTarget::EmbeddingModel: target = QStringLiteral("embedding"); break;
        case DownloadTarget::None: break;
        }
        QMetaObject::invokeMethod(m_worker, "download", Qt::QueuedConnection, Q_ARG(QString, target),
                                   Q_ARG(QString, job.modelId));
    } else {
        QMetaObject::invokeMethod(m_worker, "run", Qt::QueuedConnection,
                                   Q_ARG(QString, job.fileUrl.toLocalFile()),
                                   Q_ARG(QString, job.vaultUrl.toLocalFile()), Q_ARG(QString, job.modelId));
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
            // A predownload (or lazy download) that just landed changes the
            // set of downloaded models — let the Models page refresh.
            if (ok && job.downloadTarget != DownloadTarget::None)
                emit modelsChanged();
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
    // Items 30/50: "chapters" is pausable too — generateChaptersJob() checks
    // waitWhilePausedOrCancelled() between windows and around the merge pass
    // (an in-flight single LLM call still can't be interrupted, but between
    // windows it's honored promptly).
    if (kind != QStringLiteral("transcribe") && kind != QStringLiteral("download")
        && kind != QStringLiteral("combine") && kind != QStringLiteral("chapters"))
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
    // Item 9 (2026-07-14): "loading" (whisper.cpp's blocking, unhookable
    // model-load call) has no cooperative checkpoint to pause AT. It used to
    // silently reinterpret the tap as cancelActive() + present the job as
    // Paused-via-restart — the "pause broken again" report: a click that
    // looked like it did nothing, and could latch a stray cancel that broke
    // pause for the rest of the job. That reinterpretation is gone: the
    // bottom bar now DISABLES the pause button during "loading" (see
    // BottomBar.qml's pauseButton.enabled) so this is never reached from the
    // UI during that phase, and pauseActive() below is a safe no-op for
    // "loading" if it somehow is. A pause is never turned into a cancel here.
    // (NOTE for a later wave — items 30/50: a genuine mid-loading pause would
    // need a real interruption hook that whisper.cpp doesn't expose today;
    // the JobStatus::Paused/activeStopping machinery is left dormant for
    // whenever that lands, not removed.)
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
            && ((target != DownloadTarget::WhisperModel && target != DownloadTarget::SegmentationModel
                 && target != DownloadTarget::EmbeddingModel)
                || job.modelId == modelId)
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
        alreadyOnDisk = ModelManager::isModelDownloaded(modelId);
        break;
    case DownloadTarget::LabelModel:
        alreadyOnDisk = ModelManager::isLabelModelDownloaded();
        break;
    case DownloadTarget::VadModel:
        alreadyOnDisk = ModelManager::isVadModelDownloaded();
        break;
    case DownloadTarget::SegmentationModel:
        alreadyOnDisk = ModelManager::isSegmentationModelDownloaded(modelId);
        break;
    case DownloadTarget::EmbeddingModel:
        alreadyOnDisk = ModelManager::isEmbeddingModelDownloaded(modelId);
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
        // The containing folder, part of the group identity below. A rule
        // matches on bare filename, so without this two same-named parts in
        // different folders would group together — which is never what the
        // user means (they're separate recordings that happen to share a
        // naming scheme), and the tree can't draw one outline around rows
        // that aren't siblings anyway.
        QString dir;
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
                candidates.append(Candidate{path, ruleName, info.absolutePath(), *key, capturedValues,
                                            sortValue, info.lastModified()});
        }
    }

    QMap<std::tuple<QString, QString, QString>, QList<Candidate>> buckets;
    for (const Candidate &c : candidates)
        buckets[std::make_tuple(c.ruleName, c.dir, c.key)].append(c);

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
        group.ruleName = std::get<0>(it.key());
        group.key = std::get<2>(it.key());
        group.capturedValues = members.first().capturedValues;
        for (const Candidate &c : members)
            group.files << c.path;

        // Every member shares the bucket's folder by construction, so the
        // merged file lands beside its own inputs rather than in whichever
        // folder happened to hold the first-in-combine-order member.
        group.outputPath = combineOutputPathFor(group.ruleName, group.key, std::get<1>(it.key()),
                                                QFileInfo(members.first().path).suffix());

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
    // outputPath is the tiebreaker, not decoration: (ruleName, key) alone
    // stopped being unique once folder joined the group identity — the same
    // rule and key in two folders are now two distinct groups.
    std::sort(groups.begin(), groups.end(), [](const CombineGroup &a, const CombineGroup &b) {
        if (a.ruleName != b.ruleName)
            return a.ruleName < b.ruleName;
        if (a.key != b.key)
            return a.key < b.key;
        return a.outputPath < b.outputPath;
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
    return regexValidationErrorFor(pattern);
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
        || job.mergeSpeakersOnly || job.importOnly || job.diarizeOnly
        || job.downloadTarget != DownloadTarget::None || job.fileUrl.isEmpty())
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

        // Item 37: import jobs are deliberately NOT persisted across a restart
        // — importSourcePath names an EXTERNAL file (often on removable media)
        // that may well be gone next session, and a half-done copy left a
        // partial file the collision check would then refuse. A drag-and-drop
        // import is inherently a live, in-session action; if it didn't finish
        // before close, the user simply drags the file in again.
        if (job.importOnly)
            continue;

        // USB grabs are not persisted, for a STRONGER version of the same
        // reason: usbGrabMountPath is a mount point for removable media that is
        // very likely unplugged by next session, and a mount point that gets
        // reused by a DIFFERENT volume would make a restored job walk the wrong
        // device entirely. A grab is inherently tied to the device being
        // present — replugging it re-fires the auto-grab (or the user clicks
        // Grab now), which is the correct way to resume one.
        if (job.usbGrabOnly)
            continue;

        QJsonObject obj;
        obj["kind"] = jobKind(job);
        obj["fileUrl"] = job.fileUrl.toString();
        obj["vaultUrl"] = job.vaultUrl.toString();
        obj["modelId"] = job.modelId;
        obj["restrictTagsToExisting"] = job.restrictTagsToExisting;
        obj["tagReuseBias"] = job.tagReuseBias;
        obj["wordiness"] = job.wordiness;
        obj["chapterDensity"] = job.chapterDensity;
        obj["segmentationModelId"] = job.segmentationModelId;
        obj["embeddingModelId"] = job.embeddingModelId;
        obj["diarizationThreshold"] = job.diarizationThreshold;
        obj["oldSpeakerName"] = job.oldSpeakerName;
        obj["newSpeakerName"] = job.newSpeakerName;
        obj["sourceSpeakerName"] = job.sourceSpeakerName;
        obj["targetSpeakerName"] = job.targetSpeakerName;
        obj["combineSourcePaths"] = QJsonArray::fromStringList(job.combineSourcePaths);
        obj["deleteOriginalsAfterCombine"] = job.deleteOriginalsAfterCombine;
        switch (job.downloadTarget) {
        case DownloadTarget::WhisperModel: obj["downloadTarget"] = QStringLiteral("model"); break;
        case DownloadTarget::LabelModel: obj["downloadTarget"] = QStringLiteral("label"); break;
        case DownloadTarget::VadModel: obj["downloadTarget"] = QStringLiteral("vad"); break;
        case DownloadTarget::SegmentationModel: obj["downloadTarget"] = QStringLiteral("segmentation"); break;
        case DownloadTarget::EmbeddingModel: obj["downloadTarget"] = QStringLiteral("embedding"); break;
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
            else if (target == QStringLiteral("vad"))
                dt = DownloadTarget::VadModel;
            else if (target == QStringLiteral("segmentation"))
                dt = DownloadTarget::SegmentationModel;
            else if (target == QStringLiteral("embedding"))
                dt = DownloadTarget::EmbeddingModel;
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
    } else if (kind == QStringLiteral("diarize")) {
        // Item 52: a standalone diarize job needs only its chosen diarization
        // models (segmentation + embedding), no whisper model. Empty ids ⇒
        // defaults (an entry persisted before this became selectable).
        const QString segId = obj.value("segmentationModelId").toString().isEmpty()
                                  ? QString::fromLatin1(ModelManager::kDefaultSegmentationModelId)
                                  : obj.value("segmentationModelId").toString();
        const QString embedId = obj.value("embeddingModelId").toString().isEmpty()
                                    ? QString::fromLatin1(ModelManager::kDefaultEmbeddingModelId)
                                    : obj.value("embeddingModelId").toString();
        maybeQueueDownloadJob(DownloadTarget::SegmentationModel, jobVaultUrl, segId);
        maybeQueueDownloadJob(DownloadTarget::EmbeddingModel, jobVaultUrl, embedId);
    } else if (kind != QStringLiteral("rename") && kind != QStringLiteral("merge")
               && kind != QStringLiteral("combine")) {
        // "transcribe" — pure ASR now (a fresh transcribe is speaker-less; see
        // DESIGN.md "Speaker diarization"), so no diarization models here.
        const QString modelId = obj.value("modelId").toString();
        maybeQueueDownloadJob(DownloadTarget::WhisperModel, jobVaultUrl, modelId);
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
        job.chapterDensity = obj.value("chapterDensity").toDouble(0.5);
    } else if (kind == QStringLiteral("diarize")) {
        job.diarizeOnly = true;
        job.segmentationModelId = obj.value("segmentationModelId").toString();
        job.embeddingModelId = obj.value("embeddingModelId").toString();
        job.diarizationThreshold = obj.value("diarizationThreshold").toDouble(0.5);
    } else if (kind == QStringLiteral("combine")) {
        job.combineOnly = true;
        for (const QJsonValue &v : obj.value("combineSourcePaths").toArray())
            job.combineSourcePaths << v.toString();
        job.deleteOriginalsAfterCombine = obj.value("deleteOriginalsAfterCombine").toBool();
    } else {
        job.modelId = obj.value("modelId").toString();
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
    // doc comment. A listener is expected to call
    // VaultTreeModel::refreshFile(fileUrl) from this signal (see its doc
    // comment for why rebuild()/the QFileSystemWatcher alone never catches
    // an in-place rewrite of a file that already existed) rather than wait
    // for the next unrelated directory event.
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
    // should refresh the same way a label edit does — see setLabel()'s
    // matching emit for why (VaultTreeModel::refreshFile(fileUrl)).
    emit finished(fileUrl, true, QString());
    return true;
}

bool TranscriptionController::setCollapsedChapters(const QUrl &fileUrl, const QStringList &chapterKeys, QString *errorOut) const
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

    // Mirror setLabel()/setTags()'s frontmatter-scanning shape.
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

    // Empty list REMOVES the key (no blank `collapsedChapters:` left behind);
    // a non-empty list upserts it with the comma-joined keys.
    const QString joined = chapterKeys.join(QLatin1Char(','));
    bool replaced = false;
    for (int i = 0; i < frontmatterLines.size(); ++i) {
        const int colon = frontmatterLines[i].indexOf(':');
        if (colon >= 0 && frontmatterLines[i].left(colon).trimmed() == QStringLiteral("collapsedChapters")) {
            if (joined.isEmpty())
                frontmatterLines.removeAt(i);
            else
                frontmatterLines[i] = QStringLiteral("collapsedChapters: ") + joined;
            replaced = true;
            break;
        }
    }
    if (!replaced && !joined.isEmpty())
        frontmatterLines.append(QStringLiteral("collapsedChapters: ") + joined);

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
    // No finished() emit — see the header doc comment (collapse is pure view
    // state, must not trigger a metadata/transcript reload on every toggle).
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
    // Item P19: see setLabel()'s matching emit for why
    // (VaultTreeModel::refreshFile(fileUrl)).
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
        // Item 17: the manual sentinel is stripped from the DISPLAYED title
        // but its presence is surfaced as a "manual" flag (so the TOC can
        // show manual chapters distinctly / know editing an auto one promotes
        // it). Plain (auto) titles pass through unchanged.
        chapter["title"] = chapterDisplayTitle(m.captured(2));
        chapter["manual"] = chapterTitleCaptureIsManual(m.captured(2));
        chapters.append(chapter);
    }
    return chapters;
}

bool TranscriptionController::editChapterMarker(const QUrl &fileUrl, ChapterEditOp op, double timestampSeconds,
                                                 const QString &title, QString *errorOut) const
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
    QString body = in.readAll();
    mdFile.close();

    const QString tsText = formatTimestamp(timestampSeconds);
    switch (op) {
    case ChapterEditOp::Add: {
        // Replace any existing marker at this exact timestamp, then insert the
        // manual one in sorted position — so a re-add is idempotent and always
        // ends up manual.
        QRegularExpressionMatch existing;
        if (findChapterMarkerAt(body, timestampSeconds, &existing))
            removeChapterMarkerLine(&body, existing);
        const int offset = chapterInsertOffset(body, timestampSeconds);
        body.insert(offset, formatChapterMarker(tsText, title, /*manual=*/true) + QLatin1Char('\n'));
        break;
    }
    case ChapterEditOp::SetTitle: {
        QRegularExpressionMatch target;
        if (!findChapterMarkerAt(body, timestampSeconds, &target)) {
            if (errorOut)
                *errorOut = QStringLiteral("no chapter at %1").arg(tsText);
            return false;
        }
        // Editing an auto chapter's title PROMOTES it to manual (always write
        // the sentinel) so the next regeneration won't clobber the edit —
        // least-surprising. Reuse the marker's OWN stored timestamp text.
        body.replace(target.capturedStart(0), target.capturedLength(0),
                     formatChapterMarker(target.captured(1), title, /*manual=*/true));
        break;
    }
    case ChapterEditOp::Delete: {
        QRegularExpressionMatch target;
        if (!findChapterMarkerAt(body, timestampSeconds, &target)) {
            if (errorOut)
                *errorOut = QStringLiteral("no chapter at %1").arg(tsText);
            return false;
        }
        removeChapterMarkerLine(&body, target);
        break;
    }
    }

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

void TranscriptionController::addChapter(const QUrl &fileUrl, double timestampSeconds, const QString &title)
{
    QString error;
    const bool ok = editChapterMarker(fileUrl, ChapterEditOp::Add, timestampSeconds, title, &error);
    // Mirror setPrelude()/setLabel(): notify listeners so the sidebar/main
    // panel refresh this file's chapters immediately (item P19).
    emit finished(fileUrl, ok, error);
}

void TranscriptionController::setChapterTitle(const QUrl &fileUrl, double timestampSeconds, const QString &newTitle)
{
    QString error;
    const bool ok = editChapterMarker(fileUrl, ChapterEditOp::SetTitle, timestampSeconds, newTitle, &error);
    emit finished(fileUrl, ok, error);
}

void TranscriptionController::deleteChapter(const QUrl &fileUrl, double timestampSeconds)
{
    QString error;
    const bool ok = editChapterMarker(fileUrl, ChapterEditOp::Delete, timestampSeconds, QString(), &error);
    emit finished(fileUrl, ok, error);
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
    // See the header doc comment: GUI thread only, cached per file because
    // QML calls this from a binding on every keystroke.
    return collectVaultTags(vaultUrl.toLocalFile(), &m_vaultTagFileCache);
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
    if (job.diarizeOnly)
        return QStringLiteral("diarize");
    if (job.combineOnly)
        return QStringLiteral("combine");
    if (job.usbGrabOnly)
        return QStringLiteral("usbGrab");
    if (job.importOnly)
        return QStringLiteral("import");
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
        // Ordered source parts of a "combine" job (empty for every other kind)
        // — the exact concatenation order, so a caller/test can see WHICH files
        // this merge covers and in what order. See enqueueCombineJob().
        entry["combineSourcePaths"] = job.combineSourcePaths;
        // -1 (QML sees it as a negative number) until Done/Failed — see
        // Job::durationMs's doc comment.
        entry["durationMs"] = job.durationMs;
        // modelId is only meaningful for a "download" kind entry targeting
        // WhisperModel (empty otherwise) — fileUrl is empty for every
        // download job (nothing to open), so the Tasks popup needs SOME
        // name to show; modelId (or a fixed label for the other two
        // download targets, which have no modelId concept) is that name.
        entry["modelId"] = job.modelId;
        // Diarization tuning carried by a "diarize" kind entry (empty/0.5 for
        // every other kind) — lets the Tasks UI / tests see which models + which
        // sensitivity a queued speaker-detection job will use.
        entry["segmentationModelId"] = job.segmentationModelId;
        entry["embeddingModelId"] = job.embeddingModelId;
        entry["diarizationThreshold"] = job.diarizationThreshold;
        switch (job.downloadTarget) {
        case DownloadTarget::WhisperModel: entry["downloadTarget"] = QStringLiteral("model"); break;
        case DownloadTarget::LabelModel: entry["downloadTarget"] = QStringLiteral("label"); break;
        case DownloadTarget::VadModel: entry["downloadTarget"] = QStringLiteral("vad"); break;
        case DownloadTarget::SegmentationModel: entry["downloadTarget"] = QStringLiteral("segmentation"); break;
        case DownloadTarget::EmbeddingModel: entry["downloadTarget"] = QStringLiteral("embedding"); break;
        case DownloadTarget::None: entry["downloadTarget"] = QString(); break;
        }
        list.append(entry);
    }
    return list;
}

QString TranscriptionController::backendKind(const QString &backendName)
{
    if (backendName.isEmpty())
        return QString();
    if (backendName.compare(QStringLiteral("CPU"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("cpu");
    return QStringLiteral("gpu");
}

double TranscriptionController::effectiveWordinessForLength(double baseWordiness, int wordCount)
{
    // Coarse tiers, not a precise formula — this only needs to keep the prelude
    // PROPORTIONATE to the transcript, not fine-tune prose length. Downward
    // caps (item P29): a tiny transcript never gets a disproportionately long
    // prelude no matter how high the slider. Upward floors (item 58): a long
    // transcript is ALLOWED a fuller prelude even at a modest slider setting —
    // a 2-3 hour recording (roughly 20k+ words at ~150 wpm) reaches
    // labelPreludePhrasing()'s top >=0.9 "walk through each major topic" tier.
    if (wordCount < 50)
        return std::min(baseWordiness, 0.15);
    if (wordCount < 150)
        return std::min(baseWordiness, 0.35);
    if (wordCount > 20000)
        return std::max(baseWordiness, 0.9);
    if (wordCount > 6000)
        return std::max(baseWordiness, 0.75);
    if (wordCount > 2000)
        return std::max(baseWordiness, 0.6);
    return baseWordiness;
}

QVariantList TranscriptionController::sortJobsForDisplay(const QVariantList &jobEntries)
{
    QVariantList active;
    QVariantList queued;
    QVariantList paused;
    QVariantList finished;
    for (const QVariant &v : jobEntries) {
        const QString status = v.toMap().value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("active"))
            active.append(v);
        else if (status == QStringLiteral("queued"))
            queued.append(v);
        else if (status == QStringLiteral("paused"))
            paused.append(v);
        else // "done" / "failed" (a cancel is a Failed/"Cancelled" — still finished)
            finished.append(v);
    }
    // Finished group: most-recently-finished first. jobs() is append order and
    // jobs run/finish in that order, so reversing this group alone gives
    // newest-finished-on-top without needing a separate finishedAt timestamp.
    std::reverse(finished.begin(), finished.end());
    QVariantList out;
    out.reserve(jobEntries.size());
    out += active;
    out += queued;
    out += paused;
    out += finished;
    return out;
}

QVariantList TranscriptionController::sortedJobs() const
{
    return sortJobsForDisplay(jobs());
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
        entry["downloaded"] = ModelManager::isModelDownloaded(info.id);
        list.append(entry);
    }
    return list;
}

QVariantList TranscriptionController::availableSegmentationModels() const
{
    QVariantList list;
    for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableSegmentationModels()) {
        QVariantMap entry;
        entry["id"] = info.id;
        entry["label"] = info.label;
        entry["downloaded"] = ModelManager::isSegmentationModelDownloaded(info.id);
        entry["nonCommercial"] = info.nonCommercial;
        list.append(entry);
    }
    return list;
}

QVariantList TranscriptionController::availableEmbeddingModels() const
{
    QVariantList list;
    for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableEmbeddingModels()) {
        QVariantMap entry;
        entry["id"] = info.id;
        entry["label"] = info.label;
        entry["downloaded"] = ModelManager::isEmbeddingModelDownloaded(info.id);
        entry["nonCommercial"] = info.nonCommercial;
        list.append(entry);
    }
    return list;
}

namespace {

// Human-readable file size for the Models page (SI-ish, matches the "~148 MB"
// style used in the model labels).
QString humanSize(qint64 bytes)
{
    if (bytes <= 0)
        return QString();
    const double kb = bytes / 1024.0;
    if (kb < 1024.0)
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 0);
    const double mb = kb / 1024.0;
    if (mb < 1024.0)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', mb < 10 ? 1 : 0);
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
}

QVariantMap manageableEntry(const QString &id, const QString &label, const QString &kind, bool downloaded)
{
    QVariantMap entry;
    entry["id"] = id;
    entry["label"] = label;
    entry["kind"] = kind;
    entry["downloaded"] = downloaded;
    entry["bundledOnly"] = ModelManager::isModelBundledOnly(id);
    // Import shape: only the parakeet bundle comes from a FOLDER; every other
    // model (including each single-file diarization segmentation/embedding
    // model) from a single FILE. Lets the page pick the right native dialog.
    entry["multiFile"] = (id == QLatin1String(ModelManager::kParakeetModelId));
    const qint64 size = ModelManager::modelSizeOnDisk(id);
    entry["sizeBytes"] = static_cast<double>(size);
    entry["sizeText"] = humanSize(size);
    return entry;
}

} // namespace

QVariantList TranscriptionController::manageableModels() const
{
    QVariantList list;
    // Transcription models — the dropdown tiers + parakeet.
    for (const ModelManager::ModelInfo &info : ModelManager::availableModels()) {
        list.append(manageableEntry(info.id, info.label, QStringLiteral("transcription"),
                                     ModelManager::isModelDownloaded(info.id)));
    }
    // Support models — no dropdown entry, keyed by their pseudo-ids.
    list.append(manageableEntry(QString::fromLatin1(ModelManager::kLabelModelId),
                                tr("AI auto-labeling (Qwen3-4B) — ~2.5 GB"), QStringLiteral("support"),
                                ModelManager::isLabelModelDownloaded()));
    list.append(manageableEntry(QString::fromLatin1(ModelManager::kVadModelId),
                                tr("Voice-activity detection (Silero VAD) — ~2 MB"), QStringLiteral("support"),
                                ModelManager::isVadModelDownloaded()));
    // Diarization is now a per-model set (selectable segmentation + embedding),
    // each its own downloadable/deletable entry rather than one lumped pair.
    for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableSegmentationModels()) {
        list.append(manageableEntry(info.id, tr("Speaker segmentation — %1").arg(info.label),
                                    QStringLiteral("support"), ModelManager::isSegmentationModelDownloaded(info.id)));
    }
    for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableEmbeddingModels()) {
        list.append(manageableEntry(info.id, tr("Speaker embedding — %1").arg(info.label),
                                    QStringLiteral("support"), ModelManager::isEmbeddingModelDownloaded(info.id)));
    }
    return list;
}

void TranscriptionController::downloadModel(const QString &modelId)
{
    DownloadTarget target = DownloadTarget::WhisperModel; // whisper tiers + parakeet
    if (modelId == QLatin1String(ModelManager::kLabelModelId)) {
        target = DownloadTarget::LabelModel;
    } else if (modelId == QLatin1String(ModelManager::kVadModelId)) {
        target = DownloadTarget::VadModel;
    } else {
        for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableSegmentationModels()) {
            if (info.id == modelId) {
                target = DownloadTarget::SegmentationModel;
                break;
            }
        }
        for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableEmbeddingModels()) {
            if (info.id == modelId) {
                target = DownloadTarget::EmbeddingModel;
                break;
            }
        }
    }

    // Same queue path used lazily before a transcribe — inserted at the tail so
    // it doesn't jump ahead of in-flight work. No-op if already downloaded or
    // already queued (maybeQueueDownloadJob returns -1).
    if (maybeQueueDownloadJob(target, m_vaultUrl, modelId) >= 0) {
        emit queueChanged();
        startNext();
    }
}

void TranscriptionController::deleteModel(const QString &modelId)
{
    QString error;
    const bool ok = ModelManager::deleteModel(modelId, &error);
    emit modelOperationFinished(QStringLiteral("delete"), modelId, ok, error);
    if (ok)
        emit modelsChanged();
}

void TranscriptionController::importModel(const QString &modelId, const QUrl &sourceUrl)
{
    const QString sourcePath = sourceUrl.toLocalFile();
    // Copy (possibly multi-GB) off the GUI thread — same fire-and-forget
    // QThread pattern as loadTranscriptAsync(); result marshaled back.
    QThread *thread = QThread::create([this, modelId, sourcePath]() {
        QString error;
        const bool ok = ModelManager::importModel(modelId, sourcePath, &error);
        QMetaObject::invokeMethod(
            this,
            [this, modelId, ok, error]() {
                emit modelOperationFinished(QStringLiteral("import"), modelId, ok, error);
                if (ok)
                    emit modelsChanged();
            },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void TranscriptionController::exportModel(const QString &modelId, const QUrl &destDirUrl)
{
    const QString destDir = destDirUrl.toLocalFile();
    QThread *thread = QThread::create([this, modelId, destDir]() {
        QString error;
        const bool ok = ModelManager::exportModel(modelId, destDir, &error);
        QMetaObject::invokeMethod(
            this,
            [this, modelId, ok, error]() { emit modelOperationFinished(QStringLiteral("export"), modelId, ok, error); },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void TranscriptionController::runBenchmark(const QUrl &fileUrl, const QStringList &modelIds)
{
    // Dispatched onto the worker thread's event loop — serializes behind any
    // in-flight job (no GPU contention), then runs benchmarkJob(). No queue
    // entry, no file writes; results arrive via the benchmark* signals. Uses
    // the currently-open vault, same as availableModels()/availableEngines().
    // Light the progress glow immediately (before the first model-started
    // signal) so it doesn't lag the run kicking off.
    setBenchmarkGlow(true, 0.0);
    QMetaObject::invokeMethod(m_worker, "benchmarkJob", Qt::QueuedConnection,
                              Q_ARG(QString, fileUrl.toLocalFile()), Q_ARG(QString, m_vaultUrl.toLocalFile()),
                              Q_ARG(QStringList, modelIds));
}

void TranscriptionController::runDiarizationBenchmark(const QUrl &fileUrl, const QVariantList &combos)
{
    // Same worker/serialization/cancel story as runBenchmark(); no queue entry,
    // no file writes. Results arrive via the diarizationBenchmark* signals.
    setBenchmarkGlow(true, 0.0);
    QMetaObject::invokeMethod(m_worker, "diarizationBenchmarkJob", Qt::QueuedConnection,
                              Q_ARG(QString, fileUrl.toLocalFile()), Q_ARG(QVariantList, combos));
}

void TranscriptionController::setBenchmarkGlow(bool active, double progress)
{
    if (m_benchmarkProgress != progress) {
        m_benchmarkProgress = progress;
        emit benchmarkProgressChanged();
    }
    if (m_benchmarkActive != active) {
        m_benchmarkActive = active;
        emit benchmarkActiveChanged();
    }
}

void TranscriptionController::cancelBenchmark()
{
    if (m_worker)
        m_worker->requestCancel();
}

QVariantList TranscriptionController::availableEngines() const
{
    QVariantList list;
    const QString vaultPath = m_vaultUrl.toLocalFile();
    for (const EngineDescriptor &desc : discoverEngines(vaultPath)) {
        QVariantMap entry;
        entry["id"] = desc.id;
        entry["modelId"] = QString::fromLatin1(kEngineIdPrefix) + desc.id;
        entry["label"] = desc.manifestValid ? desc.manifest.name : desc.id;
        entry["available"] = desc.available;
        entry["unavailableReason"] = desc.unavailableReason;
        // "downloaded" mirrors the model dropdown's own flag so the UI can
        // reuse the same treatment; for an engine it's specifically whether the
        // engine's model file is present for this vault.
        entry["downloaded"] = ModelManager::isEngineModelPresent(desc.id);
        list.append(entry);
    }
    return list;
}

#include "TranscriptionController.moc"
