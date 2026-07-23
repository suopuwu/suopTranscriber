#pragma once

#include <QFile>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>
#include <utility>
#include <vector>

#include "Segment.h" // SegmentLineWriter::append() takes a const Segment &

// The transcript .md file format, in one place: timestamp/segment-line
// formatting and parsing, the frontmatter writers, the prelude/chapter/
// speaker-tag body splices, and the durable SegmentLineWriter. Every one of
// these is a FREE function over a path or a body string — none of them touches
// TranscriptionController state, which is why they used to sit as ~1400 lines
// of file-scope code at the top of TranscriptionController.cpp. Split out
// verbatim (no behavior, signature, or naming change) so both the controller
// and the Task subclasses in src/core/tasks/ have one obvious home to include.
//
// Deliberately NOT namespaced: the neighbouring single-purpose helper headers
// (VaultFileOps.h, FileRename.h, CombineNaming.h, RegexValidation.h) all
// declare plain global free functions, and every call site here predates the
// split — MdFrontmatter.h's namespace is the exception, not the convention.
//
// Raw frontmatter block reading/upserting lives in MdFrontmatter.h — this
// header reuses it rather than duplicating that parse.

struct DiarizedTurn; // DiarizationEngine.h — forward-declared so this header
                     // needn't pull in sherpa-onnx (spliceBodySpeakers only
                     // takes a const-ref to a vector of these).
struct LabelResult; // LabelingEngine.h — forward-declared for writeLabelingFields()

// Writes segment lines to an open sidecar .md file (see appendSegmentLine's
// former doc comment / CLAUDE.md "vault layout"), merging a REPEATING CYCLE
// of consecutive segments (period 1..maxCycleLength — one repeated line, or
// a repeating block of 2, 3, ... lines) into just ONE copy of the cycle —
// whisper.cpp occasionally hallucinates the exact same sentence, OR the
// exact same short alternating pattern (e.g. "[MUSIC PLAYING]" / "[APPLAUSE]"
// back and forth), across many consecutive segments during a silence
// stretch, and per the user's request that must be merged IN THE FILE
// ITSELF, not just hidden by a display-layer dedup. Each line of a merged
// cycle keeps its own FIRST-occurrence timestamp and gets its own trailing
// " (×N)" repeat-count marker (see segmentLinePattern() in
// TranscriptionController.cpp for the parse side) — N is how many times
// THAT particular line occurred, which for every line in a repeating cycle
// is the same N (the cycle's repeat count). Two lines with identical text
// but DIFFERENT speakers are never merged.
//
// maxCycleLength bounds the search purely for performance — every new
// segment does an O(maxCycleLength^2) check against a small trailing
// window, never a full-file scan, so this stays cheap regardless of
// transcript length. Configurable (AppSettings.duplicateCycleWindow in QML
// -> TranscriptionController::transcribe()'s duplicateCycleWindow param),
// default 3 (checks period 1, 2, AND 3 — not just 3 alone).
//
// Durability is preserved despite the merge: each append() call leaves the
// file in a fully valid, flushed state — a duplicate cycle doesn't append
// new lines, it seeks back to the start of the EARLIEST line in the
// matched cycle (valid because writes are always sequential/at EOF, and
// every LineRecord's fileOffset is kept up to date across rewrites) and
// rewrites just that one copy in place with incremented counts, so a kill
// mid-run still leaves a correctly-formatted (if not-yet-fully-repeated)
// file. Declared here (rather than file-local in the .cpp) so tests can
// drive it directly with synthetic Segments, without needing a real
// whisper model.
class SegmentLineWriter {
public:
    explicit SegmentLineWriter(QFile &mdFile, int maxCycleLength = 3);

    // speakerLabel: empty = speaker detection wasn't on (or produced no
    // label) for this segment — no leading tag written, matching the
    // legacy no-detection format exactly. Non-empty = the real diarized
    // speaker label ("Speaker 1", "Speaker 2", ...; see DiarizationEngine).
    // Segments with empty (whitespace-only) text are silently skipped, same
    // as the old appendSegmentLine did.
    void append(const Segment &segment, const QString &speakerLabel);

private:
    // One physical line currently at the tail of the file, as it stands
    // right now (fileOffset/repeatCount reflect the CURRENT on-disk state,
    // which may already have been rewritten by an earlier merge).
    struct LineRecord {
        qint64 fileOffset = 0;
        QString speakerLabel;
        QString text;
        double t0 = 0.0;
        int repeatCount = 1;
    };

    void writeRecord(const LineRecord &record);
    void tryMergeCycle();

    QFile &m_mdFile;
    int m_maxCycleLength;
    // Bounded to the last 2*maxCycleLength entries (see tryMergeCycle()) —
    // that's all a period-<=maxCycleLength check can ever need to look at.
    QVector<LineRecord> m_tail;
};

// Feature 3a ("generate chapters"): one resolved chapter boundary, ready to
// splice into a transcript body — t0 already resolved from whatever segment
// index the LLM proposed (see LabelingEngine::generateChapters()'s doc
// comment for why the raw index is never trusted as a timestamp directly;
// that resolution happens in TranscriptionController.cpp's
// TranscriptionWorker::generateChaptersJob(), against its own freshly-parsed
// segment list, before this struct is ever built).
struct ResolvedChapter {
    double t0 = 0.0;
    QString title;
};

// Strips every existing "## [timestamp] title" chapter marker line out of
// `body` (regeneration must replace the old marker set wholesale, never
// accumulate duplicates across repeated "generate chapters" runs — same
// unconditional-overwrite policy as Feature 2's prelude), then inserts each
// of `chapters`' marker lines immediately before the segment line whose own
// leading "[timestamp]" exactly matches that chapter's t0 (formatted via the
// same formatTimestamp() every segment line already uses, so the match is
// exact by construction whenever t0 came from parsing this same file — see
// formatSegmentLine() in the .cpp). A chapter whose t0 doesn't match any
// segment line is silently skipped rather than corrupting the body.
//
// Collision-free with segmentLinePattern() by construction (not just lower
// risk, unlike the prelude's numbered-list false-positive — see
// stripPreludeIfPresent()'s doc comment): that pattern is anchored on a
// literal "^\[", and a line starting with "## " can never match it.
//
// Chapters are inserted internally in DESCENDING offset order (the
// last-in-the-file chapter first) — inserting by ascending string offset
// would invalidate every not-yet-inserted chapter's offset as each earlier
// insertion shifts the string. Callers don't need to pre-sort `chapters`.
//
// Declared here (rather than file-local in the .cpp, mirroring
// SegmentLineWriter above) so tests can drive the splicing logic directly
// with synthetic values, without needing a real label model.
QString spliceBodyChapters(const QString &body, const QList<ResolvedChapter> &chapters);

// Item 52 (standalone speaker diarization): given an already-transcribed
// .md's `body` and its frontmatter "speakers:" allowlist (`knownSpeakers`),
// assigns each segment line the diarized speaker `turns` say it overlaps
// (labelForSegment()'s time-overlap rule) and splices a "[Speaker N]" tag
// into that line IN PLACE — replacing an existing tag if the line already had
// one (a re-diarize), or inserting one right after the timestamp if not
// (adding speakers to a file transcribed without them). Everything else
// (prelude, "## [ts] title" chapter markers, "(×N)" repeat markers, the
// spoken text itself) is preserved byte-for-byte; edits are spliced from the
// end of the body backward so earlier captured offsets stay valid. A
// hallucinated non-speech annotation line (isNonSpeechAnnotation, e.g.
// "[MUSIC]") is never tagged, matching run()'s inline path. `totalSeconds`
// (the decoded audio length) stands in for the LAST segment's end time —
// every earlier segment's end is the next segment's start (the file format
// stores no t1). *appliedSpeakersOut, if given, receives the distinct labels
// actually applied, in first-appearance (file) order — what the caller writes
// back as the "speakers:" frontmatter list (mirrors
// syncSpeakersFrontmatterToBody()'s "only tags that really landed" rule).
// Declared here (like spliceBodyChapters above) so the splicing logic is
// unit-testable with synthetic turns, no real DiarizationEngine needed.
QString spliceBodySpeakers(const QString &body, const QStringList &knownSpeakers,
                            const std::vector<DiarizedTurn> &turns, double totalSeconds,
                            QStringList *appliedSpeakersOut = nullptr);

// --- Transcript-format / vault-file helpers (Wave N task system) ---------
// Declared here (definitions stay in the .cpp, same file-scope-not-anonymous
// pattern already used above for isEngineModelId/discoverEngines/etc. and for
// spliceBodySpeakers/spliceBodyChapters) so a Task subclass in src/core/tasks/
// can call them directly without duplicating this parsing/writing logic — the
// whole point of the task-system refactor (.task/plan.md) is that a kind's
// doing-code has exactly ONE home. TranscriptionController.cpp remains the
// only .cpp that DEFINES them until step 3 of that plan deletes the old
// per-kind worker-slot bodies that used to be their only caller.

// See this function's definition-site doc comment for the full
// audio-file-name-vs-stem collision rationale. Every sidecar-path call site
// in this file and in VaultTreeModel.cpp must stay consistent with it.
QString sidecarPath(const QString &audioFilePath);

// The sentinel marking the end of an AI-generated prelude in a transcript
// body (see stripPreludeIfPresent()/spliceBodyPrelude() at the definition
// site) — a RelabelTask/ChaptersTask's skipIfAlreadyLabeled check looks for
// this to tell "already labeled" from "never labeled".
extern const QString kPreludeEndMarker;

// Written immediately when a transcribe task starts, before any decoding
// happens, so a killed/stopped run still leaves a file recording what it was
// run with. See the definition site for the full field-by-field contract
// (modelFallback/requestedModelId, speakers: allowlist, vad/vadError).
//
// silences (silence-capture feature, phase 1): std::nullopt means "not yet
// known" (VAD didn't run — TranscribeTask fills the real value in with a
// transcript-gap fallback once the transcript exists, see its doc comment)
// and OMITS the "silences:" key entirely; a present-but-empty vector means
// "checked, genuinely no silence found" and WRITES the key with an empty
// value — the key is present for every transcription that got a real answer
// one way or another, distinct from "never checked".
void writeFrontmatter(QFile &mdFile, const QString &modelUsed, bool speakerDetection, bool modelFallback,
                      const QString &requestedModelId, const QStringList &speakerLabels,
                      const QString &speakerDetectionError, bool vad, const QString &vadError, bool partial,
                      const std::optional<std::vector<std::pair<double, double>>> &silences = std::nullopt);

// Formats a list of (start, end) SILENCE spans (VadChunker::complementSpans()'
// output) into the compact "12.4-18.9, 45-52.3" frontmatter value — the same
// trimmed-timestamp style formatTimestamp() already uses for segment lines.
// Shared by writeFrontmatter()'s initial write and DetectSilenceTask's
// in-place MdFrontmatter::upsertKey() call, so every writer produces
// byte-for-byte the same shape. Empty input yields an empty string (see
// writeFrontmatter()'s doc comment for how "empty because none found" is
// distinguished from "not yet known").
QString formatSilencesValue(const std::vector<std::pair<double, double>> &silences);

// Splices a LabelingEngine result's label/tags/prelude fields into an
// already-transcribed .md in place. Used by both the manual "regenerate
// label" action and the auto-label follow-up after a transcribe.
void writeLabelingFields(const QString &mdPath, const LabelResult &result);

// Speaker-rename/merge, operating directly on a sidecar .md's frontmatter +
// body (see segmentLinePattern()'s doc comment in the .cpp for how a "[tag]"
// is recognized). renameSpeakerInFile REJECTS colliding with an existing
// name (typo protection); mergeSpeakersInFile deliberately allows it (an
// explicit "combine these two speakers" action). Both no-op successfully if
// old==new / source==target.
bool renameSpeakerInFile(const QString &path, const QString &oldName, const QString &newName, QString *errorOut);
bool mergeSpeakersInFile(const QString &path, const QString &sourceName, const QString &targetName,
                          QString *errorOut);

// Splits an already-written .md into its raw frontmatter lines (minus the
// "---" delimiters), body, the line index of "speakers:" within
// frontmatterLines (-1 if absent), its parsed value (knownSpeakers), and
// every speaker tag actually PRESENT in the body (distinctTags — the set
// segmentLinePattern() matched, which may differ from the frontmatter
// allowlist). false + *errorOut on a missing/malformed file. A DiarizeTask
// uses this directly (not just via rename/merge) to read the file it's
// about to splice speaker tags into.
bool parseSpeakerTaggedFile(const QString &path, QStringList *frontmatterLines, QString *body,
                             int *speakersLineIndex, QStringList *knownSpeakers, QStringList *distinctTags,
                             QString *errorOut);

// Rewrites path's frontmatter block + body from already-edited pieces (the
// inverse of parseSpeakerTaggedFile) — the shared write-back every speaker-
// tag-editing kind (rename/merge/diarize) funnels through.
bool writeFrontmatterBlock(const QString &path, const QStringList &frontmatterLines, const QString &body,
                           QString *errorOut);

// Rewrites a .md's "speakers:" frontmatter line to exactly the tags that
// actually appear in the body (first-appearance order), dropping the line
// entirely if none do — see the definition site for why a raw diarization
// cluster count can otherwise leave a phantom, never-used entry.
void syncSpeakersFrontmatterToBody(const QString &path);

// Splits a frontmatter "speakers: A, B, C" value into trimmed, non-empty
// names. Empty/missing input (speaker detection was off, or predates this
// key) yields an empty list.
QStringList parseSpeakersList(const QString &raw);

// Extracts the plain spoken text from a transcript body — strips prelude/
// chapter-marker/timestamp/speaker-tag/repeat-marker decoration, leaving just
// what was said (knownSpeakers scopes which "[tag]" runs are recognized as
// speaker tags rather than ordinary bracketed text — see
// segmentLinePattern()'s doc comment in the .cpp). Feeds a relabel/chapters
// task's LLM call.
QString plainTextFromTranscriptBody(const QString &body, const QStringList &knownSpeakers);

// One segment's parsed identity from a transcript body — its position in the
// returned QList IS its index, the same indexing LabelingEngine::
// generateChapters() expects and returns "afterSegment" against. A
// ChaptersTask needs both t0 (to resolve an LLM-returned index to a real
// timestamp before anything touches disk) and text (what's shown to the
// model) — plainTextFromTranscriptBody() above discards t0, so this is a
// separate walk, not a wrapper around it.
struct ParsedSegment {
    double t0 = 0.0;
    QString text;
};
QList<ParsedSegment> parsedSegmentsFromTranscriptBody(const QString &body, const QStringList &knownSpeakers);

// Splices resolved chapter markers into mdPath's "## [ts] title" region and
// rewrites the file — see spliceBodyChapters()'s doc comment for the
// splicing rule itself; this is the file-write wrapper a ChaptersTask calls
// once chapter indices are resolved to real timestamps.
bool writeChaptersField(const QString &mdPath, const QList<ResolvedChapter> &chapters, QString *errorOut);

// --- Timestamp / segment-line format ------------------------------------
// Previously file-scope in TranscriptionController.cpp; declared here now that
// the definitions live in TranscriptFormat.cpp but the controller (readChapters/
// editChapterMarker/readSentences/readSilences etc.) still calls them.

// Compact H:MM:SS.fff with leading zero-valued units and trailing fractional
// zeros dropped — see the definition site for the full rationale/examples.
QString formatTimestamp(double seconds);

// Inverse of formatTimestamp(): splits on ':' rather than matching fixed digit
// widths, since the compact format's unit count varies. A malformed value
// warns and degrades to 0.0.
double parseCompactTimestamp(const QString &raw);

// The one regex every segment-line walk uses: "[H:MM:SS.fff] [Speaker N] text"
// with optional " (×N)"/" [SPEAKER_TURN]" markers. knownSpeakers is REQUIRED
// (it scopes which "[tag]" runs count as speaker tags) — see the definition
// site's doc comment for the group map and why an open-ended match is unsafe.
QRegularExpression segmentLinePattern(const QStringList &knownSpeakers);

// Strips the prelude region (everything through kPreludeEndMarker) off a body,
// leaving just segment-line content; spliceBodyPrelude() replaces it. See the
// definition site for why the marker is explicit rather than inferred.
QString stripPreludeIfPresent(const QString &body);
QString spliceBodyPrelude(const QString &body, const QString &newPrelude);

// --- Chapter markers ("## [ts] Title", optional manual sentinel) ---------
// The parse/format/mutate set behind readChapters()/editChapterMarker()/
// addChapter() in the controller and spliceBodyChapters() below.
QRegularExpression chapterMarkerPattern();
bool chapterTitleCaptureIsManual(const QString &titleCapture);
QString chapterDisplayTitle(const QString &titleCapture);
QString formatChapterMarker(const QString &tsText, const QString &title, bool manual);
bool findChapterMarkerAt(const QString &body, double ts, QRegularExpressionMatch *out);
int chapterInsertOffset(const QString &body, double ts);
void removeChapterMarkerLine(QString *body, const QRegularExpressionMatch &m);
