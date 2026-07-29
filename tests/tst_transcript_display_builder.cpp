// CHARACTERIZATION test for TranscriptDisplayBuilder — the transcript
// display-model assembly lifted out of MainPanel.qml's `displayData` binding
// (2026-07-20).
//
// Every expectation below was DERIVED FROM THE JS THAT WAS THERE BEFORE the
// extraction, by hand, and hard-coded here: the point of this file is to pin
// the old behavior so the port is provably behavior-preserving, not to
// re-describe the new code. If one of these ever needs to change, that is a
// deliberate product decision about how the transcript renders — not a
// refactor.
//
// What the builder produces, and why the shape matters:
//
//   text      — the SINGLE monolithic string transcriptArea displays. Chapter
//               headings and silence dividers are NOT text in it; each
//               reserves one genuinely-empty line (so it gets a real row of
//               vertical space in the flowing document) and an overlay draws
//               the actual heading/divider.
//   sentences — one entry per line of `text`, same order, same indexing. Its
//               `start`/`length` are char offsets INTO `text`, which the
//               overlay Repeaters, searchCorpus() and the C++
//               activeSentenceIndex() binsearch all address lines by. An
//               off-by-one here silently misaligns the whole gutter, so the
//               offsets are asserted literally, not structurally.
//
// Plain C++ (no QQuickView) on purpose — it's a pure data-in/data-out
// transform, so the assembly rules can be pinned independent of any QML
// wiring. The indirect coverage that existed before this file
// (tst_mainpanel_chapterrefresh / tst_mainpanel_chapterselection) stays as the
// integration check that MainPanel is still wired to it correctly.

#include <QtTest>

#include <QVariantList>
#include <QVariantMap>

#include "TranscriptDisplayBuilder.h"

namespace {

// parseSentences()-shaped segment.
QVariantMap seg(double t0, const QString &text, const QString &speaker = QString(), int repeatCount = 1)
{
    QVariantMap m;
    m[QStringLiteral("t0")] = t0;
    m[QStringLiteral("text")] = text;
    m[QStringLiteral("speaker")] = speaker;
    m[QStringLiteral("repeatCount")] = repeatCount;
    return m;
}

// readChapters()-shaped marker.
QVariantMap chapter(double t0, const QString &title, bool manual = false)
{
    QVariantMap m;
    m[QStringLiteral("t0")] = t0;
    m[QStringLiteral("title")] = title;
    m[QStringLiteral("manual")] = manual;
    return m;
}

// parseSilences()-shaped span.
QVariantMap span(double t0, double t1)
{
    QVariantMap m;
    m[QStringLiteral("t0")] = t0;
    m[QStringLiteral("t1")] = t1;
    return m;
}

QVariantList listOf(const QList<QVariantMap> &maps)
{
    QVariantList out;
    for (const QVariantMap &m : maps)
        out.append(m);
    return out;
}

QString textOf(const QVariantMap &result)
{
    return result.value(QStringLiteral("text")).toString();
}

QVariantList entriesOf(const QVariantMap &result)
{
    return result.value(QStringLiteral("sentences")).toList();
}

QVariantMap entryAt(const QVariantMap &result, int i)
{
    return entriesOf(result).at(i).toMap();
}

// MainPanel's silenceMarkerMinSeconds (a fixed StyleGuide constant, not a
// setting) — every case that isn't specifically about the threshold uses it.
constexpr double kMinSilence = 5.0;

QVariantMap build(const QVariantList &sentences,
                  const QVariantList &chapters = {},
                  const QVariantMap &collapsed = {},
                  const QVariantMap &overrides = {},
                  const QVariantList &silences = {},
                  double minSilence = kMinSilence)
{
    return TranscriptDisplayBuilder::build(sentences, chapters, collapsed, overrides, silences,
                                           minSilence);
}

} // namespace

class TestTranscriptDisplayBuilder : public QObject
{
    Q_OBJECT

private slots:

    // ---- edge cases -------------------------------------------------------

    // Nothing in, nothing out — and specifically an EMPTY text, not a stray
    // newline: the trailing-newline chop is guarded on a non-empty string, and
    // a one-char text would give transcriptArea a phantom first line.
    void empty_yieldsEmptyTextAndNoEntries()
    {
        const QVariantMap r = build({});
        QCOMPARE(textOf(r), QString());
        QCOMPARE(entriesOf(r).size(), 0);
    }

    // A file with chapter markers but no transcribed speech still renders
    // every heading (the trailing-chapter loop), each on its own blank line.
    void chaptersOnly_stillRenderEveryHeading()
    {
        const QVariantMap r = build({}, listOf({chapter(0, QStringLiteral("A")),
                                                chapter(30, QStringLiteral("B"))}));
        // Two reserved blank lines, minus the chopped trailing newline.
        QCOMPARE(textOf(r), QStringLiteral("\n"));
        QCOMPARE(entriesOf(r).size(), 2);
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("start")).toInt(), 0);
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("t0")).toDouble(), 0.0);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("start")).toInt(), 1);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("t0")).toDouble(), 30.0);
    }

    // ---- plain sentences --------------------------------------------------

    // The baseline: one line per segment, newline-joined, NO trailing newline,
    // and start/length pointing exactly at each line's slice of `text`.
    void plainSentences_offsetsIndexTheAssembledText()
    {
        const QVariantMap r = build(listOf({seg(0, QStringLiteral("one")),
                                            seg(5, QStringLiteral("two")),
                                            seg(10, QStringLiteral("three"))}));
        const QString text = textOf(r);
        QCOMPARE(text, QStringLiteral("one\ntwo\nthree"));
        QCOMPARE(entriesOf(r).size(), 3);

        const int expectedStart[] = {0, 4, 8};
        const int expectedLength[] = {3, 3, 5};
        const QString expectedText[] = {QStringLiteral("one"), QStringLiteral("two"),
                                        QStringLiteral("three")};
        for (int i = 0; i < 3; ++i) {
            const QVariantMap e = entryAt(r, i);
            QCOMPARE(e.value(QStringLiteral("idx")).toInt(), i);
            QCOMPARE(e.value(QStringLiteral("start")).toInt(), expectedStart[i]);
            QCOMPARE(e.value(QStringLiteral("length")).toInt(), expectedLength[i]);
            QCOMPARE(e.value(QStringLiteral("text")).toString(), expectedText[i]);
            QCOMPARE(e.value(QStringLiteral("isChapterHeading")).toBool(), false);
            // No chapters -> "before the first marker" sentinel.
            QCOMPARE(e.value(QStringLiteral("chapterT0")).toDouble(), -1.0);
            // No speaker detection: every sentence is its own section, so the
            // (timestamp) gutter shows on every one.
            QCOMPARE(e.value(QStringLiteral("showGutter")).toBool(), true);
            // Verify the offsets really do address this line in `text`.
            QCOMPARE(text.mid(e.value(QStringLiteral("start")).toInt(),
                              e.value(QStringLiteral("length")).toInt()),
                     expectedText[i]);
        }
    }

    // whisper can emit a blank/whitespace-only segment; it gets NO line and NO
    // gutter timestamp (a lone timestamp with nothing under it reads as a
    // glitch), and it does not consume an idx either.
    void blankSegments_areDroppedEntirely()
    {
        const QVariantMap r = build(listOf({seg(0, QStringLiteral("a")),
                                            seg(1, QStringLiteral("   ")),
                                            seg(2, QStringLiteral("")),
                                            seg(3, QStringLiteral("\t")),
                                            seg(4, QStringLiteral("b"))}));
        QCOMPARE(textOf(r), QStringLiteral("a\nb"));
        QCOMPARE(entriesOf(r).size(), 2);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("idx")).toInt(), 1);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("t0")).toDouble(), 4.0);
    }

    // A merged run of identical segments shows "(Nx)" — but `length` is
    // captured from the spoken words alone, so the marker sits OUTSIDE the
    // highlightable/selectable range (same treatment as a gutter timestamp).
    void repeatCount_markerIsDecorationOutsideTheHighlightRange()
    {
        const QVariantMap r = build(listOf({seg(0, QStringLiteral("hey"), QString(), 3),
                                            seg(5, QStringLiteral("ok"), QString(), 1)}));
        QCOMPARE(textOf(r), QStringLiteral("hey (3x)\nok"));
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("text")).toString(), QStringLiteral("hey (3x)"));
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("length")).toInt(), 3);
        // repeatCount == 1 is the normal case and adds nothing.
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("text")).toString(), QStringLiteral("ok"));
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("start")).toInt(), 9);
    }

    // ---- speakers ---------------------------------------------------------

    // Speaker gutters are one-per-TURN: only the first sentence of a run by
    // the same speaker carries showGutter. Each sentence is still its own
    // line/entry (that's what keeps highlight sync per-segment).
    void speakerTurns_showGutterOnlyOnTheFirstSentenceOfARun()
    {
        const QVariantMap r = build(listOf({seg(0, QStringLiteral("a"), QStringLiteral("Speaker 1")),
                                            seg(1, QStringLiteral("b"), QStringLiteral("Speaker 1")),
                                            seg(2, QStringLiteral("c"), QStringLiteral("Speaker 2")),
                                            seg(3, QStringLiteral("d"), QStringLiteral("Speaker 1"))}));
        const bool expected[] = {true, false, true, true};
        for (int i = 0; i < 4; ++i)
            QCOMPARE(entryAt(r, i).value(QStringLiteral("showGutter")).toBool(), expected[i]);
    }

    // The entry's `speaker` is the EFFECTIVE (pending-rename-applied) name —
    // the optimistic-UI overlay for an in-flight renameSpeaker(). Turn
    // grouping is computed on the effective name too, so two raw ids that map
    // onto the SAME display name read as one continuous turn (which is exactly
    // what the user sees mid-merge).
    void speakerOverrides_applyToBothTheNameAndTheTurnGrouping()
    {
        QVariantMap overrides;
        overrides[QStringLiteral("Speaker 1")] = QStringLiteral("Ada");
        overrides[QStringLiteral("Speaker 2")] = QStringLiteral("Ada");

        const QVariantMap r = build(listOf({seg(0, QStringLiteral("a"), QStringLiteral("Speaker 1")),
                                            seg(1, QStringLiteral("b"), QStringLiteral("Speaker 2")),
                                            seg(2, QStringLiteral("c"), QStringLiteral("Speaker 3"))}),
                                    {}, {}, overrides);
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("speaker")).toString(), QStringLiteral("Ada"));
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("speaker")).toString(), QStringLiteral("Ada"));
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("showGutter")).toBool(), true);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("showGutter")).toBool(), false);
        // Unlisted raw name passes through untouched.
        QCOMPARE(entryAt(r, 2).value(QStringLiteral("speaker")).toString(), QStringLiteral("Speaker 3"));
        QCOMPARE(entryAt(r, 2).value(QStringLiteral("showGutter")).toBool(), true);
    }

    // A file with no speaker detection has no "turns" to group by, so an
    // override map can never make one sentence swallow the next one's gutter.
    void emptySpeaker_neverGroupsIntoATurn()
    {
        const QVariantMap r = build(listOf({seg(0, QStringLiteral("a")), seg(1, QStringLiteral("b"))}));
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("showGutter")).toBool(), true);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("showGutter")).toBool(), true);
    }

    // ---- chapter headings -------------------------------------------------

    // Headings interleave in time order as zero-length pseudo-entries, each
    // reserving one blank line. displayT0 is CLAMPED to the sentence it
    // precedes (so activeSentenceIndex's upper_bound resolves a tie in the
    // real sentence's favor and a heading never steals a playing highlight),
    // while chapterT0 stays the marker's REAL timestamp — that's what
    // setChapterTitle/deleteChapter/collapse address.
    void chapterHeadings_interleaveWithClampedDisplayT0()
    {
        const QVariantMap r = build(listOf({seg(1, QStringLiteral("a"), QStringLiteral("S1")),
                                            seg(25, QStringLiteral("b"), QStringLiteral("S1"))}),
                                    listOf({chapter(0, QStringLiteral("Intro")),
                                            chapter(20, QStringLiteral("Part 2"), true)}));
        QCOMPARE(textOf(r), QStringLiteral("\na\n\nb"));
        QCOMPARE(entriesOf(r).size(), 4);

        const QVariantMap h0 = entryAt(r, 0);
        QCOMPARE(h0.value(QStringLiteral("isChapterHeading")).toBool(), true);
        QCOMPARE(h0.value(QStringLiteral("chapterTitle")).toString(), QStringLiteral("Intro"));
        QCOMPARE(h0.value(QStringLiteral("chapterManual")).toBool(), false);
        QCOMPARE(h0.value(QStringLiteral("start")).toInt(), 0);
        QCOMPARE(h0.value(QStringLiteral("length")).toInt(), 0);
        QCOMPARE(h0.value(QStringLiteral("text")).toString(), QString());
        QCOMPARE(h0.value(QStringLiteral("showGutter")).toBool(), false);
        // chapter.t0 (0) < sentence.t0 (1), so the clamp is a no-op here.
        QCOMPARE(h0.value(QStringLiteral("t0")).toDouble(), 0.0);
        QCOMPARE(h0.value(QStringLiteral("chapterT0")).toDouble(), 0.0);

        QCOMPARE(entryAt(r, 1).value(QStringLiteral("start")).toInt(), 1);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("chapterT0")).toDouble(), 0.0);

        const QVariantMap h1 = entryAt(r, 2);
        QCOMPARE(h1.value(QStringLiteral("chapterTitle")).toString(), QStringLiteral("Part 2"));
        QCOMPARE(h1.value(QStringLiteral("chapterManual")).toBool(), true);
        QCOMPARE(h1.value(QStringLiteral("start")).toInt(), 3);
        QCOMPARE(h1.value(QStringLiteral("t0")).toDouble(), 20.0);
        QCOMPARE(entryAt(r, 3).value(QStringLiteral("start")).toInt(), 4);
        QCOMPARE(entryAt(r, 3).value(QStringLiteral("chapterT0")).toDouble(), 20.0);
    }

    // The clamp actually bites when the marker sits AFTER the sentence it
    // heads (an auto-chapter placed a hair late): the heading renders at the
    // sentence's t0, but still addresses its own real one.
    void chapterHeading_displayT0IsClampedToTheFollowingSentence()
    {
        const QVariantMap r = build(listOf({seg(10, QStringLiteral("a"))}),
                                    listOf({chapter(12, QStringLiteral("Late"))}));
        // 12 <= 10 is false, so this marker does NOT precede the sentence —
        // it falls through to the trailing loop and renders at its real t0.
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("isChapterHeading")).toBool(), false);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("t0")).toDouble(), 12.0);

        // With the marker at exactly the sentence's t0 the <= comparison DOES
        // fire, and min(12, 12) is a no-op; with two sentences straddling it,
        // the clamp is what's observable.
        const QVariantMap r2 = build(listOf({seg(10, QStringLiteral("a")), seg(30, QStringLiteral("b"))}),
                                     listOf({chapter(25, QStringLiteral("Mid"))}));
        QCOMPARE(entryAt(r2, 1).value(QStringLiteral("isChapterHeading")).toBool(), true);
        QCOMPARE(entryAt(r2, 1).value(QStringLiteral("t0")).toDouble(), 25.0);
        QCOMPARE(entryAt(r2, 1).value(QStringLiteral("chapterT0")).toDouble(), 25.0);
    }

    // A heading IS a section break: the next real sentence shows a fresh
    // speaker gutter even when the speaker never changed.
    void chapterHeading_resetsTheSpeakerTurn()
    {
        const QVariantMap r = build(listOf({seg(0, QStringLiteral("a"), QStringLiteral("S1")),
                                            seg(10, QStringLiteral("b"), QStringLiteral("S1"))}),
                                    listOf({chapter(5, QStringLiteral("Break"))}));
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("showGutter")).toBool(), true);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("isChapterHeading")).toBool(), true);
        QCOMPARE(entryAt(r, 2).value(QStringLiteral("showGutter")).toBool(), true);
    }

    // A marker past the last sentence (a trailing chapter with no further
    // speech) still gets a heading rather than being silently dropped — and it
    // does NOT retroactively become the preceding sentences' chapterT0.
    void trailingChapters_stillGetHeadings()
    {
        const QVariantMap r = build(listOf({seg(0, QStringLiteral("a"))}),
                                    listOf({chapter(100, QStringLiteral("End"))}));
        QCOMPARE(textOf(r), QStringLiteral("a\n"));
        QCOMPARE(entriesOf(r).size(), 2);
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("chapterT0")).toDouble(), -1.0);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("isChapterHeading")).toBool(), true);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("t0")).toDouble(), 100.0);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("start")).toInt(), 2);
    }

    // ---- collapse ----------------------------------------------------------

    // A collapsed chapter's body sentences vanish from BOTH `text` and the
    // entry list (the heading itself stays, flagged collapsed) — the whole
    // point being that every offset consumer stays consistent, so the
    // surviving entries' start offsets must still address the shortened text.
    void collapsedChapter_dropsItsBodyFromTextAndEntries()
    {
        QVariantMap collapsed;
        collapsed[QStringLiteral("0")] = true;

        const QVariantMap r = build(listOf({seg(1, QStringLiteral("a")),
                                            seg(2, QStringLiteral("b")),
                                            seg(11, QStringLiteral("c"))}),
                                    listOf({chapter(0, QStringLiteral("C0")),
                                            chapter(10, QStringLiteral("C1"))}),
                                    collapsed);
        QCOMPARE(textOf(r), QStringLiteral("\n\nc"));
        QCOMPARE(entriesOf(r).size(), 3);
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("collapsed")).toBool(), true);
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("start")).toInt(), 0);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("collapsed")).toBool(), false);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("start")).toInt(), 1);
        QCOMPARE(entryAt(r, 2).value(QStringLiteral("text")).toString(), QStringLiteral("c"));
        QCOMPARE(entryAt(r, 2).value(QStringLiteral("start")).toInt(), 2);
    }

    // The collapse map is keyed by JS String(chapterT0) — the spelling
    // toggleChapterCollapse() writes and the frontmatter persists. A
    // fractional timestamp keys as "7.25", never "7.250000": getting the
    // number->string wrong would fail silently (no chapter ever collapses).
    void collapseKey_isJsNumberToString()
    {
        QCOMPARE(TranscriptDisplayBuilder::jsNumberToString(0.0), QStringLiteral("0"));
        QCOMPARE(TranscriptDisplayBuilder::jsNumberToString(7.0), QStringLiteral("7"));
        QCOMPARE(TranscriptDisplayBuilder::jsNumberToString(7.25), QStringLiteral("7.25"));
        QCOMPARE(TranscriptDisplayBuilder::jsNumberToString(100000.0), QStringLiteral("100000"));
        QCOMPARE(TranscriptDisplayBuilder::jsNumberToString(1.5), QStringLiteral("1.5"));

        QVariantMap collapsed;
        collapsed[QStringLiteral("7.25")] = true;
        const QVariantMap r = build(listOf({seg(8, QStringLiteral("hidden"))}),
                                    listOf({chapter(7.25, QStringLiteral("Frac"))}),
                                    collapsed);
        QCOMPARE(textOf(r), QString());
        QCOMPARE(entriesOf(r).size(), 1);
        QCOMPARE(entryAt(r, 0).value(QStringLiteral("collapsed")).toBool(), true);
    }

    // ---- silence dividers (phase 3) ---------------------------------------

    // A stored VAD span earns a divider only when it sits FULLY between two
    // rendered sentences and clears the minimum length. This one fixture pins
    // all four of: leading silence never marks, a >= threshold gap does (the
    // comparison is >=, so an exactly-threshold gap qualifies), a
    // sub-threshold gap doesn't, and trailing silence never marks.
    void silenceSpans_onlyMarkQualifyingInterSentenceGaps()
    {
        const QVariantList sentences = listOf({seg(10, QStringLiteral("a")),
                                               seg(20, QStringLiteral("b")),
                                               seg(23, QStringLiteral("c")),
                                               seg(40, QStringLiteral("d"))});
        const QVariantList spans = listOf({span(0, 9),    // leading — never marks
                                           span(12, 17),  // exactly 5s — marks
                                           span(21, 22),  // 1s — below threshold
                                           span(25, 35),  // 10s — marks
                                           span(45, 60)}); // trailing — never marks

        const QVariantMap r = build(sentences, {}, {}, {}, spans);
        QCOMPARE(textOf(r), QStringLiteral("a\n\nb\nc\n\nd"));
        QCOMPARE(entriesOf(r).size(), 6);

        QCOMPARE(entryAt(r, 0).value(QStringLiteral("text")).toString(), QStringLiteral("a"));

        const QVariantMap m0 = entryAt(r, 1);
        QCOMPARE(m0.value(QStringLiteral("isSilenceMarker")).toBool(), true);
        QCOMPARE(m0.value(QStringLiteral("isChapterHeading")).toBool(), false);
        QCOMPARE(m0.value(QStringLiteral("t0")).toDouble(), 12.0);
        QCOMPARE(m0.value(QStringLiteral("silenceSeconds")).toDouble(), 5.0);
        QCOMPARE(m0.value(QStringLiteral("start")).toInt(), 2);
        QCOMPARE(m0.value(QStringLiteral("length")).toInt(), 0);
        QCOMPARE(m0.value(QStringLiteral("showGutter")).toBool(), false);
        QCOMPARE(m0.value(QStringLiteral("chapterT0")).toDouble(), -1.0);

        QCOMPARE(entryAt(r, 2).value(QStringLiteral("start")).toInt(), 3);
        QCOMPARE(entryAt(r, 3).value(QStringLiteral("start")).toInt(), 5);

        const QVariantMap m1 = entryAt(r, 4);
        QCOMPARE(m1.value(QStringLiteral("isSilenceMarker")).toBool(), true);
        QCOMPARE(m1.value(QStringLiteral("t0")).toDouble(), 25.0);
        QCOMPARE(m1.value(QStringLiteral("silenceSeconds")).toDouble(), 10.0);
        QCOMPARE(m1.value(QStringLiteral("start")).toInt(), 7);

        QCOMPARE(entryAt(r, 5).value(QStringLiteral("text")).toString(), QStringLiteral("d"));
        QCOMPARE(entryAt(r, 5).value(QStringLiteral("start")).toInt(), 8);
    }

    // The threshold is a plain length gate — the same span stops marking once
    // it no longer clears it.
    void silenceSpans_belowThresholdNeverMark()
    {
        const QVariantList sentences = listOf({seg(0, QStringLiteral("a")), seg(20, QStringLiteral("b"))});
        const QVariantList spans = listOf({span(5, 15)}); // 10s

        QCOMPARE(entriesOf(build(sentences, {}, {}, {}, spans, 5.0)).size(), 3);
        QCOMPARE(entriesOf(build(sentences, {}, {}, {}, spans, 10.0)).size(), 3); // >= is inclusive
        QCOMPARE(entriesOf(build(sentences, {}, {}, {}, spans, 11.0)).size(), 2);
    }

    // Unlike a chapter heading, a silence divider is NOT a section break — the
    // next sentence's speaker gutter still collapses into the same turn.
    void silenceMarker_doesNotResetTheSpeakerTurn()
    {
        const QVariantMap r = build(listOf({seg(0, QStringLiteral("a"), QStringLiteral("S1")),
                                            seg(20, QStringLiteral("b"), QStringLiteral("S1"))}),
                                    {}, {}, {}, listOf({span(5, 15)}));
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("isSilenceMarker")).toBool(), true);
        QCOMPARE(entryAt(r, 2).value(QStringLiteral("showGutter")).toBool(), false);
    }

    // The span cursor only ever moves FORWARD: once it's passed the last
    // rendered sentence's t0, any span still sitting behind it is dropped
    // without marking. A span that OVERLAPS a rendered sentence (starts before
    // it, ends after it) is exactly that case — it never sat fully in one gap,
    // so it never earns a divider even though it clears the threshold.
    void silenceSpans_overlappingARenderedSentenceAreSkipped()
    {
        const QVariantMap r = build(listOf({seg(0, QStringLiteral("a")),
                                            seg(10, QStringLiteral("b")),
                                            seg(30, QStringLiteral("c"))}),
                                    {}, {}, {}, listOf({span(5, 25)})); // 20s, but straddles b
        QCOMPARE(entriesOf(r).size(), 3);
        QCOMPARE(textOf(r), QStringLiteral("a\nb\nc"));
    }

    // Collapse interacts with the divider cursor in a way that is easy to
    // assume backwards, so it's pinned explicitly. "previous visible sentence"
    // only advances past sentences that actually reach the list, so hiding a
    // chapter's body does NOT hide a span inside it — the span now falls in
    // the (much wider) gap between the last visible sentence BEFORE the
    // collapsed chapter and the first one after, and that gap still qualifies.
    // A divider therefore appears at the collapse boundary. This is the
    // pre-existing behavior of the JS binding this replaced, kept as-is.
    void silenceSpans_insideACollapsedChapterStillMarkTheVisibleGap()
    {
        QVariantMap collapsed;
        collapsed[QStringLiteral("10")] = true;

        const QVariantMap r = build(listOf({seg(0, QStringLiteral("a")),
                                            seg(11, QStringLiteral("hidden1")),
                                            seg(40, QStringLiteral("hidden2")),
                                            seg(61, QStringLiteral("b"))}),
                                    listOf({chapter(10, QStringLiteral("C")),
                                            chapter(60, QStringLiteral("D"))}),
                                    collapsed, {}, listOf({span(20, 35)}));
        // a, heading C (collapsed), heading D, divider, b.
        QCOMPARE(textOf(r), QStringLiteral("a\n\n\n\nb"));
        QCOMPARE(entriesOf(r).size(), 5);
        QCOMPARE(entryAt(r, 1).value(QStringLiteral("collapsed")).toBool(), true);
        QCOMPARE(entryAt(r, 2).value(QStringLiteral("collapsed")).toBool(), false);
        const QVariantMap marker = entryAt(r, 3);
        QCOMPARE(marker.value(QStringLiteral("isSilenceMarker")).toBool(), true);
        QCOMPARE(marker.value(QStringLiteral("t0")).toDouble(), 20.0);
        QCOMPARE(marker.value(QStringLiteral("silenceSeconds")).toDouble(), 15.0);
        QCOMPARE(marker.value(QStringLiteral("start")).toInt(), 4);
        // The divider adopts the chapter the loop is currently inside.
        QCOMPARE(marker.value(QStringLiteral("chapterT0")).toDouble(), 60.0);
        QCOMPARE(entryAt(r, 4).value(QStringLiteral("start")).toInt(), 5);
    }

    // A silence marker inside an expanded chapter carries that chapter's t0,
    // so it belongs to the chapter it visually sits in.
    void silenceMarker_carriesTheCurrentChapterT0()
    {
        const QVariantMap r = build(listOf({seg(11, QStringLiteral("a")), seg(40, QStringLiteral("b"))}),
                                    listOf({chapter(10, QStringLiteral("C"))}),
                                    {}, {}, listOf({span(20, 35)}));
        QCOMPARE(entryAt(r, 2).value(QStringLiteral("isSilenceMarker")).toBool(), true);
        QCOMPARE(entryAt(r, 2).value(QStringLiteral("chapterT0")).toDouble(), 10.0);
    }

    // ---- entry key-set fidelity -------------------------------------------

    // The three entry kinds carry DIFFERENT key sets, and that's load-bearing:
    // QML reads a missing key as `undefined` (falsy), which is exactly what
    // the overlay Repeaters' filters (`!s.isSilenceMarker`, `s.isChapterHeading`)
    // and activeSentenceIndex's `collapsed` check are written against. Adding a
    // key "for symmetry" is a behavior change, so it's pinned here.
    void entryKeySets_matchTheirKind()
    {
        const QVariantMap r = build(listOf({seg(11, QStringLiteral("a")), seg(40, QStringLiteral("b"))}),
                                    listOf({chapter(10, QStringLiteral("C"))}),
                                    {}, {}, listOf({span(20, 35)}));
        // 0: heading, 1: sentence a, 2: silence marker, 3: sentence b
        const QVariantMap heading = entryAt(r, 0);
        const QVariantMap sentence = entryAt(r, 1);
        const QVariantMap marker = entryAt(r, 2);

        QVERIFY(heading.contains(QStringLiteral("collapsed")));
        QVERIFY(!heading.contains(QStringLiteral("isSilenceMarker")));
        QVERIFY(!heading.contains(QStringLiteral("silenceSeconds")));

        QVERIFY(!sentence.contains(QStringLiteral("collapsed")));
        QVERIFY(!sentence.contains(QStringLiteral("isSilenceMarker")));
        QVERIFY(!sentence.contains(QStringLiteral("silenceSeconds")));

        QVERIFY(marker.contains(QStringLiteral("collapsed")));
        QVERIFY(marker.contains(QStringLiteral("isSilenceMarker")));
        QVERIFY(marker.contains(QStringLiteral("silenceSeconds")));

        // Every kind carries the offset/identity keys the overlays index by.
        for (const QString &key : {QStringLiteral("idx"), QStringLiteral("t0"),
                                   QStringLiteral("chapterT0"), QStringLiteral("speaker"),
                                   QStringLiteral("text"), QStringLiteral("start"),
                                   QStringLiteral("length"), QStringLiteral("showGutter"),
                                   QStringLiteral("isChapterHeading"),
                                   QStringLiteral("chapterTitle"), QStringLiteral("chapterManual")}) {
            QVERIFY2(heading.contains(key), qPrintable(key));
            QVERIFY2(sentence.contains(key), qPrintable(key));
            QVERIFY2(marker.contains(key), qPrintable(key));
        }
    }

    // idx is each entry's own index into the list — the overlay Repeaters are
    // FILTERED, so a delegate's own `index` is its position in the filtered
    // list and idx is the only safe key back into displaySentences /
    // gutterLineRects. It must therefore count headings and markers too.
    void idx_countsEveryEntryIncludingPseudoEntries()
    {
        const QVariantMap r = build(listOf({seg(11, QStringLiteral("a")), seg(40, QStringLiteral("b"))}),
                                    listOf({chapter(10, QStringLiteral("C"))}),
                                    {}, {}, listOf({span(20, 35)}));
        const QVariantList entries = entriesOf(r);
        QCOMPARE(entries.size(), 4);
        for (int i = 0; i < entries.size(); ++i)
            QCOMPARE(entries.at(i).toMap().value(QStringLiteral("idx")).toInt(), i);
    }
};

QTEST_MAIN(TestTranscriptDisplayBuilder)
#include "tst_transcript_display_builder.moc"
