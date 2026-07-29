// Living spec: runUsbGrabScan() — the scan+filter+dedup+copy walk for one USB
// grab rule against one already-mounted device (see DESIGN.md's "USB import"
// section: diff against the vault by
// filename + size + mtime and copy only new audio in, never reading file
// content).
//
// The walk is a plain SYNCHRONOUS free function over its inputs, so almost
// everything here calls it directly and reads the UsbGrabScanResult it returns
// — no signals, no waiting. That's the whole point of the shape: the walk knows
// nothing about threads, the job queue, history entries or VaultTreeModel, and
// neither does its spec.
//
// Threading lives with the callers, not here: a real grab is a queued "usbGrab"
// task on TranscriptionController's worker (specced in tst_transcription_queue),
// and the dry-run preview runs on UsbGrabPreviewJob's own thread (its
// start()/previewReady() surface is exercised below by
// refusedPreview_reportsErrorRatherThanEmptyList, via QSignalSpy::wait() like
// any other cross-thread Qt signal in this suite). This file specs the walk
// itself.

#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "UsbGrabJob.h"
#include "UsbRuleStore.h"
#include "VaultTreeModel.h"

namespace {

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content);
    return true;
}

// Dedup is by basename + size + mtime (see alreadyPresentAtDestination() in
// UsbGrabJob.cpp), so "already in the vault" can't be expressed by content
// alone the way it could under the old content-hash diff — two files written
// back-to-back get mtimes milliseconds apart and would NOT match. Tests that
// mean "this is the same file" must say so by pinning the mtime explicitly.
bool setMtime(const QString &path, const QDateTime &when)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite))
        return false;
    const bool ok = f.setFileTime(when, QFileDevice::FileModificationTime);
    f.close();
    return ok;
}

// An arbitrary fixed instant, well in the past so it can never collide with a
// file's real creation time during the test run.
QDateTime fixedMtime()
{
    return QDateTime(QDate(2026, 3, 4), QTime(5, 6, 7));
}

}

class TstUsbGrabJob : public QObject
{
    Q_OBJECT

private slots:
    void matchingExtensionsAreCopied_nonMatchingAreIgnored()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("interview.wav"), "audio-data"));
        QVERIFY(writeFile(device.filePath("readme.txt"), "not audio"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.extensionFilter = {QStringLiteral("wav")};
        rule.preserveStructure = true;

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 1);
        QCOMPARE(result.filesSkipped, 0);
        QVERIFY(QFileInfo::exists(vault.filePath("interview.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("readme.txt")));
    }

    void emptyExtensionFilter_meansAllFilesNotZeroFiles()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("a.wav"), "a"));
        QVERIFY(writeFile(device.filePath("b.txt"), "b"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        // extensionFilter left empty deliberately.

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 2);
        QVERIFY(QFileInfo::exists(vault.filePath("a.wav")));
        QVERIFY(QFileInfo::exists(vault.filePath("b.txt")));
    }

    // Same basename + same size + same mtime already in the vault -> skipped.
    // A different basename is always new. (The same-basename-but-different-file
    // case is quickCheck_sameBasenameDifferentContent_... below.)
    // Progress is BYTE-weighted, not per-file-count (2026-07 fix). The bug it
    // fixes: grabbing 15 tiny already-present files + 1 big new file jumped
    // straight to ~94% (15/16 candidates "processed" instantly) and then
    // stalled on the big copy. With byte weighting, the tiny skips barely move
    // the bar and the big copy shows its real progress. Also verifies progress
    // is monotonic and ends at exactly 1.0.
    void progress_isByteWeighted_notPerFileCount()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());

        // 15 tiny files already present in the vault (identical -> skipped).
        for (int i = 0; i < 15; ++i) {
            const QString name = QStringLiteral("tiny%1.wav").arg(i);
            QVERIFY(writeFile(vault.filePath(name), "x"));
            QVERIFY(writeFile(device.filePath(name), "x"));
            QVERIFY(setMtime(vault.filePath(name), fixedMtime()));
            QVERIFY(setMtime(device.filePath(name), fixedMtime()));
        }
        // One big NEW file (~4 MiB) — the bulk of the bytes, all still to copy.
        QVERIFY(writeFile(device.filePath("big.wav"), QByteArray(4 << 20, 'a')));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.preserveStructure = false;

        QList<double> fractions;
        UsbGrabScanHooks hooks;
        hooks.onProgress = [&fractions](qint64 done, qint64 total) {
            if (total > 0)
                fractions.append(static_cast<double>(done) / static_cast<double>(total));
        };

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false, hooks);
        QCOMPARE(result.filesCopied, 1);
        QCOMPARE(result.filesSkipped, 15);

        QVERIFY(!fractions.isEmpty());
        // Monotonic non-decreasing, and reaches exactly 1.0 at the end.
        for (int i = 1; i < fractions.size(); ++i)
            QVERIFY2(fractions[i] >= fractions[i - 1] - 1e-9, "progress must not go backwards");
        QVERIFY(qFuzzyCompare(fractions.last() + 1.0, 1.0 + 1.0)); // last == 1.0

        // The 15 tiny already-present files are excluded from the
        // denominator entirely (they're skipped, not work), so the denominator
        // is exactly the 4 MiB big file and the FIRST report is that copy's own
        // first chunk (near 0), NOT near 94% as the old per-count logic gave.
        // At least one mid-copy sample must land in the wide (0.05, 0.95) band,
        // proving sub-file granularity rather than a single 0->1 jump.
        bool sawMidCopy = false;
        for (const double f : fractions)
            if (f > 0.05 && f < 0.95)
                sawMidCopy = true;
        QVERIFY2(sawMidCopy, "expected sub-file progress samples during the big copy");
    }

    // Regression for "USB grab starts at ~80%": when most of the device is
    // ALREADY in the vault, only the bytes still to be copied size the bar. The
    // already-present files are skipped instantly and must count toward NEITHER
    // the denominator nor the numerator. The inverse of
    // progress_isByteWeighted_notPerFileCount (there the duplicates are tiny;
    // here they're the bulk — the case that surfaced the bug).
    void progress_ignoresAlreadyPresentBytes_startsNearZero()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());

        // One big file already in the vault (identical -> skipped): the bulk.
        const QByteArray big(16 << 20, 'a');
        QVERIFY(writeFile(vault.filePath("big.wav"), big));
        QVERIFY(writeFile(device.filePath("big.wav"), big));
        QVERIFY(setMtime(vault.filePath("big.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("big.wav"), fixedMtime()));
        // One smaller NEW file (multi-chunk so sub-file progress shows) — the
        // only thing actually grabbed.
        QVERIFY(writeFile(device.filePath("new.wav"), QByteArray(4 << 20, 'b')));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.preserveStructure = false;

        QList<double> fractions;
        int reportedTotal = -1;
        UsbGrabScanHooks hooks;
        hooks.onProgress = [&fractions](qint64 done, qint64 total) {
            if (total > 0)
                fractions.append(static_cast<double>(done) / static_cast<double>(total));
        };
        hooks.onTotalsKnown = [&reportedTotal](int n) { reportedTotal = n; };

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false, hooks);
        QCOMPARE(result.filesCopied, 1);
        QCOMPARE(result.filesSkipped, 1);

        // The label counts only the file actually grabbed, not the duplicate.
        QCOMPARE(reportedTotal, 1);
        QVERIFY(!fractions.isEmpty());
        // The FIRST report is the new file's first chunk (~0.25), NOT the 0.8
        // the old logic emitted the instant it walked the 16 MiB skip.
        QVERIFY2(fractions.first() < 0.5, "grab must not jump to the already-present fraction");
        // Monotonic, ends at exactly 1.0.
        for (int i = 1; i < fractions.size(); ++i)
            QVERIFY2(fractions[i] >= fractions[i - 1] - 1e-9, "progress must not go backwards");
        QVERIFY(qFuzzyCompare(fractions.last() + 1.0, 1.0 + 1.0));
    }

    // onTotalsKnown fires once, before any copy, with the count of files this
    // grab will actually copy — matching the rule's filters AND not already
    // present/combined in the vault — the "N files" the Tasks row
    // names the grab by. Here all 3 .wav are new, so the count is 3; the
    // extension-filtered-out ones are excluded, same predicate as the byte
    // pre-count.
    void onTotalsKnown_reportsMatchingCandidateCount()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());

        // 3 matching .wav files + 2 non-matching .txt files.
        for (int i = 0; i < 3; ++i)
            QVERIFY(writeFile(device.filePath(QStringLiteral("clip%1.wav").arg(i)), "audio"));
        QVERIFY(writeFile(device.filePath("notes.txt"), "text"));
        QVERIFY(writeFile(device.filePath("nested/readme.txt"), "text"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.preserveStructure = false;
        rule.extensionFilter = QStringList{QStringLiteral("wav")};

        int reportedTotal = -1;
        int totalsCallbacks = 0;
        UsbGrabScanHooks hooks;
        // The pre-count pass (and thus onTotalsKnown) is gated on onProgress.
        hooks.onProgress = [](qint64, qint64) {};
        hooks.onTotalsKnown = [&](int totalFiles) {
            reportedTotal = totalFiles;
            ++totalsCallbacks;
        };

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false, hooks);
        QCOMPARE(result.filesCopied, 3); // the .txt files were filtered out
        QCOMPARE(totalsCallbacks, 1);    // fired exactly once
        QCOMPARE(reportedTotal, 3);      // only the matching .wav candidates
    }

    void quickCheck_skipsAlreadyPresentFile_copiesNewOne()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(vault.filePath("clip.wav"), "same-content"));
        QVERIFY(writeFile(device.filePath("clip.wav"), "same-content"));       // identical -> skip
        QVERIFY(setMtime(vault.filePath("clip.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("clip.wav"), fixedMtime()));
        QVERIFY(writeFile(device.filePath("nested/clip2.wav"), "different")); // different basename, always new

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.preserveStructure = false; // flatten, so clip2.wav lands at vault root too

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 1);  // filesCopied: clip2.wav only
        QCOMPARE(result.filesSkipped, 1); // filesSkipped: clip.wav (identical)
        QVERIFY(QFileInfo::exists(vault.filePath("clip2.wav")));
    }

    // A device part whose split-recording group was ALREADY combined must be
    // skipped, not re-grabbed — even though the raw part no longer sits in the
    // vault (a real combine moves it into .combined/ or deletes it), so the
    // plain basename/size/mtime quick-check can't see it. Without this,
    // replugging the device re-grabs the parts and the group re-combines,
    // doubling the disk use. runUsbGrabScan() recognises the group by
    // recomputing the merged-output name from the filename (the same
    // CombineNaming.h helpers the combiner uses) and finding it on disk. See
    // DESIGN.md's "USB import" (already-combined) and tst_filecombine for the
    // naming.
    void alreadyCombinedGroup_isSkipped_notReGrabbed()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        // The two raw parts still sit on the USB device...
        QVERIFY(writeFile(device.filePath("cam_20260710_edit_a.wav"), "part-a"));
        QVERIFY(writeFile(device.filePath("cam_20260710_edit_b.wav"), "part-b"));
        // ...but the vault holds ONLY their merged output — the parts were
        // moved into .combined/ or deleted by a prior combine.
        QVERIFY(writeFile(vault.filePath("Acme Mic 3 - 20260710 edit.combined.wav"), "merged"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.extensionFilter = {QStringLiteral("wav")};
        rule.preserveStructure = false; // combined output lands at the vault root

        QVariantMap combineRule;
        combineRule["name"] = QStringLiteral("Acme Mic 3");
        combineRule["pattern"] = QStringLiteral("^cam_(\\d{8})_(edit)_[ab]\\.wav$");
        combineRule["enabled"] = true;
        combineRule["sortGroupIndex"] = -1;

        const UsbGrabScanResult result = runUsbGrabScan(
            rule, device.path(), vault.path(), /*dryRun=*/false, /*hooks=*/{}, QVariantList{combineRule});

        QCOMPARE(result.filesCopied, 0);
        QCOMPARE(result.filesSkipped, 2);
        // The re-grab did NOT recreate the raw parts in the vault.
        QVERIFY(!QFileInfo::exists(vault.filePath("cam_20260710_edit_a.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("cam_20260710_edit_b.wav")));
    }

    // Negative control for the skip above: a rule that MATCHES the parts but
    // whose merged output isn't on disk yet must NOT skip them — the gate is
    // "the combined file exists", not "a combine rule matched". (The
    // no-combine-rules-at-all case is every other test in this file.)
    void combineRuleMatchesButNotYetCombined_partsAreGrabbed()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("cam_20260710_edit_a.wav"), "part-a"));
        QVERIFY(writeFile(device.filePath("cam_20260710_edit_b.wav"), "part-b"));
        // No merged output written to the vault.

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.extensionFilter = {QStringLiteral("wav")};
        rule.preserveStructure = false;

        QVariantMap combineRule;
        combineRule["name"] = QStringLiteral("Acme Mic 3");
        combineRule["pattern"] = QStringLiteral("^cam_(\\d{8})_(edit)_[ab]\\.wav$");
        combineRule["enabled"] = true;
        combineRule["sortGroupIndex"] = -1;

        const UsbGrabScanResult result = runUsbGrabScan(
            rule, device.path(), vault.path(), /*dryRun=*/false, /*hooks=*/{}, QVariantList{combineRule});

        QCOMPARE(result.filesCopied, 2);
        QCOMPARE(result.filesSkipped, 0);
    }

    // Same basename and same mtime, but a different size -> a genuinely
    // different file, so it's copied rather than skipped. Pinning both mtimes
    // to the same instant is what makes this prove size is part of the
    // quick-check: with mtime held equal, size is the only thing left to tell
    // the two apart.
    void quickCheck_sameBasenameDifferentSize_copiedUnderNumericSuffix()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(vault.filePath("clip.wav"), "vault-version"));
        QVERIFY(writeFile(device.filePath("clip.wav"), "a-longer-device-version")); // same name, bigger
        QVERIFY(setMtime(vault.filePath("clip.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("clip.wav"), fixedMtime()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 1); // copied, not skipped
        QCOMPARE(result.filesSkipped, 0);
        // The original vault file must survive untouched (never overwritten).
        QFile original(vault.filePath("clip.wav"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("vault-version"));
        QVERIFY(QFileInfo::exists(vault.filePath("clip_1.wav")));
    }

    void preserveStructureFlag_nestsUnderRelativeSubfolder()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("Voice/2026/session.wav"), "x"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.preserveStructure = true;

        runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QVERIFY(QFileInfo::exists(vault.filePath("Voice/2026/session.wav")));
    }

    void preserveStructureFalse_flattensIntoVaultRoot()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("Voice/2026/session.wav"), "x"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.preserveStructure = false;

        runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QVERIFY(!QFileInfo::exists(vault.filePath("Voice/2026/session.wav")));
        QVERIFY(QFileInfo::exists(vault.filePath("session.wav")));
    }

    void startFolder_scansOnlyThatSubfolderOfTheDevice()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("Podcasts/ep1.wav"), "x"));
        QVERIFY(writeFile(device.filePath("Other/ignored.wav"), "y"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.startFolder = QStringLiteral("Podcasts");

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 1);
        QVERIFY(QFileInfo::exists(vault.filePath("ep1.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("ignored.wav")));
    }

    // dryRun=true classifies every file (grab / duplicate / filtered) without
    // copying anything, and never appends a history entry — the dry-run
    // counterpart to matchingExtensionsAreCopied_nonMatchingAreIgnored()
    // and quickCheck_skipsAlreadyPresentFile_...() above, sharing the exact same
    // extension-filter + quick-check dedup logic.
    void dryRun_classifiesWithoutCopyingOrTouchingDisk()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(vault.filePath("existing.wav"), "already-here"));
        QVERIFY(writeFile(device.filePath("existing.wav"), "already-here")); // same size+mtime -> duplicate
        QVERIFY(setMtime(vault.filePath("existing.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("existing.wav"), fixedMtime()));
        QVERIFY(writeFile(device.filePath("new.wav"), "brand-new"));        // not present -> grab
        QVERIFY(writeFile(device.filePath("readme.txt"), "not audio"));     // extension filtered out

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.extensionFilter = {QStringLiteral("wav")};

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/true);

        const QVariantList entries = result.previewEntries;
        QCOMPARE(entries.size(), 3);

        QHash<QString, QString> statusByPath;
        for (const QVariant &entryVariant : entries) {
            const QVariantMap entry = entryVariant.toMap();
            statusByPath.insert(entry.value(QStringLiteral("path")).toString(),
                                entry.value(QStringLiteral("status")).toString());
        }
        QCOMPARE(statusByPath.value(QStringLiteral("existing.wav")), QStringLiteral("duplicate"));
        QCOMPARE(statusByPath.value(QStringLiteral("new.wav")), QStringLiteral("grab"));
        QCOMPARE(statusByPath.value(QStringLiteral("readme.txt")), QStringLiteral("filtered"));

        // Nothing was actually copied — "new.wav" being classified "grab"
        // must not have landed in the vault.
        QVERIFY(!QFileInfo::exists(vault.filePath("new.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("readme.txt")));
        // The history log must stay untouched by a preview run.
        QVERIFY(vaultModel.readUsbGrabHistory().isEmpty());
    }

    // A preview must NEVER read file content — not for a file that's already
    // present, not for a new one. This is the whole point of the quick-check:
    // content hashing made a preview of a 13.8 GB recorder over USB take ~25
    // minutes (it read the entire device), which read as a hang. Enforced by
    // making every source file unreadable: if the scan still classifies them
    // all correctly, it provably never opened one.
    void preview_neverReadsFileContent()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("unique.wav"), "content-never-read"));
        // Same basename+size+mtime as a vault file => "duplicate". This is the
        // case the old hash diff was forced to read in full, so it's the one
        // that matters most here.
        QVERIFY(writeFile(vault.filePath("known.wav"), "known-content"));
        QVERIFY(writeFile(device.filePath("known.wav"), "known-content"));
        QVERIFY(setMtime(vault.filePath("known.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("known.wav"), fixedMtime()));

        const QStringList sources {device.filePath("unique.wav"), device.filePath("known.wav")};
        for (const QString &path : sources)
            QVERIFY(QFile::setPermissions(path, QFileDevice::WriteOwner));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/true);

        const QVariantList entries = result.previewEntries;
        QCOMPARE(entries.size(), 2);
        QHash<QString, QString> statusByPath;
        for (const QVariant &entryVariant : entries) {
            const QVariantMap entry = entryVariant.toMap();
            statusByPath.insert(entry.value(QStringLiteral("path")).toString(),
                                entry.value(QStringLiteral("status")).toString());
        }
        QCOMPARE(statusByPath.value(QStringLiteral("unique.wav")), QStringLiteral("grab"));
        QCOMPARE(statusByPath.value(QStringLiteral("known.wav")), QStringLiteral("duplicate"));

        // Restore permissions so QTemporaryDir can clean up.
        for (const QString &path : sources)
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    // THE regression test for the quick-check: QFile::copy() stamps the copy
    // with the time of the copy, not the source's mtime. Without runUsbGrabScan()
    // explicitly restoring it, a grabbed file would never match its source
    // again and every single re-grab would re-copy the whole device under
    // _1/_2/... suffixes — silently duplicating gigabytes.
    void copiedFile_keepsSourceMtime_soARegrabSkipsEverything()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("take1.wav"), "audio"));
        QVERIFY(writeFile(device.filePath("nested/take2.wav"), "more-audio"));
        QVERIFY(setMtime(device.filePath("take1.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("nested/take2.wav"), fixedMtime()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        const UsbGrabScanResult firstRun = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);
        QCOMPARE(firstRun.filesCopied, 2); // both copied

        // The copies carry the source's mtime, not "now".
        QCOMPARE(QFileInfo(vault.filePath("take1.wav")).lastModified(), fixedMtime());
        QCOMPARE(QFileInfo(vault.filePath("nested/take2.wav")).lastModified(), fixedMtime());

        // Re-grabbing the same untouched device must copy nothing at all.
        const UsbGrabScanResult secondRun = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);
        QCOMPARE(secondRun.filesCopied, 0);  // filesCopied
        QCOMPARE(secondRun.filesSkipped, 2); // filesSkipped
        QVERIFY(!QFileInfo::exists(vault.filePath("take1_1.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("nested/take2_1.wav")));
    }

    // THE regression test for partial copies: a file that doesn't land whole
    // must not be counted, must not be kept, and must be reported.
    //
    // QFile::copy() cannot be taken at its word here. Verified against Qt 6.11:
    // when the source stops delivering bytes partway — a recorder unplugged
    // mid-grab, the normal failure on removable media — it returns TRUE having
    // written a truncated file. Believing it put a file in the vault that wears
    // a recording's name, plays, and transcribes into garbage, inside a run that
    // reported a clean success.
    //
    // The trick: a symlink to a /proc file is the one way to drive that exact
    // condition with no race, no threads and no multi-GB fixture. /proc/self/status
    // stats at size 0 but reads back ~1.5 KB, so the copy "succeeds" with a size
    // that disagrees with the source's stat — precisely what a yanked device
    // produces, just from the opposite direction (more bytes than stat'd rather
    // than fewer). The check under test rejects any MISMATCH, deliberately: a
    // source still being written on the device isn't grabbable either.
    void copyThatDoesNotLandWhole_isRejectedNotCountedAndReported()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(QFile::link(QStringLiteral("/proc/self/status"), device.filePath("clip.wav")));
        // The premise of the trick, asserted rather than assumed: if /proc ever
        // stopped lying about its size this test would silently stop testing.
        QCOMPARE(QFileInfo(device.filePath("clip.wav")).size(), 0);

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        // Not counted as copied...
        QCOMPARE(result.filesCopied, 0);
        QCOMPARE(result.filesSkipped, 0);
        // ...not left in the vault to masquerade as a real recording...
        QVERIFY(!QFileInfo::exists(vault.filePath("clip.wav")));
        // ...and reported, by name, as a per-file error rather than silently.
        QCOMPARE(result.errors.size(), 1);
        QVERIFY(result.errors.first().contains(QStringLiteral("clip.wav")));
    }

    // A partial copy is a per-file error, NOT a fatal one: the rest of the
    // device still gets grabbed. Pins the failure as non-fatal so a single
    // unreadable file can't cost the user the whole run.
    void copyThatDoesNotLandWhole_doesNotAbortTheRestOfTheGrab()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(QFile::link(QStringLiteral("/proc/self/status"), device.filePath("bad.wav")));
        QVERIFY(writeFile(device.filePath("good.wav"), "real-audio"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 1); // the healthy file still made it
        QCOMPARE(result.errors.size(), 1);
        QVERIFY(QFileInfo::exists(vault.filePath("good.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("bad.wav")));
    }

    // A grab immediately followed by a preview must agree: the files just
    // copied all report "duplicate", never "grab". Pins the two modes to the
    // same dedup rule end-to-end (they share one code path, and this is what
    // stops them silently drifting apart).
    void previewAfterGrab_reportsEverythingAsDuplicate()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("take1.wav"), "audio"));
        QVERIFY(setMtime(device.filePath("take1.wav"), fixedMtime()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        const UsbGrabScanResult grab = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);
        QCOMPARE(grab.filesCopied, 1);

        const UsbGrabScanResult preview = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/true);

        const QVariantList entries = preview.previewEntries;
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().toMap().value(QStringLiteral("status")).toString(),
                 QStringLiteral("duplicate"));
    }

    // The accepted tradeoff of the quick-check, pinned deliberately so it's a
    // decision on record rather than a surprise: a file whose bytes changed
    // while its name, size AND mtime all stayed identical is wrongly seen as
    // already-grabbed. Field recorders don't produce that (a re-recording
    // changes size and stamps a new mtime), and the cost is a missed import
    // the user can see in the preview — never an overwrite or data loss.
    // If this test ever starts failing, the dedup got STRONGER; see
    // DESIGN.md's "Why not a content hash" note before changing it back.
    void quickCheck_identicalNameSizeAndMtime_differentBytes_isTreatedAsDuplicate()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(vault.filePath("clip.wav"), "AAAAAAAAAA"));
        QVERIFY(writeFile(device.filePath("clip.wav"), "BBBBBBBBBB")); // same size, different bytes
        QVERIFY(setMtime(vault.filePath("clip.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("clip.wav"), fixedMtime()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 0);  // copied nothing
        QCOMPARE(result.filesSkipped, 1); // skipped as "already present"
        // The vault's original is untouched either way — nothing is overwritten.
        QFile original(vault.filePath("clip.wav"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("AAAAAAAAAA"));
    }

    // A destinationVaultPath that doesn't exist yet (i.e. UsbGrabberController
    // resolved a rule's destinationFolder to a not-yet-created subfolder) gets
    // created lazily by the existing per-file mkpath() call — no separate
    // folder-creation step is needed.
    void destinationSubfolder_createdLazilyOnFirstCopy()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("clip.wav"), "x"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        const QString destination = vault.filePath("Interviews/USB");
        QVERIFY(!QFileInfo::exists(destination));

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), destination, /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 1);
        QVERIFY(QFileInfo::exists(destination + "/clip.wav"));
    }

    // (The old completion_recordsHistoryEntryInDestinationVault lived here. It
    // wasn't dropped — it MOVED, along with the behavior: writing the history
    // entry is now UsbGrabberController::handleUsbGrabFinished()'s job, which
    // runUsbGrabScan() has no handle on and no business knowing about. See
    // tst_usbgrabbercontroller.cpp's grab_writesHistoryEntryForTheDeviceThatRan,
    // which specs it end-to-end through a real queued grab.)

    // Defence in depth behind UsbDeviceMonitor's external-media filter (tested in
    // tst_usbdevicemonitor.cpp): even if a scan root somehow resolves to "/", the
    // job itself refuses rather than recursively copying the entire filesystem
    // into the vault. Pure path logic, so it's checked directly here instead of
    // by actually pointing a job at "/".
    void refusalReason_rejectsFilesystemRoot()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(!usbGrabRefusalReason(QStringLiteral("/"), vault.path()).isEmpty());
    }

    // "/" reached via a symlink or ".." must be refused too — the check
    // canonicalises before comparing, so it can't be dodged lexically.
    void refusalReason_rejectsFilesystemRootReachedIndirectly()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(!usbGrabRefusalReason(QStringLiteral("/usr/.."), vault.path()).isEmpty());

        QTemporaryDir linkDir;
        QVERIFY(linkDir.isValid());
        const QString linkToRoot = linkDir.filePath("root-link");
        QVERIFY(QFile::link(QStringLiteral("/"), linkToRoot));
        QVERIFY(!usbGrabRefusalReason(linkToRoot, vault.path()).isEmpty());
    }

    // A destination vault sitting inside the scan root makes the walk feed itself
    // the files it is copying.
    void refusalReason_rejectsDestinationInsideScanRoot()
    {
        QTemporaryDir device;
        QVERIFY(device.isValid());
        QVERIFY(!usbGrabRefusalReason(device.path(), device.filePath("Vault")).isEmpty());
        // Same path on both sides is the degenerate case of the same problem.
        QVERIFY(!usbGrabRefusalReason(device.path(), device.path()).isEmpty());
    }

    // The containment check is a path-COMPONENT test: a sibling whose name merely
    // shares a string prefix with the scan root is not inside it, and refusing it
    // would block a legitimate grab.
    void refusalReason_allowsSiblingWithSharedNamePrefix()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString scanRoot = parent.filePath("rec");
        const QString destination = parent.filePath("recordings");
        QVERIFY(QDir().mkpath(scanRoot));
        QVERIFY(QDir().mkpath(destination));
        QCOMPARE(usbGrabRefusalReason(scanRoot, destination), QString());
    }

    // A rule pointing at a startFolder that isn't on the device is a
    // misconfiguration, not an empty device — say so rather than silently
    // reporting a successful zero-file grab.
    void refusalReason_rejectsNonExistentScanRoot()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(!usbGrabRefusalReason(device.filePath("no-such-folder"), vault.path()).isEmpty());
    }

    // A not-yet-created destination subfolder is normal (mkpath happens at copy
    // time — see destinationSubfolder_createdLazilyOnFirstCopy above) and must not
    // itself trip the refusal.
    void refusalReason_allowsNotYetCreatedDestinationSubfolder()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QCOMPARE(usbGrabRefusalReason(device.path(), vault.filePath("Interviews/USB")), QString());
    }

    // ...but a not-yet-created destination that would land INSIDE the scan root is
    // still refused — "doesn't exist yet" must not become a hole in the check.
    void refusalReason_rejectsNotYetCreatedDestinationInsideScanRoot()
    {
        QTemporaryDir device;
        QVERIFY(device.isValid());
        QVERIFY(!usbGrabRefusalReason(device.path(), device.filePath("Nested/Vault")).isEmpty());
    }

    // End-to-end: a refused run reports the reason through the result's refusal
    // and copies nothing. Uses a destination inside the scan root (a refusal that's
    // safe to actually execute, unlike pointing a real job at "/").
    void refusedRun_reportsErrorAndCopiesNothing()
    {
        QTemporaryDir device;
        QVERIFY(device.isValid());
        QVERIFY(writeFile(device.filePath("clip.wav"), "x"));
        const QString destination = device.filePath("Vault");

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("REFUSE-SERIAL");

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), destination, /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 0);  // copied
        QCOMPARE(result.filesSkipped, 0); // skipped
        // The reason now rides its own field rather than being stuffed into the
        // per-file errors list, which stays empty: a refusal isn't a file that
        // failed to copy, and the caller has to tell them apart to report the
        // run honestly.
        QVERIFY(!result.refusal.isEmpty());
        QVERIFY(result.errors.isEmpty());
        QVERIFY(!QFileInfo::exists(destination));
    }

    // A refused PREVIEW surfaces the reason through previewReady()'s error rather
    // than returning an empty list, which would be indistinguishable from a device
    // with nothing on it. Kept on the threaded UsbGrabPreviewJob deliberately:
    // this is the one place that specs the signal contract itself — that
    // UsbGrabScanResult::refusal reaches the GUI thread as previewReady()'s
    // error argument, and doesn't quietly arrive as an empty entries list.
    void refusedPreview_reportsErrorRatherThanEmptyList()
    {
        QTemporaryDir device;
        QVERIFY(device.isValid());
        QVERIFY(writeFile(device.filePath("clip.wav"), "x"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("REFUSE-SERIAL");

        UsbGrabPreviewJob job;
        QSignalSpy previewSpy(&job, &UsbGrabPreviewJob::previewReady);
        job.start(rule, device.path(), device.filePath("Vault"));
        QVERIFY(previewSpy.wait(10000));

        QVERIFY(previewSpy.first().at(0).toList().isEmpty());
        QVERIFY(!previewSpy.first().at(1).toString().isEmpty());
    }

    // --- filenameRegex (UsbGrabRule::filenameRegex) -----------------------
    // Same engine and same semantics as the split-recording combine rules:
    // QRegularExpression, matched against the FILE NAME only, unanchored.

    void filenameRegex_copiesOnlyMatchingFiles()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("REC_0042.wav"), "keep"));
        QVERIFY(writeFile(device.filePath("MEMO_0042.wav"), "drop"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.filenameRegex = QStringLiteral("^REC_\\d+");

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 1); // filesCopied
        QVERIFY(QFileInfo::exists(vault.filePath("REC_0042.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("MEMO_0042.wav")));
    }

    // Empty means "no regex constraint", NOT "match nothing" — the same
    // reading extensionFilter gets (see emptyExtensionFilter_... above).
    void emptyFilenameRegex_meansAllFilesNotZeroFiles()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("a.wav"), "a"));
        QVERIFY(writeFile(device.filePath("b.txt"), "b"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        // filenameRegex left empty deliberately.

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 2);
    }

    // The two filters AND: a file must clear BOTH. Each stays independently
    // "no constraint" when blank, so neither can silently widen the other.
    void filenameRegexAndExtensionFilter_bothMustPass()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("REC_1.wav"), "both"));   // ext ok, regex ok  -> grab
        QVERIFY(writeFile(device.filePath("MEMO_1.wav"), "ext"));   // ext ok, regex no  -> filtered
        QVERIFY(writeFile(device.filePath("REC_2.txt"), "regex"));  // ext no, regex ok  -> filtered

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.extensionFilter = {QStringLiteral("wav")};
        rule.filenameRegex = QStringLiteral("^REC_");

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 1);
        QVERIFY(QFileInfo::exists(vault.filePath("REC_1.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("MEMO_1.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("REC_2.txt")));
    }

    // Matched against the file name, never the path — so a pattern naming a
    // FOLDER matches nothing, and a file deep in a matching folder is judged
    // only on its own name. Mirrors matchRuleCapturedKey()'s use of
    // QFileInfo::fileName() for combine rules.
    void filenameRegex_matchesFileNameNotPath()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("DCIM/clip.wav"), "in-dcim"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.preserveStructure = false;
        rule.filenameRegex = QStringLiteral("DCIM"); // a FOLDER name, not a file name

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 0);
        QVERIFY(!QFileInfo::exists(vault.filePath("clip.wav")));
    }

    // Unanchored unless the pattern says otherwise, exactly like the combine
    // rules — "REC" finds REC anywhere in the name. This is why the field's
    // hint text calls out ^ and $.
    void filenameRegex_isUnanchoredUnlessAnchored()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("my_REC_tail.wav"), "substring"));

        UsbGrabRule unanchored;
        unanchored.deviceSerial = QStringLiteral("TEST-SERIAL");
        unanchored.filenameRegex = QStringLiteral("REC");

        const UsbGrabScanResult result = runUsbGrabScan(unanchored, device.path(), vault.path(), /*dryRun=*/false);
        QCOMPARE(result.filesCopied, 1);

        // ...and anchoring it excludes the very same file.
        QTemporaryDir vault2;
        QVERIFY(vault2.isValid());

        UsbGrabRule anchored = unanchored;
        anchored.filenameRegex = QStringLiteral("^REC");

        const UsbGrabScanResult result2 = runUsbGrabScan(anchored, device.path(), vault2.path(), /*dryRun=*/false);
        QCOMPARE(result2.filesCopied, 0);
    }

    // Fails CLOSED. An unparseable pattern must never be read as "no
    // constraint": on the "*" catch-all rule that would bulk-copy a whole
    // device on a typo. Copying nothing is recoverable and is what the
    // pattern field's own ✗ indicator already warns about.
    void invalidFilenameRegex_copiesNothing()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("clip.wav"), "audio"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.filenameRegex = QStringLiteral("(unterminated[");

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/false);

        QCOMPARE(result.filesCopied, 0);
        QVERIFY(!QFileInfo::exists(vault.filePath("clip.wav")));
    }

    // The preview must classify a regex-rejected file as "filtered" just like
    // an extension-rejected one — one status covers both filters, and the
    // preview still lists EVERY file the scan saw.
    void preview_regexRejectedFileIsFiltered()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("REC_1.wav"), "keep"));
        QVERIFY(writeFile(device.filePath("MEMO_1.wav"), "drop"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.filenameRegex = QStringLiteral("^REC_");

        const UsbGrabScanResult result = runUsbGrabScan(rule, device.path(), vault.path(), /*dryRun=*/true);

        const QVariantList entries = result.previewEntries;
        QCOMPARE(entries.size(), 2);
        QHash<QString, QString> statusByPath;
        for (const QVariant &entryVariant : entries) {
            const QVariantMap entry = entryVariant.toMap();
            statusByPath.insert(entry.value(QStringLiteral("path")).toString(),
                                entry.value(QStringLiteral("status")).toString());
        }
        QCOMPARE(statusByPath.value(QStringLiteral("REC_1.wav")), QStringLiteral("grab"));
        QCOMPARE(statusByPath.value(QStringLiteral("MEMO_1.wav")), QStringLiteral("filtered"));

        // Nothing was copied — a preview never writes.
        QVERIFY(!QFileInfo::exists(vault.filePath("REC_1.wav")));
    }
};

QTEST_GUILESS_MAIN(TstUsbGrabJob)
#include "tst_usbgrabjob.moc"
