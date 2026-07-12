// Spec under test — split-recording auto-combine (see DESIGN.md's vault
// layout section and TranscriptionController::findCombineGroups()'s doc
// comment): user-authored regex rules matched against vault filenames,
// grouped by their CAPTURED text (not just "the rule matched"), ordered by
// mtime, with the merged output filename derived from (ruleName, key) alone
// so it's re-derivable even after the original parts are gone.
//
// detectCombineGroups() is pure over its (rules, audioPaths) inputs (only
// touches disk via QFileInfo stats on the paths given), so these tests never
// need a real vault, model, or worker thread activity — just a temp dir of
// touched files with controlled mtimes.

#include <QDateTime>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "TranscriptionController.h"

namespace {

QVariantMap rule(const QString &name, const QString &pattern, bool enabled = true)
{
    QVariantMap m;
    m["name"] = name;
    m["pattern"] = pattern;
    m["enabled"] = enabled;
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
    // not "ready", so autoCombine() won't race a growing file.
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
};

QTEST_GUILESS_MAIN(TstFileCombine)
#include "tst_filecombine.moc"
