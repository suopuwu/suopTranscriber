#pragma once

#include <QSortFilterProxyModel>
#include <QStringList>
#include <QVariantMap>
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
    Q_PROPERTY(QStringList speakerFilter READ speakerFilter WRITE setSpeakerFilter NOTIFY speakerFilterChanged)
    Q_PROPERTY(
        QVariantMap fileSortKeys READ fileSortKeys WRITE setFileSortKeys NOTIFY fileSortKeysChanged)

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
    // Mirrors tagFilter exactly, but against VaultTreeModel::SpeakersRole
    // (the "speakers:" frontmatter allowlist) instead of TagsRole — a
    // second, independently-AND'd hard filter (both must pass if both are
    // set; see filterAcceptsRow()).
    QStringList speakerFilter() const;
    void setSpeakerFilter(const QStringList &speakers);

    // Sidebar.qml's file-tree sort feature (Name / Date Modified asc-desc,
    // plus keeping combine-candidate groups tree-adjacent): path -> a
    // 2-element [groupRank, ownRank] string list, precomputed in QML (it
    // already has both AppSettings.combineRules and
    // TranscriptionController::detectCombineGroups() available) and pushed
    // here as one map so lessThan() only ever does a plain QString compare —
    // same "QML computes, proxy just compares" shape as MatchesSearchRole
    // above, avoiding a second copy of the grouping logic in C++.
    QVariantMap fileSortKeys() const;
    void setFileSortKeys(const QVariantMap &keys);

    QVariant data(const QModelIndex &proxyIndex, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void searchQueryChanged();
    void includeTranscriptionsChanged();
    void tagFilterChanged();
    void speakerFilterChanged();
    void fileSortKeysChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    bool matchesDirect(const QModelIndex &sourceIndex) const;
    bool matchesSearch(const QModelIndex &sourceIndex) const;
    bool rowHasAllFilterTags(const QModelIndex &sourceIndex) const;
    bool matchesTagFilter(const QModelIndex &sourceIndex) const;
    bool rowHasAllFilterSpeakers(const QModelIndex &sourceIndex) const;
    bool matchesSpeakerFilter(const QModelIndex &sourceIndex) const;

    QString m_searchQuery;
    bool m_includeTranscriptions = true;
    QStringList m_tagFilter;
    QStringList m_speakerFilter;
    QVariantMap m_fileSortKeys;
};
