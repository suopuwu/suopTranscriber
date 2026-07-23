#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

// The file-tree ORDERING collaborator lifted out of Sidebar.qml's
// refreshTreeOrdering(). It owns the one pure transform that
// turns the vault's raw data — allAudioPaths(), audioLastModified(),
// detectCombineGroups()'s output, the pending-combine placeholders and the
// active AppSettings.sortMode — into the five plain data maps that decide
// "what order do files appear in":
//
//   readyPaths    path -> outputPath, for every member of a "ready" combine
//                 group (Sidebar.combineReadyPaths — row highlight + the
//                 per-group merge button).
//   orderPaths    path -> 1-based index in its group's combine order
//                 (Sidebar.combineOrderPaths — the combine-order badge).
//   topMerged     path -> true when the row ABOVE it in display order is in
//                 the same ready group (Sidebar.combineTopMergedPaths).
//   bottomMerged  ditto for the row below (Sidebar.combineBottomMergedPaths).
//   sortKeys      path -> a typed SortKey (src/core/SortKey.h), the value
//                 VaultSearchProxyModel::lessThan() compares. Carried in the
//                 bundle as an opaque QHash<QString, SortKey> (SortKeyMap) that
//                 QML passes straight to searchProxy.fileSortKeys untouched.
//
// WHY THIS IS IN C++: sortKeys' encoding and lessThan()'s comparison are two
// halves of one contract. That contract used to span two languages in two
// files — the single most drift-prone thing in a 4.5k-line QML file. It is now
// the single typed SortKey struct both sides go through: the field count and
// operator< ARE the contract, so a mismatch is a compile error rather than a
// silent mis-sort. Producer and consumer sit side by side in src/core/.
//
// Stateless by construction — a QML_SINGLETON purely so QML has something to
// call; compute() is a plain static and holds no state between calls.
//
// NOTE ON CALL SITES: Sidebar.qml still owns WHEN this runs and WHEN
// searchProxy.fileSortKeys is assigned. That is deliberate and load-bearing —
// writing fileSortKeys re-enters VaultSearchProxyModel::invalidate(), which
// once segfaulted by walking freed nodes when it landed mid source-model
// reset (fixed by deferring the invalidate inside the proxy). Only the
// COMPUTATION moved here; the number, ordering and timing of the property
// assignments is unchanged.
class TreeOrdering : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit TreeOrdering(QObject *parent = nullptr);

    // Compute all five ordering outputs in one sweep. They are derived from
    // shared intermediates (and MUST stay consistent with each other — see
    // Sidebar.combineTopMergedPaths' doc comment for the bug that bit when
    // adjacency was computed from a different order than the tree displayed),
    // so they are produced together rather than as five entry points.
    //
    //   audioPaths          VaultTreeModel::allAudioPaths() — raw dirs-first
    //                       alphabetical order.
    //   mtimes              VaultTreeModel::audioLastModified(), path ->
    //                       msecs-since-epoch. The caller passes an EMPTY map
    //                       for sortMode "name": that traversal stats the
    //                       whole tree and the name mode never reads it.
    //   groups              TranscriptionController::detectCombineGroups()
    //                       output: maps with files (QStringList), outputPath,
    //                       status.
    //   pendingCombines     Sidebar.pendingCombines() output: maps with
    //                       outputPath (the only field read here).
    //   sortMode            AppSettings.sortMode — "name", "dateAsc" or
    //                       "dateDesc".
    //
    // Returns a map with keys readyPaths / orderPaths / topMerged /
    // bottomMerged / sortKeys, as described in the class comment.
    Q_INVOKABLE static QVariantMap compute(const QStringList &audioPaths,
                                           const QVariantMap &mtimes,
                                           const QVariantList &groups,
                                           const QVariantList &pendingCombines,
                                           const QString &sortMode);
};
