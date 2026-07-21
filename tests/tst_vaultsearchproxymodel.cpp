// Living spec: VaultSearchProxyModel — Item 9's sidebar search behavior.
// Wraps VaultTreeModel to sort matches to the top of their sibling list
// (never hiding non-matches) and expose a "matchesSearch" role that
// propagates up through directories containing a matching descendant.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>

#include "VaultSearchProxyModel.h"
#include "VaultTreeModel.h"

namespace {

bool touch(const QString &path, const QByteArray &content = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content);
    return true;
}

int roleByName(const QAbstractItemModel &m, const QByteArray &roleName)
{
    const QHash<int, QByteArray> roles = m.roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        if (it.value() == roleName)
            return it.key();
    }
    return -1;
}

QModelIndex findByName(const QAbstractItemModel &m, const QString &name, const QModelIndex &parent = QModelIndex())
{
    const int nameRole = roleByName(m, "name");
    for (int row = 0; row < m.rowCount(parent); ++row) {
        const QModelIndex idx = m.index(row, 0, parent);
        if (m.data(idx, nameRole).toString() == name)
            return idx;
        const QModelIndex hit = findByName(m, name, idx);
        if (hit.isValid())
            return hit;
    }
    return {};
}

QStringList siblingNamesInOrder(const QAbstractItemModel &m, const QModelIndex &parent = QModelIndex())
{
    QStringList names;
    const int nameRole = roleByName(m, "name");
    for (int row = 0; row < m.rowCount(parent); ++row)
        names << m.data(m.index(row, 0, parent), nameRole).toString();
    return names;
}

// Collect a QPersistentModelIndex for every node in a nested tree. A live
// TreeView holds exactly these while expanded; a headless test doesn't, and
// they are what the reset-reentrancy regression below hinges on. It's not
// enough to merely realize the proxy's lazy source mapping (recursive index()
// calls): the crash is in QSortFilterProxyModel::invalidate() ->
// store_persistent_indexes(), which only walks (and dereferences the source
// side of) proxy persistent indexes that something is actually HOLDING. With
// none held, invalidate() has nothing to walk and the freed-Node* access never
// happens — so the test must keep these alive across the reset.
void collectPersistent(const QAbstractItemModel &m, QList<QPersistentModelIndex> &out,
                       const QModelIndex &parent = QModelIndex())
{
    for (int row = 0; row < m.rowCount(parent); ++row) {
        const QModelIndex idx = m.index(row, 0, parent);
        out << QPersistentModelIndex(idx);
        collectPersistent(m, out, idx);
    }
}

} // namespace

class TstVaultSearchProxyModel : public QObject
{
    Q_OBJECT

private slots:
    // With no query, the proxy's row order and count exactly mirror the
    // source model — it's a pass-through until a search begins.
    void noQuery_mirrorsSourceOrder()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("alpha.mp3")));
        QVERIFY(touch(vault.filePath("beta.mp3")));
        QVERIFY(touch(vault.filePath("gamma.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);

        QCOMPARE(siblingNamesInOrder(proxy), siblingNamesInOrder(source));
    }

    // filterAcceptsRow never hides rows: a query that matches nothing still
    // leaves every row present (Item 9 asks for "sort to top", not hiding).
    void query_neverHidesNonMatches()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("alpha.mp3")));
        QVERIFY(touch(vault.filePath("beta.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setSearchQuery(QStringLiteral("zzz-no-match"));

        QCOMPARE(proxy.rowCount(), 2);
    }

    // A name match sorts above a non-matching sibling, regardless of the
    // original alphabetical order.
    void nameMatch_sortsAboveNonMatch()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("alpha.mp3")));
        QVERIFY(touch(vault.filePath("zzz-budget.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setSearchQuery(QStringLiteral("budget"));

        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"zzz-budget.mp3", "alpha.mp3"}));
    }

    // Label/tag matches sort to the top too, not just filename matches.
    void labelMatch_sortsAboveNonMatch()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("aaa.mp3")));
        QVERIFY(touch(vault.filePath("zzz.mp3")));
        QVERIFY(touch(vault.filePath("zzz.mp3.md"), "---\nlabel: Budget Review\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setSearchQuery(QStringLiteral("budget"));

        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"zzz.mp3", "aaa.mp3"}));
    }

    // Transcript-body matches only count when includeTranscriptions is true.
    void transcriptMatch_onlyCountsWhenIncludeTranscriptionsEnabled()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("aaa.mp3")));
        QVERIFY(touch(vault.filePath("zzz.mp3")));
        QVERIFY(touch(vault.filePath("zzz.mp3.md"),
                      "---\n---\n\n[00:00:00.000 - 00:00:01.000] the quarterly budget numbers\n"));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setIncludeTranscriptions(false);
        proxy.setSearchQuery(QStringLiteral("budget"));
        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"aaa.mp3", "zzz.mp3"}));

        proxy.setIncludeTranscriptions(true);
        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"zzz.mp3", "aaa.mp3"}));
    }

    // A directory containing a matching descendant sorts above a directory
    // with no matches anywhere in its subtree, even though neither
    // directory's own name matches.
    void directoryWithMatchingDescendant_sortsAboveDirectoryWithNoMatches()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("aaa-empty/song.mp3")));
        QVERIFY(touch(vault.filePath("zzz-hit/budget.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setSearchQuery(QStringLiteral("budget"));

        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"zzz-hit", "aaa-empty"}));
    }

    // matchesSearch role: true for a direct match, true for a directory with
    // a matching descendant, false otherwise; always true when the query is
    // empty (nothing should appear dimmed with no active search).
    void matchesSearchRole_reflectsDirectAndSubtreeMatches()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("hit-dir/budget.mp3")));
        QVERIFY(touch(vault.filePath("miss.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);

        const int matchesRole = roleByName(proxy, "matchesSearch");
        QVERIFY(matchesRole >= 0);

        QVERIFY(proxy.data(findByName(proxy, "miss.mp3"), matchesRole).toBool());

        proxy.setSearchQuery(QStringLiteral("budget"));
        QVERIFY(proxy.data(findByName(proxy, "budget.mp3"), matchesRole).toBool());
        QVERIFY(proxy.data(findByName(proxy, "hit-dir"), matchesRole).toBool());
        QVERIFY(!proxy.data(findByName(proxy, "miss.mp3"), matchesRole).toBool());
    }

    // Item 16/17: unlike searchQuery, tagFilter actually HIDES rows that
    // don't have every requested tag.
    void tagFilter_hidesRowsMissingAnyRequestedTag()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("both.mp3")));
        QVERIFY(touch(vault.filePath("both.mp3.md"), "---\ntags: finance, planning\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));
        QVERIFY(touch(vault.filePath("onlyfinance.mp3")));
        QVERIFY(touch(vault.filePath("onlyfinance.mp3.md"), "---\ntags: finance\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));
        QVERIFY(touch(vault.filePath("untagged.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setTagFilter({QStringLiteral("finance"), QStringLiteral("planning")});

        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"both.mp3"}));
    }

    // A directory is kept if any descendant satisfies the full tag filter,
    // even though the directory itself has no tags of its own.
    void tagFilter_keepsDirectoryWithMatchingDescendant()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("keep/hit.mp3")));
        QVERIFY(touch(vault.filePath("keep/hit.mp3.md"), "---\ntags: finance\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));
        QVERIFY(touch(vault.filePath("drop/miss.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setTagFilter({QStringLiteral("finance")});

        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"keep"}));
    }

    // Empty tagFilter (the default / cleared state) hides nothing.
    void emptyTagFilter_hidesNothing()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a.mp3")));
        QVERIFY(touch(vault.filePath("b.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);

        QCOMPARE(proxy.rowCount(), 2);
    }

    // speakerFilter mirrors tagFilter exactly (same AND-all-requested,
    // hides-non-matching-rows shape), just against VaultTreeModel's
    // SpeakersRole ("speakers:" frontmatter) instead of TagsRole.
    void speakerFilter_hidesRowsMissingAnyRequestedSpeaker()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("both.mp3")));
        QVERIFY(touch(vault.filePath("both.mp3.md"),
                      "---\nspeakers: Alice, Bob\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));
        QVERIFY(touch(vault.filePath("onlyalice.mp3")));
        QVERIFY(touch(vault.filePath("onlyalice.mp3.md"),
                      "---\nspeakers: Alice\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));
        QVERIFY(touch(vault.filePath("nospeakers.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setSpeakerFilter({QStringLiteral("Alice"), QStringLiteral("Bob")});

        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"both.mp3"}));
    }

    // tagFilter and speakerFilter are independently AND'd — a row must
    // satisfy BOTH when both are set, not either.
    void tagFilterAndSpeakerFilter_bothMustMatch()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("both.mp3")));
        QVERIFY(touch(vault.filePath("both.mp3.md"),
                      "---\ntags: finance\nspeakers: Alice\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));
        QVERIFY(touch(vault.filePath("onlytag.mp3")));
        QVERIFY(touch(vault.filePath("onlytag.mp3.md"),
                      "---\ntags: finance\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));
        QVERIFY(touch(vault.filePath("onlyspeaker.mp3")));
        QVERIFY(touch(vault.filePath("onlyspeaker.mp3.md"),
                      "---\nspeakers: Alice\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);
        proxy.setTagFilter({QStringLiteral("finance")});
        proxy.setSpeakerFilter({QStringLiteral("Alice")});

        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"both.mp3"}));
    }

    // --- fileSortKeys (Sidebar.qml sort mode + combine-candidate clustering) ---

    // Two paths sharing groupRank (element 0) and clusterKey (element 1) but
    // differing in memberRank (element 2) sort adjacently, ordered by
    // memberRank within the pair — the "cluster then order within the
    // cluster" composite key TreeOrdering::compute() builds (see its doc
    // comment, and tst_tree_ordering.cpp for the producer half of this
    // contract).
    void fileSortKeys_sameGroupRank_ordersByOwnRankAndStaysAdjacent()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("b.mp3")));
        QVERIFY(touch(vault.filePath("a.mp3")));
        QVERIFY(touch(vault.filePath("z.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);

        QVariantMap keys;
        keys.insert(vault.filePath("a.mp3"), QVariantList() << "group1" << "g1" << "a");
        keys.insert(vault.filePath("b.mp3"), QVariantList() << "group1" << "g1" << "b");
        keys.insert(vault.filePath("z.mp3"), QVariantList() << "group0" << "g0" << "z");
        proxy.setFileSortKeys(keys);

        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"z.mp3", "a.mp3", "b.mp3"}));
    }

    // Bug: two DIFFERENT clusters that compute the SAME groupRank used to
    // interleave their members instead of staying clustered — with only
    // [groupRank, memberRank] to compare, element 0 tied and element 1 tied
    // too (each cluster's first member is #1 of its own cluster), so every
    // comparison fell through to the row()-order default, i.e. plain
    // alphabetical.
    //
    // This is the real shape of this app's own "acme mic 3" combine rule: it
    // keys on date+edit/orig, and the recorder writes each pair's edit and
    // orig at the same instant, so the two clusters have byte-identical
    // mtimes and therefore an identical min-mtime groupRank. Alphabetically
    // the members alternate edit/orig, so nothing in either cluster ended up
    // adjacent and every row drew its own isolated combine outline.
    // clusterKey (element 1) is what keeps the two apart.
    void fileSortKeys_distinctClustersSharingGroupRank_dontInterleave()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("mic1_edit.wav")));
        QVERIFY(touch(vault.filePath("mic1_orig.wav")));
        QVERIFY(touch(vault.filePath("mic2_edit.wav")));
        QVERIFY(touch(vault.filePath("mic2_orig.wav")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);

        // Both clusters share groupRank "r" (identical mtimes); each file is
        // member #1/#2 of its own cluster.
        QVariantMap keys;
        keys.insert(vault.filePath("mic1_edit.wav"), QVariantList() << "r" << "edit" << "1");
        keys.insert(vault.filePath("mic2_edit.wav"), QVariantList() << "r" << "edit" << "2");
        keys.insert(vault.filePath("mic1_orig.wav"), QVariantList() << "r" << "orig" << "1");
        keys.insert(vault.filePath("mic2_orig.wav"), QVariantList() << "r" << "orig" << "2");
        proxy.setFileSortKeys(keys);

        // Each cluster contiguous, in member order — NOT the alphabetical
        // mic1_edit, mic1_orig, mic2_edit, mic2_orig this used to produce.
        QCOMPARE(siblingNamesInOrder(proxy),
                 QStringList({"mic1_edit.wav", "mic2_edit.wav", "mic1_orig.wav", "mic2_orig.wav"}));
    }

    // Directories always sort ahead of files regardless of fileSortKeys —
    // only files get reordered by sort mode/clustering, folders keep their
    // existing dirs-first position.
    void fileSortKeys_directoriesStayFirst()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("aaa.mp3")));
        QVERIFY(touch(vault.filePath("zzz_dir/inside.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);

        QVariantMap keys;
        keys.insert(vault.filePath("aaa.mp3"), QVariantList() << "aaa.mp3" << "aaa.mp3" << "aaa.mp3");
        proxy.setFileSortKeys(keys);

        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"zzz_dir", "aaa.mp3"}));
    }

    // A path with no fileSortKeys entry (e.g. an orphaned .md — combine
    // clustering and date sort are audio-only) doesn't crash or vanish —
    // Bug: it used to fall back to comparing raw row() against keyed
    // siblings, which isn't a valid total order once ANY sibling is keyed
    // (see lessThan()'s doc comment) and could destabilize the WHOLE
    // sibling list's order, not just the keyless entry's own position. Keyed
    // files (here, "aaa.mp3" — alphabetically first, but it has a key
    // claiming last place) now always sort before every keyless file
    // regardless of either's own value, so "orphaned.md" (no key) lands
    // after it despite "orphaned.md" < "aaa.mp3" alphabetically.
    void fileSortKeys_pathWithNoEntry_alwaysSortsAfterKeyedSiblings()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("orphaned.md")));
        QVERIFY(touch(vault.filePath("aaa.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);

        QVariantMap keys;
        keys.insert(vault.filePath("aaa.mp3"), QVariantList() << "zzz" << "zzz" << "zzz");
        proxy.setFileSortKeys(keys);

        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"aaa.mp3", "orphaned.md"}));
    }

    // Regression (segfault): a handler that reacts to the SOURCE model's reset
    // by writing one of this proxy's properties must not crash. Live repro was
    // moving a file into/out of a nested folder: VaultTreeModel::rebuild()
    // frees the whole Node tree and re-scans (a full begin/endResetModel),
    // while Sidebar.qml's `Connections { target: sidebar.model } onModelReset`
    // synchronously re-derived fileSortKeys and wrote them back here. The
    // setter's invalidate() then walked the proxy's persistent indexes — still
    // carrying raw Node* internalPointers rebuild() had just freed — and
    // dereferenced freed memory in VaultTreeModel::parent(). A flat vault never
    // crashed only because its keys didn't change, so the setter's equality
    // short-circuit skipped the invalidate(); a file crossing a nested folder
    // boundary (sibling folder populated) changed the keys and fired it. The
    // fix defers the invalidate until the source reset finishes and the proxy
    // has remapped onto the fresh tree (VaultSearchProxyModel::onSourceReset).
    //
    // This is a heap use-after-free, so a bare Release build may pass by luck
    // (the freed Node allocation is usually still mapped and the dangling read
    // just returns garbage) — a green plain run is NOT proof. It faults
    // deterministically under AddressSanitizer, which is where the guard's
    // absence shows up as a hard failure; that's the signal that matters.
    void sourceResetReentrantSetter_doesNotCrash()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        // The live-repro shape exactly: a folder ("inner") nested inside
        // another folder ("outer"), whose sibling folder holds an item; the
        // moved file lands in the nested folder.
        QVERIFY(QDir().mkpath(vault.filePath("outer/inner")));
        QVERIFY(touch(vault.filePath("outer/sibling/clip.mp3")));
        QVERIFY(touch(vault.filePath("outer/roamer.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;

        // Connect the reentrant handler to the source reset BEFORE the proxy
        // wires its own reset handling, so it fires while the proxy's mapping
        // still holds the now-freed source indexes — the exact ordering that
        // crashed. Each reset it pushes a DIFFERENT fileSortKeys map so the
        // setter's equality short-circuit can't mask the invalidate().
        int resetCount = 0;
        QObject::connect(&source, &QAbstractItemModel::modelReset, &proxy, [&] {
            QVariantMap keys;
            keys.insert(QStringLiteral("k"),
                        QVariantList() << QString::number(++resetCount) << QStringLiteral("c")
                                       << QStringLiteral("1"));
            proxy.setFileSortKeys(keys);
        });

        proxy.setSourceModel(&source);

        // Hold persistent proxy indexes across the whole nested tree, standing
        // in for the expanded TreeView the crash needed (see collectPersistent).
        QList<QPersistentModelIndex> held;
        collectPersistent(proxy, held);
        QVERIFY(!held.isEmpty());

        // Move triggers rebuild() -> begin/endResetModel; the handler re-enters
        // the proxy mid-reset, and invalidate() walks `held` back to the freed
        // source Nodes. Pre-fix this segfaulted here.
        QVERIFY(source.moveFile(QUrl::fromLocalFile(vault.filePath("outer/roamer.mp3")),
                                QUrl::fromLocalFile(vault.filePath("outer/inner"))));

        // Survived, the tree is consistent, and the deferred invalidate applied
        // the handler's keys.
        QVERIFY(resetCount >= 1);
        QVERIFY(findByName(proxy, "roamer.mp3").isValid());
        QCOMPARE(proxy.fileSortKeys().value(QStringLiteral("k")).toList().size(), 3);
    }

    // A rename applied via VaultTreeModel::applyRenameOutcome must flow through
    // the proxy as an in-place move — NO proxy reset (which is what flashes the
    // whole tree and jumps selection in the real Sidebar) — with the renamed row
    // landing at its new alphabetical position. This is the proxy-layer proof of
    // the "only the name changes" fix; the source-level proof lives in
    // tst_vaulttreemodel::applyRenameOutcome_updatesInPlaceWithoutReset.
    void applyRename_flowsThroughProxyWithoutReset()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath("a.mp3")));
        QVERIFY(touch(vault.filePath("m.mp3")));
        QVERIFY(touch(vault.filePath("z.mp3")));

        VaultTreeModel source;
        source.setVaultPath(QUrl::fromLocalFile(vault.path()));

        VaultSearchProxyModel proxy;
        proxy.setSourceModel(&source);
        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"a.mp3", "m.mp3", "z.mp3"}));

        QSignalSpy proxyResetSpy(&proxy, &QAbstractItemModel::modelReset);

        // Rename a.mp3 -> n.mp3 on disk (as RenameFileTask would) then apply the
        // in-place model update the controller drives on success.
        QVERIFY(QFile::rename(vault.filePath("a.mp3"), vault.filePath("n.mp3")));
        source.applyRenameOutcome(QUrl::fromLocalFile(vault.filePath("a.mp3")),
                                  QUrl::fromLocalFile(vault.filePath("n.mp3")));

        QCOMPARE(proxyResetSpy.count(), 0); // the proxy must NOT reset — no flash
        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"m.mp3", "n.mp3", "z.mp3"}));

        // The watcher echo of the same rename is swallowed at the source, so the
        // proxy still never resets.
        QTest::qWait(300);
        QCOMPARE(proxyResetSpy.count(), 0);
        QCOMPARE(siblingNamesInOrder(proxy), QStringList({"m.mp3", "n.mp3", "z.mp3"}));
    }
};

QTEST_GUILESS_MAIN(TstVaultSearchProxyModel)
#include "tst_vaultsearchproxymodel.moc"
