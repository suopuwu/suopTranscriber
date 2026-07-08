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
        QVERIFY(touch(vault.filePath("zzz.md"), "---\nlabel: Budget Review\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));

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
        QVERIFY(touch(vault.filePath("zzz.md"),
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
        QVERIFY(touch(vault.filePath("both.md"), "---\ntags: finance, planning\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));
        QVERIFY(touch(vault.filePath("onlyfinance.mp3")));
        QVERIFY(touch(vault.filePath("onlyfinance.md"), "---\ntags: finance\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));
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
        QVERIFY(touch(vault.filePath("keep/hit.md"), "---\ntags: finance\n---\n\n[00:00:00.000 - 00:00:01.000] hi\n"));
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
};

QTEST_GUILESS_MAIN(TstVaultSearchProxyModel)
#include "tst_vaultsearchproxymodel.moc"
