// Spec under test — CLAUDE.md load-bearing constraint (vault layout):
//
//   "Sibling .md = transcribed. All transcription data lives in this one
//    human-readable file — YAML frontmatter (model, speaker detection,
//    transcribed-at) plus one `[start - end] text` line per segment; no
//    separate JSON sidecar."
//
// The readers/writers live in TranscriptionController: readMetadata(),
// readTranscriptBody() and setLabel() all take the AUDIO file's QUrl and
// derive the sibling .md as <dir>/<completeBaseName>.md
// (TranscriptionController.cpp, sidecarPath()). These tests hand-write
// fixture .md files in the exact format writeFrontmatter()/appendSegmentLine()
// produce and assert the read side honours it.

#include <QtTest>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

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

}

class TstTranscriptFormat : public QObject
{
    Q_OBJECT

private slots:
    // Frontmatter carries exactly the documented keys: model,
    // speakerDetection, transcribedAt. Booleans come back as real bools
    // (readMetadata maps the literal strings "true"/"false"), everything
    // else as trimmed strings.
    void readMetadata_parsesDocumentedFrontmatterKeys()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(!writeFixture(dir.path(), "meeting.md", kFixtureMd).isEmpty());

        TranscriptionController controller;
        // Callers pass the AUDIO url; the sibling .md is derived. The audio
        // file itself doesn't need to exist for the read path.
        const QVariantMap meta = controller.readMetadata(QUrl::fromLocalFile(dir.path() + "/meeting.wav"));

        QCOMPARE(meta.value("model").toString(), QStringLiteral("ggml-base.en.bin"));
        QCOMPARE(meta.value("speakerDetection"), QVariant(true));
        QCOMPARE(meta.value("transcribedAt").toString(), QStringLiteral("2026-07-06T10:12:13"));
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
        writeFixture(dir.path(), "meeting.md", kFixtureMd);

        TranscriptionController controller;
        const QString body = controller.readTranscriptBody(QUrl::fromLocalFile(dir.path() + "/meeting.wav"));
        QCOMPARE(body,
                 QStringLiteral("\n"
                                "[00:00:00.000 - 00:00:11.000] hello world\n"
                                "[00:00:11.000 - 00:00:14.500] And another speaker. [SPEAKER_TURN]\n"));

        // No sidecar / no frontmatter block => empty string ("untranscribed
        // or unreadable" per the header doc), never a partial read.
        QCOMPARE(controller.readTranscriptBody(QUrl::fromLocalFile(dir.path() + "/nothing.wav")), QString());
        writeFixture(dir.path(), "plain.md", QStringLiteral("just prose, no frontmatter\n"));
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
        writeFixture(dir.path(), "meeting.md", kFixtureMd);

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
        writeFixture(dir.path(), "meeting.md", kFixtureMd);

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

        // Legacy fixture (no leading "[Speaker N]" tag) parses with speaker=0
        // for every sentence — old already-transcribed files must not crash
        // or misparse under the new (Item 11, Wave E) regex.
        QCOMPARE(s0.value("speaker").toInt(), 0);
        QCOMPARE(s1.value("speaker").toInt(), 0);
    }

    // Item 11 (Wave E): a leading "[Speaker N]" tag (written when speaker
    // detection is on) is parsed into each sentence's "speaker" field and
    // stripped from the parsed text, mirroring the legacy trailing
    // "[SPEAKER_TURN]" marker's text-stripping behavior above.
    void parseSentences_recoversSpeakerNumberFromLeadingTag()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "interview.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\nspeakerDetection: true\n---\n\n"
                                    "[00:00:00.000 - 00:00:05.000] [Speaker 1] Welcome to the show.\n"
                                    "[00:00:05.000 - 00:00:09.000] [Speaker 2] Thanks for having me.\n"
                                    "[00:00:09.000 - 00:00:12.000] [Speaker 1] So, tell us about yourself.\n"));

        TranscriptionController controller;
        const QVariantList sentences =
            controller.parseSentences(QUrl::fromLocalFile(dir.path() + "/interview.wav"));

        QCOMPARE(sentences.size(), 3);
        QCOMPARE(sentences[0].toMap().value("speaker").toInt(), 1);
        QCOMPARE(sentences[0].toMap().value("text").toString(), QStringLiteral("Welcome to the show."));
        QCOMPARE(sentences[1].toMap().value("speaker").toInt(), 2);
        QCOMPARE(sentences[1].toMap().value("text").toString(), QStringLiteral("Thanks for having me."));
        QCOMPARE(sentences[2].toMap().value("speaker").toInt(), 1);

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
        writeFixture(dir.path(), "notes.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n\n"
                                    "[00:00:00.000] First point.\n"
                                    "[00:00:04.250] Second point.\n"));

        TranscriptionController controller;
        const QVariantList sentences = controller.parseSentences(QUrl::fromLocalFile(dir.path() + "/notes.wav"));

        QCOMPARE(sentences.size(), 2);
        QCOMPARE(sentences[0].toMap().value("t0").toDouble(), 0.0);
        QCOMPARE(sentences[0].toMap().value("speaker").toInt(), 0);
        QCOMPARE(sentences[0].toMap().value("text").toString(), QStringLiteral("First point."));
        QCOMPARE(sentences[1].toMap().value("t0").toDouble(), 4.25);
        QCOMPARE(sentences[1].toMap().value("text").toString(), QStringLiteral("Second point."));
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
        const QString mdPath = writeFixture(dir.path(), "meeting.md", kFixtureMd);
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
        const QString barePath = writeFixture(dir.path(), "bare.md", original);
        error.clear();
        QVERIFY(!controller.setLabel(QUrl::fromLocalFile(dir.path() + "/bare.wav"), QStringLiteral("x"), &error));
        QVERIFY2(error.contains(QStringLiteral("no frontmatter")), qPrintable(error));
        QCOMPARE(readAllBytes(barePath), original);
    }

    // Item 11 (Wave E): renameSpeaker() adds a speakerNames key (first
    // rename), then updates just that one id=name pair on a second rename to
    // a DIFFERENT speaker, leaving the first speaker's mapping untouched —
    // and the value round-trips through readMetadata as a flat string (same
    // convention as `tags`).
    void renameSpeaker_addsThenUpdatesOnePair_preservingOthers()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "interview.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\n"
                                    "speakerDetection: true\n---\n\n"
                                    "[00:00:00.000 - 00:00:05.000] [Speaker 1] hi\n"
                                    "[00:00:05.000 - 00:00:09.000] [Speaker 2] hello\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/interview.wav");

        TranscriptionController controller;
        QString error;
        QVERIFY2(controller.renameSpeaker(audioUrl, QStringLiteral("Speaker 1"), QStringLiteral("Alice"), &error),
                  qPrintable(error));
        QCOMPARE(controller.readMetadata(audioUrl).value("speakerNames").toString(), QStringLiteral("Speaker 1=Alice"));

        // Renaming a SECOND speaker appends its own pair, leaving the first.
        QVERIFY2(controller.renameSpeaker(audioUrl, QStringLiteral("Speaker 2"), QStringLiteral("Bob"), &error),
                  qPrintable(error));
        const QString afterSecond = controller.readMetadata(audioUrl).value("speakerNames").toString();
        QVERIFY(afterSecond.contains(QStringLiteral("Speaker 1=Alice")));
        QVERIFY(afterSecond.contains(QStringLiteral("Speaker 2=Bob")));

        // Re-renaming Speaker 1 REPLACES its pair, never duplicates, and
        // Speaker 2's mapping survives untouched.
        QVERIFY2(controller.renameSpeaker(audioUrl, QStringLiteral("Speaker 1"), QStringLiteral("Alicia"), &error),
                  qPrintable(error));
        const QString afterRename = controller.readMetadata(audioUrl).value("speakerNames").toString();
        QCOMPARE(afterRename.count(QStringLiteral("Speaker 1=")), 1);
        QVERIFY(afterRename.contains(QStringLiteral("Speaker 1=Alicia")));
        QVERIFY(afterRename.contains(QStringLiteral("Speaker 2=Bob")));
        QVERIFY(!afterRename.contains(QStringLiteral("Alice,")));

        // The transcript body (segment lines with their stable "[Speaker N]"
        // tags) is completely untouched by a rename.
        QVERIFY(controller.readTranscriptBody(audioUrl).contains(QStringLiteral("[Speaker 1] hi")));
        QVERIFY(controller.readTranscriptBody(audioUrl).contains(QStringLiteral("[Speaker 2] hello")));
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
        writeFixture(dir.path(), "meeting.md",
                     QStringLiteral("---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n\n"
                                    "[00:00:00.000 - 00:00:01.000] hi\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/meeting.wav");
        TranscriptionController controller;

        QVERIFY(controller.matchesExistingTranscription(audioUrl, QStringLiteral("base.en"), false));
        QVERIFY(!controller.matchesExistingTranscription(audioUrl, QStringLiteral("small.en"), false));
        // Same modelId, but speakerDetection differs from what's stored.
        QVERIFY(!controller.matchesExistingTranscription(audioUrl, QStringLiteral("base.en"), true));
    }

    // speakerDetection forces the tdrz model regardless of modelId (see
    // ModelManager) — a file transcribed with speaker detection matches a
    // re-request for speaker detection REGARDLESS of which modelId is
    // currently selected (the id would be ignored either way).
    void matchesExistingTranscription_speakerDetectionIgnoresModelId()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "interview.md",
                     QStringLiteral("---\nmodel: ggml-small.en-tdrz.bin\nspeakerDetection: true\n---\n\n"
                                    "[00:00:00.000 - 00:00:01.000] [Speaker 1] hi\n"));
        const QUrl audioUrl = QUrl::fromLocalFile(dir.path() + "/interview.wav");
        TranscriptionController controller;

        QVERIFY(controller.matchesExistingTranscription(audioUrl, QStringLiteral("tiny.en"), true));
        QVERIFY(controller.matchesExistingTranscription(audioUrl, QStringLiteral("medium.en"), true));
        QVERIFY(!controller.matchesExistingTranscription(audioUrl, QStringLiteral("tiny.en"), false));
    }

    // "no separate JSON sidecar" — the one .md is the entire durable record.
    // After a typical read/label workflow the audio file's directory must
    // contain no *.json / *.jsonl artifacts. (The planned resume state file
    // `.name.partial.jsonl` is a future milestone; nothing ships today.)
    void noJsonSidecar_afterTypicalOperations()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFixture(dir.path(), "meeting.md", kFixtureMd);
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
        QVERIFY(d.exists(QStringLiteral("meeting.md")));
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
        // at (or extremely near) 0.0s, so its line should read just "[0]"
        // or "[0.xxx]", never "[00:00:00...".
        const QString body = controller.readTranscriptBody(audioUrl);
        QVERIFY2(!body.isEmpty(), "expected at least one segment line from a real transcription");
        QVERIFY2(!body.contains(QStringLiteral("[00:")),
                 qPrintable(QStringLiteral("segment line still zero-padded, expected compact format:\n") + body));
        const QVariantList sentences = controller.parseSentences(audioUrl);
        QVERIFY(!sentences.isEmpty());
        QCOMPARE(sentences.first().toMap().value("t0").toDouble(), 0.0);
    }

    // Full auto-labeling wiring, end to end: transcribe(..., autoLabel=true)
    // must produce a real whisper transcript AND label/summary/tags in the
    // same frontmatter, written by the second pass after transcription (see
    // TranscriptionController.cpp's writeLabelingFields()). Gated on BOTH
    // real models since it exercises the full pipeline.
    void autoLabel_addsLabelSummaryTagsToFrontmatter()
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

        // Generous timeout: this run loads TWO models (whisper + a ~2.5GB
        // LLM) and runs a real generation pass, not just transcription.
        QVERIFY(finishedSpy.wait(60000));
        QVERIFY2(finishedSpy.at(0).at(1).toBool(), qPrintable(finishedSpy.at(0).at(2).toString()));

        // The primary transcript must still be correct — auto-labeling is a
        // bonus pass, never a substitute for it.
        const QString body = controller.readTranscriptBody(audioUrl);
        QVERIFY2(body.toLower().contains(QStringLiteral("country")), qPrintable(body));

        const QVariantMap meta = controller.readMetadata(audioUrl);
        QVERIFY2(!meta.value("label").toString().isEmpty(), "auto-label must populate label");
        QVERIFY2(!meta.value("summary").toString().isEmpty(), "auto-label must populate summary");
        QVERIFY2(!meta.value("tags").toString().isEmpty(), "auto-label must populate tags");
    }

    // Item 9: "regenerate auto-title" — relabel() re-runs ONLY the labeling
    // pass on an already-transcribed file (no re-transcription) and, unlike
    // a fresh transcribe's autoLabel pass (which only ever appends, since
    // those keys can't already exist), must REPLACE existing label/summary/
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
        const QString mdPath = vault.path() + "/jfk.md";

        TranscriptionController controller;
        const QUrl audioUrl = QUrl::fromLocalFile(samplePath);
        const QUrl vaultUrl = QUrl::fromLocalFile(vault.path());

        // Plain transcribe first — NO autoLabel — so label/summary/tags
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
        QCOMPARE(raw.count(QStringLiteral("\nsummary:")), 1);
        QCOMPARE(raw.count(QStringLiteral("\ntags:")), 1);
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
        QVERIFY2(!meta.value("label").toString().isEmpty(), "relabel must still populate label/summary as usual");
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
