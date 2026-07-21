// CHARACTERIZATION test for TreeOrdering — the file-tree ordering computation
// lifted out of Sidebar.qml's refreshTreeOrdering() (2026-07-20).
//
// Every expectation below was DERIVED FROM THE JS THAT WAS THERE BEFORE the
// extraction, by hand, and hard-coded here: the point of this file is to pin
// the old behavior so the port is provably behavior-preserving, not to
// re-describe the new code. If one of these ever needs to change, that is a
// deliberate product decision about how the vault tree orders itself — not a
// refactor.
//
// THE CONTRACT THIS GUARDS. sortKeys is a 3-element list of STRINGS —
// [groupRank, clusterKey, memberRank] — consumed by
// VaultSearchProxyModel::lessThan(), which compares element 0, then 1, then 2,
// each as a plain QString. That mirroring used to span two languages in two
// files; it is now producer-and-consumer side by side in src/core/, and these
// tests are what keep the two halves honest. tst_vaultsearchproxymodel.cpp
// covers the CONSUMER half (keys in -> row order out); this file covers the
// PRODUCER half (vault data in -> keys out). Between them the whole path is
// pinned.
//
// The encoding details that are load-bearing and asserted LITERALLY here
// rather than structurally:
//
//   * Numeric ranks are zero-padded to 16 chars, so a lexicographic string
//     compare orders them numerically. 16 = the digit count of JS's
//     Number.MAX_SAFE_INTEGER.
//   * "dateDesc" is encoded as MAX_SAFE_INTEGER - mtime, not by reversing the
//     comparison — so lessThan() never needs to know the sort direction.
//     The exact constant matters: the JS produced String(9007199254740991-ms)
//     and any float-formatted approximation would collide distinct timestamps.
//   * A group's members all share element 0 (the group's MINIMUM ownRank) and
//     element 1 (the group's identity), and are separated only by element 2 —
//     that is what makes them tree-adjacent.
//
// Plain C++ (no QQuickView) on purpose — it's a pure data-in/data-out
// transform. The 10 tst_sidebar_* harnesses stay as the integration check that
// Sidebar.qml is still wired to it correctly.

#include <QtTest>

#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "TreeOrdering.h"

namespace {

QVariantMap group(const QString &outputPath, const QString &status, const QStringList &files)
{
    QVariantMap g;
    g.insert(QStringLiteral("outputPath"), outputPath);
    g.insert(QStringLiteral("status"), status);
    g.insert(QStringLiteral("files"), files);
    return g;
}

QVariantMap pending(const QString &outputPath)
{
    QVariantMap p;
    p.insert(QStringLiteral("outputPath"), outputPath);
    return p;
}

QStringList keyOf(const QVariantMap &out, const QString &path)
{
    return out.value(QStringLiteral("sortKeys")).toMap().value(path).toStringList();
}

// The order the tree actually displays: sort the keyed paths exactly the way
// VaultSearchProxyModel::lessThan() does. Deliberately re-implemented here
// (rather than reading TreeOrdering's own internal sort) so the test asserts
// against the CONSUMER's comparison rule, not against the producer's private
// helper.
QStringList displayOrder(const QVariantMap &out, QStringList paths)
{
    std::stable_sort(paths.begin(), paths.end(), [&](const QString &a, const QString &b) {
        const QStringList ka = keyOf(out, a);
        const QStringList kb = keyOf(out, b);
        for (int i = 0; i < 3; ++i) {
            if (ka.at(i) != kb.at(i))
                return ka.at(i) < kb.at(i);
        }
        return false;
    });
    return paths;
}

const QString kName = QStringLiteral("name");
const QString kDateAsc = QStringLiteral("dateAsc");
const QString kDateDesc = QStringLiteral("dateDesc");
const QString kReady = QStringLiteral("ready");

} // namespace

class TestTreeOrdering : public QObject {
    Q_OBJECT

private slots:
    // --- Empty / degenerate input ---

    // The guarded no-model / no-vault path: Sidebar.qml hands an empty
    // audioPaths list before a vault is open. Every output must still be a
    // present-but-empty map, never undefined — QML reads them with
    // `map[path] !== undefined` and a missing map would throw.
    void emptyVault_yieldsFiveEmptyMaps()
    {
        const QVariantMap out = TreeOrdering::compute({}, {}, {}, {}, kName);
        for (const QString &key : { QStringLiteral("readyPaths"), QStringLiteral("orderPaths"),
                                    QStringLiteral("topMerged"), QStringLiteral("bottomMerged"),
                                    QStringLiteral("sortKeys") }) {
            QVERIFY2(out.contains(key), qPrintable(key));
            QVERIFY2(out.value(key).toMap().isEmpty(), qPrintable(key));
        }
    }

    // --- sortMode "name" ---

    // ownRank for "name" is the BASENAME, lowercased — not the full path. A
    // vault where the directory names would sort differently from the file
    // names is the case that tells those two apart.
    void nameMode_ranksByLowercasedBasename_notFullPath()
    {
        const QStringList paths = { QStringLiteral("/v/zeta/apple.wav"),
                                    QStringLiteral("/v/alpha/Banana.wav") };
        const QVariantMap out = TreeOrdering::compute(paths, {}, {}, {}, kName);

        QCOMPARE(keyOf(out, QStringLiteral("/v/zeta/apple.wav")).at(0), QStringLiteral("apple.wav"));
        // Lowercased: an uppercase B would otherwise sort before every
        // lowercase letter in a raw code-unit compare.
        QCOMPARE(keyOf(out, QStringLiteral("/v/alpha/Banana.wav")).at(0), QStringLiteral("banana.wav"));
        QCOMPARE(displayOrder(out, paths), paths);
    }

    // A path with no slash at all falls back to the whole string.
    void nameMode_pathWithoutSlash_ranksByWholeString()
    {
        const QVariantMap out = TreeOrdering::compute({ QStringLiteral("Loose.WAV") }, {}, {}, {}, kName);
        QCOMPARE(keyOf(out, QStringLiteral("Loose.WAV")).at(0), QStringLiteral("loose.wav"));
    }

    // With no combine groups, an ungrouped file's key is [ownRank, its own
    // PATH, ownRank]. The path in the middle is what stops two files with an
    // identical basename (different folders) from being treated as one
    // cluster.
    void nameMode_ungroupedFile_keyIsOwnRankPathOwnRank()
    {
        const QString p = QStringLiteral("/v/a/take.wav");
        const QVariantMap out = TreeOrdering::compute({ p }, {}, {}, {}, kName);
        QCOMPARE(keyOf(out, p), QStringList({ QStringLiteral("take.wav"), p, QStringLiteral("take.wav") }));
    }

    // Two files sharing a basename in different folders: element 0 ties, and
    // element 1 (the distinct path) is what resolves them — deterministically,
    // never interleaving into some other group.
    void nameMode_identicalBasenames_resolvedByPathIdentity()
    {
        const QStringList paths = { QStringLiteral("/v/b/take.wav"), QStringLiteral("/v/a/take.wav") };
        const QVariantMap out = TreeOrdering::compute(paths, {}, {}, {}, kName);

        QCOMPARE(keyOf(out, paths.at(0)).at(0), keyOf(out, paths.at(1)).at(0));
        QVERIFY(keyOf(out, paths.at(0)).at(1) != keyOf(out, paths.at(1)).at(1));
        QCOMPARE(displayOrder(out, paths),
                 QStringList({ QStringLiteral("/v/a/take.wav"), QStringLiteral("/v/b/take.wav") }));
    }

    // --- sortMode "dateAsc" / "dateDesc" ---

    // The literal encoding, asserted character-for-character. This is the one
    // that a careless number->string port breaks silently: a float formatter
    // would render 9007199254740991-1700000000000 with 6 significant digits
    // (or in scientific notation) and collide every timestamp in the vault
    // into one rank.
    void dateModes_encodeRankAsZeroPadded16CharInteger()
    {
        const QString p = QStringLiteral("/v/a.wav");
        QVariantMap mtimes;
        mtimes.insert(p, QVariant::fromValue<qint64>(1700000000000LL));

        const QVariantMap asc = TreeOrdering::compute({ p }, mtimes, {}, {}, kDateAsc);
        QCOMPARE(keyOf(asc, p).at(0), QStringLiteral("0001700000000000"));

        // 9007199254740991 - 1700000000000 = 9005499254740991. All 16 digits
        // are significant, which is the whole point: this is the value a
        // 6-significant-digit float formatter would have mangled.
        const QVariantMap desc = TreeOrdering::compute({ p }, mtimes, {}, {}, kDateDesc);
        QCOMPARE(keyOf(desc, p).at(0), QStringLiteral("9005499254740991"));
        QCOMPARE(keyOf(desc, p).at(0).length(), 16);
    }

    // dateDesc is achieved purely by the encoding, so lessThan() stays a
    // direction-agnostic ascending string compare.
    void dateModes_ascAndDescProduceOppositeOrders()
    {
        const QStringList paths = { QStringLiteral("/v/old.wav"), QStringLiteral("/v/new.wav") };
        QVariantMap mtimes;
        mtimes.insert(paths.at(0), QVariant::fromValue<qint64>(1000000000000LL));
        mtimes.insert(paths.at(1), QVariant::fromValue<qint64>(1700000000000LL));

        QCOMPARE(displayOrder(TreeOrdering::compute(paths, mtimes, {}, {}, kDateAsc), paths),
                 QStringList({ QStringLiteral("/v/old.wav"), QStringLiteral("/v/new.wav") }));
        QCOMPARE(displayOrder(TreeOrdering::compute(paths, mtimes, {}, {}, kDateDesc), paths),
                 QStringList({ QStringLiteral("/v/new.wav"), QStringLiteral("/v/old.wav") }));
    }

    // A file with no mtime entry ranks as 0 — the JS read `mtimes[path] || 0`
    // and an absent QVariant converts identically. Under dateAsc that puts it
    // at the oldest edge; under dateDesc the subtraction sends it to the
    // newest edge (the widest possible rank).
    void dateModes_missingMtime_ranksAsZero()
    {
        const QString p = QStringLiteral("/v/unstatted.wav");
        QCOMPARE(keyOf(TreeOrdering::compute({ p }, {}, {}, {}, kDateAsc), p).at(0),
                 QStringLiteral("0000000000000000"));
        QCOMPARE(keyOf(TreeOrdering::compute({ p }, {}, {}, {}, kDateDesc), p).at(0),
                 QStringLiteral("9007199254740991"));
    }

    // Equal mtimes tie element 0; element 1 (the path) breaks it, so the order
    // is stable rather than dependent on iteration order.
    void dateModes_equalMtimes_resolvedByPathIdentity()
    {
        const QStringList paths = { QStringLiteral("/v/b.wav"), QStringLiteral("/v/a.wav") };
        QVariantMap mtimes;
        for (const QString &p : paths)
            mtimes.insert(p, QVariant::fromValue<qint64>(1700000000000LL));

        const QVariantMap out = TreeOrdering::compute(paths, mtimes, {}, {}, kDateAsc);
        QCOMPARE(keyOf(out, paths.at(0)).at(0), keyOf(out, paths.at(1)).at(0));
        QCOMPARE(displayOrder(out, paths),
                 QStringList({ QStringLiteral("/v/a.wav"), QStringLiteral("/v/b.wav") }));
    }

    // --- Combine-group clustering ---

    // Clustering applies to ANY 2+-member group regardless of status — a
    // not-currently-combinable group still benefits from its members sitting
    // together. Only readyPaths is status-gated.
    void groups_clusterRegardlessOfStatus_butReadyPathsIsReadyOnly()
    {
        const QStringList paths = { QStringLiteral("/v/a1.wav"), QStringLiteral("/v/mid.wav"),
                                    QStringLiteral("/v/z9.wav") };
        const QVariantList groups = { group(QStringLiteral("/v/out.wav"),
                                            QStringLiteral("alreadyTranscribed"),
                                            { QStringLiteral("/v/a1.wav"), QStringLiteral("/v/z9.wav") }) };
        const QVariantMap out = TreeOrdering::compute(paths, {}, groups, {}, kName);

        // Not ready -> no highlight, no adjacency flags...
        QVERIFY(out.value(QStringLiteral("readyPaths")).toMap().isEmpty());
        QVERIFY(out.value(QStringLiteral("topMerged")).toMap().isEmpty());
        QVERIFY(out.value(QStringLiteral("bottomMerged")).toMap().isEmpty());
        // ...but the badge order map and the clustering both still apply.
        QCOMPARE(out.value(QStringLiteral("orderPaths")).toMap().value(QStringLiteral("/v/z9.wav")).toInt(), 2);
        QCOMPARE(displayOrder(out, paths),
                 QStringList({ QStringLiteral("/v/a1.wav"), QStringLiteral("/v/z9.wav"),
                               QStringLiteral("/v/mid.wav") }));
    }

    // A 1-member group is NOT a cluster — it stays keyed as an ungrouped file
    // (identity = its own path), and earns no orderPaths badge entry.
    void groups_singleMemberGroup_isNotClustered()
    {
        const QString p = QStringLiteral("/v/lone.wav");
        const QVariantList groups = { group(QStringLiteral("/v/out.wav"), kReady, { p }) };
        const QVariantMap out = TreeOrdering::compute({ p }, {}, groups, {}, kName);

        QCOMPARE(keyOf(out, p).at(1), p);
        QVERIFY(out.value(QStringLiteral("orderPaths")).toMap().isEmpty());
        // readyPaths is a separate, non-size-gated sweep — the highlight still
        // applies to a ready group of one.
        QCOMPARE(out.value(QStringLiteral("readyPaths")).toMap().value(p).toString(),
                 QStringLiteral("/v/out.wav"));
    }

    // groupRank is the MINIMUM ownRank among members, so the cluster sorts at
    // its own most-relevant edge — and because ownRank already flips for
    // dateDesc, "minimum" means earliest under name/dateAsc and most-recent
    // under dateDesc, with no direction-specific code.
    void groups_groupRankIsMinimumOwnRank_inBothDirections()
    {
        const QStringList paths = { QStringLiteral("/v/g_old.wav"), QStringLiteral("/v/g_new.wav"),
                                    QStringLiteral("/v/solo.wav") };
        QVariantMap mtimes;
        mtimes.insert(paths.at(0), QVariant::fromValue<qint64>(1000LL));
        mtimes.insert(paths.at(1), QVariant::fromValue<qint64>(3000LL));
        mtimes.insert(paths.at(2), QVariant::fromValue<qint64>(2000LL));
        const QVariantList groups = { group(QStringLiteral("/v/out.wav"), kReady,
                                            { paths.at(0), paths.at(1) }) };

        // dateAsc: group anchors to its OLDEST member (1000) -> before solo.
        const QVariantMap asc = TreeOrdering::compute(paths, mtimes, groups, {}, kDateAsc);
        QCOMPARE(keyOf(asc, paths.at(0)).at(0), QStringLiteral("0000000000001000"));
        QCOMPARE(keyOf(asc, paths.at(0)).at(0), keyOf(asc, paths.at(1)).at(0));
        QCOMPARE(displayOrder(asc, paths),
                 QStringList({ paths.at(0), paths.at(1), paths.at(2) }));

        // dateDesc: same "minimum", but now it resolves to the NEWEST member
        // (3000) — group still lands before solo, which is the point.
        const QVariantMap desc = TreeOrdering::compute(paths, mtimes, groups, {}, kDateDesc);
        QCOMPARE(keyOf(desc, paths.at(0)).at(0), QStringLiteral("9007199254737991"));
        QCOMPARE(displayOrder(desc, paths),
                 QStringList({ paths.at(0), paths.at(1), paths.at(2) }));
    }

    // Members are ordered among themselves by g.files' own combine order
    // (element 2), NEVER by the active sort mode — the first-to-be-combined
    // file is always at the top. Here g.files order is the exact opposite of
    // both alphabetical and mtime order, so anything re-deriving a rank
    // instead of reusing g.files would flip these two.
    void groups_memberOrderFollowsCombineOrder_notSortMode()
    {
        const QStringList paths = { QStringLiteral("/v/a.wav"), QStringLiteral("/v/b.wav") };
        QVariantMap mtimes;
        mtimes.insert(QStringLiteral("/v/a.wav"), QVariant::fromValue<qint64>(1000LL));
        mtimes.insert(QStringLiteral("/v/b.wav"), QVariant::fromValue<qint64>(2000LL));
        // Combine order is b, then a.
        const QVariantList groups = { group(QStringLiteral("/v/out.wav"), kReady,
                                            { QStringLiteral("/v/b.wav"), QStringLiteral("/v/a.wav") }) };

        for (const QString &mode : { kName, kDateAsc, kDateDesc }) {
            const QVariantMap out = TreeOrdering::compute(paths, mtimes, groups, {}, mode);
            QCOMPARE(keyOf(out, QStringLiteral("/v/b.wav")).at(2), QStringLiteral("0000000000000001"));
            QCOMPARE(keyOf(out, QStringLiteral("/v/a.wav")).at(2), QStringLiteral("0000000000000002"));
            QCOMPARE(displayOrder(out, paths),
                     QStringList({ QStringLiteral("/v/b.wav"), QStringLiteral("/v/a.wav") }));
        }

        // The badge map is the SAME numbering — that lockstep is the whole
        // reason memberOrderRank reads groupFileOrder rather than recomputing.
        const QVariantMap orderPaths =
            TreeOrdering::compute(paths, mtimes, groups, {}, kName).value(QStringLiteral("orderPaths")).toMap();
        QCOMPARE(orderPaths.value(QStringLiteral("/v/b.wav")).toInt(), 1);
        QCOMPARE(orderPaths.value(QStringLiteral("/v/a.wav")).toInt(), 2);
    }

    // THE REGRESSION THAT MADE cluster-identity its own tier. Two distinct
    // groups whose members share byte-identical mtimes compute the SAME
    // groupRank (this is the norm for the app's own "acme mic 3" rule, whose
    // edit/orig files are written at the same instant). With only
    // [groupRank, memberRank] the members interleaved; element 1 keeps each
    // cluster contiguous.
    void groups_twoClustersSharingGroupRank_stayContiguous()
    {
        const QStringList paths = { QStringLiteral("/v/d1_edit.wav"), QStringLiteral("/v/d1_orig.wav"),
                                    QStringLiteral("/v/d2_edit.wav"), QStringLiteral("/v/d2_orig.wav") };
        QVariantMap mtimes;
        for (const QString &p : paths)
            mtimes.insert(p, QVariant::fromValue<qint64>(5000LL));
        const QVariantList groups = {
            // outEdit sorts before outOrig, so the edit cluster comes first.
            group(QStringLiteral("/v/outEdit.wav"), kReady,
                  { QStringLiteral("/v/d1_edit.wav"), QStringLiteral("/v/d2_edit.wav") }),
            group(QStringLiteral("/v/outOrig.wav"), kReady,
                  { QStringLiteral("/v/d1_orig.wav"), QStringLiteral("/v/d2_orig.wav") })
        };
        const QVariantMap out = TreeOrdering::compute(paths, mtimes, groups, {}, kDateAsc);

        // Element 0 really is tied across BOTH clusters — the precondition.
        QCOMPARE(keyOf(out, paths.at(0)).at(0), keyOf(out, paths.at(2)).at(0));
        QCOMPARE(keyOf(out, paths.at(0)).at(0), keyOf(out, paths.at(1)).at(0));
        // ...and element 2 is tied too between the two clusters' first
        // members, which is exactly what left nothing to compare before.
        QCOMPARE(keyOf(out, paths.at(0)).at(2), keyOf(out, paths.at(1)).at(2));

        QCOMPARE(displayOrder(out, paths),
                 QStringList({ QStringLiteral("/v/d1_edit.wav"), QStringLiteral("/v/d2_edit.wav"),
                               QStringLiteral("/v/d1_orig.wav"), QStringLiteral("/v/d2_orig.wav") }));
    }

    // A lone file whose ownRank ties a group's groupRank must land cleanly
    // outside the cluster, never inside it — the ungrouped-file identity is
    // its own path, so it sorts wholly before or after the group's block.
    void groups_loneFileTyingGroupRank_landsOutsideTheCluster()
    {
        const QStringList paths = { QStringLiteral("/v/m1.wav"), QStringLiteral("/v/m2.wav"),
                                    QStringLiteral("/v/solo.wav") };
        QVariantMap mtimes;
        for (const QString &p : paths)
            mtimes.insert(p, QVariant::fromValue<qint64>(7000LL));
        const QVariantList groups = { group(QStringLiteral("/v/out.wav"), kReady,
                                            { QStringLiteral("/v/m1.wav"), QStringLiteral("/v/m2.wav") }) };
        const QVariantMap out = TreeOrdering::compute(paths, mtimes, groups, {}, kDateAsc);

        QCOMPARE(keyOf(out, QStringLiteral("/v/solo.wav")).at(0), keyOf(out, QStringLiteral("/v/m1.wav")).at(0));
        const QStringList order = displayOrder(out, paths);
        // "/v/out.wav" < "/v/solo.wav", so the whole cluster precedes solo.
        QCOMPARE(order, QStringList({ QStringLiteral("/v/m1.wav"), QStringLiteral("/v/m2.wav"),
                                      QStringLiteral("/v/solo.wav") }));
    }

    // --- Adjacency (topMerged / bottomMerged) ---

    // Adjacency is computed from DISPLAY order, not allAudioPaths()' raw
    // order: interior members get both flags, the ends get one each. The bug
    // this pins is a 3-member run rendering three isolated outline boxes
    // instead of one merged run.
    void adjacency_readyRunFlagsInteriorBothEnds()
    {
        // Raw order deliberately interleaves a non-member between two members.
        const QStringList paths = { QStringLiteral("/v/g1.wav"), QStringLiteral("/v/other.wav"),
                                    QStringLiteral("/v/g2.wav"), QStringLiteral("/v/g3.wav") };
        QVariantMap mtimes;
        mtimes.insert(QStringLiteral("/v/g1.wav"), QVariant::fromValue<qint64>(1000LL));
        mtimes.insert(QStringLiteral("/v/other.wav"), QVariant::fromValue<qint64>(1500LL));
        mtimes.insert(QStringLiteral("/v/g2.wav"), QVariant::fromValue<qint64>(2000LL));
        mtimes.insert(QStringLiteral("/v/g3.wav"), QVariant::fromValue<qint64>(3000LL));
        const QVariantList groups = { group(QStringLiteral("/v/out.wav"), kReady,
                                            { QStringLiteral("/v/g1.wav"), QStringLiteral("/v/g2.wav"),
                                              QStringLiteral("/v/g3.wav") }) };
        const QVariantMap out = TreeOrdering::compute(paths, mtimes, groups, {}, kDateAsc);

        // The cluster really did pull together despite the raw interleave.
        QCOMPARE(displayOrder(out, paths),
                 QStringList({ QStringLiteral("/v/g1.wav"), QStringLiteral("/v/g2.wav"),
                               QStringLiteral("/v/g3.wav"), QStringLiteral("/v/other.wav") }));

        const QVariantMap top = out.value(QStringLiteral("topMerged")).toMap();
        const QVariantMap bottom = out.value(QStringLiteral("bottomMerged")).toMap();
        QVERIFY(!top.contains(QStringLiteral("/v/g1.wav")));
        QVERIFY(bottom.value(QStringLiteral("/v/g1.wav")).toBool());
        QVERIFY(top.value(QStringLiteral("/v/g2.wav")).toBool());
        QVERIFY(bottom.value(QStringLiteral("/v/g2.wav")).toBool());
        QVERIFY(top.value(QStringLiteral("/v/g3.wav")).toBool());
        QVERIFY(!bottom.contains(QStringLiteral("/v/g3.wav")));
        // A non-member never gets a flag, even sitting right beside the run.
        QVERIFY(!top.contains(QStringLiteral("/v/other.wav")));
        QVERIFY(!bottom.contains(QStringLiteral("/v/other.wav")));
    }

    // Two ready groups that end up adjacent in display order must NOT merge
    // their outlines across the seam — the flags compare outputPath, not mere
    // membership.
    void adjacency_neighbouringDifferentGroups_dontMergeAcrossTheSeam()
    {
        const QStringList paths = { QStringLiteral("/v/a1.wav"), QStringLiteral("/v/a2.wav"),
                                    QStringLiteral("/v/b1.wav"), QStringLiteral("/v/b2.wav") };
        const QVariantList groups = {
            group(QStringLiteral("/v/outA.wav"), kReady,
                  { QStringLiteral("/v/a1.wav"), QStringLiteral("/v/a2.wav") }),
            group(QStringLiteral("/v/outB.wav"), kReady,
                  { QStringLiteral("/v/b1.wav"), QStringLiteral("/v/b2.wav") })
        };
        const QVariantMap out = TreeOrdering::compute(paths, {}, groups, {}, kName);

        QCOMPARE(displayOrder(out, paths), paths);
        const QVariantMap top = out.value(QStringLiteral("topMerged")).toMap();
        const QVariantMap bottom = out.value(QStringLiteral("bottomMerged")).toMap();
        // The seam: a2 is last of group A, b1 is first of group B.
        QVERIFY(!bottom.contains(QStringLiteral("/v/a2.wav")));
        QVERIFY(!top.contains(QStringLiteral("/v/b1.wav")));
        // ...while each group's own internal edge is still flagged.
        QVERIFY(bottom.value(QStringLiteral("/v/a1.wav")).toBool());
        QVERIFY(top.value(QStringLiteral("/v/a2.wav")).toBool());
    }

    // --- Pending combine placeholders ---

    // A placeholder isn't in audioPaths but still gets a key, keyed exactly as
    // an ungrouped file at its own output path — so it sits where the merged
    // file will land instead of falling into the proxy's keyless tier (which
    // sorts after every real audio file, making the row jump to the bottom and
    // back).
    void pending_placeholderKeyedAsUngroupedFileAtItsOwnPath()
    {
        const QStringList paths = { QStringLiteral("/v/a.wav"), QStringLiteral("/v/z.wav") };
        const QString out1 = QStringLiteral("/v/m.wav");
        const QVariantMap out = TreeOrdering::compute(paths, {}, {}, { pending(out1) }, kName);

        QCOMPARE(keyOf(out, out1),
                 QStringList({ QStringLiteral("m.wav"), out1, QStringLiteral("m.wav") }));
        // And it sorts into place among the real files, not after them.
        QCOMPARE(displayOrder(out, paths + QStringList{ out1 }),
                 QStringList({ QStringLiteral("/v/a.wav"), out1, QStringLiteral("/v/z.wav") }));
    }

    // The documented approximation: a placeholder has no mtime, so under the
    // date modes it ranks as 0 and parks at one edge for the seconds it
    // exists. Pinned so the compromise stays a decision rather than a
    // surprise.
    void pending_underDateModes_ranksAtTheZeroMtimeEdge()
    {
        const QString out1 = QStringLiteral("/v/m.wav");
        const QVariantMap asc = TreeOrdering::compute({}, {}, {}, { pending(out1) }, kDateAsc);
        QCOMPARE(keyOf(asc, out1).at(0), QStringLiteral("0000000000000000"));
        const QVariantMap desc = TreeOrdering::compute({}, {}, {}, { pending(out1) }, kDateDesc);
        QCOMPARE(keyOf(desc, out1).at(0), QStringLiteral("9007199254740991"));
    }

    // A placeholder is not audio, so it takes part in no group and never gets
    // an adjacency flag — but it must not disturb the flags of the real rows
    // around it either.
    void pending_placeholderDoesNotAffectAdjacencyOfRealRows()
    {
        const QStringList paths = { QStringLiteral("/v/g1.wav"), QStringLiteral("/v/g2.wav") };
        const QVariantList groups = { group(QStringLiteral("/v/out.wav"), kReady, paths) };
        const QVariantMap out =
            TreeOrdering::compute(paths, {}, groups, { pending(QStringLiteral("/v/g15.wav")) }, kName);

        QVERIFY(out.value(QStringLiteral("bottomMerged")).toMap().value(paths.at(0)).toBool());
        QVERIFY(out.value(QStringLiteral("topMerged")).toMap().value(paths.at(1)).toBool());
        QVERIFY(!out.value(QStringLiteral("topMerged")).toMap().contains(QStringLiteral("/v/g15.wav")));
    }

    // --- Every path gets a key ---

    // The proxy splits on "has a well-formed 3-element key" as its FIRST
    // ordering tier, so a missing or wrong-length entry silently demotes that
    // file below every sibling. Every audio path and every placeholder must
    // come back with exactly 3 elements.
    void everyPathGetsAThreeElementKey()
    {
        const QStringList paths = { QStringLiteral("/v/a.wav"), QStringLiteral("/v/b.wav"),
                                    QStringLiteral("/v/c.wav") };
        const QVariantList groups = { group(QStringLiteral("/v/out.wav"), kReady,
                                            { QStringLiteral("/v/a.wav"), QStringLiteral("/v/b.wav") }) };
        const QVariantMap out =
            TreeOrdering::compute(paths, {}, groups, { pending(QStringLiteral("/v/p.wav")) }, kName);

        const QVariantMap sortKeys = out.value(QStringLiteral("sortKeys")).toMap();
        QCOMPARE(sortKeys.size(), 4);
        for (auto it = sortKeys.constBegin(); it != sortKeys.constEnd(); ++it) {
            const QVariantList key = it.value().toList();
            QCOMPARE(key.size(), 3);
            for (const QVariant &element : key)
                QCOMPARE(element.metaType().id(), QMetaType::QString);
        }
    }
};

QTEST_MAIN(TestTreeOrdering)
#include "tst_tree_ordering.moc"
