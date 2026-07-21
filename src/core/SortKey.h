#pragma once

#include <QHash>
#include <QMetaType>
#include <QString>

#include <tuple>

// The vault file-tree sort key — ONE definition of what a key IS and how two of
// them compare, shared by its sole producer (TreeOrdering::compute) and its sole
// consumer (VaultSearchProxyModel::lessThan).
//
// This used to be a 3-element QVariantList: TreeOrdering flattened its internal
// triple into {k0, k1, k2}, and lessThan independently re-hardcoded the element
// count (`kFileSortKeyElements == 3`) and re-implemented the element-wise
// compare — two halves of one contract, tied together only by a doc comment
// begging the next editor to keep them in sync. A wrong element count or a
// stray `double` in a rank silently mis-sorted the tree (see
// operator< / the padded-integer ranks in TreeOrdering.cpp).
//
// As a typed struct the contract is STRUCTURAL, not documented: the field count
// is fixed by the type (adding/removing a rank is a compile error at every use
// site), operator< is the single comparison both sides go through, and a rank
// can only ever be a QString — a raw double can't be assigned into one. The
// value rides producer -> consumer as an opaque QHash<QString, SortKey>
// (SortKeyMap) that QML passes through untouched (it never inspects it).
struct SortKey {
    QString groupRank;  // where the whole cluster sits among everything else
    QString clusterKey; // the cluster's own identity — its own tie-break tier
    QString memberRank; // this member's position within its cluster

    // Element order IS the comparison contract (groupRank, then cluster
    // identity, then within-cluster position) — see TreeOrdering::compute()'s
    // sortKeys comment for why identity is its own middle tier. std::tie rather
    // than a defaulted operator<=> so this never depends on QString gaining a
    // three-way comparison: it needs only QString::operator</operator==.
    bool operator<(const SortKey &o) const
    {
        return std::tie(groupRank, clusterKey, memberRank)
                < std::tie(o.groupRank, o.clusterKey, o.memberRank);
    }
    bool operator==(const SortKey &o) const
    {
        return std::tie(groupRank, clusterKey, memberRank)
                == std::tie(o.groupRank, o.clusterKey, o.memberRank);
    }
};

// path -> key: the whole-tree ordering table, transported from producer to
// consumer. A QHash (not a QVariantMap) precisely so QML treats it as an opaque
// blob and carries it through refreshTreeOrdering() without iterating it.
using SortKeyMap = QHash<QString, SortKey>;

Q_DECLARE_METATYPE(SortKey)
Q_DECLARE_METATYPE(SortKeyMap)
