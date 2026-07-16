// Spec under test — CLAUDE.md load-bearing constraint (vault layout):
//
//   "Sibling .md = transcribed. All transcription data lives in this one
//    human-readable file — YAML frontmatter (model, speaker detection,
//    transcribed-at) plus one `[start - end] text` line per segment; no
//    separate JSON sidecar."
//
// The readers/writers live in TranscriptionController: readMetadata(),
// readTranscriptBody() and setLabel() all take the AUDIO file's QUrl and
// derive the sibling .md as <dir>/<fileName>.md — the FULL audio filename,
// extension included (e.g. "meeting.wav" -> "meeting.wav.md"), not just its
// stripped stem, so two audio files sharing a stem with different
// extensions (e.g. "meeting.wav" + "meeting.mp3") never collide on one
// sidecar (TranscriptionController.cpp, sidecarPath()). These tests
// hand-write fixture .md files in the exact format
// writeFrontmatter()/appendSegmentLine() produce and assert the read side
// honours it.

#include <QtTest>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include "ModelManager.h"
#include "TranscriptionController.h"

namespace {

// Byte-for-byte what a finished run looks like on disk
// (TranscriptionController.cpp:59-86): frontmatter block written at job
// START (durability — a killed run still records its settings), a blank
// line, then one segment line per whisper segment.
const QString kFixtureMd = QStringLiteral(
    "---\n"
    "model: ggml-base.en.bin\n"
    "speakerDetection: true\n"
    "transcribedAt: 2026-07-06T10:12:13\n"
    "---\n"
    "\n"
    "[00:00:00.000 - 00:00:11.000] hello world\n"
    "[00:00:11.000 - 00:00:14.500] And another speaker. [SPEAKER_TURN]\n");

QString writeFixture(const QString &dirPath, const QString &mdName, const QString &content)
{
    const QString path = dirPath + "/" + mdName;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    f.write(content.toUtf8());
    f.close();
    return path;
}

QString readAllBytes(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// Same detection strategy as tst_engine_contract's locateRealModel(): env
// override first, else the user's real vault, preferring a plain (non-tdrz)
// .en model. The for-tests-ggml-*.bin dummies under third_party carry no
// real weights and are never used.
QString locateRealModel()
{
    const QString envModel = qEnvironmentVariable("SUOP_TEST_MODEL");
    if (!envModel.isEmpty() && QFileInfo::exists(envModel))
        return envModel;

    QDir vaultModels(QStringLiteral("/home/user/recordings/.transcriber/models"));
    const QStringList bins = vaultModels.entryList({QStringLiteral("*.bin")}, QDir::Files, QDir::Name);
    QString fallback;
    for (const QString &bin : bins) {
        if (bin.contains(QStringLiteral("tdrz")))
            continue;
        if (bin.contains(QStringLiteral(".en")))
            return vaultModels.filePath(bin);
        if (fallback.isEmpty())
            fallback = vaultModels.filePath(bin);
    }
    return fallback;
}

// AI auto-labeling's model has no per-vault default location convention yet
// (unlike whisper's small models) — env override only.
QString locateRealLabelModel()
{
    const QString envModel = qEnvironmentVariable("SUOP_TEST_LABEL_MODEL");
    if (!envModel.isEmpty() && QFileInfo::exists(envModel))
        return envModel;
    return {};
}

// Speaker diarization models have no per-vault default location convention
// either (see ModelManager::ensureDiarizationModels — downloaded on first
// use, same as the label model) — env override only, same pattern as
// locateRealLabelModel above.
QString locateRealDiarizationModel(const char *envVar)
{
    const QString path = qEnvironmentVariable(envVar);
    if (!path.isEmpty() && QFileInfo::exists(path))
        return path;
    return {};
}

}

class TstTranscriptFormat : public QObject
{
    Q_OBJECT

private slots:
    // See tst_transcription_queue.cpp's initTestCase() — same reasoning:
    // TranscriptionController now persists rolling-average phase timing via
    // QSettings, which must never touch the real ~/.config/suopTranscriber.
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // Frontmatter carries exactly the documented keys: model,
    // speakerDetection, transcribedAt. Booleans come back as real bools
    // (readMetadata maps the literal strings "true"/"false"), everything
    // else as trimmed strings.
    void readMetadata_parsesDocumentedFrontmatterKeys()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(!writeFixture(dir.path(), "meeting.wav.md", kFixtureMd).isEmpty());

        TranscriptionController controller;
        // Callers pass the AUDIO url; the sibling .md is derived. The audio
        // file itself doesn't need to exist for the read path.
        const QVariantMap meta = controller.readMetadata(QUrl::fromLocalFile(dir.path() + "/meeting.wav"));

        QCOMPARE(meta.value("model").toString(), QStringLiteral("ggml-base.en.bin"));
        QCOMPARE(meta.value("speakerDetection"), QVariant(true));
        QCOMPARE(meta.value("transcribedAt").toString(), QStringLiteral("2026-07-06T10:12:13"));
    }

    // --- existingVaultTags() + its per-file cache (perf fix: UI lag) ----------

    // Item 17: existingVaultTags() is the vault-wide tag vocabulary behind the
    // sidebar's tag autocomplete. Sidebar.qml's tagSuggestions() calls it from
    // a BINDING, so it re-runs on every keystroke in the tag filter — and the
    // underlying scan (collectVaultTags) walks every .md in the vault. It is
    // now backed by a per-file mtime+size cache so a keystroke re-parses
    // nothing that hasn't changed.
    //
    // The cache must be externally invisible. This pins the full contract:
    // de-duplicated across files, sorted case-insensitively, split on commas,
    // stable across repeated calls (the 2nd is cache-served), and — the part a
    // cache can actually break — correct after an in-place tags: rewrite, after
    // a brand-new .md appears, and after one is deleted.
    void existingVaultTags_dedupesAndSorts_andSurvivesVaultEdits()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(!writeFixture(vault.path(), "a.wav.md",
                              QStringLiteral("---\nmodel: base\ntags: zebra, apple\n---\n\n[0] hi\n")).isEmpty());
        // Overlapping tag ("apple") — must be de-duplicated across files.
        QVERIFY(!writeFixture(vault.path(), "b.wav.md",
                              QStringLiteral("---\nmodel: base\ntags: apple, Mango\n---\n\n[0] hi\n")).isEmpty());

        TranscriptionController controller;
        const QUrl vaultUrl = QUrl::fromLocalFile(vault.path());

        const QStringList expected{QStringLiteral("apple"), QStringLiteral("Mango"), QStringLiteral("zebra")};
        // Twice: the second call is served from the cache and must agree.
        QCOMPARE(controller.existingVaultTags(vaultUrl), expected);
        QCOMPARE(controller.existingVaultTags(vaultUrl), expected);

        // In-place rewrite of an already-scanned file: mtime+size change, so
        // the cached entry for a.wav.md must be discarded.
        QVERIFY(!writeFixture(vault.path(), "a.wav.md",
                              QStringLiteral("---\nmodel: base\ntags: quokka\n---\n\n[0] hi\n")).isEmpty());
        QCOMPARE(controller.existingVaultTags(vaultUrl),
                 (QStringList{QStringLiteral("apple"), QStringLiteral("Mango"), QStringLiteral("quokka")}));

        // A brand-new .md has no cache entry at all — the directory walk still
        // has to find and parse it.
        QVERIFY(!writeFixture(vault.path(), "c.wav.md",
                              QStringLiteral("---\nmodel: base\ntags: banana\n---\n\n[0] hi\n")).isEmpty());
        QCOMPARE(controller.existingVaultTags(vaultUrl),
                 (QStringList{QStringLiteral("apple"), QStringLiteral("banana"),
                              QStringLiteral("Mango"), QStringLiteral("quokka")}));

        // A deleted file's tags must vanish — a stale cache entry must never
        // resurrect a tag for a file that no longer exists (the walk, not the
        // cache, decides which files count).
        QVERIFY(QFile::remove(vault.path() + "/b.wav.md"));
        QCOMPARE(controller.existingVaultTags(vaultUrl),
                 (QStringList{QStringLiteral("banana"), QStringLiteral("quokka")}));
    }

    // A vault whose .md files have no tags: at all yields an empty vocabulary
    // rather than a list of empty strings — and stays empty on the cached
    // second call.
    void existingVaultTags_noTagsAnywhere_isEmpty()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(!writeFixture(vault.path(), "plain.wav.md",
                              QStringLiteral("---\nmodel: base\n---\n\n[0] hi\n")).isEmpty());

        TranscriptionController controller;
        const QUrl vaultUrl = QUrl::fromLocalFile(vault.path());
        QVERIFY(controller.existingVaultTags(vaultUrl).isEmpty());
        QVERIFY(controller.existingVaultTags(vaultUrl).isEmpty());
    }

    // Spec (this feature — see WhisperCppEngine::setVadModelPath()/
    // VadChunker.h): writeFrontmatter()'s new `vad`/`vadError` keys round-trip
    // through readMetadata() with the SAME generic key:value parsing every
    // other frontmatter field already uses — no special-casing needed,
    // mirroring speakerDetection/speakerDetectionError exactly.
    void readMetadata_parsesVadKeys()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "gated.wav.md",
                     QStringLiteral("---\n"
                                    "model: ggml-base.en.bin\n"
                                    "speakerDetection: false\n"
                                    "vad: true\n"
                                    "transcribedAt: 2026-07-06T10:12:13\n"
                                    "---\n\n"
                                    "[0] hello\n"));
        writeFixture(dir.path(), "ungated.wav.md",
                     QStringLiteral("---\n"
                                    "model: ggml-base.en.bin\n"
                                    "speakerDetection: false\n"
                                    "vad: false\n"
                                    "vadError: no VAD model available\n"
                                    "transcribedAt: 2026-07-06T10:12:13\n"
                                    "---\n\n"
                                    "[0] hello\n"));

        TranscriptionController controller;
        const QVariantMap gated = controller.readMetadata(QUrl::fromLocalFile(dir.path() + "/gated.wav"));
        QCOMPARE(gated.value("vad"), QVariant(true));
        QVERIFY(!gated.contains("vadError"));

        const QVariantMap ungated = controller.readMetadata(QUrl::fromLocalFile(dir.path() + "/ungated.wav"));
        QCOMPARE(ungated.value("vad"), QVariant(false));
        QCOMPARE(ungated.value("vadError").toString(), QStringLiteral("no VAD model available"));
    }

    // Regression: an orphaned .md (no matching audio file left in the vault)
    // is passed to readMetadata() as ITS OWN path, not an audio path —
    // sidecarPath() must recognize a path that already ends in .md and
    // return it unchanged instead of appending another ".md" (which would
    // look for a nonexistent "meeting.wav.md.md" and silently come back
    // empty, the bug that made the sidebar fall back to showing orphaned
    // rows' filename instead of their stored label).
    void readMetadata_orphanedMdPath_isSelfReferential()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString mdPath = writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);
        QVERIFY(!mdPath.isEmpty());

        TranscriptionController controller;
        const QVariantMap meta = controller.readMetadata(QUrl::fromLocalFile(mdPath));

        QCOMPARE(meta.value("model").toString(), QStringLiteral("ggml-base.en.bin"));
    }

    // "Sibling .md = transcribed" — no .md means not transcribed, reported
    // as an empty map (the QML side keys the green/yellow vault indicator
    // off this).
    void readMetadata_missingSidecarMeansUntranscribed_emptyMap()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        TranscriptionController controller;
        const QVariantMap meta = controller.readMetadata(QUrl::fromLocalFile(dir.path() + "/never-transcribed.wav"));
        QVERIFY(meta.isEmpty());
    }

    // readTranscriptBody returns everything after the frontmatter's closing
    // "---" line — INCLUDING the leading blank line writeFrontmatter() emits
    // ("---\n\n") before the first segment. The body is served raw for
    // main-panel search, not re-parsed.
    void readTranscriptBody_returnsEverythingAfterClosingDelimiter()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);

        TranscriptionController controller;
        const QString body = controller.readTranscriptBody(QUrl::fromLocalFile(dir.path() + "/meeting.wav"));
        QCOMPARE(body,
                 QStringLiteral("\n"
                                "[00:00:00.000 - 00:00:11.000] hello world\n"
                                "[00:00:11.000 - 00:00:14.500] And another speaker. [SPEAKER_TURN]\n"));

        // No sidecar / no frontmatter block => empty string ("untranscribed
        // or unreadable" per the header doc), never a partial read.
        QCOMPARE(controller.readTranscriptBody(QUrl::fromLocalFile(dir.path() + "/nothing.wav")), QString());
        writeFixture(dir.path(), "plain.wav.md", QStringLiteral("just prose, no frontmatter\n"));
        QCOMPARE(controller.readTranscriptBody(QUrl::fromLocalFile(dir.path() + "/plain.wav")), QString());
    }

    // The segment line format is the durable, greppable record of per-segment
    // timing — "[HH:MM:SS.mmm - HH:MM:SS.mmm] text", with an optional
    // trailing " [SPEAKER_TURN]" marker when tinydiarize flags a speaker
    // change. Lines must survive the read path verbatim (no re-formatting):
    // sentence-highlight sync re-derives timing from these lines alone.
    void segmentLineFormat_isHumanReadableAndSurvivesRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);

        TranscriptionController controller;
        const QString body = controller.readTranscriptBody(QUrl::fromLocalFile(dir.path() + "/meeting.wav"));

        QVERIFY(body.contains(QStringLiteral("[00:00:00.000 - 00:00:11.000] hello world")));
        QVERIFY(body.contains(QStringLiteral("[00:00:11.000 - 00:00:14.500] And another speaker. [SPEAKER_TURN]")));
    }

    // DESIGN.md/CLAUDE.md "Sentence highlighting": parseSentences() turns the
    // segment lines back into {t0, text} — the structured form the
    // playback-highlight binary search needs. Timestamps round-trip exactly
    // (parseTimestamp is the literal inverse of formatTimestamp), and the
    // optional " [SPEAKER_TURN]" marker is stripped from the parsed text.
    // (No "t1": it's never stored — see appendSegmentLine — and this legacy
    // fixture's end timestamps are simply ignored, not round-tripped.)
    void parseSentences_recoversStructuredTimingFromSegmentLines()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);

        TranscriptionController controller;
        const QVariantList sentences = controller.parseSentences(QUrl::fromLocalFile(dir.path() + "/meeting.wav"));

        QCOMPARE(sentences.size(), 2);
        const QVariantMap s0 = sentences[0].toMap();
        QCOMPARE(s0.value("t0").toDouble(), 0.0);
        QCOMPARE(s0.value("text").toString(), QStringLiteral("hello world"));

        const QVariantMap s1 = sentences[1].toMap();
        QCOMPARE(s1.value("t0").toDouble(), 11.0);
        // The [SPEAKER_TURN] marker is metadata about the boundary, not part
        // of the spoken text — stripped from the parsed sentence.
        QCOMPARE(s1.value("text").toString(), QStringLiteral("And another speaker."));

        // start/length must index the EXACT substring of readTranscriptBody's
        // own return value — this is what lets a caller (MainPanel.qml)
        // span-highlight the active sentence without re-deriving offsets.
        const QString body = controller.readTranscriptBody(QUrl::fromLocalFile(dir.path() + "/meeting.wav"));
        for (const QVariant &v : sentences) {
            const QVariantMap s = v.toMap();
            const int start = s.value("start").toInt();
            const int length = s.value("length").toInt();
            QCOMPARE(body.mid(start, length), s.value("text").toString());
        }

        // Legacy fixture (no leading "[Speaker N]" tag) parses with an empty
        // speaker string for every sentence — old already-transcribed files
        // must not crash or misparse under the new (Item 11, Wave E) regex.
        QCOMPARE(s0.value("speaker").toString(), QString());
        QCOMPARE(s1.value("speaker").toString(), QString());
    }

    // Item 11 (Wave E): a leading "[Speaker N]" tag (written when speaker
    // detection is on) is parsed into each sentence's "speaker" field
    // verbatim (the literal tag text, not translated through any lookup —
    // see the 2026-07-07 rewrite) and stripped from the parsed text,
    // mirroring the legacy trailing "[SPEAKER_TURN]" marker's text-stripping
    // behavior above. Requires the frontmatter "speakers:" allowlist (2026
    // rewrite, see segmentLinePattern()'s doc comment) to list the tag —
    // that's what distinguishes a real speaker tag from a hallucinated
    // bracketed annotation.
    void parseSentences_recoversSpeakerNumberFromLeadingTag()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "interview.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\nspeakerDetection: true\n"
                                    "speakers: Speaker 1, Speaker 2\n---\n\n"
                                    "[00:00:00.000 - 00:00:05.000] [Speaker 1] Welcome to the show.\n"
                                    "[00:00:05.000 - 00:00:09.000] [Speaker 2] Thanks for having me.\n"
                                    "[00:00:09.000 - 00:00:12.000] [Speaker 1] So, tell us about yourself.\n"));

        TranscriptionController controller;
        const QVariantList sentences =
            controller.parseSentences(QUrl::fromLocalFile(dir.path() + "/interview.wav"));

        QCOMPARE(sentences.size(), 3);
        QCOMPARE(sentences[0].toMap().value("speaker").toString(), QStringLiteral("Speaker 1"));
        QCOMPARE(sentences[0].toMap().value("text").toString(), QStringLiteral("Welcome to the show."));
        QCOMPARE(sentences[1].toMap().value("speaker").toString(), QStringLiteral("Speaker 2"));
        QCOMPARE(sentences[1].toMap().value("text").toString(), QStringLiteral("Thanks for having me."));
        QCOMPARE(sentences[2].toMap().value("speaker").toString(), QStringLiteral("Speaker 1"));

        // start/length still index the raw body correctly (tag is excluded
        // from the substring, same contract as before).
        const QString body = controller.readTranscriptBody(QUrl::fromLocalFile(dir.path() + "/interview.wav"));
        for (const QVariant &v : sentences) {
            const QVariantMap s = v.toMap();
            QCOMPARE(body.mid(s.value("start").toInt(), s.value("length").toInt()), s.value("text").toString());
        }
    }

    // The CURRENT (not legacy) write format: a single "[HH:MM:SS.mmm]" start
    // timestamp only, no "- end" range — appendSegmentLine() no longer
    // writes the end timestamp at all (it was always identical to the next
    // segment's start). Confirms parseSentences() handles this format
    // directly, not just via the backward-compat range-tolerant branch.
    void parseSentences_recoversTimingFromStartOnlyTimestamp()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "notes.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n\n"
                                    "[00:00:00.000] First point.\n"
                                    "[00:00:04.250] Second point.\n"));

        TranscriptionController controller;
        const QVariantList sentences = controller.parseSentences(QUrl::fromLocalFile(dir.path() + "/notes.wav"));

        QCOMPARE(sentences.size(), 2);
        QCOMPARE(sentences[0].toMap().value("t0").toDouble(), 0.0);
        QCOMPARE(sentences[0].toMap().value("speaker").toString(), QString());
        QCOMPARE(sentences[0].toMap().value("text").toString(), QStringLiteral("First point."));
        QCOMPARE(sentences[1].toMap().value("t0").toDouble(), 4.25);
        QCOMPARE(sentences[1].toMap().value("text").toString(), QStringLiteral("Second point."));
    }

    // A trailing " (×N)" marker (SegmentLineWriter's identical-consecutive-
    // line merge, N > 1) is stripped from the parsed text and surfaced as
    // repeatCount, mirroring how the legacy "[SPEAKER_TURN]" marker is
    // stripped and surfaced separately. A normal (unmerged) line has no
    // marker and reports repeatCount 1.
    void parseSentences_recoversRepeatCountFromMergeMarker()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "standup.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n\n"
                                    "[0] Testing, testing. (×3)\n"
                                    "[9] Any updates?\n"));

        TranscriptionController controller;
        const QVariantList sentences = controller.parseSentences(QUrl::fromLocalFile(dir.path() + "/standup.wav"));

        QCOMPARE(sentences.size(), 2);
        QCOMPARE(sentences[0].toMap().value("text").toString(), QStringLiteral("Testing, testing."));
        QCOMPARE(sentences[0].toMap().value("repeatCount").toInt(), 3);
        QCOMPARE(sentences[1].toMap().value("text").toString(), QStringLiteral("Any updates?"));
        QCOMPARE(sentences[1].toMap().value("repeatCount").toInt(), 1);

        // start/length still index the exact "Testing, testing." substring,
        // excluding the marker, same contract as the speaker tag/
        // SPEAKER_TURN stripping above.
        const QString body = controller.readTranscriptBody(QUrl::fromLocalFile(dir.path() + "/standup.wav"));
        QCOMPARE(body.mid(sentences[0].toMap().value("start").toInt(), sentences[0].toMap().value("length").toInt()),
                 QStringLiteral("Testing, testing."));
    }

    // Regression: whisper.cpp occasionally hallucinates a bracketed
    // non-speech annotation (e.g. "[BLANK_AUDIO]") that sits in EXACTLY the
    // position a real "[Speaker N]" tag would. Before the frontmatter
    // "speakers:" allowlist (segmentLinePattern()'s doc comment), an open
    // `[^\]]+` speaker-tag match misread this as a brand new "speaker" — and
    // once SegmentLineWriter started merging repeated identical lines, a
    // silence stretch that hallucinated the same annotation 32 times in a
    // row collapsed into one bogus "speaker said [BLANK_AUDIO] (×32)"
    // entry. Speaker detection IS on here (with real speakers "Speaker 1"/
    // "Speaker 2" in the allowlist) — the annotation must still fall through
    // to plain text because "BLANK_AUDIO" itself is not a KNOWN speaker
    // name, proving the fix is a real allowlist, not just "ignore brackets
    // when speakerDetection is off".
    void parseSentences_doesNotMisreadHallucinatedBracketAnnotationAsASpeaker()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "silence.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\nspeakerDetection: true\n"
                                    "speakers: Speaker 1, Speaker 2\n---\n\n"
                                    "[0] [Speaker 1] hello\n"
                                    "[5] [BLANK_AUDIO] (×32)\n"));

        TranscriptionController controller;
        const QVariantList sentences = controller.parseSentences(QUrl::fromLocalFile(dir.path() + "/silence.wav"));

        QCOMPARE(sentences.size(), 2);
        QCOMPARE(sentences[0].toMap().value("speaker").toString(), QStringLiteral("Speaker 1"));
        QCOMPARE(sentences[0].toMap().value("text").toString(), QStringLiteral("hello"));

        // The critical assertion: NOT parsed as speaker="BLANK_AUDIO",
        // text="" — the whole bracketed annotation stays part of the text,
        // and the merge count is still recovered correctly alongside it.
        QCOMPARE(sentences[1].toMap().value("speaker").toString(), QString());
        QCOMPARE(sentences[1].toMap().value("text").toString(), QStringLiteral("[BLANK_AUDIO]"));
        QCOMPARE(sentences[1].toMap().value("repeatCount").toInt(), 32);
    }

    // SegmentLineWriter (TranscriptionController.h/.cpp): whisper.cpp
    // occasionally hallucinates/repeats the exact same sentence across
    // several consecutive segments (silence stretches, looping audio) — per
    // the user's request, a run of consecutive segments with the same
    // speaker AND the same trimmed text must be merged IN THE FILE ITSELF
    // (not just hidden by a display-layer dedup), collapsing to one line
    // that carries the FIRST occurrence's timestamp and a trailing " (×N)"
    // repeat count. Driven directly with synthetic Segments (no whisper
    // model needed) since the class is a plain QFile-based writer.
    void segmentLineWriter_mergesConsecutiveIdenticalLines()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + "/out.md";

        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            SegmentLineWriter writer(file);

            Segment hello1;
            hello1.t0 = 0.0;
            hello1.text = "hello";
            writer.append(hello1, QString());

            // Same speaker (0 = no speaker detection), same trimmed text as
            // the immediately-preceding line -> merges in place, no new
            // line appended.
            Segment hello2;
            hello2.t0 = 1.0;
            hello2.text = "hello";
            writer.append(hello2, QString());

            Segment hello3;
            hello3.t0 = 2.0;
            hello3.text = "  hello  "; // whitespace-only differences still match (trimmed)
            writer.append(hello3, QString());

            // Different text -> breaks the run, starts a fresh (unmerged) line.
            Segment world;
            world.t0 = 3.0;
            world.text = "world";
            writer.append(world, QString());
        }

        QCOMPARE(readAllBytes(path), QStringLiteral("[0] hello (×3)\n[3] world\n"));
    }

    // Two consecutive lines with identical text but DIFFERENT speakers must
    // never be merged, even though the text alone matches — a repeated
    // phrase from two different people is not a whisper hallucination.
    void segmentLineWriter_doesNotMergeAcrossDifferentSpeakers()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.path() + "/out.md";

        {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            SegmentLineWriter writer(file);

            Segment hi1;
            hi1.t0 = 0.0;
            hi1.text = "hi";
            writer.append(hi1, QStringLiteral("Speaker 1"));

            Segment hi2;
            hi2.t0 = 1.0;
            hi2.text = "hi";
            writer.append(hi2, QStringLiteral("Speaker 2"));

            // Back to speaker 1 with the same text again — not merged either,
            // since the immediately-preceding line was speaker 2's, not a
            // consecutive run of speaker 1's "hi".
            Segment hi3;
            hi3.t0 = 2.0;
            hi3.text = "hi";
            writer.append(hi3, QStringLiteral("Speaker 1"));
        }

        QCOMPARE(readAllBytes(path),
                 QStringLiteral("[0] [Speaker 1] hi\n[1] [Speaker 2] hi\n[2] [Speaker 1] hi\n"));
    }

    // Round-trips a SegmentLineWriter-produced merge through
    // parseSentences() end to end — the write and read sides must agree on
    // the exact on-disk shape without a real transcription.
    void segmentLineWriter_roundTripsThroughParseSentences()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString mdPath = dir.path() + "/meeting.wav.md";

        {
            QFile file(mdPath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QTextStream out(&file);
            out << "---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n\n";
            out.flush();
            file.flush();

            SegmentLineWriter writer(file);
            Segment hello1;
            hello1.t0 = 0.0;
            hello1.text = "hello";
            writer.append(hello1, QString());
            Segment hello2;
            hello2.t0 = 1.0;
            hello2.text = "hello";
            writer.append(hello2, QString());
            Segment world;
            world.t0 = 3.0;
            world.text = "world";
            writer.append(world, QString());
        }

        TranscriptionController controller;
        const QVariantList sentences = controller.parseSentences(QUrl::fromLocalFile(dir.path() + "/meeting.wav"));

        QCOMPARE(sentences.size(), 2);
        QCOMPARE(sentences[0].toMap().value("t0").toDouble(), 0.0);
        QCOMPARE(sentences[0].toMap().value("text").toString(), QStringLiteral("hello"));
        QCOMPARE(sentences[0].toMap().value("repeatCount").toInt(), 2);
        QCOMPARE(sentences[1].toMap().value("t0").toDouble(), 3.0);
        QCOMPARE(sentences[1].toMap().value("text").toString(), QStringLiteral("world"));
        QCOMPARE(sentences[1].toMap().value("repeatCount").toInt(), 1);
    }

    // An untranscribed/unreadable file (no sidecar) parses to an empty list,
    // not an error — mirrors readTranscriptBody's "empty string" contract.
    void parseSentences_emptyForUntranscribedFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        TranscriptionController controller;
        const QVariantList sentences =
            controller.parseSentences(QUrl::fromLocalFile(dir.path() + "/never-transcribed.wav"));
        QVERIFY(sentences.isEmpty());
    }

    // activeSentenceIndex binary-searches (DESIGN.md: "binary-search the
    // active sentence... don't linear scan") for the sentence covering a
    // given playback position, in seconds. It is "sticky" through silence
    // gaps between segments: a position after sentence i's t1 but before
    // i+1's t0 still resolves to i, so the highlight doesn't blank out
    // between segments — and -1 before the very first sentence starts.
    void activeSentenceIndex_binarySearchWithStickyGaps()
    {
        TranscriptionController controller;
        QVariantList sentences;
        auto addSentence = [&](double t0, double t1, const QString &text) {
            QVariantMap s;
            s["t0"] = t0;
            s["t1"] = t1;
            s["text"] = text;
            sentences.append(s);
        };
        addSentence(10.0, 15.0, QStringLiteral("first"));
        addSentence(20.0, 25.0, QStringLiteral("second"));
        addSentence(25.0, 30.0, QStringLiteral("third, starts exactly when second ends"));

        QCOMPARE(controller.activeSentenceIndex(sentences, 0.0), -1);   // before everything
        QCOMPARE(controller.activeSentenceIndex(sentences, 9.999), -1); // just before the first t0
        QCOMPARE(controller.activeSentenceIndex(sentences, 10.0), 0);  // exactly on the first t0
        QCOMPARE(controller.activeSentenceIndex(sentences, 12.0), 0);  // mid-sentence
        QCOMPARE(controller.activeSentenceIndex(sentences, 17.0), 0);  // silence gap after 1st -> sticky on 1st
        QCOMPARE(controller.activeSentenceIndex(sentences, 20.0), 1);  // exactly on the second t0
        QCOMPARE(controller.activeSentenceIndex(sentences, 25.0), 2);  // boundary shared by 2nd/3rd -> the later one
        QCOMPARE(controller.activeSentenceIndex(sentences, 29.0), 2);  // mid third
        QCOMPARE(controller.activeSentenceIndex(sentences, 1000.0), 2); // long after everything -> sticky on last

        QCOMPARE(controller.activeSentenceIndex(QVariantList(), 5.0), -1); // empty list
    }

    // setLabel adds a `label:` key inside the existing frontmatter block,
    // preserving every other key AND the transcript body byte-for-byte
    // (including the blank line after the closing "---"). Setting it again
    // REPLACES the existing key — never duplicates it — and the value
    // round-trips through readMetadata.
    void setLabel_addsThenReplacesKey_preservingEverythingElse()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString mdPath = writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/meeting.wav");

        TranscriptionController controller;
        QString error;
        QVERIFY(controller.setLabel(audioUrl, QStringLiteral("Weekly sync"), &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));

        // Exact expected bytes: same frontmatter with `label:` appended as
        // the last key, same body verbatim.
        QCOMPARE(readAllBytes(mdPath),
                 QStringLiteral("---\n"
                                "model: ggml-base.en.bin\n"
                                "speakerDetection: true\n"
                                "transcribedAt: 2026-07-06T10:12:13\n"
                                "label: Weekly sync\n"
                                "---\n"
                                "\n"
                                "[00:00:00.000 - 00:00:11.000] hello world\n"
                                "[00:00:11.000 - 00:00:14.500] And another speaker. [SPEAKER_TURN]\n"));

        // Re-labeling replaces, never duplicates.
        QVERIFY(controller.setLabel(audioUrl, QStringLiteral("Renamed"), &error));
        const QString afterRelabel = readAllBytes(mdPath);
        QCOMPARE(afterRelabel.count(QStringLiteral("label:")), 1);
        QVERIFY(afterRelabel.contains(QStringLiteral("label: Renamed\n")));
        QVERIFY(!afterRelabel.contains(QStringLiteral("Weekly sync")));

        // Round-trip through the metadata reader.
        QCOMPARE(controller.readMetadata(audioUrl).value("label").toString(), QStringLiteral("Renamed"));
        // ...and the body is still untouched.
        QVERIFY(controller.readTranscriptBody(audioUrl)
                    .contains(QStringLiteral("[00:00:00.000 - 00:00:11.000] hello world")));
    }

    // setLabel refuses to touch anything that isn't a well-formed transcript
    // sidecar: a missing .md means "not transcribed yet"; an existing .md
    // whose first line is not "---" is reported as having no frontmatter
    // block. Both fail with a non-empty error and leave the file alone
    // (current behavior: setLabel does NOT synthesize a frontmatter block
    // on a bare .md).
    void setLabel_failsOnMissingOrFrontmatterlessSidecar()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        TranscriptionController controller;
        QString error;

        // No sidecar at all -> "has not been transcribed yet".
        QVERIFY(!controller.setLabel(QUrl::fromLocalFile(dir.path() + "/absent.wav"), QStringLiteral("x"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY2(error.contains(QStringLiteral("not been transcribed")), qPrintable(error));

        // Sidecar exists but has no frontmatter -> refused, file untouched.
        const QString original = QStringLiteral("free-form notes, not a transcript\n");
        const QString barePath = writeFixture(dir.path(), "bare.wav.md", original);
        error.clear();
        QVERIFY(!controller.setLabel(QUrl::fromLocalFile(dir.path() + "/bare.wav"), QStringLiteral("x"), &error));
        QVERIFY2(error.contains(QStringLiteral("no frontmatter")), qPrintable(error));
        QCOMPARE(readAllBytes(barePath), original);
    }

    // Feature 2: setPrelude() on a body with no existing "<!-- prelude
    // end -->" marker (fresh transcribe, or a pre-Feature-2 file) inserts
    // the marker and the new prelude text, leaving the frontmatter and every
    // segment line completely untouched.
    void setPrelude_freshFile_insertsMarkerAndPreservesSegments()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/meeting.wav");

        TranscriptionController controller;
        QString error;
        QVERIFY(controller.setPrelude(audioUrl, QStringLiteral("This is the intro."), &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));

        const QString raw = readAllBytes(dir.path() + "/meeting.wav.md");
        QCOMPARE(raw.count(QStringLiteral("<!-- prelude end -->")), 1);
        QVERIFY(raw.startsWith(QStringLiteral("---\n"
                                               "model: ggml-base.en.bin\n"
                                               "speakerDetection: true\n"
                                               "transcribedAt: 2026-07-06T10:12:13\n"
                                               "---\n"
                                               "This is the intro.\n"
                                               "\n"
                                               "<!-- prelude end -->\n")));
        QVERIFY(raw.contains(QStringLiteral("[00:00:00.000 - 00:00:11.000] hello world")));
        QVERIFY(raw.contains(QStringLiteral("[00:00:11.000 - 00:00:14.500] And another speaker. [SPEAKER_TURN]")));
    }

    // Regenerating (relabel, or a second manual edit) must REPLACE the old
    // prelude region in place — not duplicate the marker, and not leave the
    // old text lying around — while segment lines after the marker stay
    // untouched.
    void setPrelude_replacesOldPreludeNotDuplicates()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/meeting.wav");

        TranscriptionController controller;
        QVERIFY(controller.setPrelude(audioUrl, QStringLiteral("Old intro.")));
        QVERIFY(controller.setPrelude(audioUrl, QStringLiteral("New intro.")));

        const QString raw = readAllBytes(dir.path() + "/meeting.wav.md");
        QCOMPARE(raw.count(QStringLiteral("<!-- prelude end -->")), 1);
        QVERIFY(raw.contains(QStringLiteral("New intro.")));
        QVERIFY(!raw.contains(QStringLiteral("Old intro.")));
        QVERIFY(raw.contains(QStringLiteral("[00:00:00.000 - 00:00:11.000] hello world")));
        QVERIFY(raw.contains(QStringLiteral("[00:00:11.000 - 00:00:14.500] And another speaker. [SPEAKER_TURN]")));
    }

    // Formatting must be stable across repeated regeneration with UNCHANGED
    // text — otherwise every no-op relabel would show as a spurious diff.
    void setPrelude_idempotentOnRepeatedSameText()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/meeting.wav");

        TranscriptionController controller;
        QVERIFY(controller.setPrelude(audioUrl, QStringLiteral("Same intro text.")));
        const QString afterFirst = readAllBytes(dir.path() + "/meeting.wav.md");
        QVERIFY(controller.setPrelude(audioUrl, QStringLiteral("Same intro text.")));
        const QString afterSecond = readAllBytes(dir.path() + "/meeting.wav.md");
        QCOMPARE(afterSecond, afterFirst);
    }

    // readPrelude() returns empty before any prelude exists, and round-trips
    // a real multi-paragraph value (with actual embedded newlines — the
    // whole point of moving this out of the single-line frontmatter) through
    // setPrelude().
    void setPrelude_readPrelude_roundTrips()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/meeting.wav");

        TranscriptionController controller;
        QCOMPARE(controller.readPrelude(audioUrl), QString());

        const QString prelude = QStringLiteral("Paragraph one.\n\nParagraph two, with more detail.");
        QVERIFY(controller.setPrelude(audioUrl, prelude));
        QCOMPARE(controller.readPrelude(audioUrl), prelude);
    }

    // Feature 3a ("generate chapters"): spliceBodyChapters() inserts a
    // "## [timestamp] title" marker line immediately before the segment line
    // whose own leading "[timestamp]" exactly matches — collision-free with
    // segmentLinePattern() (anchored on a literal "^\[", never "^## ") by
    // construction. A fresh body with no prior chapters gets every marker
    // inserted, segment lines themselves untouched.
    void spliceBodyChapters_freshBody_insertsMarkersBeforeMatchingSegments()
    {
        const QString body = QStringLiteral("\n"
                                             "[0] First point.\n"
                                             "[10] Second point.\n"
                                             "[20] Third point.\n");

        const QList<ResolvedChapter> chapters = {
            {0.0, QStringLiteral("Intro")},
            {20.0, QStringLiteral("Conclusion")},
        };

        const QString spliced = spliceBodyChapters(body, chapters);

        QCOMPARE(spliced, QStringLiteral("\n"
                                          "## [0] Intro\n"
                                          "[0] First point.\n"
                                          "[10] Second point.\n"
                                          "## [20] Conclusion\n"
                                          "[20] Third point.\n"));
    }

    // Regenerating (a later "generate chapters" call) must strip the OLD
    // marker set entirely before inserting the new one — never accumulate
    // duplicates across repeated calls, mirroring Feature 2's prelude
    // unconditional-overwrite semantics.
    void spliceBodyChapters_regenerating_stripsOldMarkersNotDuplicating()
    {
        const QString body = QStringLiteral("\n"
                                             "[0] First point.\n"
                                             "[10] Second point.\n");

        const QString afterFirst = spliceBodyChapters(body, {{0.0, QStringLiteral("Old Intro")}});
        QVERIFY(afterFirst.contains(QStringLiteral("## [0] Old Intro")));

        const QString afterSecond = spliceBodyChapters(afterFirst, {{10.0, QStringLiteral("New Chapter")}});

        QCOMPARE(afterSecond.count(QStringLiteral("## [")), 1);
        QVERIFY(!afterSecond.contains(QStringLiteral("Old Intro")));
        QVERIFY(afterSecond.contains(QStringLiteral("## [10] New Chapter")));
        QVERIFY(afterSecond.contains(QStringLiteral("[0] First point.")));
        QVERIFY(afterSecond.contains(QStringLiteral("[10] Second point.")));
    }

    // Multiple chapters at multiple positions, given to spliceBodyChapters()
    // in DELIBERATELY non-file-order (scrambled) input order — the function
    // must still land every marker in the right place (proving its internal
    // descending-offset sort, not just "whatever order the caller happened
    // to pass them in"), and every segment line must survive byte-for-byte.
    void spliceBodyChapters_multipleChaptersOutOfOrderInput_allLandCorrectly()
    {
        const QString body = QStringLiteral("\n"
                                             "[0] Segment zero.\n"
                                             "[5] Segment one.\n"
                                             "[10] Segment two.\n"
                                             "[15] Segment three.\n");

        const QList<ResolvedChapter> chapters = {
            {10.0, QStringLiteral("Middle")},
            {0.0, QStringLiteral("Start")},
            {15.0, QStringLiteral("End")},
        };

        const QString spliced = spliceBodyChapters(body, chapters);

        QCOMPARE(spliced, QStringLiteral("\n"
                                          "## [0] Start\n"
                                          "[0] Segment zero.\n"
                                          "[5] Segment one.\n"
                                          "## [10] Middle\n"
                                          "[10] Segment two.\n"
                                          "## [15] End\n"
                                          "[15] Segment three.\n"));
    }

    // readChapters() scans for "## [timestamp] title" marker lines and
    // returns them as {t0, title} maps in file order — the read-side
    // counterpart to spliceBodyChapters() above, exercised through the
    // actual TranscriptionController Q_INVOKABLE (not the free function) on
    // a hand-written fixture. No model needed since this is a pure parse.
    void readChapters_roundTripsMarkerLinesFromFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n\n"
                                    "## [0] Intro\n"
                                    "[0] First point.\n"
                                    "[10] Second point.\n"
                                    "## [20] Conclusion\n"
                                    "[20] Third point.\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/meeting.wav");

        TranscriptionController controller;
        const QVariantList chapters = controller.readChapters(audioUrl);

        QCOMPARE(chapters.size(), 2);
        QCOMPARE(chapters[0].toMap().value("t0").toDouble(), 0.0);
        QCOMPARE(chapters[0].toMap().value("title").toString(), QStringLiteral("Intro"));
        QCOMPARE(chapters[1].toMap().value("t0").toDouble(), 20.0);
        QCOMPARE(chapters[1].toMap().value("title").toString(), QStringLiteral("Conclusion"));

        // No chapters yet -> empty list, not an error.
        writeFixture(dir.path(), "notranscript.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n\n"
                                    "[0] Just a segment, no chapters.\n"));
        QCOMPARE(controller.readChapters(QUrl::fromLocalFile(dir.path() + "/notranscript.wav")).size(), 0);
    }

    // Full pipeline: generateChapters() job dispatch through
    // TranscriptionWorker::generateChaptersJob() — real transcribe first (no
    // chapters yet), then generateChapters() must run to completion and
    // produce well-formed "## [timestamp] title" marker lines (if any —
    // jfk.wav is short enough that zero chapters is a legitimate outcome,
    // not asserted here) readable back via readChapters(), WITHOUT
    // disturbing the segment lines themselves. Gated on both real models
    // like autoLabel_addsLabelPreludeTagsToTranscript() above (loads whisper
    // + the ~2.5GB label GGUF and runs a real generation pass).
    void generateChapters_endToEnd_dispatchesJobAndProducesWellFormedMarkers()
    {
        const QString whisperModelPath = locateRealModel();
        if (whisperModelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        const QString labelModelPath = locateRealLabelModel();
        if (labelModelPath.isEmpty())
            QSKIP("no real label GGUF model available; set SUOP_TEST_LABEL_MODEL to run this test");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample (DESIGN.md: ffmpeg decodes, Qt only plays)");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(whisperModelPath, modelsDir + "/ggml-base.en.bin"));
        QVERIFY(QFile::copy(labelModelPath, modelsDir + "/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"));

        const QString samplePath = vault.path() + "/jfk.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        TranscriptionController controller;
        const QUrl audioUrl = QUrl::fromLocalFile(samplePath);
        const QUrl vaultUrl = QUrl::fromLocalFile(vault.path());

        {
            QSignalSpy spy(&controller, &TranscriptionController::finished);
            controller.transcribe(audioUrl, vaultUrl, QStringLiteral("base.en"), false, /*autoLabel=*/false);
            QVERIFY(spy.wait(30000));
            QVERIFY2(spy.at(0).at(1).toBool(), qPrintable(spy.at(0).at(2).toString()));
        }
        const QString bodyBeforeChapters = controller.readTranscriptBody(audioUrl);
        QVERIFY(controller.readChapters(audioUrl).isEmpty());

        {
            QSignalSpy spy(&controller, &TranscriptionController::finished);
            controller.generateChapters(audioUrl, vaultUrl);
            QCOMPARE(controller.jobs().last().toMap().value("kind").toString(), QStringLiteral("chapters"));
            QVERIFY(spy.wait(60000));
            QVERIFY2(spy.at(0).at(1).toBool(), qPrintable(spy.at(0).at(2).toString()));
        }

        const QVariantList chapters = controller.readChapters(audioUrl);
        for (const QVariant &v : chapters)
            QVERIFY(!v.toMap().value("title").toString().isEmpty());

        // Segment content itself must be untouched — chapters only ever
        // INSERT new lines, never modify existing ones.
        const QString bodyAfterChapters = controller.readTranscriptBody(audioUrl);
        for (const QString &segmentLine : bodyBeforeChapters.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            QVERIFY2(bodyAfterChapters.contains(segmentLine), qPrintable(segmentLine));
    }

    // 2026-07-07 rewrite: renameSpeaker() now bakes the new name directly
    // into every "[oldName]" segment-line tag in the BODY (no more separate
    // frontmatter name-lookup layer) — runs asynchronously on the
    // background thread, reporting completion via
    // speakerRenamed(fileUrl, oldName, ok, error). A LATER rewrite added the
    // frontmatter "speakers:" allowlist (segmentLinePattern()'s doc
    // comment); renameSpeaker() must keep THAT in sync too, or a second
    // rename of the same (now-renamed) speaker would fail the allowlist
    // check even though the body tag itself was updated correctly.
    void renameSpeaker_rewritesEveryBodyTagAsync()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "interview.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\n"
                                    "speakerDetection: true\nspeakers: Speaker 1, Speaker 2\n---\n\n"
                                    "[0] [Speaker 1] hi\n"
                                    "[3.5] [Speaker 2] hello\n"
                                    "[7] [Speaker 1] how are you\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/interview.wav");

        TranscriptionController controller;

        {
            QSignalSpy spy(&controller, &TranscriptionController::speakerRenamed);
            controller.renameSpeaker(audioUrl, QStringLiteral("Speaker 1"), QStringLiteral("Alice"));
            QVERIFY(spy.wait(5000));
            const QList<QVariant> args = spy.takeFirst();
            QCOMPARE(args.at(0).toUrl(), audioUrl);
            QCOMPARE(args.at(1).toString(), QStringLiteral("Speaker 1"));
            QVERIFY2(args.at(2).toBool(), qPrintable(args.at(3).toString()));
        }

        // EVERY occurrence of the old tag is rewritten, not just the first —
        // Speaker 1 appears on two separate lines here.
        const QString bodyAfterFirst = controller.readTranscriptBody(audioUrl);
        QVERIFY(bodyAfterFirst.contains(QStringLiteral("[Alice] hi")));
        QVERIFY(bodyAfterFirst.contains(QStringLiteral("[Alice] how are you")));
        QVERIFY(bodyAfterFirst.contains(QStringLiteral("[Speaker 2] hello")));
        QVERIFY(!bodyAfterFirst.contains(QStringLiteral("Speaker 1")));
        // The frontmatter allowlist is updated too, not just the body.
        QVERIFY(controller.readMetadata(audioUrl).value("speakers").toString().contains(QStringLiteral("Alice")));

        // Renaming a SECOND, different speaker only touches its own tag.
        {
            QSignalSpy spy(&controller, &TranscriptionController::speakerRenamed);
            controller.renameSpeaker(audioUrl, QStringLiteral("Speaker 2"), QStringLiteral("Bob"));
            QVERIFY(spy.wait(5000));
            QVERIFY(spy.takeFirst().at(2).toBool());
        }
        const QString bodyAfterSecond = controller.readTranscriptBody(audioUrl);
        QVERIFY(bodyAfterSecond.contains(QStringLiteral("[Alice] hi")));
        QVERIFY(bodyAfterSecond.contains(QStringLiteral("[Bob] hello")));

        // Re-renaming (matched by its NOW-current name, "Alice") updates
        // every one of that speaker's lines again.
        {
            QSignalSpy spy(&controller, &TranscriptionController::speakerRenamed);
            controller.renameSpeaker(audioUrl, QStringLiteral("Alice"), QStringLiteral("Alicia"));
            QVERIFY(spy.wait(5000));
            QVERIFY(spy.takeFirst().at(2).toBool());
        }
        const QString bodyAfterThird = controller.readTranscriptBody(audioUrl);
        QVERIFY(bodyAfterThird.contains(QStringLiteral("[Alicia] hi")));
        QVERIFY(bodyAfterThird.contains(QStringLiteral("[Alicia] how are you")));
        QVERIFY(bodyAfterThird.contains(QStringLiteral("[Bob] hello")));
        QCOMPARE(controller.readMetadata(audioUrl).value("speakers").toString(), QStringLiteral("Alicia, Bob"));
    }

    // Manual-edit counterpart for segment text (MainPanel's always-editable
    // transcriptArea) — replaces just the TEXT of the matched t0 line(s),
    // leaving timestamp/speaker tag/every other line byte-for-byte
    // untouched. Multiple edits in one call splice correctly regardless of
    // which line comes first in the body (see the C++ end-to-start splice
    // order).
    void setSentenceTexts_replacesTextPreservingTimestampAndSpeaker()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "interview.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\n"
                                    "speakerDetection: true\nspeakers: Speaker 1, Speaker 2\n---\n\n"
                                    "[0] [Speaker 1] hi\n"
                                    "[3.5] [Speaker 2] hello\n"
                                    "[7] [Speaker 1] how are you\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/interview.wav");

        TranscriptionController controller;
        QString error;
        QVariantList edits;
        edits.append(QVariantMap{{"t0", 0.0}, {"text", QStringLiteral("hi there")}});
        edits.append(QVariantMap{{"t0", 7.0}, {"text", QStringLiteral("how ARE you")}});
        QVERIFY(controller.setSentenceTexts(audioUrl, edits, &error));
        QVERIFY2(error.isEmpty(), qPrintable(error));

        const QString body = controller.readTranscriptBody(audioUrl);
        QVERIFY(body.contains(QStringLiteral("[0] [Speaker 1] hi there")));
        QVERIFY(body.contains(QStringLiteral("[3.5] [Speaker 2] hello"))); // untouched
        QVERIFY(body.contains(QStringLiteral("[7] [Speaker 1] how ARE you")));
        QVERIFY(!body.contains(QStringLiteral("how are you")));

        // Round-trips through parseSentences too, not just the raw body.
        const QVariantList sentences = controller.parseSentences(audioUrl);
        QCOMPARE(sentences.size(), 3);
        QCOMPARE(sentences.at(0).toMap().value("text").toString(), QStringLiteral("hi there"));
    }

    // Fails (no partial write) if a requested t0 doesn't match any existing
    // segment line — the file is left completely untouched.
    void setSentenceTexts_rejectsUnknownTimestamp()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/meeting.wav");
        const QString original = readAllBytes(dir.path() + "/meeting.wav.md");

        TranscriptionController controller;
        QString error;
        QVariantList edits;
        edits.append(QVariantMap{{"t0", 999.0}, {"text", QStringLiteral("nonexistent")}});
        QVERIFY(!controller.setSentenceTexts(audioUrl, edits, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(readAllBytes(dir.path() + "/meeting.wav.md"), original);
    }

    // A newline embedded in the new text is collapsed to a space rather than
    // corrupting the one-line-per-segment file format (QML already blocks
    // Return/strips pasted newlines before ever calling this, but the C++
    // side is the actual safety boundary — see setSentenceTexts' doc
    // comment).
    void setSentenceTexts_collapsesEmbeddedNewline()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/meeting.wav");

        TranscriptionController controller;
        QString error;
        QVariantList edits;
        edits.append(QVariantMap{{"t0", 0.0}, {"text", QStringLiteral("hello\nworld")}});
        QVERIFY(controller.setSentenceTexts(audioUrl, edits, &error));

        // kFixtureMd's body is the blank separator line + 2 segment lines
        // (3 newlines total) — an embedded newline in the new text must
        // collapse to a space, not introduce a 4th line.
        const QString body = controller.readTranscriptBody(audioUrl);
        QCOMPARE(body.count(QLatin1Char('\n')), 3);
        QVERIFY(body.contains(QStringLiteral("hello world")));
    }

    // Disallows setting two speakers to the same (case-insensitive) name —
    // the file is left completely untouched on rejection.
    void renameSpeaker_rejectsDuplicateName()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "interview.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\n"
                                    "speakerDetection: true\nspeakers: Speaker 1, Speaker 2\n---\n\n"
                                    "[0] [Speaker 1] hi\n"
                                    "[3.5] [Speaker 2] hello\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/interview.wav");
        const QString original = readAllBytes(dir.path() + "/interview.wav.md");

        TranscriptionController controller;
        QSignalSpy spy(&controller, &TranscriptionController::speakerRenamed);
        // Case-insensitive match against Speaker 1's own tag.
        controller.renameSpeaker(audioUrl, QStringLiteral("Speaker 2"), QStringLiteral("speaker 1"));
        QVERIFY(spy.wait(5000));
        const QList<QVariant> args = spy.takeFirst();
        QVERIFY(!args.at(2).toBool());
        QVERIFY2(args.at(3).toString().contains(QStringLiteral("already exists")), qPrintable(args.at(3).toString()));
        QCOMPARE(readAllBytes(dir.path() + "/interview.wav.md"), original);
    }

    // mergeSpeakers() collapses an over-split diarized speaker back into a
    // real one: every "[sourceName]" body tag becomes "[targetName]",
    // target's own lines are untouched, and sourceName drops out of the
    // frontmatter "speakers:" allowlist (targetName stays, unchanged).
    void mergeSpeakers_relabelsSourceTagsToTarget()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "interview.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\n"
                                    "speakerDetection: true\nspeakers: Speaker 1, Speaker 2, Speaker 3\n---\n\n"
                                    "[0] [Speaker 1] hi there\n"
                                    "[3.5] [Speaker 3] hello back\n"
                                    "[7] [Speaker 1] how are you\n"
                                    "[10] [Speaker 2] I'm good thanks\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/interview.wav");

        TranscriptionController controller;
        QSignalSpy spy(&controller, &TranscriptionController::speakersMerged);
        controller.mergeSpeakers(audioUrl, QStringLiteral("Speaker 3"), QStringLiteral("Speaker 1"));
        QVERIFY(spy.wait(5000));
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(0).toUrl(), audioUrl);
        QCOMPARE(args.at(1).toString(), QStringLiteral("Speaker 3"));
        QCOMPARE(args.at(2).toString(), QStringLiteral("Speaker 1"));
        QVERIFY2(args.at(3).toBool(), qPrintable(args.at(4).toString()));

        const QString body = controller.readTranscriptBody(audioUrl);
        QVERIFY(body.contains(QStringLiteral("[Speaker 1] hi there")));
        QVERIFY(body.contains(QStringLiteral("[Speaker 1] hello back"))); // was Speaker 3
        QVERIFY(body.contains(QStringLiteral("[Speaker 1] how are you")));
        QVERIFY(body.contains(QStringLiteral("[Speaker 2] I'm good thanks"))); // untouched
        QVERIFY(!body.contains(QStringLiteral("Speaker 3")));
        QCOMPARE(controller.readMetadata(audioUrl).value("speakers").toString(),
                 QStringLiteral("Speaker 1, Speaker 2"));
    }

    // Dragging a pill onto itself is a no-op — succeeds trivially, file
    // untouched.
    void mergeSpeakers_noopWhenSourceEqualsTarget()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "interview.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\n"
                                    "speakerDetection: true\nspeakers: Speaker 1, Speaker 2\n---\n\n"
                                    "[0] [Speaker 1] hi\n"
                                    "[3.5] [Speaker 2] hello\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/interview.wav");
        const QString original = readAllBytes(dir.path() + "/interview.wav.md");

        TranscriptionController controller;
        QSignalSpy spy(&controller, &TranscriptionController::speakersMerged);
        controller.mergeSpeakers(audioUrl, QStringLiteral("Speaker 1"), QStringLiteral("Speaker 1"));
        QVERIFY(spy.wait(5000));
        QVERIFY(spy.takeFirst().at(3).toBool());
        QCOMPARE(readAllBytes(dir.path() + "/interview.wav.md"), original);
    }

    // Rejects (ok=false) if either the source or the target name isn't an
    // existing distinct tag in the body — file left untouched.
    void mergeSpeakers_failsWhenEitherNameMissing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "interview.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\n"
                                    "speakerDetection: true\nspeakers: Speaker 1, Speaker 2\n---\n\n"
                                    "[0] [Speaker 1] hi\n"
                                    "[3.5] [Speaker 2] hello\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/interview.wav");
        const QString original = readAllBytes(dir.path() + "/interview.wav.md");

        TranscriptionController controller;
        {
            QSignalSpy spy(&controller, &TranscriptionController::speakersMerged);
            controller.mergeSpeakers(audioUrl, QStringLiteral("Speaker 9"), QStringLiteral("Speaker 1"));
            QVERIFY(spy.wait(5000));
            const QList<QVariant> args = spy.takeFirst();
            QVERIFY(!args.at(3).toBool());
            QVERIFY2(args.at(4).toString().contains(QStringLiteral("not found")), qPrintable(args.at(4).toString()));
        }
        {
            QSignalSpy spy(&controller, &TranscriptionController::speakersMerged);
            controller.mergeSpeakers(audioUrl, QStringLiteral("Speaker 1"), QStringLiteral("Speaker 9"));
            QVERIFY(spy.wait(5000));
            const QList<QVariant> args = spy.takeFirst();
            QVERIFY(!args.at(3).toBool());
            QVERIFY2(args.at(4).toString().contains(QStringLiteral("not found")), qPrintable(args.at(4).toString()));
        }
        QCOMPARE(readAllBytes(dir.path() + "/interview.wav.md"), original);
    }

    // matchesExistingTranscription() lets "transcribe all" skip files whose
    // stored settings already match, while a manual single-file re-run
    // (transcribe() itself) never consults this — see the Q_INVOKABLE's doc
    // comment. False for an untranscribed file: nothing to match yet.
    void matchesExistingTranscription_falseWhenUntranscribed()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        TranscriptionController controller;
        QVERIFY(!controller.matchesExistingTranscription(QUrl::fromLocalFile(dir.path() + "/nothing.wav"),
                                                           QStringLiteral("base.en"), false));
    }

    // Same modelId (mapped to the same ggml file) and same speakerDetection
    // (false) as what's stored -> matches; a DIFFERENT modelId -> doesn't.
    void matchesExistingTranscription_comparesModelAndSpeakerDetection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n\n"
                                    "[00:00:00.000 - 00:00:01.000] hi\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/meeting.wav");
        TranscriptionController controller;

        QVERIFY(controller.matchesExistingTranscription(audioUrl, QStringLiteral("base.en"), false));
        QVERIFY(!controller.matchesExistingTranscription(audioUrl, QStringLiteral("small.en"), false));
        // Same modelId, but speakerDetection differs from what's stored.
        QVERIFY(!controller.matchesExistingTranscription(audioUrl, QStringLiteral("base.en"), true));
    }

    // Speaker diarization is a separate pass, independent of which whisper
    // model is selected (see DESIGN.md "Speaker diarization" — unlike the
    // removed tdrz override, it no longer forces a specific model file). So
    // a DIFFERENT modelId genuinely means "differs" even when
    // speakerDetection matches on both sides.
    void matchesExistingTranscription_speakerDetectionNoLongerOverridesModelId()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "interview.wav.md",
                     QStringLiteral("---\nmodel: ggml-tiny.en.bin\nspeakerDetection: true\n"
                                    "speakers: Speaker 1\n---\n\n"
                                    "[00:00:00.000 - 00:00:01.000] [Speaker 1] hi\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/interview.wav");
        TranscriptionController controller;

        QVERIFY(controller.matchesExistingTranscription(audioUrl, QStringLiteral("tiny.en"), true));
        QVERIFY(!controller.matchesExistingTranscription(audioUrl, QStringLiteral("medium.en"), true));
    }

    // "no separate JSON sidecar" — the one .md is the entire durable record.
    // After a typical read/label workflow the audio file's directory must
    // contain no *.json / *.jsonl artifacts. (The planned resume state file
    // `.name.partial.jsonl` is a future milestone; nothing ships today.)
    void noJsonSidecar_afterTypicalOperations()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.wav.md", kFixtureMd);
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/meeting.wav");

        TranscriptionController controller;
        controller.readMetadata(audioUrl);
        controller.readTranscriptBody(audioUrl);
        QVERIFY(controller.setLabel(audioUrl, QStringLiteral("Labeled"), nullptr));

        const QDir d(dir.path());
        const QStringList jsonArtifacts =
            d.entryList({QStringLiteral("*.json"), QStringLiteral("*.jsonl")}, QDir::Files | QDir::Hidden);
        QVERIFY2(jsonArtifacts.isEmpty(), qPrintable(jsonArtifacts.join(", ")));
        // The .md is still the only transcript artifact.
        QVERIFY(d.exists(QStringLiteral("meeting.wav.md")));
    }

    // The WRITE side (writeFrontmatter/appendSegmentLine/formatTimestamp)
    // lives in TranscriptionController.cpp's anonymous namespace and is only
    // reachable through a real whisper run — gated below since it needs a
    // real ggml model and ffmpeg.
    void writtenFileFormat_endToEnd()
    {
        const QString modelPath = locateRealModel();
        if (modelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample (DESIGN.md: ffmpeg decodes, Qt only plays)");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        // Plant the real model at the FALLBACK's target name (kDefaultModelId
        // == "base.en" -> "ggml-base.en.bin") so ensureModel's substitution
        // resolves to a model that actually loads, without ever downloading.
        const QString plantedPath = modelsDir + "/ggml-base.en.bin";
        QVERIFY(QFile::copy(modelPath, plantedPath));

        const QString samplePath = vault.path() + "/jfk.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        // Plant whisper.cpp's own vendored for-tests VAD model (real,
        // full-size — whisper.cpp's own test suite uses it too, see
        // third_party/whisper.cpp/tests/CMakeLists.txt's VAD_MODEL_PATH) at
        // ensureVadModel()'s expected filename, so this test's VAD gating
        // (this feature) never needs a live network download — same
        // plant-don't-download precedent as the whisper model above.
        const QString plantedVadPath = modelsDir + "/" + ModelManager::kVadModelFileName;
        QVERIFY(QFile::copy(
            QStringLiteral(SUOP_REPO_DIR "/third_party/whisper.cpp/models/for-tests-silero-v6.2.0-ggml.bin"),
            plantedVadPath));

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        const QUrl audioUrl = QUrl::fromLocalFile(samplePath);
        // "no-such-model-id" is not in ModelManager::availableModels() —
        // this is the exact unknown-modelId case tst_modelmanager unit-tests
        // at the ModelManager level; here we confirm the fallback is
        // recorded where the user actually looks: the transcript itself.
        controller.transcribe(audioUrl, QUrl::fromLocalFile(vault.path()), QStringLiteral("no-such-model-id"), false);

        QVERIFY(finishedSpy.wait(30000));
        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY2(finishedSpy.at(0).at(1).toBool(), qPrintable(finishedSpy.at(0).at(2).toString()));

        const QVariantMap meta = controller.readMetadata(audioUrl);
        QCOMPARE(meta.value("model").toString(), QStringLiteral("ggml-base.en.bin"));
        QVERIFY2(meta.value("modelFallback").toBool(),
                 "an unrecognized modelId must show up as modelFallback: true in the transcript itself, "
                 "not just in a log line");
        QCOMPARE(meta.value("modelRequested").toString(), QStringLiteral("no-such-model-id"));

        // The REAL write path (appendSegmentLine/formatTimestamp) must
        // produce the compact, zero-trimmed timestamp — not the old
        // zero-padded "[00:00:00.000]" shape. The very first segment starts
        // near the beginning of the file, so its line should read just "[0]"
        // or "[0.xxx]", never "[00:00:00...".
        const QString body = controller.readTranscriptBody(audioUrl);
        QVERIFY2(!body.isEmpty(), "expected at least one segment line from a real transcription");
        QVERIFY2(!body.contains(QStringLiteral("[00:")),
                 qPrintable(QStringLiteral("segment line still zero-padded, expected compact format:\n") + body));
        // Not an exact 0.0: with VAD gating active (this feature — see
        // VadChunker.h/WhisperCppEngine::setVadModelPath()), whisper.cpp
        // correctly excludes jfk.wav's genuine ~0.3s of leading silence
        // before "We choose..." starts, so the first segment's t0 reflects
        // real speech onset rather than an ungated "starts at 0 regardless
        // of silence" artifact. Still asserts "near the start of the file",
        // which is the property this check actually cares about.
        const QVariantList sentences = controller.parseSentences(audioUrl);
        QVERIFY(!sentences.isEmpty());
        QVERIFY2(sentences.first().toMap().value("t0").toDouble() < 2.0,
                 "first segment should start near the beginning of the file");
    }

    // Real end-to-end run of the FULL speaker-detection wiring (not just
    // DiarizationEngine in isolation, see tst_diarizationengine.cpp): a real
    // whisper transcription with speakerDetection=true must produce a
    // "speakers:" frontmatter list, real "[Speaker N]" tags on segment
    // lines (assigned via TranscriptionController's time-overlap lookup),
    // and speakerDetection: true with NO speakerDetectionError — the
    // graceful-degradation path is exercised separately below.
    void speakerDetection_endToEnd_producesRealSpeakerLabels()
    {
        const QString modelPath = locateRealModel();
        if (modelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        const QString segModel = locateRealDiarizationModel("SUOP_TEST_DIARIZATION_SEG_MODEL");
        const QString embedModel = locateRealDiarizationModel("SUOP_TEST_DIARIZATION_EMBED_MODEL");
        if (segModel.isEmpty() || embedModel.isEmpty())
            QSKIP("no real diarization models available; set SUOP_TEST_DIARIZATION_SEG_MODEL and "
                  "SUOP_TEST_DIARIZATION_EMBED_MODEL to real .onnx files to run this test");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample (DESIGN.md: ffmpeg decodes, Qt only plays)");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(modelPath, modelsDir + "/ggml-base.en.bin"));
        // Planted at ModelManager's exact expected local names (see
        // ModelManager::kDiarizationSegmentationFileName/
        // kDiarizationEmbeddingFileName) so ensureDiarizationModels() finds
        // them already "downloaded" — no network touch, same strategy as
        // every other gated real-model test in this file.
        QVERIFY(QFile::copy(segModel, modelsDir + "/pyannote-segmentation-3.0-int8.onnx"));
        QVERIFY(QFile::copy(embedModel, modelsDir + "/wespeaker-resnet293-lm.onnx"));

        const QString samplePath = vault.path() + "/jfk.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        const QUrl audioUrl = QUrl::fromLocalFile(samplePath);
        controller.transcribe(audioUrl, QUrl::fromLocalFile(vault.path()), QStringLiteral("base.en"),
                               /*speakerDetection=*/true);

        QVERIFY(finishedSpy.wait(30000));
        QVERIFY2(finishedSpy.at(0).at(1).toBool(), qPrintable(finishedSpy.at(0).at(2).toString()));

        const QVariantMap meta = controller.readMetadata(audioUrl);
        QCOMPARE(meta.value("speakerDetection"), QVariant(true));
        QVERIFY2(meta.value("speakerDetectionError").toString().isEmpty(),
                 qPrintable(meta.value("speakerDetectionError").toString()));
        const QString speakers = meta.value("speakers").toString();
        QVERIFY2(!speakers.isEmpty(), "expected a non-empty speakers: list in frontmatter");
        QVERIFY2(speakers.contains(QStringLiteral("Speaker 1")), qPrintable(speakers));

        const QVariantList sentences = controller.parseSentences(audioUrl);
        QVERIFY(!sentences.isEmpty());
        bool anySpeakerTagged = false;
        for (const QVariant &v : sentences) {
            if (!v.toMap().value("speaker").toString().isEmpty())
                anySpeakerTagged = true;
        }
        QVERIFY2(anySpeakerTagged, "expected at least one sentence with a real [Speaker N] tag");
    }

    // Graceful degradation (DESIGN.md decision): when speaker detection is
    // requested but the diarization models are present-but-unloadable
    // (corrupt/incompatible file — the deterministic, network-independent
    // way to exercise this: plant EMPTY files at the exact expected paths,
    // so ensureDiarizationModels() finds them "already downloaded" but
    // DiarizationEngine's Create() fails to load them as real ONNX), ASR
    // must still complete successfully — speakerDetection reads false and
    // speakerDetectionError explains why, rather than failing the whole job.
    void speakerDetection_modelsUnavailable_degradesGracefully()
    {
        const QString modelPath = locateRealModel();
        if (modelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample (DESIGN.md: ffmpeg decodes, Qt only plays)");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(modelPath, modelsDir + "/ggml-base.en.bin"));
        // Fake (empty, unloadable) diarization models — see comment above:
        // this makes ensureDiarizationModels() skip its (network-dependent)
        // download entirely, while DiarizationEngine still fails to load
        // them, deterministically exercising the degrade-gracefully path
        // without depending on network being unavailable in this environment.
        QFile fakeSeg(modelsDir + "/pyannote-segmentation-3.0-int8.onnx");
        QVERIFY(fakeSeg.open(QIODevice::WriteOnly));
        fakeSeg.close();
        QFile fakeEmbed(modelsDir + "/wespeaker-resnet293-lm.onnx");
        QVERIFY(fakeEmbed.open(QIODevice::WriteOnly));
        fakeEmbed.close();

        const QString samplePath = vault.path() + "/jfk.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        const QUrl audioUrl = QUrl::fromLocalFile(samplePath);
        controller.transcribe(audioUrl, QUrl::fromLocalFile(vault.path()), QStringLiteral("base.en"),
                               /*speakerDetection=*/true);

        QVERIFY(finishedSpy.wait(30000));
        // ASR itself must still succeed — a diarization failure never blocks
        // the transcription.
        QVERIFY2(finishedSpy.at(0).at(1).toBool(), qPrintable(finishedSpy.at(0).at(2).toString()));

        const QVariantMap meta = controller.readMetadata(audioUrl);
        QCOMPARE(meta.value("speakerDetection"), QVariant(false));
        QVERIFY2(!meta.value("speakerDetectionError").toString().isEmpty(),
                 "expected a non-empty speakerDetectionError explaining why detection didn't run");
        QVERIFY(!controller.readTranscriptBody(audioUrl).isEmpty());
    }

    // Full auto-labeling wiring, end to end: transcribe(..., autoLabel=true)
    // must produce a real whisper transcript AND label/tags in frontmatter
    // plus a prelude in the body, written by the second pass after
    // transcription (see TranscriptionController.cpp's
    // writeLabelingFields()). Gated on BOTH real models since it exercises
    // the full pipeline.
    void autoLabel_addsLabelPreludeTagsToTranscript()
    {
        const QString whisperModelPath = locateRealModel();
        if (whisperModelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        const QString labelModelPath = locateRealLabelModel();
        if (labelModelPath.isEmpty())
            QSKIP("no real label GGUF model available; set SUOP_TEST_LABEL_MODEL to run this test");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample (DESIGN.md: ffmpeg decodes, Qt only plays)");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(whisperModelPath, modelsDir + "/ggml-base.en.bin"));
        QVERIFY(QFile::copy(labelModelPath, modelsDir + "/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"));

        const QString samplePath = vault.path() + "/jfk.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        const QUrl audioUrl = QUrl::fromLocalFile(samplePath);
        controller.transcribe(audioUrl, QUrl::fromLocalFile(vault.path()), QStringLiteral("base.en"), false,
                               /*autoLabel=*/true);

        // Both the transcribe AND its follow-up label job appear immediately
        // (not one appended later once transcription finishes) — see
        // transcribe()'s autoLabel doc comment.
        QCOMPARE(controller.tasksTotal(), 2);
        QCOMPARE(controller.jobs().at(1).toMap().value("kind").toString(), QStringLiteral("relabel"));
        QCOMPARE(controller.jobs().at(1).toMap().value("status").toString(), QStringLiteral("queued"));

        // Generous timeout: this run loads TWO models (whisper + a ~2.5GB
        // LLM) and runs a real generation pass, not just transcription.
        QVERIFY(finishedSpy.wait(60000));
        QVERIFY2(finishedSpy.at(0).at(1).toBool(), qPrintable(finishedSpy.at(0).at(2).toString()));

        // The primary transcript must still be correct — auto-labeling is a
        // bonus pass, never a substitute for it.
        const QString body = controller.readTranscriptBody(audioUrl);
        QVERIFY2(body.toLower().contains(QStringLiteral("country")), qPrintable(body));

        // The follow-up label job (already queued above, before transcription
        // even started) only actually RUNS once this transcribe job's own
        // worker call completes (single worker thread) — waits for its own,
        // second finished() signal here.
        QVERIFY(finishedSpy.count() >= 2 || finishedSpy.wait(60000));
        QCOMPARE(finishedSpy.count(), 2);
        QVERIFY2(finishedSpy.at(1).at(1).toBool(), qPrintable(finishedSpy.at(1).at(2).toString()));
        QCOMPARE(controller.jobs().at(1).toMap().value("status").toString(), QStringLiteral("done"));

        const QVariantMap meta = controller.readMetadata(audioUrl);
        QVERIFY2(!meta.value("label").toString().isEmpty(), "auto-label must populate label");
        QVERIFY2(!controller.readPrelude(audioUrl).isEmpty(), "auto-label must populate prelude");
        QVERIFY2(!meta.value("tags").toString().isEmpty(), "auto-label must populate tags");
    }

    // Item 9: "regenerate auto-title" — relabel() re-runs ONLY the labeling
    // pass on an already-transcribed file (no re-transcription) and, unlike
    // a fresh transcribe's autoLabel pass (which only ever appends, since
    // those keys can't already exist), must REPLACE existing label/prelude/
    // tags in place rather than duplicating them.
    void relabel_regeneratesWithoutRetranscribing_andReplacesNotDuplicates()
    {
        const QString whisperModelPath = locateRealModel();
        if (whisperModelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        const QString labelModelPath = locateRealLabelModel();
        if (labelModelPath.isEmpty())
            QSKIP("no real label GGUF model available; set SUOP_TEST_LABEL_MODEL to run this test");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample (DESIGN.md: ffmpeg decodes, Qt only plays)");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(whisperModelPath, modelsDir + "/ggml-base.en.bin"));
        QVERIFY(QFile::copy(labelModelPath, modelsDir + "/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"));

        const QString samplePath = vault.path() + "/jfk.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));
        const QString mdPath = vault.path() + "/jfk.wav.md";

        TranscriptionController controller;
        const QUrl audioUrl = QUrl::fromLocalFile(samplePath);
        const QUrl vaultUrl = QUrl::fromLocalFile(vault.path());

        // Plain transcribe first — NO autoLabel — so label/prelude/tags
        // don't exist yet.
        {
            QSignalSpy spy(&controller, &TranscriptionController::finished);
            controller.transcribe(audioUrl, vaultUrl, QStringLiteral("base.en"), false, /*autoLabel=*/false);
            QVERIFY(spy.wait(30000));
            QVERIFY2(spy.at(0).at(1).toBool(), qPrintable(spy.at(0).at(2).toString()));
        }
        QVERIFY(controller.readMetadata(audioUrl).value("label").toString().isEmpty());

        // relabel() must not require re-transcription — assert the segment
        // lines are untouched before/after by comparing the transcript body.
        const QString bodyBeforeRelabel = controller.readTranscriptBody(audioUrl);

        {
            QSignalSpy spy(&controller, &TranscriptionController::finished);
            controller.relabel(audioUrl, vaultUrl);
            QVERIFY(spy.wait(30000));
            QVERIFY2(spy.at(0).at(1).toBool(), qPrintable(spy.at(0).at(2).toString()));
        }
        QCOMPARE(controller.readTranscriptBody(audioUrl), bodyBeforeRelabel);
        const QString firstLabel = controller.readMetadata(audioUrl).value("label").toString();
        QVERIFY2(!firstLabel.isEmpty(), "relabel() must populate label from the existing transcript");

        // Regenerating again must REPLACE, not duplicate, the frontmatter
        // keys — read the raw file and assert each key appears exactly once.
        {
            QSignalSpy spy(&controller, &TranscriptionController::finished);
            controller.relabel(audioUrl, vaultUrl);
            QVERIFY(spy.wait(30000));
            QVERIFY2(spy.at(0).at(1).toBool(), qPrintable(spy.at(0).at(2).toString()));
        }
        const QString raw = readAllBytes(mdPath);
        QCOMPARE(raw.count(QStringLiteral("\nlabel:")), 1);
        QCOMPARE(raw.count(QStringLiteral("\ntags:")), 1);
        QCOMPARE(raw.count(QStringLiteral("<!-- prelude end -->")), 1);
        QCOMPARE(controller.readTranscriptBody(audioUrl), bodyBeforeRelabel);
    }

    // Item 14 "auto-categorize" mode, full pipeline: with restrictTagsToExisting,
    // relabel() must scan the REST OF THE VAULT for existing tags (not just
    // the file being labeled — it has none of its own yet) and constrain
    // generation to that vocabulary. Exercises the actual vault-scanning
    // code path (collectVaultTags), not just LabelingEngine's own prompt
    // handling (already covered directly in tst_labelingengine.cpp).
    void relabel_restrictTagsToExisting_scansWholeVaultVocabulary()
    {
        const QString whisperModelPath = locateRealModel();
        if (whisperModelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        const QString labelModelPath = locateRealLabelModel();
        if (labelModelPath.isEmpty())
            QSKIP("no real label GGUF model available; set SUOP_TEST_LABEL_MODEL to run this test");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample (DESIGN.md: ffmpeg decodes, Qt only plays)");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(whisperModelPath, modelsDir + "/ggml-base.en.bin"));
        QVERIFY(QFile::copy(labelModelPath, modelsDir + "/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"));

        const QString samplePath = vault.path() + "/jfk.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        // A sibling file elsewhere in the vault whose ONLY purpose is to
        // supply the existing tag vocabulary — jfk.wav itself starts with
        // no tags of its own. "politics"/"history" are plausible for JFK's
        // inauguration-speech clip; "cooking" is an implausible decoy, so a
        // non-empty result confirms the model picked meaningfully rather
        // than echoing everything it was given.
        QVERIFY(QDir().mkpath(vault.path() + "/other"));
        writeFixture(vault.path() + "/other", QStringLiteral("unrelated.md"),
                     QStringLiteral("---\n"
                                    "model: ggml-base.en.bin\n"
                                    "speakerDetection: false\n"
                                    "transcribedAt: 2026-07-06T10:12:13\n"
                                    "tags: politics, history, cooking\n"
                                    "---\n"
                                    "\n"
                                    "[00:00:00.000 - 00:00:01.000] unrelated placeholder content\n"));

        TranscriptionController controller;
        const QUrl audioUrl = QUrl::fromLocalFile(samplePath);
        const QUrl vaultUrl = QUrl::fromLocalFile(vault.path());

        {
            QSignalSpy spy(&controller, &TranscriptionController::finished);
            controller.transcribe(audioUrl, vaultUrl, QStringLiteral("base.en"), false, /*autoLabel=*/false);
            QVERIFY(spy.wait(30000));
            QVERIFY2(spy.at(0).at(1).toBool(), qPrintable(spy.at(0).at(2).toString()));
        }

        {
            QSignalSpy spy(&controller, &TranscriptionController::finished);
            controller.relabel(audioUrl, vaultUrl, /*restrictTagsToExisting=*/true);
            QVERIFY(spy.wait(30000));
            QVERIFY2(spy.at(0).at(1).toBool(), qPrintable(spy.at(0).at(2).toString()));
        }

        const QVariantMap meta = controller.readMetadata(audioUrl);
        QVERIFY2(!meta.value("label").toString().isEmpty(), "relabel must still populate label/prelude as usual");
        const QString tags = meta.value("tags").toString();
        const QStringList allowed = {QStringLiteral("politics"), QStringLiteral("history"), QStringLiteral("cooking")};
        for (const QString &tag : tags.split(QStringLiteral(","), Qt::SkipEmptyParts)) {
            const QString trimmed = tag.trimmed();
            QVERIFY2(allowed.contains(trimmed, Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("tag \"%1\" wasn't in the vault-scanned vocabulary "
                                                "(from other/unrelated.md's tags: politics, history, cooking)")
                                    .arg(trimmed)));
        }
    }
};

QTEST_GUILESS_MAIN(TstTranscriptFormat)
#include "tst_transcript_format.moc"
