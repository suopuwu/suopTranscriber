// Living spec: a structural vault change must take the sidebar from the state
// before it to the state after it, and show nothing else on the way.
//
// A file appearing or disappearing routes through VaultTreeModel::rebuild(),
// which is a full beginResetModel/endResetModel — every QModelIndex dies, so
// TreeView drops its expansion state AND the Flickable clamps contentY against
// the momentarily-collapsed contentHeight. Sidebar.qml recovers both
// (expandAllNow + restorePendingContentY); this file measures whether the
// recovery is invisible.
//
// Two bugs have lived here, both found only by frame-level sampling:
//   - the re-EXPANSION ran off a 16ms polling Timer, so the collapsed tree was
//     genuinely painted (covered by tst_sidebar_dragmove's
//     dropIntoFolder_hasNoIntermediateState).
//   - the SCROLL OFFSET restore was deferred through Qt.callLater while the
//     clamp had already zeroed it. Measured on an 80-row tree scrolled to
//     contentY 900: one frame at cy=18 with rows/contentHeight ALREADY back to
//     81/2268 (so only the offset was wrong), then a frame still rendering the
//     top rows — ~15ms of scroll-jump-and-snap-back, reported as "the whole
//     sidebar reflows when files change". That's what this file covers.
//
// Why sampling and not source review: none of it is visible in the source. The
// offset round-trip is an interaction between TableView's deferred layout,
// Flickable's clamp, and Qt.callLater ordering. Same harness shape as
// tst_sidebar_dragmove.cpp — offscreen QQuickView, real Sidebar.qml, real
// VaultTreeModel, real QFileSystemWatcher, sandboxed org/app name so
// AppSettings never touches the user's real config.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include "IconTheme.h"
#include "TreeOrdering.h"
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

// NOT tst_sidebar_dragmove.cpp's type-only stub, and the difference is the
// whole point of this file. Sidebar.qml guards every transcriptionController
// use site on the controller being non-null — so a null one makes
// refreshTreeOrdering() short-circuit allAudioPaths() to [] and assign an
// EMPTY searchProxy.fileSortKeys. That skips the proxy's real work entirely:
// with keys, the assignment calls invalidate() and re-sorts every row.
//
// A harness with a null controller therefore cannot see any reflow the
// re-sort causes, and will happily report microseconds for a call that costs
// milliseconds in the app. This stub exists to make the measurement honest:
// enough surface for the ordering path to run for real, no engine behind it.
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)

public:
    QVariantList jobs() const { return {}; }

    // Only reached when AppSettings.combineRules is non-empty; present so the
    // call site resolves either way.
    Q_INVOKABLE QVariantList detectCombineGroups(const QVariantList &, const QStringList &) const { return {}; }
    Q_INVOKABLE int combineStabilityWindowSecs() const { return 5; }
    // Every realized row calls this (TreeRowDelegate's refreshJobStatus);
    // -1 is the controller's own "no job for this path".
    Q_INVOKABLE int jobStatusForPath(const QString &) const { return -1; }
    Q_INVOKABLE QStringList existingVaultTags(const QUrl &) const { return {}; }
    Q_INVOKABLE QStringList existingVaultSpeakers(const QUrl &) const { return {}; }
    Q_INVOKABLE bool matchesExistingTranscription(const QUrl &, const QString &) const { return false; }

    // Lets a test drive the signal a finishing task emits, which is what makes
    // the app re-run the ordering (and thus the proxy re-sort) on a change
    // that never resets the model at all.
    Q_INVOKABLE void emitFinished(const QUrl &fileUrl, bool structural)
    {
        emit finished(fileUrl, true, QString(), structural);
    }

signals:
    void jobsChanged();
    void queueChanged();
    void jobStatusesChanged();
    void finished(const QUrl &fileUrl, bool ok, const QString &error, bool structural);
    void fileRenamed(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);
    void fileMoved(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);
    void folderRenamed(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);
    void folderMoved(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);
    void trashResult(const QString &scope, const QStringList &untrashable);
};

} // namespace

class TstSidebarReflow : public QObject
{
    Q_OBJECT

private:
    // Which vault change to make. Add/Remove reset the model; Transcribe does
    // NOT — sameStructure() holds for a sidecar appearing, so it goes through
    // emitContentChanges()'s plain dataChanged. It is covered here because the
    // app ALSO re-runs the ordering on that path (Sidebar's onFinished ->
    // refreshTreeOrderingCoalesced), which re-assigns fileSortKeys, which
    // invalidates the proxy. A row changing colour can therefore reflow the
    // tree with no reset involved anywhere.
    enum Change { AddFile, RemoveFile, TranscribeFile, RemoveNested };

    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    static QQuickItem *findByClassNamePrefix(QQuickItem *root, const QByteArray &prefix)
    {
        for (QQuickItem *child : root->findChildren<QQuickItem *>()) {
            if (QByteArray(child->metaObject()->className()).startsWith(prefix))
                return child;
        }
        return nullptr;
    }

    // Independent ground truth for "which visible row is this path at" — reads
    // the model's roles through QAbstractItemModel's public API rather than
    // reusing Sidebar.qml's own JS, so a pass can't just be the feature
    // agreeing with itself. Same helper as tst_sidebar_dragmove.cpp.
    static int rowForPath(QQuickItem *treeItem, const QString &path)
    {
        auto *model = qobject_cast<QAbstractItemModel *>(
            treeItem->property("model").value<QObject *>());
        if (!model)
            return -1;
        int pathRole = -1;
        const QHash<int, QByteArray> roles = model->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
            if (it.value() == "path") {
                pathRole = it.key();
                break;
            }
        }
        if (pathRole < 0)
            return -1;
        const int rowCount = treeItem->property("rows").toInt();
        for (int r = 0; r < rowCount; ++r) {
            QModelIndex idx;
            QMetaObject::invokeMethod(treeItem, "index", Qt::DirectConnection,
                                       Q_RETURN_ARG(QModelIndex, idx), Q_ARG(int, r), Q_ARG(int, 0));
            if (model->data(idx, pathRole).toString() == path)
                return r;
        }
        return -1;
    }

    static QQuickItem *rowItem(QQuickItem *treeItem, int row)
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, item), Q_ARG(QPoint, QPoint(0, row)));
        return item;
    }

    // Sidebar.qml hooks its auto-expand off the proxy's modelReset, and this
    // harness hands VaultTreeModel its vaultPath at CONSTRUCTION — so that
    // reset can land before the Sidebar's Connections exist to hear it. Expand
    // explicitly rather than depend on the race (same note as
    // tst_sidebar_dragmove.cpp).
    static void expandEverything(QQuickItem *treeItem)
    {
        QMetaObject::invokeMethod(treeItem, "expandRecursively", Qt::DirectConnection,
                                   Q_ARG(int, -1), Q_ARG(int, -1));
        QTest::qWait(50);
    }

    QQuickItem *openVault(QQuickView *view, const QString &vaultPath, QQuickItem **sidebarOut = nullptr)
    {
        view->engine()->addImportPath(m_moduleDir.path());
        view->engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                          new IconImageProvider);
        view->rootContext()->setContextProperty(QStringLiteral("vaultUrl"),
                                                 QUrl::fromLocalFile(vaultPath));
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : view->errors())
                msg += e.toString() + QStringLiteral("\n");
            // A QML error here is exactly what a green cmake build does NOT
            // catch (qmlcachegen can compile a file that still fails to load).
            qWarning("%s", qPrintable(msg.isEmpty() ? QStringLiteral("QML failed to load, no error text")
                                                     : msg));
            return nullptr;
        }
        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        QQuickItem *root = view->rootObject();
        if (!root)
            return nullptr;
        QQuickItem *sidebarItem = root->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        if (!sidebarItem)
            return nullptr;
        if (sidebarOut)
            *sidebarOut = sidebarItem;
        return findByClassNamePrefix(sidebarItem, "QQuickTreeView");
    }

    // 5 folders x 12 takes + 15 at the root = 80 rows, ~90% with a .md
    // sidecar. Deliberately sized past the viewport: the offset bug this file
    // covers cannot happen in a tree that fits on screen, because there is no
    // scroll position for the reset's clamp to destroy.
    static bool buildScrollableVault(const QString &root)
    {
        const QStringList folders = {QStringLiteral("Interviews"), QStringLiteral("Lectures"),
                                      QStringLiteral("Meetings"),  QStringLiteral("Notes"),
                                      QStringLiteral("Voice")};
        int made = 0;
        for (const QString &folder : folders) {
            for (int i = 0; i < 12; ++i) {
                const QString audio = QStringLiteral("%1/%2/take-%3.mp3")
                                          .arg(root, folder).arg(i, 2, 10, QLatin1Char('0'));
                if (!touch(audio))
                    return false;
                if (made % 10 != 0 && !touch(audio + QStringLiteral(".md"),
                                              "---\nlabel: A recorded take\ntags: [a, b]\n---\n\n[0] hello there\n"))
                    return false;
                ++made;
            }
        }
        for (int i = 0; i < 15; ++i) {
            const QString audio = QStringLiteral("%1/root-%2.mp3").arg(root).arg(i, 2, 10, QLatin1Char('0'));
            if (!touch(audio))
                return false;
            if (i % 10 != 0 && !touch(audio + QStringLiteral(".md"),
                                       "---\nlabel: A root take\ntags: [c]\n---\n\n[0] hello\n"))
                return false;
        }

        // NESTED + EMPTY subfolders, mirroring the real vault this was
        // diagnosed against ("test folder" holding two childless folders and
        // three recordings). Both properties matter and a flat fixture has
        // neither: expandAll() walks rows bottom-up calling expandRecursively,
        // and QQuickTreeView refuses to expand a CHILDLESS node at all (see
        // Sidebar.qml's collapsedEmptyDirs comment) — so the empty ones are
        // the case the expansion recovery has to get right. Without these the
        // reset flash simply does not reproduce.
        if (!QDir().mkpath(root + QStringLiteral("/test folder/inner folder"))
            || !QDir().mkpath(root + QStringLiteral("/test folder/second inner folder")))
            return false;
        for (int i = 0; i < 3; ++i) {
            const QString audio = QStringLiteral("%1/test folder/rec-%2.mp3").arg(root).arg(i);
            if (!touch(audio))
                return false;
        }
        return true;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        // Symlink (not copy) to the real checked-out qml/ — must reflect what
        // is on disk right now, never a stale build-time copy.
        QVERIFY2(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")),
                 "failed to symlink qml/ into the test's scratch QML module dir");

        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
        qmldir.write("module SuopTranscriber\n"
                      "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                      "singleton Theme 1.0 qml/Theme.qml\n"
                      "singleton Notifications 1.0 qml/Notifications.qml\n"
                      "HoverBorder 1.0 qml/HoverBorder.qml\n"
                      "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
                      "AppIcon 1.0 qml/AppIcon.qml\n"
                      "AppIconButton 1.0 qml/AppIconButton.qml\n"
                      "CompressButton 1.0 qml/CompressButton.qml\n"
                      "ConfirmPopup 1.0 qml/ConfirmPopup.qml\n"
                      "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                      "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                      "Sidebar 1.0 qml/Sidebar.qml\n");
        qmldir.close();

        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterSingletonType<TreeOrdering>(
            "SuopTranscriber", 1, 0, "TreeOrdering",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new TreeOrdering; });
        qmlRegisterType<VaultTreeModel>("SuopTranscriber", 1, 0, "VaultTreeModel");
        qmlRegisterType<VaultSearchProxyModel>("SuopTranscriber", 1, 0, "VaultSearchProxyModel");
        // Creatable, unlike the other sidebar harnesses' stub: this one is
        // instantiated and ASSIGNED in the driver below, so fileSortKeys is
        // populated and the proxy actually sorts — see the stub's comment.
        qmlRegisterType<TranscriptionControllerStub>(
            "SuopTranscriber", 1, 0, "TranscriptionController");

        // Taller than tst_sidebar_dragmove's driver so the tree is genuinely
        // scrollable with room to spare above and below the change.
        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    id: root\n"
            "    width: 320\n"
            "    height: 600\n"
            "    property string openPath: \"\"\n"
            "    VaultTreeModel { id: vaultModel; objectName: \"vaultModel\"; vaultPath: Qt.resolvedUrl(vaultUrl) }\n"
            "    TranscriptionController { id: controller; objectName: \"controller\" }\n"
            "    Sidebar {\n"
            "        id: sidebar\n"
            "        objectName: \"sidebar\"\n"
            "        anchors.fill: parent\n"
            "        model: vaultModel\n"
            "        transcriptionController: controller\n"
            "        currentPath: root.openPath\n"
            "        onFileActivated: (path) => root.openPath = path\n"
            "        onOrphanedMdActivated: (path) => root.openPath = path\n"
            "    }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driverFile(m_driverQmlPath);
        QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
        driverFile.write(driverQml.toUtf8());
        driverFile.close();
    }

    // DIAGNOSTIC: does the ribbon's height depend on how many files are
    // selected, and at which sidebar widths? The TreeView is anchored to
    // ribbon.bottom and the ribbon's height is a wrapping Flow's
    // implicitHeight, so any width where the selected-vs-unselected button
    // count crosses a wrap boundary moves the ENTIRE tree.
    void probe_ribbonHeightVsSelectionWidth()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(buildScrollableVault(vault.path()));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        expandEverything(treeItem);
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        QQuickItem *ribbonFlow = findByClassNamePrefix(sidebarItem, "SidebarRibbon");
        QVERIFY(ribbonFlow);
        QQuickItem *ribbon = ribbonFlow->parentItem();
        QVERIFY(ribbon);
        QQuickItem *root = view.rootObject();

        const QStringList two = {vault.filePath(QStringLiteral("Notes/take-00.mp3")),
                                  vault.filePath(QStringLiteral("Notes/take-01.mp3"))};

        QString table = QStringLiteral("\nwidth | ribbonH(sel=0/1/2) treeY(sel=0/1/2) btns(0/1/2)\n");
        QStringList offenders;
        for (int w = 200; w <= 440; w += 20) {
            root->setWidth(w);
            QTest::qWait(60);
            QList<int> hs, ys, bs;
            for (int n = 0; n <= 2; ++n) {
                sidebarItem->setProperty("selectedPaths", QStringList(two.mid(0, n)));
                QMetaObject::invokeMethod(sidebarItem, "refreshSelectionEligibility", Qt::DirectConnection);
                QTest::qWait(60);
                int btns = 0;
                for (QQuickItem *kid : ribbonFlow->childItems()) {
                    if (kid->isVisible())
                        ++btns;
                }
                hs << qRound(ribbon->height());
                ys << qRound(treeItem->y());
                bs << btns;
            }
            table += QStringLiteral("%1   | %2/%3/%4   %5/%6/%7   %8/%9/%10\n")
                         .arg(w, 5).arg(hs[0]).arg(hs[1]).arg(hs[2])
                         .arg(ys[0]).arg(ys[1]).arg(ys[2])
                         .arg(bs[0]).arg(bs[1]).arg(bs[2]);
            if (hs[0] != hs[1] || hs[1] != hs[2])
                offenders << QStringLiteral("w=%1 ribbonH %2->%3->%4").arg(w).arg(hs[0]).arg(hs[1]).arg(hs[2]);
        }
        qInfo().noquote() << table << "\nWIDTHS WHERE THE TREE MOVES ON SELECTION: "
                          << (offenders.isEmpty() ? QStringLiteral("(none)") : offenders.join(QStringLiteral("; ")));
    }

    // The structural guarantee the no-flash property now rests on: an ordinary
    // vault change must SPLICE rows, never reset the model. A reset invalidates
    // every QModelIndex, and recovering expansion + scroll from that inside one
    // frame is what was never reliably achievable on a real vault. If this
    // starts failing, the flash is back regardless of how fast the reset is.
    void ordinaryVaultChange_splicesRowsInsteadOfResetting()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(buildScrollableVault(vault.path()));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        expandEverything(treeItem);
        const int kRows = 5 + 60 + 15 + 1 + 2 + 3;
        QTRY_COMPARE(treeItem->property("rows").toInt(), kRows);

        auto *model = qobject_cast<QAbstractItemModel *>(
            view.rootObject()->findChild<QObject *>(QStringLiteral("vaultModel")));
        QVERIFY(model);
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removed(model, &QAbstractItemModel::rowsRemoved);

        // A file appearing at the vault root.
        QVERIFY(touch(vault.filePath(QStringLiteral("root-99-new.mp3"))));
        QTRY_COMPARE(treeItem->property("rows").toInt(), kRows + 1);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(resets.count(), 0);

        // A file disappearing from a nested folder.
        QVERIFY(QFile::remove(vault.filePath(QStringLiteral("test folder/rec-0.mp3"))));
        QTRY_COMPARE(treeItem->property("rows").toInt(), kRows);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(resets.count(), 0);

        // And the nested folder must still be expanded afterwards — the whole
        // point of not resetting. rec-2 lives inside "test folder", so its
        // being a visible row proves the folder never collapsed.
        QVERIFY2(rowForPath(treeItem, vault.filePath(QStringLiteral("test folder/rec-2.mp3"))) >= 0,
                 "the nested folder's contents must survive a vault change");
    }

    // A brand-new folder has no expansion state, and TreeView expands nothing
    // on its own — so the splice path has to expand it explicitly or new
    // folders would silently arrive collapsed.
    void newlyInsertedFolder_arrivesExpanded()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(buildScrollableVault(vault.path()));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        expandEverything(treeItem);
        const int kRows = 5 + 60 + 15 + 1 + 2 + 3;
        QTRY_COMPARE(treeItem->property("rows").toInt(), kRows);

        QVERIFY(touch(vault.filePath(QStringLiteral("Zebra/inside-new.mp3"))));
        // +1 for the folder, +1 for the file it contains — the file is only a
        // row at all if the folder came in expanded.
        QTRY_COMPARE(treeItem->property("rows").toInt(), kRows + 2);
        QVERIFY2(rowForPath(treeItem, vault.filePath(QStringLiteral("Zebra/inside-new.mp3"))) >= 0,
                 "a newly created folder must arrive expanded, not collapsed");
    }

    // The behaviour behind the ribbon half of the reflow, asserted directly
    // rather than only through the frame sampler: a vault change elsewhere
    // must leave the selection alone, and a vault change that removes a
    // SELECTED file must drop exactly that file.
    void selection_survivesUnrelatedChange_andPrunesRemovedFiles()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(buildScrollableVault(vault.path()));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        expandEverything(treeItem);
        const int kRows = 5 + 60 + 15 + 1 + 2 + 3; // + "test folder", 2 empty subfolders, 3 recordings
        QTRY_COMPARE(treeItem->property("rows").toInt(), kRows);

        const QString keep = vault.filePath(QStringLiteral("Notes/take-00.mp3"));
        const QString doomed = vault.filePath(QStringLiteral("Notes/take-01.mp3"));
        sidebarItem->setProperty("selectedPaths", QStringList{keep, doomed});
        sidebarItem->setProperty("lastClickedPath", doomed);
        sidebarItem->setProperty("selectionCursorPath", doomed);

        // A file appearing somewhere else entirely.
        QVERIFY(touch(vault.filePath(QStringLiteral("Voice/take-99-new.mp3"))));
        QTRY_COMPARE(treeItem->property("rows").toInt(), kRows + 1);
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList(), (QStringList{keep, doomed}));
        QCOMPARE(sidebarItem->property("lastClickedPath").toString(), doomed);

        // Now remove one of the SELECTED files: that one goes, the other stays.
        QVERIFY(QFile::remove(doomed));
        QVERIFY(QFile::remove(doomed + QStringLiteral(".md")));
        QTRY_COMPARE(treeItem->property("rows").toInt(), kRows);
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList(), (QStringList{keep}));
        // Both range endpoints named the removed file, so both have to clear —
        // a Shift-click anchored on a vanished path extends from nowhere.
        QCOMPARE(sidebarItem->property("lastClickedPath").toString(), QString());
        QCOMPARE(sidebarItem->property("selectionCursorPath").toString(), QString());
    }

    void structuralChange_hasNoIntermediateState_data()
    {
        QTest::addColumn<int>("change");
        QTest::addColumn<bool>("scrollToBottom");

        // Mid-scroll add: the plain case — a finished recording, an import, a
        // USB grab landing a file while the user is somewhere in the tree.
        QTest::newRow("file added, scrolled mid-tree") << int(AddFile) << false;
        // Bottom-scroll remove: the case the restore's clamp can get wrong in
        // the opposite direction. contentHeight SHRINKS, so the pre-reset
        // height hint restorePendingContentY() clamps against is now too
        // generous — restoring past the new maximum would show blank space
        // below the last row for a frame.
        QTest::newRow("file removed, scrolled to bottom") << int(RemoveFile) << true;
        // No reset at all — just a row's transcribed indicator turning on.
        QTest::newRow("transcript finished, scrolled mid-tree") << int(TranscribeFile) << false;
        // The reported repro: deleting a recording out of a folder that also
        // holds EMPTY subfolders. The empty ones are what expandAll() cannot
        // re-expand through, so this is the row that catches the collapse.
        QTest::newRow("recording deleted from nested folder") << int(RemoveNested) << false;
    }

    // The assertion is that the tree only ever shows one of exactly two states
    // — the one before the change and the one after — so any third state is a
    // frame that flashed something the user never asked for.
    void structuralChange_hasNoIntermediateState()
    {
        QFETCH(int, change);
        QFETCH(bool, scrollToBottom);

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(buildScrollableVault(vault.path()));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        expandEverything(treeItem);
        const int kRows = 5 + 60 + 15 + 1 + 2 + 3; // + "test folder", 2 empty subfolders, 3 recordings
        QTRY_COMPARE(treeItem->property("rows").toInt(), kRows);

        // The harness's most important self-check. With a null controller,
        // refreshTreeOrdering() short-circuits and hands the proxy an EMPTY
        // fileSortKeys — the sort never runs, and this whole file measures a
        // cheaper app than the real one. See TranscriptionControllerStub.
        QObject *controller = view.rootObject()->findChild<QObject *>(QStringLiteral("controller"));
        QVERIFY2(controller, "the stub controller must exist");
        QCOMPARE(sidebarItem->property("transcriptionController").value<QObject *>(), controller);
        QStringList audioPaths;
        QObject *modelObj = view.rootObject()->findChild<QObject *>(QStringLiteral("vaultModel"));
        QVERIFY(modelObj);
        QMetaObject::invokeMethod(modelObj, "allAudioPaths", Qt::DirectConnection,
                                   Q_RETURN_ARG(QStringList, audioPaths));
        QCOMPARE(audioPaths.size(), 78);

        // Scroll away from the top: with contentY at 0 the reset's clamp has
        // nothing to destroy and the bug cannot reproduce at all.
        const qreal maxY = treeItem->property("contentHeight").toReal() - treeItem->height();
        QVERIFY2(maxY > 200, "fixture check: the tree must be genuinely scrollable");
        treeItem->setProperty("contentY", scrollToBottom ? maxY : 900.0);
        QTest::qWait(120);
        QVERIFY2(treeItem->property("contentY").toReal() > 200,
                 "fixture check: the scroll offset must have stuck before the change");

        // The RIBBON, and the tree's own position under it. TreeView is
        // anchored to ribbon.bottom, and the ribbon's height is a wrapping
        // Flow's implicitHeight — so a button appearing or disappearing
        // reflows the ENTIRE tree downward. onModelReset clears selectedPaths
        // and re-runs refreshSelectionEligibility, so a file change can do
        // exactly that. Row y is in the tree's CONTENT coordinates and cannot
        // see it; this is why the sample has to reach outside the TreeView.
        QQuickItem *ribbonFlow = findByClassNamePrefix(sidebarItem, "SidebarRibbon");
        QVERIFY2(ribbonFlow, "could not locate SidebarRibbon");
        QQuickItem *ribbon = ribbonFlow->parentItem();
        QVERIFY(ribbon);

        // One frame of truth: every REALIZED row's file, indent depth and y,
        // plus the scroll offset, the tree's height, the tree's position in
        // the sidebar, and the ribbon geometry driving that position. Depth
        // catches a lost expansion, row y catches an in-tree reflow, contentY
        // catches the offset round-trip, contentHeight catches a half-applied
        // layout, and treeY/ribbonH catch the whole tree sliding.
        auto sample = [&]() {
            QStringList parts;
            const int n = treeItem->property("rows").toInt();
            for (int r = 0; r < n; ++r) {
                QQuickItem *it = rowItem(treeItem, r);
                if (!it)
                    continue; // unrealized: off-screen, and off-screen is not visible
                parts << QStringLiteral("%1@d%2y%3")
                             .arg(it->property("path").toString().section(QLatin1Char('/'), -1))
                             .arg(it->property("depth").toInt())
                             .arg(qRound(it->y()));
            }
            int ribbonVisible = 0;
            const auto ribbonKids = ribbonFlow->childItems();
            for (QQuickItem *kid : ribbonKids) {
                if (kid->isVisible())
                    ++ribbonVisible;
            }
            return QStringLiteral("rows=%1 cy=%2 ch=%3 treeY=%4 treeH=%5 ribbonH=%6 flowH=%7 btns=%8 | %9")
                    .arg(n)
                    .arg(qRound(treeItem->property("contentY").toReal()))
                    .arg(qRound(treeItem->property("contentHeight").toReal()))
                    .arg(qRound(treeItem->y()))
                    .arg(qRound(treeItem->height()))
                    .arg(qRound(ribbon->height()))
                    .arg(qRound(ribbonFlow->implicitHeight()))
                    .arg(ribbonVisible)
                    .arg(parts.join(QLatin1Char(' ')));
        };

        // Select rows before the change. The ribbon's buttons are gated on
        // selection eligibility, and onModelReset CLEARS selectedPaths — so
        // measuring with an empty selection measures the one state that
        // cannot show the ribbon changing shape.
        const QStringList selected = {vault.filePath(QStringLiteral("Notes/take-00.mp3")),
                                       vault.filePath(QStringLiteral("Notes/take-01.mp3"))};
        sidebarItem->setProperty("selectedPaths", selected);
        QMetaObject::invokeMethod(sidebarItem, "refreshSelectionEligibility", Qt::DirectConnection);
        QTest::qWait(80);
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList().size(), 2);

        const QString before = sample();

        QStringList frames;
        // Per-frame (what is about to be painted) AND a precise 1ms event-loop
        // sampler — a disturbance living in either is one a real compositor
        // could show.
        QObject::connect(&view, &QQuickWindow::afterAnimating, treeItem, [&]() { frames << sample(); });
        QTimer sampler;
        sampler.setTimerType(Qt::PreciseTimer);
        sampler.setInterval(1);
        QObject::connect(&sampler, &QTimer::timeout, treeItem, [&]() { frames << sample(); });

        // Into/out of "Notes", which at 28px per row sits inside the viewport
        // at either scroll position — so the changed row is realized and the
        // sample can actually see it. Notes/take-04.mp3 is one of the ~10%
        // buildScrollableVault() leaves without a sidecar, so it's the one
        // that can have a transcript "finish".
        const QString target = vault.filePath(
            change == RemoveFile       ? QStringLiteral("Voice/take-11.mp3")
            : change == TranscribeFile ? QStringLiteral("Notes/take-04.mp3")
            : change == RemoveNested   ? QStringLiteral("test folder/rec-1.mp3")
                                        : QStringLiteral("Notes/take-99-new.mp3"));
        if (change == TranscribeFile)
            QVERIFY2(!QFile::exists(target + QStringLiteral(".md")),
                     "fixture check: the file must start untranscribed");

        sampler.start();
        switch (change) {
        case RemoveFile:
            QVERIFY(QFile::remove(target));
            QVERIFY(QFile::remove(target + QStringLiteral(".md")));
            break;
        case RemoveNested:
            QVERIFY(QFile::remove(target)); // no sidecar on this one
            break;
        case AddFile:
            QVERIFY(touch(target));
            break;
        case TranscribeFile:
            // Both halves of what a finishing transcription does: the sidecar
            // lands on disk, and the controller reports it. structural=false,
            // as Task::isStructuralFileOp says for a transcribe.
            QVERIFY(touch(target + QStringLiteral(".md"),
                           "---\nlabel: Freshly transcribed\ntags: [x]\n---\n\n[0] hello world\n"));
            QMetaObject::invokeMethod(controller, "emitFinished", Qt::DirectConnection,
                                       Q_ARG(QUrl, QUrl::fromLocalFile(target)), Q_ARG(bool, false));
            break;
        }

        // A real event loop, not QTest::qWait: qWait pumps events in ~10ms
        // chunks, starving the 1ms sampler down to ~10ms — coarse enough to
        // step right over the ~15ms window being hunted. Long enough to cover
        // the QFileSystemWatcher's debounce plus the rebuild it drives.
        QEventLoop settle;
        QTimer::singleShot(1200, &settle, &QEventLoop::quit);
        settle.exec();
        sampler.stop();

        QCOMPARE(treeItem->property("rows").toInt(),
                 (change == RemoveFile || change == RemoveNested) ? kRows - 1
                 : change == AddFile ? kRows + 1 : kRows);
        const QString after = sample();
        QVERIFY2(frames.size() > 200,
                 qPrintable(QStringLiteral("only %1 samples — the samplers never really ran, so a flash "
                                            "could have slipped through unobserved").arg(frames.size())));

        // KNOWN RESIDUAL, deliberately recorded rather than hidden. Deleting a
        // row while the view is pinned at the very bottom leaves one frame
        // drawn about a row off: contentHeight is still the pre-removal value
        // when rowsRemoved fires (TableView publishes it a layout later), so
        // both the Flickable's own clamp and any clamp of ours land a frame
        // late. Two fixes were tried and measured as no-ops (see the
        // searchProxy Connections in Sidebar.qml). It is ~28px for one frame,
        // and only at the exact bottom — unlike the whole-folder collapse this
        // file exists for, which the splice removed outright.
        //
        // An XPASS here is good news and means this can be deleted.
        if (change == RemoveFile && scrollToBottom)
            QEXPECT_FAIL("", "bottom-pinned removal: contentHeight lags one layout", Abort);

        for (int i = 0; i < frames.size(); ++i) {
            if (frames.at(i) == before || frames.at(i) == after)
                continue;
            // Run-length the whole timeline so the failure shows the transient
            // itself rather than just one bad sample.
            QString timeline;
            for (int j = 0; j < frames.size(); ++j) {
                int run = 1;
                while (j + run < frames.size() && frames.at(j + run) == frames.at(j))
                    ++run;
                timeline += QStringLiteral("  x%1  %2\n").arg(run, 4).arg(frames.at(j));
                j += run - 1;
            }
            QFAIL(qPrintable(QStringLiteral("sample %1/%2 showed a state that is neither the before nor "
                                             "the after — the sidebar flashed it on the way:\n%3\nbefore: %4\nafter:  %5")
                                 .arg(i).arg(frames.size()).arg(timeline).arg(before).arg(after)));
        }
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppSettings.qml's Settings{} element
    // is loaded transitively by Sidebar.qml and must never touch the real
    // ~/.config/suopTranscriber. Same setup as tst_sidebar_dragmove.cpp.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_reflow"));
    QGuiApplication app(argc, argv);
    TstSidebarReflow tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_reflow.moc"
