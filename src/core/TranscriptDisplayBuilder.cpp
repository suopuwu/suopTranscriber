#include "TranscriptDisplayBuilder.h"

#include <QtGlobal>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <system_error>

namespace {

// Field-name keys, spelled once. Every entry pushed below carries EXACTLY the
// key set its JS object literal carried — no more, no less. That is
// load-bearing, not tidiness: a real sentence entry deliberately has NO
// "isSilenceMarker"/"collapsed"/"silenceSeconds" keys, and QML reads a missing
// key as `undefined` (falsy), which is what the overlay Repeaters' filters
// (`!s.isSilenceMarker`, `s.isChapterHeading`) are written against.
const QString kIdx = QStringLiteral("idx");
const QString kT0 = QStringLiteral("t0");
const QString kT1 = QStringLiteral("t1");
const QString kChapterT0 = QStringLiteral("chapterT0");
const QString kSpeaker = QStringLiteral("speaker");
const QString kText = QStringLiteral("text");
const QString kStart = QStringLiteral("start");
const QString kLength = QStringLiteral("length");
const QString kShowGutter = QStringLiteral("showGutter");
const QString kIsChapterHeading = QStringLiteral("isChapterHeading");
const QString kIsSilenceMarker = QStringLiteral("isSilenceMarker");
const QString kChapterTitle = QStringLiteral("chapterTitle");
const QString kChapterManual = QStringLiteral("chapterManual");
const QString kCollapsed = QStringLiteral("collapsed");
const QString kSilenceSeconds = QStringLiteral("silenceSeconds");
const QString kTitle = QStringLiteral("title");
const QString kManual = QStringLiteral("manual");
const QString kRepeatCount = QStringLiteral("repeatCount");

// The effective (pending-rename-applied) display name for a raw on-disk
// speaker id — the C++ half of MainPanel's effectiveSpeakerName(). The raw
// name stays the identity everywhere else (distinctSpeakerIds /
// commitSpeakerRename address it directly); this override is purely the
// optimistic-UI overlay for an in-flight rename.
QString effectiveSpeakerName(const QString &rawName, const QVariantMap &overrides)
{
    const auto it = overrides.constFind(rawName);
    return it == overrides.constEnd() ? rawName : it->toString();
}

} // namespace

QString TranscriptDisplayBuilder::jsNumberToString(double value)
{
    // The collapse map is keyed by String(chapterT0) — a JS number-to-string,
    // not any of Qt's number formatters. Getting this wrong is silent: the
    // lookup simply never matches and no chapter ever appears collapsed. JS
    // renders the SHORTEST decimal that round-trips (String(7) -> "7",
    // String(7.25) -> "7.25", never "7.000000"), which is exactly what
    // std::to_chars produces with no precision argument.
    if (std::isnan(value))
        return QStringLiteral("NaN");
    if (std::isinf(value))
        return value > 0 ? QStringLiteral("Infinity") : QStringLiteral("-Infinity");
    // JS String(-0) is "0"; to_chars would emit "-0".
    if (value == 0.0)
        return QStringLiteral("0");

    char buf[64];
    const double magnitude = std::fabs(value);
    // JS only switches to exponential notation outside [1e-6, 1e21). Inside
    // that window — every timestamp this is ever called with — shortest-FIXED
    // is character-for-character what String() produces. Plain to_chars
    // (chars_format-less) would NOT do: it picks whichever of fixed/scientific
    // is shorter, so String(100000) would come out "1e+05" instead of
    // "100000". Outside the window we fall back to to_chars' general form;
    // no timestamp can reach it (that would be a >31000-year offset), it just
    // keeps the helper total.
    const std::to_chars_result res = (magnitude >= 1e-6 && magnitude < 1e21)
        ? std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::fixed)
        : std::to_chars(buf, buf + sizeof(buf), value);
    if (res.ec != std::errc())
        return QString::number(value);
    return QString::fromLatin1(buf, static_cast<int>(res.ptr - buf));
}

QVariantMap TranscriptDisplayBuilder::build(const QVariantList &sentences,
                                            const QVariantList &chapters,
                                            const QVariantMap &collapsedChapters,
                                            const QVariantMap &speakerOverrides,
                                            const QVariantList &silenceSpans,
                                            double minSilenceSeconds)
{
    QString text;
    QVariantList list;
    // null in the JS ("no previous sentence yet") — distinct from an empty
    // speaker name, which is a real value (a file with no speaker detection).
    std::optional<QString> previousSpeaker;
    int chapterIdx = 0;
    // Real t0 of the chapter the CURRENT sentence falls under (-1 = before
    // the first chapter marker / no chapters). Collapse is keyed off
    // this; the displayT0 the heading itself renders at is separately
    // clamped (see below), so the two are tracked independently.
    double currentChapterT0 = -1;

    const auto isCollapsed = [&collapsedChapters](double chapterT0) {
        return collapsedChapters.value(jsNumberToString(chapterT0)).toBool();
    };

    // displayT0 is the (possibly clamped) time the heading is positioned/
    // highlighted at; chapter.t0 is the chapter's REAL timestamp, carried
    // through as chapterT0 so the heading's edit/collapse/delete controls
    // address the actual chapter marker (setChapterTitle/deleteChapter/
    // collapse all key off the real t0, never the clamped one).
    const auto pushHeading = [&](const QVariantMap &chapter, double displayT0) {
        const double chapterT0 = chapter.value(kT0).toDouble();
        QVariantMap entry;
        entry[kIdx] = int(list.size());
        entry[kT0] = displayT0;
        entry[kChapterT0] = chapterT0;
        entry[kSpeaker] = QString();
        entry[kText] = QString();
        entry[kStart] = int(text.length());
        entry[kLength] = 0;
        entry[kShowGutter] = false;
        entry[kIsChapterHeading] = true;
        entry[kChapterTitle] = chapter.value(kTitle).toString();
        entry[kChapterManual] = chapter.value(kManual).toBool();
        // Whether THIS chapter is collapsed (body sentences dropped below).
        // The C++ activeSentenceIndex reads this to decide whether the
        // playback highlight rests ON a collapsed chapter's heading (its only
        // visible line) or walks back past an expanded chapter's lead-in
        // heading.
        entry[kCollapsed] = isCollapsed(chapterT0);
        list.append(entry);
        text += QLatin1Char('\n');
        previousSpeaker.reset(); // next real sentence's gutter shows fresh after a section break
    };

    // Silence phase 3: a THIRD kind of zero-length
    // pseudo-entry, reserving its own blank document line exactly like
    // pushHeading above — isSilenceMarker (not isChapterHeading) is what
    // tells every downstream consumer (gutterTimestampModel, speakerDimModel,
    // firstRealSentenceIndex, commitTranscriptEdits, and the C++
    // activeSentenceIndex walk-back) this is decoration, not spoken content.
    // Deliberately does NOT reset previousSpeaker (unlike pushHeading) — a
    // silence gap is not a section break, so the next sentence's speaker
    // gutter should still collapse into the same turn when the speaker hasn't
    // actually changed.
    const auto pushSilenceMarker = [&](const QVariantMap &span) {
        const double spanT0 = span.value(kT0).toDouble();
        const double spanT1 = span.value(kT1).toDouble();
        QVariantMap entry;
        entry[kIdx] = int(list.size());
        entry[kT0] = spanT0;
        entry[kChapterT0] = currentChapterT0;
        entry[kSpeaker] = QString();
        entry[kText] = QString();
        entry[kStart] = int(text.length());
        entry[kLength] = 0;
        entry[kShowGutter] = false;
        entry[kIsChapterHeading] = false;
        entry[kIsSilenceMarker] = true;
        entry[kChapterTitle] = QString();
        entry[kChapterManual] = false;
        entry[kCollapsed] = false;
        entry[kSilenceSeconds] = spanT1 - spanT0;
        list.append(entry);
        text += QLatin1Char('\n');
    };

    // Cursor into silenceSpans (stored raw in phase 1, sorted by t0) —
    // advanced monotonically alongside the sentence loop below, never
    // rescanned. previousRealT0 is the t0 of the last VISIBLE real sentence
    // pushed (unset before the first one), so a span only earns a divider when
    // it falls fully BETWEEN two rendered segments — per the locked decision,
    // leading silence (before the first sentence) and trailing silence (after
    // the last) never qualify, and neither does a span that overlaps a
    // rendered sentence rather than sitting between two (previousRealT0 only
    // advances past sentences that actually reach `list`, exactly mirroring
    // the `collapsed` skip below).
    //
    // A consequence worth stating, because it reads backwards at first
    // glance: collapsing a chapter does NOT hide the spans inside its body.
    // previousRealT0 skips right over the hidden sentences, so such a span
    // now falls in the much wider gap between the last visible sentence
    // before the chapter and the first one after — which still qualifies, so
    // a divider appears at the collapse boundary. That's the behavior the JS
    // had; tst_transcript_display_builder pins it either way.
    int silenceIdx = 0;
    std::optional<double> previousRealT0;

    for (const QVariant &sentenceVar : sentences) {
        const QVariantMap s = sentenceVar.toMap();
        const QString rawText = s.value(kText).toString();
        // Skip blank/whitespace-only segments entirely — whisper can
        // occasionally emit one (e.g. a near-silent stretch), and giving it
        // its own empty line + gutter timestamp is exactly the "one timestamp
        // with nothing under it" pattern that reads as a gutter glitch. A
        // blank segment has no content to show, so it shouldn't get a line OR
        // a timestamp.
        if (rawText.trimmed().isEmpty())
            continue;

        const double sT0 = s.value(kT0).toDouble();
        while (chapterIdx < chapters.size()
               && chapters.at(chapterIdx).toMap().value(kT0).toDouble() <= sT0) {
            const QVariantMap chapter = chapters.at(chapterIdx).toMap();
            const double chapterT0 = chapter.value(kT0).toDouble();
            currentChapterT0 = chapterT0;
            pushHeading(chapter, std::min(chapterT0, sT0));
            chapterIdx++;
        }
        // A collapsed chapter hides its body entirely — the sentence
        // is dropped from BOTH `text` and `list` (its heading was already
        // pushed above and stays visible), keeping every downstream offset
        // consumer (transcriptArea's string, the overlay Repeaters,
        // searchCorpus()) consistent.
        if (currentChapterT0 != -1 && isCollapsed(currentChapterT0))
            continue;

        // Silence phase 3: any stored span(s) starting at/after the previous
        // visible sentence and ending at/before THIS one sit fully in the gap
        // between them — mark each that clears minSilenceSeconds
        // (sub-threshold gaps are ordinary inter-sentence breathing room, not
        // shown; see silenceMarkerMinSeconds' own doc comment). Spans before
        // previousRealT0 (already passed, or interior to a since-collapsed
        // chapter) are skipped without marking so the cursor only ever moves
        // forward.
        if (previousRealT0.has_value()) {
            while (silenceIdx < silenceSpans.size()
                   && silenceSpans.at(silenceIdx).toMap().value(kT0).toDouble() < *previousRealT0)
                silenceIdx++;
            while (silenceIdx < silenceSpans.size()) {
                const QVariantMap span = silenceSpans.at(silenceIdx).toMap();
                const double spanT0 = span.value(kT0).toDouble();
                const double spanT1 = span.value(kT1).toDouble();
                if (!(spanT0 >= *previousRealT0 && spanT1 <= sT0))
                    break;
                if ((spanT1 - spanT0) >= minSilenceSeconds)
                    pushSilenceMarker(span);
                silenceIdx++;
            }
        }

        const int start = int(text.length());
        const QString rawSpeaker = s.value(kSpeaker).toString();
        const QString speaker =
            rawSpeaker.isEmpty() ? QString() : effectiveSpeakerName(rawSpeaker, speakerOverrides);
        const bool showGutter =
            speaker.isEmpty() || !previousSpeaker.has_value() || speaker != *previousSpeaker;
        // repeatCount > 1 (SegmentLineWriter merged consecutive identical
        // whisper segments into one on-disk line, see its doc comment) —
        // append a "(Nx)" marker so that's still visible here, not just in the
        // raw .md file. Appended AFTER length is captured from s.text alone,
        // so the marker itself sits outside the highlightable/selectable
        // "spoken words" range (same treatment as gutter timestamps/speaker
        // names — decoration, not content).
        const double repeatCount = s.value(kRepeatCount).toDouble();
        const QString displayText = repeatCount > 1
            ? rawText + QStringLiteral(" (") + jsNumberToString(repeatCount) + QStringLiteral("x)")
            : rawText;

        QVariantMap entry;
        entry[kIdx] = int(list.size());
        entry[kT0] = sT0;
        entry[kSpeaker] = speaker;
        entry[kText] = displayText;
        entry[kStart] = start;
        entry[kLength] = int(rawText.length());
        entry[kShowGutter] = showGutter;
        entry[kIsChapterHeading] = false;
        entry[kChapterTitle] = QString();
        entry[kChapterT0] = currentChapterT0;
        entry[kChapterManual] = false;
        list.append(entry);

        previousSpeaker = speaker;
        previousRealT0 = sT0;
        text += displayText;
        text += QLatin1Char('\n');
    }
    // Any chapter at/after the last real sentence (e.g. a trailing marker with
    // no further speech) still gets a heading rather than being silently
    // dropped.
    while (chapterIdx < chapters.size()) {
        const QVariantMap chapter = chapters.at(chapterIdx).toMap();
        pushHeading(chapter, chapter.value(kT0).toDouble());
        chapterIdx++;
    }
    if (text.length() > 0)
        text.chop(1); // drop the final trailing newline

    QVariantMap result;
    result[kText] = text;
    result[QStringLiteral("sentences")] = list;
    return result;
}

TranscriptDisplayBuilder::TranscriptDisplayBuilder(QObject *parent)
    : QObject(parent)
{
}
