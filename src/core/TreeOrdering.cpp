#include "TreeOrdering.h"

#include "SortKey.h"

#include <QHash>

#include <algorithm>

namespace {

const QString kFiles = QStringLiteral("files");
const QString kOutputPath = QStringLiteral("outputPath");
const QString kStatus = QStringLiteral("status");
const QString kReady = QStringLiteral("ready");

// JS's Number.MAX_SAFE_INTEGER, spelled as an exact integer.
//
// Not some larger power of ten — mtimes are milliseconds-since-epoch
// (~1.7e12 today), safely inside JS's exact-integer range, so the subtraction
// below (for "newest first") stays exact. A bigger constant like 1e20 would
// lose enough float64 precision at this magnitude to collide distinct
// timestamps into the same padded string. Keeping the value identical to the
// JS this was ported from also keeps the produced sort keys byte-identical.
constexpr qint64 kMaxSafeInteger = 9007199254740991LL;

// Width every numeric rank is zero-padded to, so a plain lexicographic
// QString compare (all VaultSearchProxyModel::lessThan() ever does) orders
// them numerically. 16 = the digit count of kMaxSafeInteger, the widest value
// ownRank() can produce.
constexpr int kRankPadWidth = 16;

// ownRank: this file's own comparable value for the active sort mode, as a
// plain string so the C++ proxy only ever does a QString compare (see
// VaultSearchProxyModel::fileSortKeys' doc comment).
//
// TRAP (why sortMode and mtimes are explicit parameters): in the QML original
// this was a nested closure that silently captured both, plus groupFileOrder
// in its sibling below — signatures that revealed none of their real inputs.
// Every input is named here on purpose.
QString ownRank(const QString &path, const QString &sortMode, const QVariantMap &mtimes)
{
    if (sortMode == QLatin1String("name")) {
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        return (slash >= 0 ? path.mid(slash + 1) : path).toLower();
    }
    // JS read a missing key as undefined and `|| 0`-ed it; an absent/invalid
    // QVariant converts to 0 the same way.
    const qint64 ms = mtimes.value(path).toLongLong();
    const qint64 value = sortMode == QLatin1String("dateAsc") ? ms : (kMaxSafeInteger - ms);
    // JS was String(value).padStart(16, "0"). Both operands are exact
    // integers well inside the safe range, so QString::number(qint64) is
    // character-for-character what String() produced — no float formatting is
    // involved and none must be introduced (QString::number(double) would
    // truncate to 6 significant digits and silently collide timestamps).
    // rightJustified() matches padStart(): pad on the left, never truncate.
    return QString::number(value).rightJustified(kRankPadWidth, QLatin1Char('0'));
}

// memberOrderRank: a group member's position AMONG ITS OWN groupmates, always
// following g.files' own combine order (via groupFileOrder) regardless of
// sortMode — so the first-to-be-combined file is always at the top. That order
// is mtime ascending BY DEFAULT, but a rule with a configured sort
// differentiator orders members by that instead (see groupFileOrder's doc
// comment in compute()) — deriving this from the same map the combine-order
// badge uses, rather than independently recomputing an mtime rank, keeps
// display order and badge numbering from ever disagreeing. Only ever used as
// sortKeys' third element for grouped files — ungrouped files use plain
// ownRank, so vaults without combine rules see no change.
QString memberOrderRank(const QString &path, const QHash<QString, int> &groupFileOrder)
{
    return QString::number(groupFileOrder.value(path, 0)).rightJustified(kRankPadWidth, QLatin1Char('0'));
}

} // namespace

TreeOrdering::TreeOrdering(QObject *parent)
    : QObject(parent)
{
}

QVariantMap TreeOrdering::compute(const QStringList &audioPaths,
                                  const QVariantMap &mtimes,
                                  const QVariantList &groups,
                                  const QVariantList &pendingCombines,
                                  const QString &sortMode)
{
    QVariantMap readyPaths;
    for (const QVariant &groupVar : groups) {
        const QVariantMap g = groupVar.toMap();
        if (g.value(kStatus).toString() != kReady)
            continue;
        const QString outputPath = g.value(kOutputPath).toString();
        const QStringList files = g.value(kFiles).toStringList();
        for (const QString &f : files)
            readyPaths.insert(f, outputPath);
    }

    // clusterKeyOf: path -> stable group id, for every group with 2+ files
    // regardless of ready/pending/alreadyTranscribed/alreadyCombined status —
    // clustering ("stick together in the file tree") is a broader concern than
    // the ready-only outline highlight above, since even a
    // not-currently-combinable group still benefits from its members sitting
    // next to each other.
    //
    // groupFileOrder: path -> 1-based index within g.files, same all-statuses
    // sweep. g.files is already in whatever order findCombineGroups() actually
    // uses to combine (the rule's configured sort differentiator if it has one
    // and every member captured a value for it, else mtime ascending — see
    // that function's doc comment in TranscriptionController.cpp) — reusing
    // that order here, instead of independently re-deriving an mtime-only
    // rank, is what guarantees the tree's display order and the per-row
    // combine-order badge's numbering (both driven from this one map — see
    // memberOrderRank above and Sidebar.combineOrderPaths) can never disagree,
    // even for a rule whose combine order isn't mtime-based at all.
    QHash<QString, QString> clusterKeyOf;
    QHash<QString, int> groupFileOrder;
    QVariantMap orderPaths;
    for (const QVariant &groupVar : groups) {
        const QVariantMap g = groupVar.toMap();
        const QStringList files = g.value(kFiles).toStringList();
        if (files.size() < 2)
            continue;
        const QString outputPath = g.value(kOutputPath).toString();
        for (int i = 0; i < files.size(); ++i) {
            clusterKeyOf.insert(files.at(i), outputPath);
            groupFileOrder.insert(files.at(i), i + 1);
            orderPaths.insert(files.at(i), i + 1);
        }
    }

    // groupRank: the MINIMUM ownRank among a group's members, for every file
    // in a 2+-member group, else just the file's own ownRank (a lone file is
    // its own one-member "group"). Since ownRank's encoding already flips for
    // "dateDesc" (newest maps to the smallest padded string), taking the min
    // here naturally picks whichever member is "first" under the CURRENT sort
    // direction as the group's representative — its earliest member for
    // "name"/"dateAsc", its most recently modified member for "dateDesc" — so
    // a group sorts alongside other items near its own most-relevant edge
    // instead of always anchoring to one fixed member regardless of direction.
    // With sortMode "name" and no active groups, groupRank === ownRank for
    // every file, so ordering is byte-identical to plain alphabetical — no
    // behavior change for vaults without combine rules.
    QHash<QString, QString> groupRankOf;
    for (const QString &path : audioPaths) {
        const auto key = clusterKeyOf.constFind(path);
        if (key == clusterKeyOf.constEnd())
            continue;
        const QString rank = ownRank(path, sortMode, mtimes);
        const auto existing = groupRankOf.constFind(*key);
        if (existing == groupRankOf.constEnd() || rank < *existing)
            groupRankOf.insert(*key, rank);
    }

    // Three elements, compared in order: groupRank (where the whole cluster
    // sits among everything else), then the cluster's own IDENTITY, then the
    // member's position within it.
    //
    // Bug: this used to be just [groupRank, memberOrderRank], which silently
    // stopped clustering whenever two DIFFERENT groups computed the same
    // groupRank — not a contrived case, it's the norm for this app's own "acme
    // mic 3" rule. That rule keys on date+edit/orig, and the recorder writes
    // each pair's edit and orig files at the same instant, so both groups have
    // byte-identical mtimes and therefore an identical min-mtime groupRank.
    // With element 0 tied, element 1 tied too (each pair's edit and orig are
    // both member #1 of their own group), so every comparison fell through to
    // the proxy's row()-order default — i.e. plain alphabetical, which
    // interleaves edit/orig one row apart. Nothing was adjacent, so every
    // member also drew its own isolated CombineGroupOutline box instead of one
    // merged run (that outline reads adjacency off displayOrder below, so it
    // was consistently wrong in the same way, not separately broken). Making
    // cluster identity its own tier means a tie on groupRank orders the two
    // clusters arbitrarily-but-consistently rather than shuffling their
    // members together.
    //
    // Ungrouped files use their own path as the identity — unique per file, so
    // a lone file whose ownRank happens to tie a group's groupRank lands
    // cleanly before or after that whole group instead of inside it.
    //
    // This encoding IS VaultSearchProxyModel::lessThan()'s comparison contract.
    // It is no longer a 3-element stringly-typed list held together by this
    // comment: both sides now go through the typed SortKey (src/core/SortKey.h),
    // whose field count and operator< ARE the contract — a mismatched count is
    // a compile error, not a silent mis-sort. This producer sits next to that
    // consumer for the same reason.
    //
    // The one typed table doubles as both the transport value (an opaque
    // SortKeyMap QML hands straight to the proxy) and the side table the
    // displayOrder sort below compares directly via SortKey::operator<.
    SortKeyMap sortKeys;
    const auto addKey = [&](const QString &path, const SortKey &key) {
        sortKeys.insert(path, key);
    };
    for (const QString &path : audioPaths) {
        const auto key = clusterKeyOf.constFind(path);
        if (key == clusterKeyOf.constEnd()) {
            const QString own = ownRank(path, sortMode, mtimes);
            addKey(path, SortKey{own, path, own});
        } else {
            addKey(path, SortKey{groupRankOf.value(*key), *key, memberOrderRank(path, groupFileOrder)});
        }
    }

    // Combine placeholders (see Sidebar.pendingCombines()) are not in
    // audioPaths — they aren't audio, or even files yet — but they still need
    // a key. Without one the proxy drops them into its keyless tier, which
    // sorts after every real audio file in the folder (see lessThan()'s "has a
    // key" comment), so an in-progress merge would jump to the bottom and then
    // jump back when it landed.
    //
    // Keyed as an ungrouped file at the output's own path, so the placeholder
    // sits exactly where the merged file will appear once it exists — the row
    // doesn't move when the real thing replaces it. That's exact for sortMode
    // "name". For the date modes it's approximate: ownRank() wants an mtime,
    // and a file that hasn't been written has none (mtimes has no entry -> 0),
    // so a placeholder sorts to the newest/oldest edge for the seconds it
    // exists. Chasing that would mean ranking by the sources' mtimes, and the
    // sources are the very rows being hidden — not worth the machinery for a
    // transient row that resolves itself.
    for (const QVariant &pendingVar : pendingCombines) {
        const QString p = pendingVar.toMap().value(kOutputPath).toString();
        const QString own = ownRank(p, sortMode, mtimes);
        addKey(p, {own, p, own});
    }

    // Adjacency (topMerged/bottomMerged) has to be computed from the SAME
    // order the tree actually ends up displaying — see
    // Sidebar.combineTopMergedPaths' doc comment for the bug that bit when
    // this used allAudioPaths()'s raw order instead. Must compare every
    // element VaultSearchProxyModel::lessThan() does, in the same order, for
    // exactly that reason.
    QStringList displayOrder = audioPaths;
    // stable_sort, not sort: the JS this replaces used Array.prototype.sort,
    // which ES2019 onward REQUIRES to be stable, so a fully-tied pair kept its
    // allAudioPaths() order. std::sort would be free to swap it.
    // Read through a CONST alias: QHash::operator[] on a mutable hash would
    // insert-on-miss, and an insert mid-sort can rehash and dangle the
    // references the comparator is holding. The compare IS SortKey::operator<,
    // the exact same one lessThan() uses — one definition, no re-implemented
    // element loop to drift.
    const SortKeyMap &keys = sortKeys;
    std::stable_sort(displayOrder.begin(), displayOrder.end(),
                     [&keys](const QString &a, const QString &b) {
                         return *keys.constFind(a) < *keys.constFind(b);
                     });

    QVariantMap topMerged;
    QVariantMap bottomMerged;
    // JS compared newMap[neighbour] === newMap[path] — an absent neighbour
    // read as undefined and could never equal a defined outputPath, so
    // "neighbour is in a ready group at all" is part of the test, not an
    // extra guard on top of it.
    const auto sameReadyGroup = [&readyPaths](const QString &neighbour, const QString &output) {
        const auto it = readyPaths.constFind(neighbour);
        return it != readyPaths.constEnd() && it->toString() == output;
    };
    for (int i = 0; i < displayOrder.size(); ++i) {
        const QString &path = displayOrder.at(i);
        const auto ready = readyPaths.constFind(path);
        if (ready == readyPaths.constEnd())
            continue;
        const QString output = ready->toString();
        if (i > 0 && sameReadyGroup(displayOrder.at(i - 1), output))
            topMerged.insert(path, true);
        if (i < displayOrder.size() - 1 && sameReadyGroup(displayOrder.at(i + 1), output))
            bottomMerged.insert(path, true);
    }

    QVariantMap out;
    out.insert(QStringLiteral("readyPaths"), readyPaths);
    out.insert(QStringLiteral("orderPaths"), orderPaths);
    out.insert(QStringLiteral("topMerged"), topMerged);
    out.insert(QStringLiteral("bottomMerged"), bottomMerged);
    out.insert(QStringLiteral("sortKeys"), QVariant::fromValue(sortKeys));
    return out;
}
