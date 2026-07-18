#include "TranscriptionController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
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
#include "tasks/BenchmarkTask.h"
#include "tasks/ChaptersTask.h"
#include "tasks/CombineTask.h"
#include "tasks/DiarizationSweepTask.h"
#include "tasks/DiarizeTask.h"
#include "tasks/DownloadTask.h"
#include "tasks/ImportTask.h"
#include "tasks/MergeSpeakersTask.h"
#include "tasks/RelabelTask.h"
#include "tasks/RenameSpeakerTask.h"
#include "tasks/Task.h"
#include "tasks/TaskFactory.h"
#include "tasks/TaskRunner.h"
#include "tasks/TranscribeTask.h"
#include "tasks/UsbGrabTask.h"


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

QString taskStatusToString(Task::Status status)
{
    switch (status) {
    case Task::Status::Queued:
        return QStringLiteral("queued");
    case Task::Status::Active:
        return QStringLiteral("active");
    case Task::Status::Paused:
        return QStringLiteral("paused");
    case Task::Status::Done:
        return QStringLiteral("done");
    case Task::Status::Failed:
        return QStringLiteral("failed");
    }
    return QString();
}

// Item P6: priority order rebuildJobStatusIndex() uses to pick ONE status
// when multiple m_tasks entries share a path (e.g. a Done history entry plus
// a freshly re-queued run) — "in progress" beats "finished", and among
// finished entries Failed beats Done since a failure is the more actionable
// one for the file tree to surface.
int taskStatusPriority(Task::Status status)
{
    switch (status) {
    case Task::Status::Active:
        return 5;
    case Task::Status::Paused:
        return 4;
    case Task::Status::Queued:
        return 3;
    case Task::Status::Failed:
        return 2;
    case Task::Status::Done:
        return 1;
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
extern const QString kPreludeEndMarker = QStringLiteral("<!-- prelude end -->");

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
// Defined below; declared here so the frontmatter writers above it (this one
// and its neighbours) route through the single atomic writer.
bool writeFrontmatterBlock(const QString &path, const QStringList &frontmatterLines, const QString &body,
                           QString *errorOut);

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

    QString writeError;
    if (!writeFrontmatterBlock(mdPath, frontmatterLines, newBody, &writeError))
        qWarning() << "auto-label: could not write" << mdPath << ":" << writeError;
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

// ParsedSegment is now declared in TranscriptionController.h (Wave N task
// system) so a ChaptersTask can build one without duplicating the struct.

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

// Writes frontmatterLines + body out in the standard "---\n<lines>\n---\n<body>"
// shape shared by every frontmatter rewrite (rename/merge/sync/label/tags/
// prelude/chapters). Opens `path` itself; the caller must already have closed
// any read handle it held on the same file.
bool writeFrontmatterBlock(const QString &path, const QStringList &frontmatterLines, const QString &body,
                           QString *errorOut)
{
    // Atomic: QSaveFile stages into a temp file in the same directory and
    // only renames into place on commit(). A crash, full disk, or write
    // failure leaves the ORIGINAL .md byte-for-byte intact (the old
    // WriteOnly|Truncate path destroyed the transcript body before the new
    // bytes landed, then still returned true — permanent data loss reported
    // as "saved"). Every frontmatter rewrite routes through here.
    QSaveFile mdFile(path);
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
    if (!mdFile.commit()) {
        if (errorOut)
            *errorOut = QStringLiteral("could not write %1 (%2)").arg(path, mdFile.errorString());
        return false;
    }
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

// Wave N (task system): TranscriptionWorker is GONE (1601 lines). It was one
// QObject with 12 kind-specific slots and ~22 unioned signals, owning the warm
// engines and every kind's doing-code in one body. All of it now lives in
// src/core/tasks/: one Task subclass per kind (its own run() + its own typed
// result signals), EngineCache (the warm ASR/label/diarization engines +
// GPU-hygiene eviction), TaskControl (the pause/cancel atomics),
// TaskContext::checkpoint() (the one cooperative interruption point every
// pausable kind funnels through), and TaskRunner (the thread host that turns
// run()-returned-or-threw into exactly one taskFinished). See .task/plan.md.


TranscriptionController::TranscriptionController(QObject *parent)
    : QObject(parent)
{
    loadPhaseTimings();
    // See recomputeOverallProgress()'s doc comment — the only way
    // "loading"/"decoding" (no real progress callback) ever advance
    // overallProgress mid-phase. Only ever running while m_phase is one of
    // those two — started/stopped from the phase() handler in connectTask().
    m_indeterminatePhaseTimer = new QTimer(this);
    m_indeterminatePhaseTimer->setInterval(200);
    connect(m_indeterminatePhaseTimer, &QTimer::timeout, this, &TranscriptionController::recomputeOverallProgress);

    m_runner = new TaskRunner;
    m_runner->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_runner, &QObject::deleteLater);
    // The ONE terminal-report path from the runner. A task cancelled before
    // it ever ran never reaches the runner at all — the queue synthesizes its
    // report directly (see cancelTaskAt()), so BOTH routes converge here and
    // every enqueued task reports exactly once.
    connect(m_runner, &TaskRunner::taskFinished, this, &TranscriptionController::onTaskFinished);

    connect(this, &TranscriptionController::queueChanged, this, &TranscriptionController::persistQueue);
    connect(this, &TranscriptionController::queueChanged, this, &TranscriptionController::rebuildJobStatusIndex);
    m_thread.start();
}

// Wires ONE task's signals to this controller. The generic ones (progress/
// phase/paused) are identical for every kind and are connected here; the
// kind-specific ones are the small localized switch at the bottom — the
// deliberate trade for typed per-subclass result signals instead of the old
// worker's ~22 signals unioned onto one object (see .task/plan.md's
// "Results" decision).
//
// Every connection is scoped to `task` as the sender AND `this` as context,
// so they die with either object; a task is connected exactly once, right
// before its first dispatch (see startTask()).
void TranscriptionController::connectTask(Task *task)
{
    connect(task, &Task::progress, this, [this](double fraction) {
        m_progress = fraction;
        emit progressChanged();
        // "combining"/"chaptering"/"importing" aren't one of
        // recomputeOverallProgress()'s fixed transcribe phase buckets — each
        // emits a single already-cumulative fraction, so assign it directly
        // rather than trying to blend sub-phases.
        if (m_phase == QStringLiteral("combining") || m_phase == QStringLiteral("chaptering")
            || m_phase == QStringLiteral("importing") || m_phase == QStringLiteral("downloading")
            || m_phase == QStringLiteral("grabbing") || m_phase == QStringLiteral("benchmarking")) {
            m_overallProgress = fraction;
            emit overallProgressChanged();
        } else {
            recomputeOverallProgress();
        }
    });
    connect(task, &Task::phase, this, [this](const QString &phase) {
        m_phase = phase;
        m_phaseStartMs = activeElapsedMs();
        emit currentPhaseChanged();
        // See recomputeOverallProgress()'s doc comment — only "loading" and
        // "decoding" need the timer (no real progress callback of their own);
        // every other phase already advances overallProgress via progress().
        if (phase == QStringLiteral("loading") || phase == QStringLiteral("decoding"))
            m_indeterminatePhaseTimer->start();
        else
            m_indeterminatePhaseTimer->stop();
        recomputeOverallProgress();
    });
    connect(task, &Task::paused, this, [this](bool isPaused) {
        m_activePaused = isPaused;
        if (isPaused)
            m_activeAccumulatedMs += m_activeSegmentTimer.elapsed();
        else
            m_activeSegmentTimer.restart();
        emit activePausedChanged();
    });

    // --- Kind-specific result signals ---
    if (auto *transcribe = qobject_cast<TranscribeTask *>(task)) {
        connect(transcribe, &TranscribeTask::phaseDurationMs, this, &TranscriptionController::recordPhaseDuration);
        connect(transcribe, &TranscribeTask::segmentWritten, this, [this] {
            m_liveUpdateTick++;
            emit liveUpdateTickChanged();
        });
        connect(transcribe, &TranscribeTask::backendNameChanged, this, [this](const QString &name) {
            if (m_activeBackendName != name) {
                m_activeBackendName = name;
                emit activeBackendNameChanged();
            }
        });
    } else if (auto *rename = qobject_cast<RenameSpeakerTask *>(task)) {
        connect(rename, &RenameSpeakerTask::speakerRenamed, this,
                [this](const QString &path, const QString &oldName, bool ok, const QString &error) {
                    emit speakerRenamed(QUrl::fromLocalFile(path), oldName, ok, error);
                });
    } else if (auto *merge = qobject_cast<MergeSpeakersTask *>(task)) {
        connect(merge, &MergeSpeakersTask::speakersMerged, this,
                [this](const QString &path, const QString &sourceName, const QString &targetName, bool ok,
                       const QString &error) {
                    emit speakersMerged(QUrl::fromLocalFile(path), sourceName, targetName, ok, error);
                });
    } else if (auto *grab = qobject_cast<UsbGrabTask *>(task)) {
        // The task id rides along from HERE rather than being carried by the
        // task itself, keeping the task ignorant of the queue — same split the
        // old worker kept. Captured by value: `grab` outlives this lambda
        // (the queue owns it), and the id is stable for its whole lifetime.
        const int id = grab->id;
        connect(grab, &UsbGrabTask::grabCompleted, this,
                [this, id](int filesCopied, int filesSkipped, const QStringList &errors, bool cancelled) {
                    emit usbGrabFinished(id, filesCopied, filesSkipped, errors, cancelled);
                });
    } else if (auto *bench = qobject_cast<BenchmarkTask *>(task)) {
        connect(bench, &BenchmarkTask::benchmarkModelStarted, this,
                &TranscriptionController::benchmarkModelStarted);
        connect(bench, &BenchmarkTask::benchmarkModelDone, this, &TranscriptionController::benchmarkModelDone);
        connect(bench, &BenchmarkTask::benchmarkRunFinished, this, &TranscriptionController::benchmarkFinished);
        // No separate glow wiring: a BenchmarkTask is a real queued task now,
        // so m_busy + overallProgress drive the existing glow for free. That
        // parallel path (setBenchmarkGlow) is deleted — see the
        // activeIsForeground Q_PROPERTY doc comment.
    } else if (auto *sweep = qobject_cast<DiarizationSweepTask *>(task)) {
        connect(sweep, &DiarizationSweepTask::comboStarted, this,
                &TranscriptionController::diarizationBenchmarkComboStarted);
        connect(sweep, &DiarizationSweepTask::comboDone, this,
                &TranscriptionController::diarizationBenchmarkComboDone);
        connect(sweep, &DiarizationSweepTask::sweepFinished, this,
                &TranscriptionController::diarizationBenchmarkFinished);
    }
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
    // TaskContext::checkpoint()'s doc comment for which phases those
    // are), so shutdown is prompt instead of an indefinite silent hang.
    // requestResume() wakes it first in case it was sitting paused, which
    // would otherwise block forever waiting for a pause that will never be
    // lifted by anyone once this destructor is running.
    if (m_runner) {
        m_runner->requestResume();
        m_runner->requestCancel();
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
    if (m_activeIndex >= 0 && m_activeIndex < static_cast<int>(m_tasks.size())) {
        if (auto *t = qobject_cast<const TranscribeTask *>(m_tasks[m_activeIndex].get()))
            return t->modelId();
    }
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
    for (const auto &task : m_tasks) {
        if (task->fileUrl == fileUrl
            && (task->status == Task::Status::Queued || task->status == Task::Status::Active
                || task->status == Task::Status::Paused))
            return true;
    }
    return false;
}

void TranscriptionController::rebuildJobStatusIndex()
{
    QHash<QString, Task::Status> rebuilt;
    for (const auto &task : m_tasks) {
        if (task->fileUrl.isEmpty())
            continue; // download task — names no single vault file
        const QString path = task->fileUrl.toLocalFile();
        const auto it = rebuilt.constFind(path);
        if (it == rebuilt.constEnd() || taskStatusPriority(task->status) > taskStatusPriority(it.value()))
            rebuilt[path] = task->status;
    }
    if (rebuilt != m_jobStatusByPath) {
        m_jobStatusByPath = rebuilt;
        emit jobStatusesChanged();
    }
}

int TranscriptionController::jobStatusForPath(const QString &path) const
{
    const auto it = m_jobStatusByPath.constFind(path);
    // -1 == "no task for this path" (the sentinel Task::Status has no member
    // for — see the class doc comment where JobStatus used to live).
    return it == m_jobStatusByPath.constEnd() ? -1 : static_cast<int>(it.value());
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

// The ONE way a task enters the queue — see the header doc comment.
int TranscriptionController::enqueueTask(std::unique_ptr<Task> task, int insertIndex)
{
    int insertAt = (insertIndex < 0) ? frontOfQueueInsertIndex() : insertIndex;
    const int startIndex = insertAt;

    // The models this kind declared it needs, each queued as its own
    // DownloadTask ahead of it if absent. Every enqueue path gets this
    // uniformly, from the kind's own prerequisites() — no site has to
    // remember which downloads which kind wants.
    queuePrerequisites(*task, task->vaultUrl, &insertAt);

    task->id = m_nextJobId++;
    task->status = Task::Status::Queued;
    const int id = task->id;
    m_tasks.insert(m_tasks.begin() + std::clamp(insertAt, 0, static_cast<int>(m_tasks.size())), std::move(task));
    if (m_activeIndex >= insertAt)
        ++m_activeIndex;

    emit queueChanged();

    if (m_activeIndex < 0)
        startTask(startIndex);
    return id;
}

void TranscriptionController::queuePrerequisites(const Task &task, const QUrl &vaultUrl, int *insertAt)
{
    for (const ModelPrerequisite &prereq : task.prerequisites()) {
        if (maybeQueueDownloadTask(prereq.target, vaultUrl, prereq.modelId, *insertAt) >= 0)
            ++(*insertAt);
    }
}

void TranscriptionController::transcribe(const QUrl &fileUrl, const QUrl &vaultUrl, const QString &modelId,
                                          bool autoLabel, bool restrictTagsToExisting,
                                          double tagReuseBias, double wordiness)
{
    // Done/Failed entries for the same fileUrl deliberately do NOT block
    // re-queueing (see class doc) — only Queued/Active does.
    if (isQueued(fileUrl))
        return;

    // Item 23: a transcribe call ONLY transcribes now. It used to chain a
    // second relabel-only job whenever autoLabel was true, so clicking
    // "Transcribe" silently also labeled — the dedicated "Regenerate label"
    // action is the sole way to label. The autoLabel parameter is kept (QML
    // call sites in Sidebar.qml/MainPanel.qml still pass
    // AppSettings.autoLabelEnabled, and this invokable signature must stay
    // call-compatible with them) but is now a NO-OP.
    Q_UNUSED(autoLabel);
    Q_UNUSED(restrictTagsToExisting);
    Q_UNUSED(tagReuseBias);
    Q_UNUSED(wordiness);

    enqueueTask(std::make_unique<TranscribeTask>(fileUrl, vaultUrl, modelId));
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

    auto task = std::make_unique<ImportTask>(sourceUrl.toLocalFile(), destUrl);
    task->vaultUrl = vaultUrl;
    enqueueTask(std::move(task));
}

int TranscriptionController::enqueueUsbGrab(const UsbGrabRule &rule, const QString &mountPath, const QUrl &destUrl,
                                             const QUrl &vaultUrl, bool autoTriggered)
{
    // No isQueued(destUrl) dedup here, deliberately — see the header's doc
    // comment: a grab's fileUrl is a destination FOLDER that a device's two
    // volumes legitimately share.
    auto task = std::make_unique<UsbGrabTask>(rule, mountPath, destUrl, autoTriggered);
    task->vaultUrl = vaultUrl;
    return enqueueTask(std::move(task));
}

void TranscriptionController::diarizeFile(const QUrl &fileUrl, const QUrl &vaultUrl,
                                          const QString &segmentationModelId, const QString &embeddingModelId,
                                          double sensitivity)
{
    // Same dedup rule as transcribe()/relabel(): a Queued/Active entry blocks
    // a duplicate request.
    if (isQueued(fileUrl))
        return;

    // Empty id ⇒ the ModelManager default (keeps the task's recorded ids
    // concrete for persistence + the download dedup key).
    const QString segId = segmentationModelId.isEmpty()
                              ? QString::fromLatin1(ModelManager::kDefaultSegmentationModelId)
                              : segmentationModelId;
    const QString embedId = embeddingModelId.isEmpty()
                                ? QString::fromLatin1(ModelManager::kDefaultEmbeddingModelId)
                                : embeddingModelId;

    enqueueTask(std::make_unique<DiarizeTask>(fileUrl, vaultUrl, segId, embedId, sensitivity));
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
    // transcribe or a relabel-only task) blocks a duplicate request.
    if (isQueued(fileUrl))
        return;

    enqueueTask(std::make_unique<RelabelTask>(fileUrl, vaultUrl, restrictTagsToExisting, tagReuseBias, wordiness,
                                               /*skipIfAlreadyLabeled=*/false));
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

    enqueueTask(std::make_unique<ChaptersTask>(fileUrl, vaultUrl, density));
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
    // re-detect speakers and refresh its label+chapters). An orphaned .md's
    // sidecarPath() is itself, so this is true for those too.
    const bool alreadyTranscribed = QFileInfo::exists(sidecarPath(fileUrl.toLocalFile()));

    // Item O15: jump ahead of whatever's already Queued, but keep this file's
    // tasks CONTIGUOUS and IN ORDER by walking insertAt forward as each is
    // inserted (rather than each new task re-jumping to the very front —
    // which is exactly why this doesn't just call transcribe()/diarizeFile()/
    // relabel()/generateChapters() in sequence). Each enqueueTask() also
    // inserts that step's own prerequisites() ahead of it; maybeQueueDownloadTask()
    // dedups, so the label model the relabel AND chapters steps both want is
    // queued exactly once.
    int insertAt = frontOfQueueInsertIndex();
    const int startIndex = insertAt;
    const auto step = [&](std::unique_ptr<Task> task) {
        // Count what enqueueTask() actually inserted (the task + however many
        // prerequisite downloads it queued ahead of it) so the next step lands
        // after all of it.
        const int before = static_cast<int>(m_tasks.size());
        enqueueTask(std::move(task), insertAt);
        insertAt += static_cast<int>(m_tasks.size()) - before;
    };

    // (1) transcribe (pure ASR) — skipped if already transcribed.
    if (!alreadyTranscribed)
        step(std::make_unique<TranscribeTask>(fileUrl, vaultUrl, modelId));
    // (2) detect speakers — reads the .md the transcribe step (if any) just
    // wrote and splices "[Speaker N]" tags in.
    step(std::make_unique<DiarizeTask>(fileUrl, vaultUrl, segId, embedId, sensitivity));
    // (3) label — reads the .md the diarize step just updated.
    step(std::make_unique<RelabelTask>(fileUrl, vaultUrl, restrictTagsToExisting, tagReuseBias, wordiness,
                                        /*skipIfAlreadyLabeled=*/false));
    // (4) chapters — reads the labeled .md.
    step(std::make_unique<ChaptersTask>(fileUrl, vaultUrl, chapterDensity));

    // No trailing startTask(): the FIRST step()'s enqueueTask() already
    // auto-started the queue if it was idle (and startIndex just marks where
    // that first item landed). Every later step inserts behind the now-Active
    // task and correctly stays Queued.
    Q_UNUSED(startIndex);
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

void TranscriptionController::startTask(int index)
{
    // Safety-net preflight, before any state mutation below — see
    // maybeRedirectForCombine()'s doc comment. A true return means it already
    // recursed onto whatever should run instead (a newly-inserted combine
    // task); this call contributes nothing further.
    if (maybeRedirectForCombine(index))
        return;

    m_activeIndex = index;
    Task *task = m_tasks[index].get();
    task->status = Task::Status::Active;
    task->error.clear();

    // A pause/cancel left over from whatever the PREVIOUS task did must never
    // leak into this one. Elapsed-time tracking (activeElapsedMs()) restarts
    // from zero for every new Active task.
    m_runner->resetControl();
    m_activePaused = false;
    emit activePausedChanged();
    m_activeAccumulatedMs = 0;
    m_activeSegmentTimer.start();
    m_overallProgress = 0.0;
    emit overallProgressChanged();
    // See recomputeOverallProgress()'s doc comment — true ONLY for a
    // "diarize" task, whose progress bar is weighted around the
    // detecting-speakers phase. A "transcribe" task is pure ASR (its bar is
    // weighted around loading/decoding/detecting-speech/transcribing). Every
    // other kind never emits one of those phases at all, so this is simply
    // never read for them.
    m_activeSpeakerDetection = task->kind() == QStringLiteral("diarize");
    m_phaseStartMs = 0;
    m_indeterminatePhaseTimer->stop();

    m_busy = true;
    emit busyChanged();
    // Presentation, not m_busy, is what decides the main-panel overlay now —
    // so a Background task (benchmark/sweep/download) gets a Tasks row + the
    // glow without hijacking the panel the user is reading, which is exactly
    // what the old benchmark side-door existed to work around.
    const bool foreground = task->presentation() == Task::Presentation::Foreground;
    if (m_activeIsForeground != foreground) {
        m_activeIsForeground = foreground;
        emit activeIsForegroundChanged();
    }
    m_progress = 0.0;
    emit progressChanged();
    // Cleared here (not just at the end of the previous task) so a kind that
    // never emits phase() at all doesn't inherit a stale phase left over from
    // an earlier one.
    m_phase.clear();
    emit currentPhaseChanged();
    m_currentFile = task->fileUrl;
    emit currentFileChanged();
    emit currentJobKindChanged();
    emit queueChanged();

    // Connected exactly once, on first dispatch. A Paused task that resumes
    // re-enters startTask(), so guard against double-wiring (which would
    // double-count every progress tick).
    if (!m_connectedTasks.contains(task)) {
        m_connectedTasks.insert(task);
        connectTask(task);
    }

    // The ENTIRE dispatch, for every kind. This replaced a 10-branch if/else
    // that marshalled each kind's own argument list into a string-named worker
    // slot — the single biggest reason adding a kind meant editing five
    // disjoint sites. Now the kind IS the object.
    QMetaObject::invokeMethod(m_runner, "execute", Qt::QueuedConnection, Q_ARG(Task *, task));
}

int TranscriptionController::indexOfTask(const Task *task) const
{
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        if (m_tasks[i].get() == task)
            return i;
    }
    return -1;
}

void TranscriptionController::onTaskFinished(Task *task, bool ok, const QString &error)
{
    m_busy = false;
    emit busyChanged();
    if (m_activeIsForeground) {
        m_activeIsForeground = false;
        emit activeIsForegroundChanged();
    }
    // A task that fails/gets cancelled mid-"loading"/"decoding" never gets
    // another phase() signal to naturally stop this.
    m_indeterminatePhaseTimer->stop();
    const QUrl finishedFile = m_currentFile;

    // Same requestCancel()->TaskError("Cancelled") mechanism as a real cancel,
    // distinguished only by this flag, which togglePauseActive() sets right
    // before triggering it (a pause during the unhookable "loading" phase).
    const bool wasStopping = m_pausingActiveAsRestart;
    const bool pausedAsRestart = m_pausingActiveAsRestart && !ok && error == QStringLiteral("Cancelled");
    m_pausingActiveAsRestart = false;
    if (wasStopping)
        emit activeStoppingChanged();

    const int taskIndex = indexOfTask(task);
    if (taskIndex >= 0) {
        if (pausedAsRestart) {
            task->status = Task::Status::Paused;
            task->error.clear();
        } else {
            task->status = ok ? Task::Status::Done : Task::Status::Failed;
            task->error = error;
            // Snapshot active-work elapsed time (excludes any paused stretches
            // — see activeElapsedMs()) as this task's final duration, shown in
            // the Tasks popup's history rows. Left unset (-1) for a Paused
            // task — it isn't finished.
            task->durationMs = activeElapsedMs();
            // A predownload (or lazy download) that just landed changes the
            // set of downloaded models — let the Models page refresh.
            if (ok && task->kind() == QStringLiteral("download"))
                emit modelsChanged();
        }
    }
    m_activeIndex = -1;
    m_activePaused = false;
    emit activePausedChanged();

    if (pausedAsRestart) {
        // Deliberately do NOT advance to the next Queued task — the Paused
        // task sits at the front until the user explicitly resumes it
        // (startNext() prioritizes any Paused task over Queued ones).
        m_currentFile = QUrl();
        emit currentFileChanged();
        emit currentJobKindChanged();
        emit queueChanged();
        QMetaObject::invokeMethod(m_runner, "evictIdle", Qt::QueuedConnection);
        emit finished(finishedFile, ok, error);
        return;
    }

    int nextIndex = -1;
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        if (m_tasks[i]->status == Task::Status::Queued) {
            nextIndex = i;
            break;
        }
    }

    if (nextIndex >= 0) {
        startTask(nextIndex);
    } else {
        m_currentFile = QUrl();
        emit currentFileChanged();
        emit currentJobKindChanged();
        emit queueChanged();
        // Nothing left queued to reuse a warm engine, so release whichever one
        // the task that just finished was using.
        QMetaObject::invokeMethod(m_runner, "evictIdle", Qt::QueuedConnection);
    }

    emit finished(finishedFile, ok, error);
}

void TranscriptionController::removeTaskAt(int index)
{
    m_connectedTasks.remove(m_tasks[index].get());
    m_tasks.erase(m_tasks.begin() + index);
    if (m_activeIndex > index)
        --m_activeIndex;
}

void TranscriptionController::cancelTaskAt(int index)
{
    // Snapshot before erasing — this is the pre-run-cancel half of the
    // one-terminal-report invariant (.task/plan.md invariant 1): a task
    // cancelled while still Queued/Paused never reached the runner, so the
    // queue must synthesize its terminal report itself. The old system just
    // dropped the entry, which is why a consumer waiting on finished() (e.g.
    // UsbGrabberController's in-flight tracking) never heard back for a
    // cancelled-while-queued task.
    const QUrl url = m_tasks[index]->fileUrl;
    removeTaskAt(index);
    emit queueChanged();
    emit finished(url, false, QStringLiteral("Cancelled"));
}

void TranscriptionController::dismiss(const QUrl &fileUrl)
{
    // A fileUrl may have multiple entries (re-queueing a Done/Failed task is
    // allowed) — remove only the first Done/Failed match, skipping over any
    // Queued/Active entry for the same url rather than no-op'ing on it.
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        const Task *task = m_tasks[i].get();
        if (task->fileUrl == fileUrl
            && (task->status == Task::Status::Done || task->status == Task::Status::Failed)) {
            removeTaskAt(i);
            emit queueChanged();
            return;
        }
    }
}

void TranscriptionController::clearFinished()
{
    bool changed = false;
    for (int i = static_cast<int>(m_tasks.size()) - 1; i >= 0; --i) {
        if (m_tasks[i]->status == Task::Status::Done || m_tasks[i]->status == Task::Status::Failed) {
            removeTaskAt(i);
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
    if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_tasks.size()))
        return;
    // The task itself declares whether it can pause — the string allowlist
    // (transcribe/download/combine/chapters) is gone; a kind with a real
    // interruption point returns capabilities().pausable, and everything else
    // is a safe no-op here. This is the same capability the UI reads to enable
    // the pause button, so the two can never disagree (the 2026-07-09 bug was
    // exactly such a disagreement).
    if (!m_tasks[m_activeIndex]->capabilities().pausable)
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
    m_runner->requestPause();
}

void TranscriptionController::resumeActive()
{
    m_runner->requestResume();
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
    if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_tasks.size()))
        return;
    cancelJob(m_tasks[m_activeIndex]->id);
}

void TranscriptionController::cancelJob(int id)
{
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        Task *task = m_tasks[i].get();
        if (task->id != id)
            continue;
        // A Queued or Paused task has nothing running to interrupt — remove it
        // and synthesize its terminal report (cancelTaskAt), so a consumer
        // waiting on finished() hears back even though run() never ran.
        if (task->status == Task::Status::Queued || task->status == Task::Status::Paused) {
            cancelTaskAt(i);
            return;
        }
        if (task->status == Task::Status::Active) {
            // requestResume() wakes it if currently blocked in a pause wait —
            // otherwise a paused-then-cancelled task would sit paused forever
            // instead of ever reaching the cancel check. The runner emits the
            // terminal report for THIS case (run() returns via TaskError or
            // early-return), so no synthesis here. See cancelJob()'s header
            // doc comment for immediate-vs-next-checkpoint per kind.
            m_runner->requestCancel();
            m_runner->requestResume();
        }
        return;
    }
}

void TranscriptionController::cancelAll()
{
    // Snapshot the ids to cancel first — cancelTaskAt() mutates m_tasks (and
    // emits finished() per task), so iterating it live while removing would
    // be unsafe. Active job handled separately below (it's not removed here;
    // the runner reports it).
    QList<int> queuedIds;
    for (const auto &task : m_tasks) {
        if (task->status == Task::Status::Queued || task->status == Task::Status::Paused)
            queuedIds.append(task->id);
    }
    for (const int id : std::as_const(queuedIds)) {
        const int idx = [this, id] {
            for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i)
                if (m_tasks[i]->id == id)
                    return i;
            return -1;
        }();
        if (idx >= 0)
            cancelTaskAt(idx);
    }
    if (m_activeIndex >= 0) {
        m_runner->requestCancel();
        m_runner->requestResume();
    }
}

void TranscriptionController::moveQueuedJob(int id, int newIndex)
{
    int fromRealIndex = -1;
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        if (m_tasks[i]->id == id && m_tasks[i]->status == Task::Status::Queued) {
            fromRealIndex = i;
            break;
        }
    }
    if (fromRealIndex < 0)
        return;

    // Real (m_tasks) indices of every Queued entry, in current order — the
    // ordering moveQueuedJob()'s newIndex counts within (Active/Done/Failed
    // rows are never part of it).
    QVector<int> queuedIndices;
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        if (m_tasks[i]->status == Task::Status::Queued)
            queuedIndices.append(i);
    }
    const int fromQueuedPos = queuedIndices.indexOf(fromRealIndex);
    const int clampedNewIndex = std::clamp(newIndex, 0, static_cast<int>(queuedIndices.size()) - 1);
    if (clampedNewIndex == fromQueuedPos)
        return;

    std::unique_ptr<Task> moved = std::move(m_tasks[fromRealIndex]);
    m_tasks.erase(m_tasks.begin() + fromRealIndex);
    if (m_activeIndex > fromRealIndex)
        --m_activeIndex;

    // Recompute the remaining Queued real-indices after the removal shifted
    // everything past fromRealIndex down by one, then reinsert `moved` right
    // at the target Queued slot's (now-current) real index — or at the very
    // end of m_tasks if newIndex names the last Queued slot.
    queuedIndices.removeAt(fromQueuedPos);
    for (int &idx : queuedIndices) {
        if (idx > fromRealIndex)
            --idx;
    }
    const int insertRealIndex =
        clampedNewIndex >= queuedIndices.size() ? static_cast<int>(m_tasks.size()) : queuedIndices[clampedNewIndex];
    m_tasks.insert(m_tasks.begin() + insertRealIndex, std::move(moved));
    if (m_activeIndex >= insertRealIndex)
        ++m_activeIndex;
    emit queueChanged();
}

bool TranscriptionController::isDownloadQueued(ModelDownloadTarget target, const QUrl &vaultUrl,
                                                const QString &modelId) const
{
    const QString targetStr = DownloadTask::targetToString(target);
    for (const auto &task : m_tasks) {
        auto *dl = qobject_cast<DownloadTask *>(task.get());
        if (!dl || dl->target() != target || dl->vaultUrl != vaultUrl)
            continue;
        const bool idMatters = target == ModelDownloadTarget::WhisperModel
            || target == ModelDownloadTarget::SegmentationModel || target == ModelDownloadTarget::EmbeddingModel;
        if ((!idMatters || dl->modelId() == modelId)
            && (task->status == Task::Status::Queued || task->status == Task::Status::Active))
            return true;
    }
    return false;
}

int TranscriptionController::maybeQueueDownloadTask(ModelDownloadTarget target, const QUrl &vaultUrl,
                                                     const QString &modelId, int insertIndex)
{
    bool alreadyOnDisk = false;
    switch (target) {
    case ModelDownloadTarget::WhisperModel:
        // An unrecognized modelId has no real file to pre-download — there
        // is nothing sensible to queue (ensureModel()'s own fallback-to-
        // default logic, run inline when the transcribe task actually
        // starts, is what actually resolves this case; see ModelManager's
        // class doc comment). Queuing a phantom download here would silently
        // redirect to the default model under a label that still says the
        // unrecognized id.
        if (ModelManager::modelFileName(modelId).isEmpty())
            return -1;
        alreadyOnDisk = ModelManager::isModelDownloaded(modelId);
        break;
    case ModelDownloadTarget::LabelModel:
        alreadyOnDisk = ModelManager::isLabelModelDownloaded();
        break;
    case ModelDownloadTarget::VadModel:
        alreadyOnDisk = ModelManager::isVadModelDownloaded();
        break;
    case ModelDownloadTarget::SegmentationModel:
        alreadyOnDisk = ModelManager::isSegmentationModelDownloaded(modelId);
        break;
    case ModelDownloadTarget::EmbeddingModel:
        alreadyOnDisk = ModelManager::isEmbeddingModelDownloaded(modelId);
        break;
    }
    if (alreadyOnDisk || isDownloadQueued(target, vaultUrl, modelId))
        return -1;

    auto task = std::make_unique<DownloadTask>(target, modelId);
    task->id = m_nextJobId++;
    task->vaultUrl = vaultUrl;
    task->status = Task::Status::Queued;
    const int id = task->id;
    const int at =
        (insertIndex < 0) ? static_cast<int>(m_tasks.size()) : std::clamp(insertIndex, 0, static_cast<int>(m_tasks.size()));
    m_tasks.insert(m_tasks.begin() + at, std::move(task));
    return id;
}

// See this method's header doc comment.
int TranscriptionController::frontOfQueueInsertIndex() const
{
    if (m_activeIndex >= 0)
        return m_activeIndex + 1;
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        if (m_tasks[i]->status == Task::Status::Queued)
            return i;
    }
    return static_cast<int>(m_tasks.size());
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
    for (const auto &task : m_tasks) {
        if (task->kind() == QStringLiteral("combine") && task->fileUrl == outputUrl
            && (task->status == Task::Status::Queued || task->status == Task::Status::Active))
            return true;
    }
    return false;
}

void TranscriptionController::enqueueCombineJob(const CombineGroup &group, const QUrl &vaultUrl,
                                                 bool deleteOriginals, int insertIndex)
{
    auto task = std::make_unique<CombineTask>(group.files, QUrl::fromLocalFile(group.outputPath), deleteOriginals);
    task->id = m_nextJobId++;
    task->vaultUrl = vaultUrl;
    task->status = Task::Status::Queued;
    const int at = (insertIndex < 0) ? frontOfQueueInsertIndex() : insertIndex;
    m_tasks.insert(m_tasks.begin() + std::clamp(at, 0, static_cast<int>(m_tasks.size())), std::move(task));
}

// See this method's header doc comment. This is the scheduler-level preflight
// .task/plan.md flagged as the hardest port: it can rewrite the task's target
// file, insert a combine task ahead of it, and recurse — all strictly BEFORE
// dispatch, on the GUI thread, which is what makes mutating Task::fileUrl safe
// (run() hasn't started; see that field's doc comment).
bool TranscriptionController::maybeRedirectForCombine(int index)
{
    Task *task = m_tasks[index].get();
    // Only a plain transcribe is ever redirected — it's the sole kind whose
    // input file might turn out to be a combine fragment. Every *Only kind in
    // the old flag-soup guard was just "not transcribe"; a single qobject_cast
    // expresses that now.
    auto *transcribe = qobject_cast<TranscribeTask *>(task);
    if (!transcribe || transcribe->fileUrl.isEmpty())
        return false;

    const QFileInfo info(transcribe->fileUrl.toLocalFile());

    // Outcome 1: an earlier manual runCombineScan() run may already have
    // merged this exact file's group (and moved the original fragment into
    // .combined/) since this task was queued. The merged output's path is
    // recomputable from this filename alone, so check for it directly.
    for (const QVariant &ruleVar : m_combineRules) {
        const QVariantMap rule = ruleVar.toMap();
        const auto key = matchRuleCapturedKey(rule, info.fileName());
        if (!key)
            continue;
        const QString outputPath = combineOutputPathFor(rule.value(QStringLiteral("name")).toString(), *key,
                                                          info.absolutePath(), info.suffix());
        if (QFileInfo::exists(outputPath))
            transcribe->fileUrl = QUrl::fromLocalFile(outputPath);
        break; // only the first matching rule is used, mirroring findCombineGroups()
    }

    // Outcome 2: this file is a member of a currently-"ready" group per the
    // last runCombineScan() scan that hasn't been merged yet — insert the
    // combine task ahead of this one and redirect.
    const QString filePath = transcribe->fileUrl.toLocalFile();
    for (const CombineGroup &group : findCombineGroups(m_combineRules, m_combineAudioPaths)) {
        if (group.status != QStringLiteral("ready") || !group.files.contains(filePath))
            continue;
        transcribe->fileUrl = QUrl::fromLocalFile(group.outputPath);
        if (!isCombineQueued(group.outputPath))
            enqueueCombineJob(group, transcribe->vaultUrl, m_combineDeleteOriginals, index);
        emit queueChanged();
        // Recurses onto the freshly-inserted combine task at this same index
        // (safe — see this method's header doc comment). If a combine task
        // for this group was somehow already queued elsewhere, nothing was
        // inserted at `index` here, so this recurses onto the SAME transcribe
        // task; its redirected fileUrl still won't exist yet and it fails
        // cleanly via the normal decode error rather than looping.
        startTask(index);
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
    for (const auto &task : m_tasks) {
        // Only Queued survives a restart — an Active OR Paused task is
        // persisted AS Queued (no true mid-file resume yet); Done/Failed
        // history is session-only by design.
        if (task->status != Task::Status::Queued && task->status != Task::Status::Active
            && task->status != Task::Status::Paused)
            continue;

        // Persistence is OPT-IN: import/usbGrab (external/removable paths)
        // return isPersistable() == false and are simply never written — see
        // the header doc comment on why this replaced the old opt-OUT list.
        if (!task->isPersistable())
            continue;

        // The queue owns kind/fileUrl/vaultUrl; toJson() adds whatever this
        // kind needs to rebuild itself (createTaskFromJson()).
        QJsonObject obj = task->toJson();
        obj["kind"] = task->kind();
        obj["fileUrl"] = task->fileUrl.toString();
        obj["vaultUrl"] = task->vaultUrl.toString();
        array.append(obj);
    }

    QSaveFile file(tasksJsonPath(m_vaultUrl));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    file.commit();
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

    // Replayed in file order — the order persistQueue() wrote. A download a
    // dependent task needs was written just before it, so replaying in order
    // queues each download ahead of its dependent (and the dependent's own
    // prerequisites() sees it already queued via isDownloadQueued(), no dup).
    //
    // Deliberately does NOT auto-start anything: every restored entry just
    // sits Queued until the user resumes via startNext(). So this appends at
    // the tail (explicit insertIndex == size) rather than going through the
    // front-jumping enqueueTask(), and reproduces the exact saved order —
    // which is ALSO why a "transcribe" entry followed by its "relabel"
    // sibling for the same fileUrl both replay: neither goes through the
    // public transcribe()/relabel() dedup guard (that guard would reject the
    // second), only through createTaskFromJson() + a raw insert.
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject obj = v.toObject();
        const QString kind = obj.value("kind").toString();

        std::unique_ptr<Task> task = createTaskFromJson(kind, obj);
        if (!task)
            continue; // unrecognized kind, or one whose isPersistable() is false

        // A download entry is self-contained (no prerequisites of its own); a
        // dependent kind gets its downloads queued ahead of it, deduped
        // against whatever the persisted download entries already queued.
        int insertAt = static_cast<int>(m_tasks.size());
        queuePrerequisites(*task, task->vaultUrl, &insertAt);

        task->id = m_nextJobId++;
        task->status = Task::Status::Queued;
        m_tasks.insert(m_tasks.begin() + std::clamp(insertAt, 0, static_cast<int>(m_tasks.size())), std::move(task));
        emit queueChanged();
    }
}

void TranscriptionController::startNext()
{
    if (m_activeIndex >= 0)
        return;
    // A Paused task is what the user just clicked "resume" on — it takes
    // priority over unrelated Queued tasks regardless of list position;
    // there's at most one at a time.
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        if (m_tasks[i]->status == Task::Status::Paused) {
            startTask(i);
            return;
        }
    }
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        if (m_tasks[i]->status == Task::Status::Queued) {
            startTask(i);
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

    if (!writeFrontmatterBlock(mdFile.fileName(), frontmatterLines, body, errorOut))
        return false;
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

    if (!writeFrontmatterBlock(mdFile.fileName(), frontmatterLines, body, errorOut))
        return false;
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

    if (!writeFrontmatterBlock(mdFile.fileName(), frontmatterLines, body, errorOut))
        return false;
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

    if (!writeFrontmatterBlock(mdFile.fileName(), frontmatterLines, newBody, errorOut))
        return false;
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

    if (!writeFrontmatterBlock(mdFile.fileName(), frontmatterLines, body, errorOut))
        return false;
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

    // Appended at the tail (explicit insertIndex == size), not jumped ahead
    // like transcribe() — matches the old renameSpeaker()'s push_back. A
    // rename is cheap and non-urgent; it can wait behind queued transcription.
    enqueueTask(std::make_unique<RenameSpeakerTask>(fileUrl, oldName, newName),
                static_cast<int>(m_tasks.size()));
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

    // Appended at the tail, same as renameSpeaker() — see its doc comment.
    enqueueTask(std::make_unique<MergeSpeakersTask>(fileUrl, sourceName, targetName),
                static_cast<int>(m_tasks.size()));
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
    return static_cast<int>(m_tasks.size());
}

int TranscriptionController::tasksCompleted() const
{
    int count = 0;
    for (const auto &task : m_tasks) {
        if (task->status == Task::Status::Done || task->status == Task::Status::Failed)
            ++count;
    }
    return count;
}

QVariantList TranscriptionController::queuedFileUrls() const
{
    QVariantList list;
    for (const auto &task : m_tasks) {
        if (task->status == Task::Status::Queued)
            list.append(task->fileUrl);
    }
    return list;
}

QString TranscriptionController::currentJobKind() const
{
    if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_tasks.size()))
        return QString();
    return m_tasks[m_activeIndex]->kind();
}

bool TranscriptionController::activeCanPause() const
{
    if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_tasks.size()))
        return false;
    return m_tasks[m_activeIndex]->capabilities().pausable;
}

QVariantList TranscriptionController::jobs() const
{
    QVariantList list;
    for (const auto &task : m_tasks) {
        // The base row every kind shares; each kind's own extra fields come
        // from its toVariantMap() (the giant per-flag switch this used to be
        // is now distributed authorship — each Task authors its own row).
        QVariantMap entry;
        entry["id"] = task->id;
        entry["fileUrl"] = task->fileUrl;
        entry["status"] = taskStatusToString(task->status);
        entry["error"] = task->error;
        entry["kind"] = task->kind();
        entry["displayName"] = task->displayName();
        // -1 (QML sees it as a negative number) until Done/Failed.
        entry["durationMs"] = task->durationMs;
        const QVariantMap extra = task->toVariantMap();
        for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
            entry[it.key()] = it.value();
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
    ModelDownloadTarget target = ModelDownloadTarget::WhisperModel; // whisper tiers + parakeet
    if (modelId == QLatin1String(ModelManager::kLabelModelId)) {
        target = ModelDownloadTarget::LabelModel;
    } else if (modelId == QLatin1String(ModelManager::kVadModelId)) {
        target = ModelDownloadTarget::VadModel;
    } else {
        for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableSegmentationModels()) {
            if (info.id == modelId) {
                target = ModelDownloadTarget::SegmentationModel;
                break;
            }
        }
        for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableEmbeddingModels()) {
            if (info.id == modelId) {
                target = ModelDownloadTarget::EmbeddingModel;
                break;
            }
        }
    }

    // Same queue path used lazily before a transcribe — inserted at the tail so
    // it doesn't jump ahead of in-flight work. No-op if already downloaded or
    // already queued (maybeQueueDownloadTask returns -1).
    if (maybeQueueDownloadTask(target, m_vaultUrl, modelId) >= 0) {
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
    // Wave N (task system): a REAL queued task now. This used to bypass the
    // queue entirely — invoking a worker slot directly, with "no queue entry,
    // no file writes" and its own parallel setBenchmarkGlow() path — purely
    // because a queue entry would have raised MainPanel's blocking overlay
    // (m_busy was overloaded to mean both "glow" and "overlay"). That escape
    // hatch IS the bug class this refactor exists to remove: a task that runs
    // for minutes with no Tasks row and no cancel/progress plumbing of its
    // own. Presentation::Background now expresses "glow + row, no overlay"
    // directly, so it can just be enqueued like everything else — and it
    // inherits the queue's serialization (no GPU contention with a real
    // transcription) for free, which is what the manual worker dispatch was
    // hand-rolling.
    enqueueTask(std::make_unique<BenchmarkTask>(fileUrl, m_vaultUrl, modelIds));
}

void TranscriptionController::runDiarizationBenchmark(const QUrl &fileUrl, const QVariantList &combos)
{
    // Same story as runBenchmark() above.
    enqueueTask(std::make_unique<DiarizationSweepTask>(fileUrl, combos));
}

void TranscriptionController::cancelBenchmark()
{
    // Cancels whichever benchmark/sweep task is Active or Queued. Now that
    // both are real queued tasks, this is just cancelJob() over the two kinds
    // rather than a bare requestCancel() at the worker (which would also have
    // cancelled an unrelated real job that happened to be running).
    for (int i = static_cast<int>(m_tasks.size()) - 1; i >= 0; --i) {
        const QString kind = m_tasks[i]->kind();
        if (kind == QStringLiteral("benchmark") || kind == QStringLiteral("diarizationSweep"))
            cancelJob(m_tasks[i]->id);
    }
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
