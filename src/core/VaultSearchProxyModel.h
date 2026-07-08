#pragma once

#include <QSortFilterProxyModel>
#include <QStringList>
#include <qqml.h>

// Item 9: sidebar search sort + match highlighting/snippet support. Wraps a
// VaultTreeModel (a tree, not a flat list) so matches sort to the top of
// their sibling list without ever hiding non-matches — the sidebar already
// dims non-matches visually rather than removing them, so filterAcceptsRow()
// always accepts free-text (searchQuery) matching.
//
// Item 16/17 (Wave E): tagFilter is a SEPARATE, harder filter — unlike free
// text search, a tag filter genuinely HIDES rows that don't have every
// requested tag (a directory is kept if ANY descendant has all the
// requested tags, same recursive-match shape as matchesSearch below).
class VaultSearchProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY searchQueryChanged)
    Q_PROPERTY(bool includeTranscriptions READ includeTranscriptions WRITE setIncludeTranscriptions NOTIFY
                   includeTranscriptionsChanged)
    Q_PROPERTY(QStringList tagFilter READ tagFilter WRITE setTagFilter NOTIFY tagFilterChanged)

public:
    // Extra role beyond what VaultTreeModel::roleNames() forwards: whether
    // this row (or, for a directory, anything in its subtree) matches
    // searchQuery. Computed once here so lessThan() and QML's opacity
    // dimming read the exact same answer instead of two logics drifting
    // apart.
    enum { MatchesSearchRole = Qt::UserRole + 500 };

    explicit VaultSearchProxyModel(QObject *parent = nullptr);

    QString searchQuery() const;
    void setSearchQuery(const QString &query);
    bool includeTranscriptions() const;
    void setIncludeTranscriptions(bool include);
    QStringList tagFilter() const;
    void setTagFilter(const QStringList &tags);

    QVariant data(const QModelIndex &proxyIndex, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void searchQueryChanged();
    void includeTranscriptionsChanged();
    void tagFilterChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    bool matchesDirect(const QModelIndex &sourceIndex) const;
    bool matchesSearch(const QModelIndex &sourceIndex) const;
    bool rowHasAllFilterTags(const QModelIndex &sourceIndex) const;
    bool matchesTagFilter(const QModelIndex &sourceIndex) const;

    QString m_searchQuery;
    bool m_includeTranscriptions = true;
    QStringList m_tagFilter;
};
