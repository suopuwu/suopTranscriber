#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

// The transcript DISPLAY-MODEL collaborator lifted out of MainPanel.qml's
// `displayData` binding. It owns the one pure transform that
// turns the raw on-disk data — parseSentences() segments, readChapters()
// markers, the collapse map, the optimistic speaker-rename overlay and
// parseSilences() spans — into the two things the transcript view renders:
//
//   text      — the single monolithic string transcriptArea displays, one
//               line per visible entry.
//   sentences — one entry per line, in the SAME order/indexing as `text`,
//               carrying the char offsets (start/length) every overlay
//               Repeater, searchCorpus() and the C++ activeSentenceIndex()
//               binsearch address lines by.
//
// It was ~140 lines of JS re-run on every sentences/chapters/collapse/rename/
// silence change — the hottest binding in a 4.8k-line QML file, and entirely
// model-shaped: no accessor reaches back into the component (the one apparent
// exception, effectiveSpeakerName(), is a pure lookup in pendingSpeakerRenames,
// so that map is passed in as `speakerOverrides` instead).
//
// Stateless by construction — a QML_SINGLETON purely so QML has something to
// call; build() is a plain static and holds no state between calls. The
// caller-side binding in MainPanel.qml stays a `readonly property var` with
// exactly the same dependency set, so every downstream binding still
// re-evaluates at exactly the same moments (MainPanel has a documented
// stale-derived-binding hazard — see resolvedMdUrl — that turning this into an
// imperatively-called function would walk straight into).
class TranscriptDisplayBuilder : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit TranscriptDisplayBuilder(QObject *parent = nullptr);

    // Assemble the transcript document + its per-line entry list.
    //
    //   sentences         parseSentences() output: maps with t0 (double
    //                     seconds), speaker (raw on-disk name, "" when the
    //                     file has no speaker detection), text, repeatCount.
    //   chapters          readChapters() output: maps with t0, title, manual.
    //                     Assumed sorted by t0, like sentences.
    //   collapsedChapters MainPanel.collapsedChapters — keyed by the chapter's
    //                     REAL t0 rendered the way JS's String(number) renders
    //                     it (see jsNumberToString in the .cpp), value true.
    //   speakerOverrides  MainPanel.pendingSpeakerRenames — raw on-disk
    //                     speaker name -> the optimistically-displayed name.
    //   silenceSpans      parseSilences() output: maps with t0/t1, sorted by
    //                     t0.
    //   minSilenceSeconds MainPanel.silenceMarkerMinSeconds — a span shorter
    //                     than this is ordinary inter-sentence breathing room
    //                     and earns no divider.
    //
    // Returns {text: QString, sentences: QVariantList}.
    Q_INVOKABLE static QVariantMap build(const QVariantList &sentences,
                                         const QVariantList &chapters,
                                         const QVariantMap &collapsedChapters,
                                         const QVariantMap &speakerOverrides,
                                         const QVariantList &silenceSpans,
                                         double minSilenceSeconds);

    // JS `String(aNumber)` for a double — the EXACT spelling the collapse map
    // is keyed by, both here and on the QML side (toggleChapterCollapse builds
    // its key with String(chapterT0), and the frontmatter persists those same
    // strings). Exposed for the characterization test, which has to build
    // fixture collapse maps with keys that actually match.
    static QString jsNumberToString(double value);
};
