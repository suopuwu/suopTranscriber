#include "TranscriptFormat.h"

#include "MdFrontmatter.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QLatin1Char>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <limits>

#include "DiarizationEngine.h" // DiarizedTurn — labelForSegment/spliceBodySpeakers
#include "LabelingEngine.h"    // LabelResult — writeLabelingFields

// See TranscriptFormat.h's top comment. Everything below moved here verbatim
// out of TranscriptionController.cpp's file-scope prologue; anything NOT
// declared in the header is local to this translation unit.

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

// Derived from the audio file's FULL name (fi.fileName(), extension
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
    // A malformed/non-numeric timestamp used to silently fall back
    // to 0.0 with no diagnostic — a corrupted .md file's mis-sorted/
    // mis-highlighted segment was otherwise undiagnosable. The 0.0 fallback
    // itself is unchanged (graceful degradation is still correct here), this
    // just stops it from being silent.
    if (!ok)
        qWarning() << "parseCompactTimestamp: could not parse timestamp" << raw;
    return result;
}

// Splits a frontmatter "speakers: A, B, C" value into trimmed, non-empty
// names — same comma-splitting shape as collectVaultFrontmatterList()' "tags:"
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
// shape varies), 2 the speaker tag's literal text (optional, only present
// when it matches one of knownSpeakers exactly), 3
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

// Written immediately when a job starts, before whisper_full runs, so a
// killed/stopped run still leaves a file recording which settings it was
// transcribed with (durability, not full resume).
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
// See this function's doc comment in TranscriptionController.h.
QString formatSilencesValue(const std::vector<std::pair<double, double>> &silences)
{
    QStringList parts;
    parts.reserve(static_cast<int>(silences.size()));
    for (const auto &span : silences)
        parts << formatTimestamp(span.first) + QLatin1Char('-') + formatTimestamp(span.second);
    return parts.join(QStringLiteral(", "));
}

void writeFrontmatter(QFile &mdFile, const QString &modelUsed, bool speakerDetection, bool modelFallback,
                      const QString &requestedModelId, const QStringList &speakerLabels,
                      const QString &speakerDetectionError, bool vad, const QString &vadError, bool partial,
                      const std::optional<std::vector<std::pair<double, double>>> &silences,
                      const QString &corruptAudioNote)
{
    QTextStream out(&mdFile);
    out << "---\n";
    out << "model: " << modelUsed << '\n';
    // Written up front for EVERY run; TranscribeTask removes it only on a
    // fully-completed run, so a cancelled/killed job's truncated .md keeps it
    // and reads back as PartialStatus in the vault (VaultTreeModel).
    if (partial)
        out << "partial: true\n";
    out << "speakerDetection: " << (speakerDetection ? "true" : "false") << '\n';
    if (speakerDetection && !speakerLabels.isEmpty())
        out << "speakers: " << speakerLabels.join(QStringLiteral(", ")) << '\n';
    if (!speakerDetectionError.isEmpty())
        out << "speakerDetectionError: " << speakerDetectionError << '\n';
    out << "vad: " << (vad ? "true" : "false") << '\n';
    if (!vadError.isEmpty())
        out << "vadError: " << vadError << '\n';
    // silences: nullopt (VAD didn't run yet) omits the key entirely — see
    // this function's doc comment in the header.
    if (silences)
        out << "silences: " << formatSilencesValue(*silences) << '\n';
    if (!corruptAudioNote.isEmpty())
        out << "corruptAudioNote: " << corruptAudioNote << '\n';
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

bool transcriptIsLabeled(const QString &label, const QString &tags, const QString &body)
{
    return !label.isEmpty() || !tags.isEmpty() || body.contains(kPreludeEndMarker);
}

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

// ParsedSegment is declared in TranscriptionController.h so a ChaptersTask
// can build one without duplicating the struct.

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

// A MANUAL chapter marker carries this trailing HTML-comment sentinel
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

// Chapter-marker body mutators (used by TranscriptionController::editChapterMarker()).
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
    QStringList frontmatterLines;
    QString body;
    if (!MdFrontmatter::read(mdPath, &frontmatterLines, &body, errorOut))
        return false;

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

    // Strip only AUTO markers; PRESERVE manual ones (they carry the
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
