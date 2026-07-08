#include "VaultSearchProxyModel.h"

#include "VaultTreeModel.h"

VaultSearchProxyModel::VaultSearchProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    // Establishes column 0 as the active sort column so later invalidate()
    // calls (from setSearchQuery/setIncludeTranscriptions) re-trigger
    // lessThan() without needing to call sort() again each time.
    sort(0);
}

QString VaultSearchProxyModel::searchQuery() const
{
    return m_searchQuery;
}

void VaultSearchProxyModel::setSearchQuery(const QString &query)
{
    if (query == m_searchQuery)
        return;
    m_searchQuery = query;
    emit searchQueryChanged();
    invalidate();
}

bool VaultSearchProxyModel::includeTranscriptions() const
{
    return m_includeTranscriptions;
}

void VaultSearchProxyModel::setIncludeTranscriptions(bool include)
{
    if (include == m_includeTranscriptions)
        return;
    m_includeTranscriptions = include;
    emit includeTranscriptionsChanged();
    invalidate();
}

QStringList VaultSearchProxyModel::tagFilter() const
{
    return m_tagFilter;
}

void VaultSearchProxyModel::setTagFilter(const QStringList &tags)
{
    if (tags == m_tagFilter)
        return;
    m_tagFilter = tags;
    emit tagFilterChanged();
    invalidate();
}

bool VaultSearchProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    // Free-text searchQuery never hides rows (item 9: matches sort to the
    // top, non-matches just dim). tagFilter is a genuinely harder filter
    // (item 16/17): a row failing it IS hidden.
    if (m_tagFilter.isEmpty())
        return true;
    return matchesTagFilter(sourceModel()->index(sourceRow, 0, sourceParent));
}

bool VaultSearchProxyModel::rowHasAllFilterTags(const QModelIndex &sourceIndex) const
{
    const QString tagsRaw = sourceModel()->data(sourceIndex, VaultTreeModel::TagsRole).toString();
    const QStringList rowTags = tagsRaw.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &required : m_tagFilter) {
        const QString trimmedRequired = required.trimmed();
        if (trimmedRequired.isEmpty())
            continue;
        bool found = false;
        for (const QString &rowTag : rowTags) {
            if (rowTag.trimmed().compare(trimmedRequired, Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

bool VaultSearchProxyModel::matchesTagFilter(const QModelIndex &sourceIndex) const
{
    if (m_tagFilter.isEmpty())
        return true;

    if (rowHasAllFilterTags(sourceIndex))
        return true;

    const QAbstractItemModel *src = sourceModel();
    if (!src->data(sourceIndex, VaultTreeModel::IsDirRole).toBool())
        return false;

    // A directory is kept if ANY descendant has all the requested tags —
    // same recursive-match shape as matchesSearch, but this one actually
    // hides the row when false (see filterAcceptsRow).
    const int rows = src->rowCount(sourceIndex);
    for (int i = 0; i < rows; ++i) {
        if (matchesTagFilter(src->index(i, 0, sourceIndex)))
            return true;
    }
    return false;
}

bool VaultSearchProxyModel::matchesDirect(const QModelIndex &sourceIndex) const
{
    const QString q = m_searchQuery.trimmed().toLower();
    if (q.isEmpty())
        return true;

    const QAbstractItemModel *src = sourceModel();

    const QString name = src->data(sourceIndex, VaultTreeModel::NameRole).toString();
    if (name.toLower().contains(q))
        return true;

    const QString label = src->data(sourceIndex, VaultTreeModel::LabelRole).toString();
    if (label.toLower().contains(q))
        return true;

    const QString tags = src->data(sourceIndex, VaultTreeModel::TagsRole).toString();
    if (tags.toLower().contains(q))
        return true;

    if (m_includeTranscriptions) {
        const int status = src->data(sourceIndex, VaultTreeModel::StatusRole).toInt();
        if (status == VaultTreeModel::TranscribedStatus) {
            const QString body = src->data(sourceIndex, VaultTreeModel::TranscriptBodyRole).toString();
            if (body.toLower().contains(q))
                return true;
        }
    }
    return false;
}

bool VaultSearchProxyModel::matchesSearch(const QModelIndex &sourceIndex) const
{
    if (m_searchQuery.trimmed().isEmpty())
        return true;

    if (matchesDirect(sourceIndex))
        return true;

    const QAbstractItemModel *src = sourceModel();
    if (!src->data(sourceIndex, VaultTreeModel::IsDirRole).toBool())
        return false;

    // A directory "matches" if anything in its subtree does, so it still
    // sorts above a directory with no matching descendants anywhere.
    const int rows = src->rowCount(sourceIndex);
    for (int i = 0; i < rows; ++i) {
        if (matchesSearch(src->index(i, 0, sourceIndex)))
            return true;
    }
    return false;
}

bool VaultSearchProxyModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    if (!m_searchQuery.trimmed().isEmpty()) {
        const bool leftMatches = matchesSearch(left);
        const bool rightMatches = matchesSearch(right);
        if (leftMatches != rightMatches)
            return leftMatches;
    }
    // No query, or tied match status: preserve the source model's own order
    // (scanDirectory() already sorts dirs-first, alphabetical) instead of
    // re-deriving a comparison here.
    return left.row() < right.row();
}

QVariant VaultSearchProxyModel::data(const QModelIndex &proxyIndex, int role) const
{
    if (role == MatchesSearchRole)
        return matchesSearch(mapToSource(proxyIndex));
    return QSortFilterProxyModel::data(proxyIndex, role);
}

QHash<int, QByteArray> VaultSearchProxyModel::roleNames() const
{
    QHash<int, QByteArray> roles = QSortFilterProxyModel::roleNames();
    roles.insert(MatchesSearchRole, "matchesSearch");
    return roles;
}
