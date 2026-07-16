// Spec under test — split-recording combine (see DESIGN.md's vault
// layout section and TranscriptionController::findCombineGroups()'s doc
// comment): user-authored regex rules matched against vault filenames,
// grouped by their CAPTURED text (not just "the rule matched") AND their
// containing folder (a rule only sees the bare filename, so same-named parts
// in two folders are two groups, never one), ordered by mtime by default — or
// by one designated, numeric-aware "sort differentiator" capture group
// instead, which is then EXCLUDED from the grouping key (it can't be both) —
// with the merged output FILENAME derived from (ruleName, key) alone so it's
// re-derivable even after the original parts are gone, and placed in the
// group's own folder.
//
// detectCombineGroups() is pure over its (rules, audioPaths) inputs (only
// touches disk via QFileInfo stats on the paths given), so these tests never
// need a real vault, model, or worker thread activity — just a temp dir of
// touched files with controlled mtimes.

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "TranscriptionController.h"

namespace {

QVariantMap rule(const QString &name, const QString &pattern, bool enabled = true, int sortGroupIndex = -1)
{
    QVariantMap m;
    m["name"] = name;
    m["pattern"] = pattern;
    m["enabled"] = enabled;
    m["sortGroupIndex"] = sortGroupIndex;
    return m;
}

// Backdated well past findCombineGroups()'s stability window so these don't
// read as "pending" (the recorder might still be writing the newest part) —
// see kCombineStabilityWindowSecs in TranscriptionController.cpp.
QString touchFile(const QString &dir, const QString &name, int secondsAgo)
{
    const QString path = dir + "/" + name;
    QFile f(path);
    const bool opened = f.open(QIODevice::WriteOnly);
    Q_ASSERT(opened);
    f.write("x");
    f.close(); // must close the WRITE handle first — closing after a write
                // re-touches mtime to "now", clobbering any setFileTime()
                // called before close() (confirmed empirically).

    QFile retime(path);
    const bool reopened = retime.open(QIODevice::ReadWrite);
    Q_ASSERT(reopened);
    retime.setFileTime(QDateTime::currentDateTime().addSecs(-secondsAgo), QFileDevice::FileModificationTime);
    retime.close();
    return path;
}

QVariantMap findGroup(const QVariantList &groups, const QString &ruleName)
{
    for (const QVariant &g : groups) {
        if (g.toMap().value("ruleName").toString() == ruleName)
            return g.toMap();
    }
    return {};
}

}

class TstFileCombine : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // The "acme mic 3" example from the design discussion: mic index and
    // timestamp vary freely (uncaptured \d{3}/\d{8}_\d{6}), only the device
    // id (TX02) is captured — three parts sharing that capture group into
    // one "ready" group, ordered oldest-to-newest.
    void capturedGroup_groupsByRealString_orderedByMtime()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString p1 = touchFile(dir.path(), "TX02_MIC008_20260711_011600_orig.wav", 60);
        const QString p2 = touchFile(dir.path(), "TX02_MIC009_20260711_014600_orig.wav", 40);
        const QString p3 = touchFile(dir.path(), "TX02_MIC010_20260711_021600_orig.wav", 20);

        TranscriptionController controller;
        const QVariantList rules = {rule("acme mic 3", "^(TX\\d{2})_MIC\\d{3}_\\d{8}_\\d{6}_orig\\.wav$")};
        const QVariantList groups = controller.detectCombineGroups(rules, {p1, p2, p3});

        const QVariantMap group = findGroup(groups, "acme mic 3");
        QCOMPARE(group.value("key").toString(), QStringLiteral("TX02"));
        QCOMPARE(group.value("status").toString(), QStringLiteral("ready"));
        const QStringList files = group.value("files").toStringList();
        QCOMPARE(files, QStringList({p1, p2, p3}));
        QVERIFY(group.value("outputPath").toString().endsWith(QStringLiteral(".combined.wav")));
    }

    // sortGroupIndex names a SECOND capture group as the sort differentiator
    // instead of a grouping one — group.key must then contain ONLY the
    // device id (group 1), not the part number (group 2), proving the sort
    // group is excluded from the grouping key. Order must come from the
    // NUMERIC part number ("1" < "2" < "10"), not mtime — the files are
    // touched in the OPPOSITE order (oldest = part 10, newest = part 1) so
    // an mtime-ordered result would be provably wrong here.
    void sortDifferentiator_numericOverridesMtimeOrder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString p10 = touchFile(dir.path(), "TX02_PART10.wav", 60);
        const QString p2 = touchFile(dir.path(), "TX02_PART2.wav", 40);
        const QString p1 = touchFile(dir.path(), "TX02_PART1.wav", 20);

        TranscriptionController controller;
        const QVariantList rules = {
            rule("numbered parts", "^(TX\\d{2})_PART(\\d+)\\.wav$", /*enabled=*/true, /*sortGroupIndex=*/2)};
        const QVariantList groups = controller.detectCombineGroups(rules, {p10, p2, p1});

        const QVariantMap group = findGroup(groups, "numbered parts");
        QCOMPARE(group.value("key").toString(), QStringLiteral("TX02"));
        QCOMPARE(group.value("status").toString(), QStringLiteral("ready"));
        QCOMPARE(group.value("files").toStringList(), QStringList({p1, p2, p10}));
    }

    // When the sort differentiator's captured values aren't ALL numeric
    // (here "10" parses but "a"/"b" don't), the WHOLE group falls back to
    // plain string comparison rather than a mixed numeric/lexicographic
    // comparator (which wouldn't be a strict weak ordering). Files are
    // touched in string-sorted order reversed, so an mtime-ordered result
    // would again be provably wrong.
    void sortDifferentiator_fallsBackToStringWhenNotAllNumeric()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString pb = touchFile(dir.path(), "TX02_TAKEb.wav", 60);
        const QString pa = touchFile(dir.path(), "TX02_TAKEa.wav", 40);
        const QString p10 = touchFile(dir.path(), "TX02_TAKE10.wav", 20);

        TranscriptionController controller;
        const QVariantList rules = {
            rule("string takes", "^(TX\\d{2})_TAKE(\\w+)\\.wav$", /*enabled=*/true, /*sortGroupIndex=*/2)};
        const QVariantList groups = controller.detectCombineGroups(rules, {pb, pa, p10});

        const QVariantMap group = findGroup(groups, "string takes");
        QCOMPARE(group.value("files").toStringList(), QStringList({p10, pa, pb}));
    }

    // Both capture groups here are written with the IDENTICAL sub-pattern
    // "(\d+)" — sortGroupIndex=2 must still pick out group 2 specifically
    // (by POSITION, the way QRegularExpression itself numbers captures —
    // entirely unaffected by whether two groups happen to look the same in
    // the pattern text), not group 1. Group 1's captured value ("9") is
    // constant across every file, so this also proves grouping itself
    // still works via group 1 while group 2 is excluded from the key. If
    // the exclusion/sort logic somehow mixed up which group is which, the
    // resulting order would not match the group-2-ascending order asserted
    // here.
    void sortDifferentiator_duplicatePatternTextStillPicksCorrectGroupByPosition()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // mtime order (oldest->newest): 9_20, 9_5, 9_10 — deliberately NOT
        // the same as the expected group-2-ascending order below, so an
        // accidental mtime fallback would also fail this test.
        const QString p20 = touchFile(dir.path(), "9_20.wav", 60);
        const QString p5 = touchFile(dir.path(), "9_5.wav", 40);
        const QString p10 = touchFile(dir.path(), "9_10.wav", 20);

        TranscriptionController controller;
        const QVariantList rules = {rule("duplicate groups", "^(\\d+)_(\\d+)\\.wav$", /*enabled=*/true,
                                          /*sortGroupIndex=*/2)};
        const QVariantList groups = controller.detectCombineGroups(rules, {p20, p5, p10});

        const QVariantMap group = findGroup(groups, "duplicate groups");
        QCOMPARE(group.value("key").toString(), QStringLiteral("9"));
        QCOMPARE(group.value("status").toString(), QStringLiteral("ready"));
        // group 2 (the part after "_") ascending: 5, 10, 20
        QCOMPARE(group.value("files").toStringList(), QStringList({p5, p10, p20}));
    }

    // A rule with zero capture groups has no "real string" to distinguish
    // members by — every match shares one bucket, which is expected regex
    // behavior (per the design discussion), not a bug.
    void noCaptureGroups_groupsEverythingTogether()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString p1 = touchFile(dir.path(), "partA.wav", 60);
        const QString p2 = touchFile(dir.path(), "partB.wav", 30);

        TranscriptionController controller;
        const QVariantList rules = {rule("catch-all", "^part.\\.wav$")};
        const QVariantList groups = controller.detectCombineGroups(rules, {p1, p2});

        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first().toMap().value("key").toString(), QString());
        QCOMPARE(groups.first().toMap().value("status").toString(), QStringLiteral("ready"));
    }

    // Group identity includes the containing folder, so identically-named
    // parts in two folders are two separate groups — never one cross-folder
    // group. A rule only ever sees the bare filename, so without the folder
    // in the bucket key these four files would collapse into one group whose
    // members aren't siblings in the vault tree (which then can't draw a
    // single outline around them, since folder rows sit between them and they
    // render at different indent depths).
    void sameKeyInDifferentFolders_areSeparateGroups()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath("interviews"));
        QVERIFY(QDir(dir.path()).mkpath("lectures"));
        const QString a1 = touchFile(dir.path() + "/interviews", "TX02_PART1.wav", 60);
        const QString a2 = touchFile(dir.path() + "/interviews", "TX02_PART2.wav", 50);
        const QString b1 = touchFile(dir.path() + "/lectures", "TX02_PART1.wav", 40);
        const QString b2 = touchFile(dir.path() + "/lectures", "TX02_PART2.wav", 30);

        TranscriptionController controller;
        const QVariantList rules = {rule("parts", "^(TX\\d{2})_PART\\d+\\.wav$")};
        const QVariantList groups = controller.detectCombineGroups(rules, {a1, a2, b1, b2});

        QCOMPARE(groups.size(), 2);
        // Both groups share (ruleName, key) — only the folder separates them,
        // and outputPath is the deterministic-sort tiebreaker that orders them.
        for (const QVariant &g : groups)
            QCOMPARE(g.toMap().value("key").toString(), QStringLiteral("TX02"));
        QCOMPARE(groups.at(0).toMap().value("files").toStringList(), QStringList({a1, a2}));
        QCOMPARE(groups.at(1).toMap().value("files").toStringList(), QStringList({b1, b2}));

        // Each merged file lands in its OWN group's folder, not in whichever
        // folder held the first-in-combine-order member of a merged blob.
        QCOMPARE(QFileInfo(groups.at(0).toMap().value("outputPath").toString()).absolutePath(),
                 QDir(dir.path() + "/interviews").absolutePath());
        QCOMPARE(QFileInfo(groups.at(1).toMap().value("outputPath").toString()).absolutePath(),
                 QDir(dir.path() + "/lectures").absolutePath());
    }

    // The folder is part of group identity, but it is NOT part of the key or
    // the merged filename — a group in a subfolder keys and names itself
    // exactly as the same group would at the vault root.
    void folderScoping_doesNotLeakIntoKeyOrOutputName()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath("nested/deeper"));
        const QString p1 = touchFile(dir.path() + "/nested/deeper", "TX02_PART1.wav", 60);
        const QString p2 = touchFile(dir.path() + "/nested/deeper", "TX02_PART2.wav", 50);

        TranscriptionController controller;
        const QVariantList rules = {rule("parts", "^(TX\\d{2})_PART\\d+\\.wav$")};
        const QVariantList groups = controller.detectCombineGroups(rules, {p1, p2});

        const QVariantMap group = findGroup(groups, "parts");
        QCOMPARE(group.value("key").toString(), QStringLiteral("TX02"));
        QCOMPARE(group.value("status").toString(), QStringLiteral("ready"));
        QCOMPARE(QFileInfo(group.value("outputPath").toString()).absolutePath(),
                 QDir(dir.path() + "/nested/deeper").absolutePath());
    }

    // A singleton stays a singleton even when a same-key file exists in
    // another folder — the classic cross-folder false pairing, from the
    // status side rather than the grouping side.
    void loneMemberPerFolder_areBothSingletons()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath("a"));
        QVERIFY(QDir(dir.path()).mkpath("b"));
        const QString pa = touchFile(dir.path() + "/a", "TX02_PART1.wav", 60);
        const QString pb = touchFile(dir.path() + "/b", "TX02_PART2.wav", 50);

        TranscriptionController controller;
        const QVariantList rules = {rule("parts", "^(TX\\d{2})_PART\\d+\\.wav$")};
        const QVariantList groups = controller.detectCombineGroups(rules, {pa, pb});

        QCOMPARE(groups.size(), 2);
        for (const QVariant &g : groups)
            QCOMPARE(g.toMap().value("status").toString(), QStringLiteral("singleton"));
    }

    void onlyOneMatch_isSingleton()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString p1 = touchFile(dir.path(), "TX02_MIC008_20260711_011600_orig.wav", 60);

        TranscriptionController controller;
        const QVariantList rules = {rule("acme mic 3", "^(TX\\d{2})_MIC\\d{3}_\\d{8}_\\d{6}_orig\\.wav$")};
        const QVariantList groups = controller.detectCombineGroups(rules, {p1});

        QCOMPARE(findGroup(groups, "acme mic 3").value("status").toString(), QStringLiteral("singleton"));
    }

    // Never auto-combine over existing work: a group where any member
    // already has a sibling .md is left alone entirely.
    void existingTranscript_skipsGroup()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString p1 = touchFile(dir.path(), "TX02_MIC008_20260711_011600_orig.wav", 60);
        const QString p2 = touchFile(dir.path(), "TX02_MIC009_20260711_014600_orig.wav", 30);
        QFile md(p1 + ".md");
        QVERIFY(md.open(QIODevice::WriteOnly));
        md.close();

        TranscriptionController controller;
        const QVariantList rules = {rule("acme mic 3", "^(TX\\d{2})_MIC\\d{3}_\\d{8}_\\d{6}_orig\\.wav$")};
        const QVariantList groups = controller.detectCombineGroups(rules, {p1, p2});

        QCOMPARE(findGroup(groups, "acme mic 3").value("status").toString(), QStringLiteral("alreadyTranscribed"));
    }

    // A recorder may still be flushing the newest part — a group otherwise
    // "ready" but whose newest member was JUST modified reads as "pending",
    // not "ready", so runCombineScan() won't race a growing file.
    void freshestMemberTooRecent_isPending()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString p1 = touchFile(dir.path(), "TX02_MIC008_20260711_011600_orig.wav", 60);
        const QString p2 = touchFile(dir.path(), "TX02_MIC009_20260711_014600_orig.wav", 0);

        TranscriptionController controller;
        const QVariantList rules = {rule("acme mic 3", "^(TX\\d{2})_MIC\\d{3}_\\d{8}_\\d{6}_orig\\.wav$")};
        const QVariantList groups = controller.detectCombineGroups(rules, {p1, p2});

        QCOMPARE(findGroup(groups, "acme mic 3").value("status").toString(), QStringLiteral("pending"));
    }

    // outputPath is derived from (ruleName, key) alone — recomputable from a
    // single surviving filename even once the rest of the group's members
    // are gone (moved into .combined/ by a real combine job). This is what
    // startJob()'s per-job safety-net redirect relies on.
    void outputPath_stableAcrossPartialGroupMembership()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString p1 = touchFile(dir.path(), "TX02_MIC008_20260711_011600_orig.wav", 60);
        const QString p2 = touchFile(dir.path(), "TX02_MIC009_20260711_014600_orig.wav", 30);

        TranscriptionController controller;
        const QVariantList rules = {rule("acme mic 3", "^(TX\\d{2})_MIC\\d{3}_\\d{8}_\\d{6}_orig\\.wav$")};
        const QString fullGroupOutput =
            findGroup(controller.detectCombineGroups(rules, {p1, p2}), "acme mic 3").value("outputPath").toString();
        // Same rule, but only p1 is still around (p2 pretend-moved away) —
        // p1 alone is a "singleton" now, yet its outputPath must still match
        // exactly what the full-group detection computed above.
        const QString singletonOutput =
            findGroup(controller.detectCombineGroups(rules, {p1}), "acme mic 3").value("outputPath").toString();

        QCOMPARE(singletonOutput, fullGroupOutput);
    }

    // detectCombineGroups() also backs the disabled/malformed-pattern paths
    // a user can hit while authoring a rule in Settings — neither should
    // ever match anything (nor crash).
    void disabledOrInvalidPattern_matchesNothing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString p1 = touchFile(dir.path(), "TX02_MIC008_20260711_011600_orig.wav", 60);
        const QString p2 = touchFile(dir.path(), "TX02_MIC009_20260711_014600_orig.wav", 30);

        TranscriptionController controller;
        const QVariantList disabledRules = {
            rule("acme mic 3", "^(TX\\d{2})_MIC\\d{3}_\\d{8}_\\d{6}_orig\\.wav$", /*enabled=*/false)};
        QVERIFY(controller.detectCombineGroups(disabledRules, {p1, p2}).isEmpty());

        const QVariantList invalidRules = {rule("broken", "(unterminated[")};
        QVERIFY(controller.detectCombineGroups(invalidRules, {p1, p2}).isEmpty());
    }

    // regexValidationError() backs the Settings pattern field's live validity
    // indicator — it must validate against the exact same QRegularExpression
    // engine findCombineGroups()/matchRuleCapturedKey() use, not QML/JS
    // RegExp, so "valid" here really does mean "will match as expected"
    // rather than merely "looked fine to a different regex dialect".
    void regexValidationError_emptyValidAndInvalidPatterns()
    {
        TranscriptionController controller;
        QCOMPARE(controller.regexValidationError(QString()), QString());
        QCOMPARE(controller.regexValidationError(QStringLiteral("^(TX\\d{2})_.*\\.wav$")), QString());
        QVERIFY(!controller.regexValidationError(QStringLiteral("(unterminated[")).isEmpty());
    }

    // differentiatorNonParticipatingFiles() backs the Settings differentiator
    // buttons' "block the toggle" check — a capturing group sitting in an
    // alternation branch that isn't always taken doesn't reliably
    // differentiate anything for files that took the OTHER branch, so those
    // files must be reported (capturedStart() < 0 == didn't participate at
    // all), distinct from a valid-but-empty capture.
    void differentiatorNonParticipatingFiles_flagsUntakenAlternationBranch()
    {
        TranscriptionController controller;
        // Group 1 only participates for "sometext", group 2 only for
        // "othertext" — for either file, exactly one of the two groups never
        // participated in the match at all.
        const QString pattern = QStringLiteral("^(sometext)|(othertext)$");
        const QStringList files = {QStringLiteral("sometext"), QStringLiteral("othertext")};

        const QStringList group1Offenders = controller.differentiatorNonParticipatingFiles(pattern, 1, files);
        QCOMPARE(group1Offenders, QStringList({QStringLiteral("othertext")}));

        const QStringList group2Offenders = controller.differentiatorNonParticipatingFiles(pattern, 2, files);
        QCOMPARE(group2Offenders, QStringList({QStringLiteral("sometext")}));
    }

    // A zero-width capture (the group matched, just an empty string) is NOT
    // "non-participating" — capturedStart() is a valid non-negative index
    // even when the captured length is 0, only an untaken alternation branch
    // produces -1.
    void differentiatorNonParticipatingFiles_zeroWidthCaptureIsNotFlagged()
    {
        TranscriptionController controller;
        const QString pattern = QStringLiteral("^prefix(\\d*)suffix$");
        const QStringList files = {QStringLiteral("prefixsuffix"), QStringLiteral("prefix123suffix")};

        QVERIFY(controller.differentiatorNonParticipatingFiles(pattern, 1, files).isEmpty());
    }

    // Invalid pattern, out-of-range group index, or a non-matching file must
    // all be handled without crashing and without false positives.
    void differentiatorNonParticipatingFiles_invalidInputsReturnEmpty()
    {
        TranscriptionController controller;
        const QStringList files = {QStringLiteral("foo.wav")};
        QVERIFY(controller.differentiatorNonParticipatingFiles(QStringLiteral("(unterminated["), 1, files).isEmpty());
        QVERIFY(controller.differentiatorNonParticipatingFiles(QStringLiteral("^(\\d+)$"), 5, files).isEmpty());
        QVERIFY(controller.differentiatorNonParticipatingFiles(QStringLiteral("^(\\d+)$"), 0, files).isEmpty());
    }

    // Item 51: combineGroup() is the sidebar's per-row "combine this group"
    // button — same matching as runCombineScan() but scoped to the ONE group
    // whose outputPath matches, and must not touch any other detected group.
    void combineGroup_enqueuesOnlyTheMatchingReadyGroup()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a1 = touchFile(dir.path(), "TXA_MIC001_orig.wav", 60);
        const QString a2 = touchFile(dir.path(), "TXA_MIC002_orig.wav", 40);
        const QString b1 = touchFile(dir.path(), "TXB_MIC001_orig.wav", 60);
        const QString b2 = touchFile(dir.path(), "TXB_MIC002_orig.wav", 40);

        TranscriptionController controller;
        const QVariantList rules = {rule("two groups", "^(TX[AB])_MIC\\d{3}_orig\\.wav$")};
        const QStringList allPaths = {a1, a2, b1, b2};
        const QVariantList groups = controller.detectCombineGroups(rules, allPaths);
        QCOMPARE(groups.size(), 2);

        // findGroup() only returns the first match by ruleName — both groups
        // share "two groups" as ruleName, so pick apart by key directly.
        QVariantMap targetA, targetB;
        for (const QVariant &g : groups) {
            const QVariantMap m = g.toMap();
            if (m.value("key").toString() == "TXA")
                targetA = m;
            else if (m.value("key").toString() == "TXB")
                targetB = m;
        }
        QVERIFY(!targetA.isEmpty());
        QVERIFY(!targetB.isEmpty());
        QCOMPARE(targetA.value("status").toString(), QStringLiteral("ready"));
        QCOMPARE(targetB.value("status").toString(), QStringLiteral("ready"));

        const QUrl vaultUrl = QUrl::fromLocalFile(dir.path());
        controller.combineGroup(rules, allPaths, targetA.value("outputPath").toString(), vaultUrl, false);

        const QVariantList jobsAfter = controller.jobs();
        QCOMPARE(jobsAfter.size(), 1);
        const QVariantMap job = jobsAfter.first().toMap();
        QCOMPARE(job.value("kind").toString(), QStringLiteral("combine"));
        QCOMPARE(job.value("fileUrl").toUrl(), QUrl::fromLocalFile(targetA.value("outputPath").toString()));
    }

    // A group that isn't "ready" (here: a singleton, only one match) must be
    // silently ignored — no job enqueued.
    void combineGroup_nonReadyGroupIsIgnored()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString only = touchFile(dir.path(), "TX01_MIC001_orig.wav", 60);

        TranscriptionController controller;
        const QVariantList rules = {rule("singleton rule", "^(TX\\d{2})_MIC\\d{3}_orig\\.wav$")};
        const QStringList allPaths = {only};
        const QVariantMap group = findGroup(controller.detectCombineGroups(rules, allPaths), "singleton rule");
        QCOMPARE(group.value("status").toString(), QStringLiteral("singleton"));

        controller.combineGroup(rules, allPaths, group.value("outputPath").toString(),
                                 QUrl::fromLocalFile(dir.path()), false);
        QVERIFY(controller.jobs().isEmpty());
    }

    // Calling combineGroup() twice for the same group must not double-queue
    // it — mirrors runCombineScan()'s own isCombineQueued() dedup.
    void combineGroup_alreadyQueuedIsNotDuplicated()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString p1 = touchFile(dir.path(), "TX01_MIC001_orig.wav", 60);
        const QString p2 = touchFile(dir.path(), "TX01_MIC002_orig.wav", 40);

        TranscriptionController controller;
        const QVariantList rules = {rule("dup rule", "^(TX\\d{2})_MIC\\d{3}_orig\\.wav$")};
        const QStringList allPaths = {p1, p2};
        const QVariantMap group = findGroup(controller.detectCombineGroups(rules, allPaths), "dup rule");
        QCOMPARE(group.value("status").toString(), QStringLiteral("ready"));

        const QUrl vaultUrl = QUrl::fromLocalFile(dir.path());
        controller.combineGroup(rules, allPaths, group.value("outputPath").toString(), vaultUrl, false);
        controller.combineGroup(rules, allPaths, group.value("outputPath").toString(), vaultUrl, false);
        QCOMPARE(controller.jobs().size(), 1);
    }

    // Item 44: manual combine — combineSelected() takes an EXPLICIT, user-
    // selected list of audio files (no regex rule involved) and must enqueue
    // exactly ONE combine job over exactly those files, ordered mtime
    // ascending (findCombineGroups()'s default order, reused so the
    // concatenation isn't in arbitrary selection/click order). The files here
    // are PASSED newest-first so a result that echoed input order (rather than
    // sorting by mtime) would be provably wrong.
    void combineSelected_enqueuesCombineJobOverSelectionInMtimeOrder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString oldest = touchFile(dir.path(), "part_a.wav", 60);
        const QString middle = touchFile(dir.path(), "part_b.wav", 40);
        const QString newest = touchFile(dir.path(), "part_c.wav", 20);

        TranscriptionController controller;
        const QUrl vaultUrl = QUrl::fromLocalFile(dir.path());
        // Deliberately newest -> oldest input order; expected output is
        // oldest -> newest (mtime ascending).
        const QVariantList selection = {QUrl::fromLocalFile(newest), QUrl::fromLocalFile(middle),
                                        QUrl::fromLocalFile(oldest)};
        controller.combineSelected(selection, vaultUrl);

        const QVariantList jobsAfter = controller.jobs();
        QCOMPARE(jobsAfter.size(), 1);
        const QVariantMap job = jobsAfter.first().toMap();
        QCOMPARE(job.value("kind").toString(), QStringLiteral("combine"));
        QCOMPARE(job.value("combineSourcePaths").toStringList(), QStringList({oldest, middle, newest}));
        // Output is named from the earliest part's stem, ".combined.<ext>",
        // beside it — same shape as the auto path's outputPath.
        QCOMPARE(job.value("fileUrl").toUrl(),
                 QUrl::fromLocalFile(dir.path() + QStringLiteral("/part_a.combined.wav")));
    }

    // Fewer than two audio files is a silent no-op — a single audio file, or a
    // selection whose only extra member is a non-audio .md (dropped by the
    // audio filter), leaves zero jobs enqueued.
    void combineSelected_underTwoAudioFilesIsNoOp()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a1 = touchFile(dir.path(), "solo.wav", 60);
        const QString notAudio = touchFile(dir.path(), "notes.md", 40);

        TranscriptionController controller;
        const QUrl vaultUrl = QUrl::fromLocalFile(dir.path());

        // One audio file only.
        controller.combineSelected({QUrl::fromLocalFile(a1)}, vaultUrl);
        QVERIFY(controller.jobs().isEmpty());

        // One audio + one .md: the .md is dropped, leaving <2 audio -> no-op.
        controller.combineSelected({QUrl::fromLocalFile(a1), QUrl::fromLocalFile(notAudio)}, vaultUrl);
        QVERIFY(controller.jobs().isEmpty());

        // Empty selection -> no-op.
        controller.combineSelected({}, vaultUrl);
        QVERIFY(controller.jobs().isEmpty());
    }

    // A non-audio member mixed into an otherwise-valid selection is dropped,
    // not fed to the merge — combineSourcePaths must contain only the audio
    // files, still ordered by mtime.
    void combineSelected_dropsNonAudioMembers()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a1 = touchFile(dir.path(), "one.wav", 60);
        const QString a2 = touchFile(dir.path(), "two.wav", 40);
        const QString md = touchFile(dir.path(), "one.wav.md", 30);

        TranscriptionController controller;
        const QUrl vaultUrl = QUrl::fromLocalFile(dir.path());
        controller.combineSelected(
            {QUrl::fromLocalFile(a1), QUrl::fromLocalFile(md), QUrl::fromLocalFile(a2)}, vaultUrl);

        const QVariantList jobsAfter = controller.jobs();
        QCOMPARE(jobsAfter.size(), 1);
        QCOMPARE(jobsAfter.first().toMap().value("combineSourcePaths").toStringList(),
                 QStringList({a1, a2}));
    }

    // Calling combineSelected() twice for the same selection must not double-
    // queue it — the output path is deterministic (earliest part's stem), so
    // the second call hits isCombineQueued() and no-ops, mirroring
    // combineGroup()'s dedup.
    void combineSelected_alreadyQueuedIsNotDuplicated()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a1 = touchFile(dir.path(), "x.wav", 60);
        const QString a2 = touchFile(dir.path(), "y.wav", 40);

        TranscriptionController controller;
        const QUrl vaultUrl = QUrl::fromLocalFile(dir.path());
        const QVariantList selection = {QUrl::fromLocalFile(a1), QUrl::fromLocalFile(a2)};
        controller.combineSelected(selection, vaultUrl);
        controller.combineSelected(selection, vaultUrl);
        QCOMPARE(controller.jobs().size(), 1);
    }
};

QTEST_GUILESS_MAIN(TstFileCombine)
#include "tst_filecombine.moc"
