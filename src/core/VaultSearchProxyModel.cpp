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

QVariantMap VaultSearchProxyModel::fileSortKeys() const
{
    return m_fileSortKeys;
}

void VaultSearchProxyModel::setFileSortKeys(const QVariantMap &keys)
{
    if (keys == m_fileSortKeys)
        return;
    m_fileSortKeys = keys;
    emit fileSortKeysChanged();
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

    const QAbstractItemModel *src = sourceModel();
    const bool leftIsDir = src->data(left, VaultTreeModel::IsDirRole).toBool();
    const bool rightIsDir = src->data(right, VaultTreeModel::IsDirRole).toBool();
    if (leftIsDir != rightIsDir) {
        // Directories always sort first — was an accidental side effect of
        // scanDirectory()'s row order below when that was the only ordering
        // signal; now explicit, since fileSortKeys below can reorder files
        // independently of their source row.
        return leftIsDir;
    }

    if (!leftIsDir && !m_fileSortKeys.isEmpty()) {
        const QVariantList leftKey = m_fileSortKeys.value(src->data(left, VaultTreeModel::PathRole).toString()).toList();
        const QVariantList rightKey =
            m_fileSortKeys.value(src->data(right, VaultTreeModel::PathRole).toString()).toList();
        // Sidebar.qml's file-tree sort (sort mode + combine-candidate
        // clustering) — see VaultSearchProxyModel.h's fileSortKeys doc
        // comment. Orphaned .md files have no entry (out of scope — combine
        // candidates and mtime sorting are audio-specific).
        //
        // Bug: a keyed/keyless split that falls straight through to the
        // row()-order default below (as this used to) isn't a valid total
        // order once EITHER side can be keyed — e.g. keyed-A < row-based-B
        // and row-based-B < keyed-C says nothing about how A compares to C,
        // so QSortFilterProxyModel's sort (which assumes a strict weak
        // ordering) can misplace or destabilize entries that never even
        // touch fileSortKeys. Making "has a key" its own ordering tier first
        // — keyed files always before keyless ones — keeps every pairwise
        // comparison consistent with every other, at the cost of keyless
        // files (orphaned .md) always landing after every audio file in the
        // same folder instead of interleaving at their old alphabetical spot.
        const bool leftHasKey = leftKey.size() == 2;
        const bool rightHasKey = rightKey.size() == 2;
        if (leftHasKey != rightHasKey)
            return leftHasKey;
        if (leftHasKey) {
            const QString leftGroup = leftKey.at(0).toString();
            const QString rightGroup = rightKey.at(0).toString();
            if (leftGroup != rightGroup)
                return leftGroup < rightGroup;
            const QString leftOwn = leftKey.at(1).toString();
            const QString rightOwn = rightKey.at(1).toString();
            if (leftOwn != rightOwn)
                return leftOwn < rightOwn;
        }
    }

    // No query, tied match status, and no (or tied) fileSortKeys: preserve
    // the source model's own order (scanDirectory() already sorts
    // dirs-first, alphabetical) instead of re-deriving a comparison here.
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
