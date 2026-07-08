#include "TranscriptionController.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTextStream>
#include <QVector>

#include <algorithm>
#include <limits>
#include <memory>

#include "AudioDecoder.h"
#include "DiarizationEngine.h"
#include "LabelingEngine.h"
#include "ModelManager.h"
#include "Segment.h"
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

QString sidecarPath(const QString &audioFilePath)
{
    const QFileInfo fi(audioFilePath);
    return fi.absolutePath() + "/" + fi.completeBaseName() + ".md";
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
    if (parts.size() >= 3)
        return parts[0].toDouble() * 3600.0 + parts[1].toDouble() * 60.0 + parts[2].toDouble();
    if (parts.size() == 2)
        return parts[0].toDouble() * 60.0 + parts[1].toDouble();
    return parts[0].toDouble();
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
    case TranscriptionController::JobStatus::Queued:
        return QStringLiteral("queued");
    case TranscriptionController::JobStatus::Active:
        return QStringLiteral("active");
    case TranscriptionController::JobStatus::Done:
        return QStringLiteral("done");
    case TranscriptionController::JobStatus::Failed:
        return QStringLiteral("failed");
    }
    return QString();
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
void writeFrontmatter(QFile &mdFile, const QString &modelUsed, bool speakerDetection, bool modelFallback,
                      const QString &requestedModelId, const QStringList &speakerLabels,
                      const QString &speakerDetectionError)
{
    QTextStream out(&mdFile);
    out << "---\n";
    out << "model: " << modelUsed << '\n';
    out << "speakerDetection: " << (speakerDetection ? "true" : "false") << '\n';
    if (speakerDetection && !speakerLabels.isEmpty())
        out << "speakers: " << speakerLabels.join(QStringLiteral(", ")) << '\n';
    if (!speakerDetectionError.isEmpty())
        out << "speakerDetectionError: " << speakerDetectionError << '\n';
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

// AI auto-labeling's second pass runs AFTER the primary frontmatter+segments
// have already been written and mdFile closed (needs the full transcript
// text, which isn't known until transcription completes) — reopens the same
// .md and splices in label/summary/tags. Splice-or-append (not append-only):
// a fresh transcribe job's frontmatter never has these keys yet (so this
// falls through to append), but "regenerate auto-title" (relabel, see
// TranscriptionController::relabel()) reruns this against a .md that already
// has them — must replace in place, not duplicate. A failure here IS
// surfaced as its own job's failure (see TranscriptionController::relabel())
// — auto-labeling now always runs as a separate, visible follow-up task (see
// transcribe()'s autoLabel doc), not a silent bonus pass baked into the
// transcription job itself. Newlines are stripped from the values since this
// is a flat line-based format — a raw '\n' inside a JSON string (from a "\n"
// escape) would otherwise corrupt the frontmatter block.
void writeLabelingFields(const QString &mdPath, const LabelResult &result)
{
    QFile mdFile(mdPath);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "auto-label: could not reopen" << mdPath << "to write label/summary/tags";
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
    spliceOrAppend(QStringLiteral("summary"), sanitize(result.summary));
    spliceOrAppend(QStringLiteral("tags"), sanitize(result.tags));

    if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "auto-label: could not reopen" << mdPath << "for writing";
        return;
    }
    QTextStream out(&mdFile);
    out << "---\n";
    for (const QString &fmLine : frontmatterLines)
        out << fmLine << '\n';
    out << "---\n";
    out << body;
    out.flush();
    mdFile.flush();
}

// Plain prose for the labeling prompt (see relabel()'s slot below): strips
// the "[start - end]"/" [SPEAKER_TURN]" formatting back out of an existing
// transcript body, leaving just the spoken text — same shape as the text
// TranscriptionWorker::run() accumulates live during a fresh transcription.
// knownSpeakers is the file's own frontmatter "speakers:" allowlist (see
// segmentLinePattern()'s doc comment) — the caller reads it alongside the
// body since both come from the same frontmatter block.
QString plainTextFromTranscriptBody(const QString &body, const QStringList &knownSpeakers)
{
    QStringList pieces;
    QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString text = m.captured(3).trimmed();
        if (!text.isEmpty())
            pieces.append(text);
    }
    return pieces.join(QLatin1Char(' '));
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

// Lives on TranscriptionController's dedicated QThread; never touches the
// GUI thread. Owns the warm WhisperCppEngine so the model stays resident
// across transcribe() calls.
class TranscriptionWorker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

public slots:
    void run(const QString &filePath, const QString &vaultPath, const QString &modelId, bool speakerDetection)
    {
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

        QString decodeError;
        auto pcm = AudioDecoder::decodeTo16kMono(filePath, &decodeError);
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
                    // Diarization + transcription share ONE continuous
                    // progress bar rather than each getting its own 0->1
                    // sweep (which would look like the bar finishes then
                    // restarts from empty) — detection is the first half,
                    // transcription is the second (see the "transcribing"
                    // phase below, which picks up at 0.5 to match).
                    diarizedTurns = m_diarizationEngine->diarize(
                        *pcm, [this](double fraction) { emit progress(fraction * 0.5); });
                    effectiveSpeakerDetection = true;
                } else {
                    speakerDetectionError = QStringLiteral("failed to load speaker diarization models");
                }
            } else {
                speakerDetectionError =
                    diarizeError.isEmpty() ? QStringLiteral("speaker diarization models unavailable") : diarizeError;
            }
        }

        QFile mdFile(sidecarPath(filePath));
        if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit finished(false, QStringLiteral("could not open %1 for writing").arg(mdFile.fileName()));
            return;
        }

        QStringList speakerLabels;
        for (int i = 1; i <= distinctSpeakerCount(diarizedTurns); ++i)
            speakerLabels.append(QStringLiteral("Speaker %1").arg(i));

        writeFrontmatter(mdFile, QFileInfo(modelPath).fileName(), effectiveSpeakerDetection, modelFallback, modelId,
                          speakerLabels, speakerDetectionError);
        SegmentLineWriter lineWriter(mdFile);

        AudioChunk chunk;
        chunk.pcm16kMono = std::move(*pcm);
        chunk.offsetSeconds = 0.0; // whole-file, single chunk — no VAD chunking yet

        // "transcribing": whisper's own per-segment progress. When
        // diarization actually ran (effectiveSpeakerDetection), this phase
        // is the SECOND half of one continuous bar (0.5 -> 1.0), picking up
        // exactly where diarization's own progress left off — not a reset
        // to 0, which would look like the bar finished then restarted. When
        // diarization didn't run (speakerDetection off, or skipped/failed
        // before ever calling diarize()), transcription gets the WHOLE bar
        // (0 -> 1.0) as before, since no time was spent on detection at all.
        emit phase(QStringLiteral("transcribing"));
        emit progress(effectiveSpeakerDetection ? 0.5 : 0.0);
        ProgressCallback onProgress = [this, effectiveSpeakerDetection](double done, double total) {
            const double fraction = total > 0 ? done / total : 0.0;
            emit progress(effectiveSpeakerDetection ? 0.5 + fraction * 0.5 : fraction);
        };
        SegmentCallback onSegment = [&lineWriter, &diarizedTurns, effectiveSpeakerDetection](const Segment &segment) {
            // Chunk offset merge is the app's job (see WhisperCppEngine.cpp);
            // with offsetSeconds always 0 this milestone it's a no-op, kept
            // explicit here as the seam for when chunking lands.
            // See isNonSpeechAnnotation()'s doc comment: a hallucinated
            // "[APPLAUSE]"/"[MUSIC]"/etc. segment never gets a speaker
            // label, regardless of what diarization found nearby.
            const bool isSpeech = !isNonSpeechAnnotation(QString::fromStdString(segment.text).trimmed());
            const QString label = (effectiveSpeakerDetection && isSpeech)
                ? labelForSegment(diarizedTurns, segment.t0, segment.t1) : QString();
            lineWriter.append(segment, label);
        };

        m_engine->transcribe(chunk, onProgress, onSegment);
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

        emit finished(true, {});
    }

    // "Regenerate auto-title" (item 9): re-runs JUST the labeling pass on an
    // already-transcribed file — no audio decode, no whisper, no frontmatter
    // reset. Unlike the autoLabel branch above, a failure here IS the job's
    // result (there's nothing else for this job to accomplish).
    void relabelJob(const QString &filePath, const QString &vaultPath, bool restrictTagsToExisting,
                     double tagReuseBias)
    {
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
        // Pull out "speakers:" while skipping the rest of the frontmatter —
        // plainTextFromTranscriptBody() needs the file's own known-speaker
        // allowlist (see segmentLinePattern()'s doc comment) to correctly
        // strip ONLY real speaker tags, not hallucinated bracketed text.
        QStringList knownSpeakers;
        QString line;
        while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
            const int colon = line.indexOf(':');
            if (colon >= 0 && line.left(colon).trimmed() == QStringLiteral("speakers"))
                knownSpeakers = parseSpeakersList(line.mid(colon + 1));
        }
        const QString transcriptText = plainTextFromTranscriptBody(in.readAll(), knownSpeakers);
        mdFile.close();

        if (transcriptText.trimmed().isEmpty()) {
            emit finished(false, QStringLiteral("%1 has no transcript text to label").arg(mdPath));
            return;
        }

        QString error;
        if (!runLabelingPass(vaultPath, transcriptText, mdPath, restrictTagsToExisting, tagReuseBias, &error)) {
            emit finished(false, error);
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

signals:
    void progress(double fraction);
    // See TranscriptionController::currentPhase's doc comment — emitted at
    // each phase transition within run() (loading/detecting-speakers/
    // transcribing); never emitted by relabelJob()/renameSpeaker()/
    // mergeSpeakers() (none of these have phases worth naming).
    void phase(const QString &phase);
    void finished(bool ok, const QString &error);
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
                         bool restrictTagsToExisting, double tagReuseBias, QString *errorOut = nullptr)
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
        QString genError;
        const auto result =
            m_labelingEngine->label(transcriptText, existingTags, restrictTagsToExisting, tagReuseBias, &genError);
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
};

TranscriptionController::TranscriptionController(QObject *parent)
    : QObject(parent)
{
    m_worker = new TranscriptionWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &TranscriptionWorker::progress, this, [this](double fraction) {
        m_progress = fraction;
        emit progressChanged();
    });
    connect(m_worker, &TranscriptionWorker::phase, this, [this](const QString &phase) {
        m_phase = phase;
        emit currentPhaseChanged();
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
    m_thread.quit();
    m_thread.wait();
}

bool TranscriptionController::isQueued(const QUrl &fileUrl) const
{
    for (const Job &job : m_jobs) {
        if (job.fileUrl == fileUrl && (job.status == JobStatus::Queued || job.status == JobStatus::Active))
            return true;
    }
    return false;
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
                                          double tagReuseBias)
{
    // Done/Failed entries for the same fileUrl deliberately do NOT block
    // re-queueing (see class doc) — only Queued/Active does.
    if (isQueued(fileUrl))
        return;

    Job job;
    job.fileUrl = fileUrl;
    job.vaultUrl = vaultUrl;
    job.modelId = modelId;
    job.speakerDetection = speakerDetection;
    job.autoLabel = autoLabel;
    job.restrictTagsToExisting = restrictTagsToExisting;
    job.tagReuseBias = tagReuseBias;
    job.status = JobStatus::Queued;
    m_jobs.push_back(job);
    const int jobIndex = m_jobs.size() - 1;

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
        Job labelJob;
        labelJob.fileUrl = fileUrl;
        labelJob.vaultUrl = vaultUrl;
        labelJob.relabelOnly = true;
        labelJob.restrictTagsToExisting = restrictTagsToExisting;
        labelJob.tagReuseBias = tagReuseBias;
        labelJob.status = JobStatus::Queued;
        m_jobs.push_back(labelJob);
    }

    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(jobIndex);
}

void TranscriptionController::transcribeAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                             const QString &modelId, bool speakerDetection, bool autoLabel,
                                             bool restrictTagsToExisting, double tagReuseBias)
{
    for (const QVariant &v : fileUrls)
        transcribe(v.toUrl(), vaultUrl, modelId, speakerDetection, autoLabel, restrictTagsToExisting, tagReuseBias);
}

void TranscriptionController::relabel(const QUrl &fileUrl, const QUrl &vaultUrl, bool restrictTagsToExisting,
                                       double tagReuseBias)
{
    // Same dedup rule as transcribe(): a Queued/Active entry (whether a full
    // transcribe or a relabel-only job) blocks a duplicate request.
    if (isQueued(fileUrl))
        return;

    Job job;
    job.fileUrl = fileUrl;
    job.vaultUrl = vaultUrl;
    job.relabelOnly = true;
    job.restrictTagsToExisting = restrictTagsToExisting;
    job.tagReuseBias = tagReuseBias;
    job.status = JobStatus::Queued;
    m_jobs.push_back(job);
    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(m_jobs.size() - 1);
}

void TranscriptionController::relabelAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                          bool restrictTagsToExisting, double tagReuseBias)
{
    for (const QVariant &v : fileUrls)
        relabel(v.toUrl(), vaultUrl, restrictTagsToExisting, tagReuseBias);
}

void TranscriptionController::startJob(int index)
{
    m_activeIndex = index;
    Job &job = m_jobs[index];
    job.status = JobStatus::Active;
    job.error.clear();

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
                                   Q_ARG(bool, job.restrictTagsToExisting), Q_ARG(double, job.tagReuseBias));
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
    const QUrl finishedFile = m_currentFile;

    if (m_activeIndex >= 0 && m_activeIndex < m_jobs.size()) {
        Job &job = m_jobs[m_activeIndex];
        job.status = ok ? JobStatus::Done : JobStatus::Failed;
        job.error = error;
    }
    m_activeIndex = -1;

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

QVariantMap TranscriptionController::readMetadata(const QUrl &fileUrl) const
{
    QVariantMap result;

    const QFileInfo fi(fileUrl.toLocalFile());
    QFile mdFile(fi.absolutePath() + "/" + fi.completeBaseName() + ".md");
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
    return result;
}

bool TranscriptionController::setLabel(const QUrl &fileUrl, const QString &label, QString *errorOut) const
{
    const QFileInfo fi(fileUrl.toLocalFile());
    const QString path = fi.absolutePath() + "/" + fi.completeBaseName() + ".md";

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
    return true;
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
    const QFileInfo fi(fileUrl.toLocalFile());
    QFile mdFile(fi.absolutePath() + "/" + fi.completeBaseName() + ".md");
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---"))
        return QString();

    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        // skip frontmatter lines
    }
    return in.readAll();
}

QVariantList TranscriptionController::parseSentences(const QUrl &fileUrl) const
{
    QVariantList sentences;
    const QString body = readTranscriptBody(fileUrl);
    // The file's own known-speaker allowlist (see writeFrontmatter()'s and
    // segmentLinePattern()'s doc comments) — read via readMetadata() rather
    // than re-scanning frontmatter lines by hand, since that already parses
    // arbitrary "key: value" frontmatter lines generically (vaults are
    // small, a second open+read costs nothing worth avoiding here).
    const QStringList knownSpeakers =
        parseSpeakersList(readMetadata(fileUrl).value(QStringLiteral("speakers")).toString());
    // globalMatch over the whole (multi-line) body, not a per-line split, so
    // capturedStart(3)/capturedLength(3) are exact offsets into this SAME
    // body string — what MainPanel.qml needs to span-highlight the active
    // sentence's substring without re-deriving offsets itself. No "t1": it's
    // no longer stored (see SegmentLineWriter's doc comment — it was always
    // identical to the next segment's t0) and nothing ever consumed it here.
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
    return sentences;
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
    if (job.renameSpeakerOnly)
        return QStringLiteral("rename");
    if (job.mergeSpeakersOnly)
        return QStringLiteral("merge");
    if (job.relabelOnly)
        return QStringLiteral("relabel");
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
        entry["fileUrl"] = job.fileUrl;
        entry["status"] = jobStatusToString(job.status);
        entry["error"] = job.error;
        entry["kind"] = jobKind(job);
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
        list.append(entry);
    }
    return list;
}

#include "TranscriptionController.moc"
