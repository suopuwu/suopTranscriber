// Living spec + runtime verification for Sidebar.qml's drag-to-move gesture
// (Item 40): drives a REAL QQuickView (offscreen, sandboxed config — see
// main() below) loading the actual Sidebar.qml against a real VaultTreeModel,
// dragging real rows with real QTest::mousePress/mouseMove/mouseRelease
// events, and then checking the actual filesystem for the result.
//
// Same harness shape as tst_sidebar_keynav.cpp (read its top-of-file comment
// first) and for the same reason: none of what this covers is answerable from
// source review. Specifically —
//   - whether the row DragHandler wins the pointer grab against the enclosing
//     TreeView's own flick-to-scroll (the entire bug being fixed: a left-drag
//     used to just scroll the tree, "merely drags the whole thing"),
//   - whether the Drag/DropArea handshake actually resolves to the intended
//     destination folder, and
//   - whether the deferred (Qt.callLater) move survives the model rebuild that
//     destroys the very delegate whose handler started it.
// tst_styleguide.cpp keeps the cheap source-scan guards over the same feature;
// this file is the one that proves the gesture MOVES FILES.
//
// No ydotool/wtype/xdotool on this machine (see suopTranscriber's own GUI-
// verification notes) — QTest's synthetic events need none of that; they go
// straight into the QQuickWindow's event pipeline, the same mechanism Qt's own
// autotests use.

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

// Captures qWarning/console.warn output so a test can assert on it, while
// still chaining to the previous handler so the messages stay visible in the
// test log.
QStringList *g_captured = nullptr;
QtMessageHandler g_prevHandler = nullptr;
void captureMessages(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (g_captured)
        g_captured->append(msg);
    if (g_prevHandler)
        g_prevHandler(type, ctx, msg);
}

// See tst_sidebar_keynav.cpp's identical stub: Sidebar.qml's every
// transcriptionController use site guards on it being non-null first, and this
// harness never assigns one — so the property only needs its TYPE to resolve,
// not the real engine (whisper/llama/sherpa-onnx, all irrelevant to dragging).
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
};

} // namespace

class TstSidebarDragMove : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // TreeView/ListView declared plainly in QML are plain QQuickTreeView
    // instances; match by class-name prefix so this never has to name
    // QQuickTreeView as a C++ type (Qt private API). Same helper as
    // tst_sidebar_keynav.cpp.
    static QQuickItem *findByClassNamePrefix(QQuickItem *root, const QByteArray &prefix)
    {
        for (QQuickItem *child : root->findChildren<QQuickItem *>()) {
            if (QByteArray(child->metaObject()->className()).startsWith(prefix))
                return child;
        }
        return nullptr;
    }

    // Independent ground truth for "which visible row is this path at" — reads
    // the model's own roles through QAbstractItemModel's public API rather than
    // reusing any of Sidebar.qml's JS, so a passing test can't just be the
    // feature agreeing with itself.
    static int rowForPath(QQuickItem *treeItem, const QString &path)
    {
        QObject *modelObj = treeItem->property("model").value<QObject *>();
        auto *model = qobject_cast<QAbstractItemModel *>(modelObj);
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

    // Sidebar.qml auto-expands the whole tree, but it hooks that off the
    // proxy's modelReset — and this harness hands VaultTreeModel its vaultPath
    // at CONSTRUCTION, so that reset can land before the Sidebar's Connections
    // exist to hear it. (The real app opens its vault after load, from
    // AppSettings.lastVaultPath, so it does hear it.) Expand explicitly through
    // TreeView's own API rather than depending on that race: these tests are
    // about the drag gesture, not about when expansion happens.
    static void expandEverything(QQuickItem *treeItem)
    {
        QMetaObject::invokeMethod(treeItem, "expandRecursively", Qt::DirectConnection,
                                   Q_ARG(int, -1), Q_ARG(int, -1));
        QTest::qWait(50);
    }

    static QQuickItem *rowItem(QQuickItem *treeItem, int row)
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, item), Q_ARG(QPoint, QPoint(0, row)));
        return item;
    }

    static QPoint rowCenter(QQuickItem *row)
    {
        return row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint();
    }

    // The row's OWN hover outline. By objectName, not by type: every per-row
    // action button in the cluster carries a HoverBorder of its own, so a
    // type-based lookup would hand back whichever one findChildren happened to
    // reach first.
    static QQuickItem *rowHoverBorder(QQuickItem *row)
    {
        return row->findChild<QQuickItem *>(QStringLiteral("rowHoverBorder"));
    }

    // One drag, delivered the way a mouse actually delivers one: press, then
    // SEVERAL small moves (a single jump to the destination can be swallowed —
    // handlers need movement past the drag threshold to activate, and the
    // Drag/DropArea handshake needs at least one move event landing inside the
    // target to fire its onEntered), then release. qWait after the release lets
    // the Qt.callLater-deferred move + the model rebuild it triggers run.
    static void dragRow(QQuickView *view, const QPoint &from, const QPoint &to)
    {
        QTest::mousePress(view, Qt::LeftButton, Qt::NoModifier, from);
        const int steps = 12;
        for (int i = 1; i <= steps; ++i) {
            const QPoint p(from.x() + (to.x() - from.x()) * i / steps,
                            from.y() + (to.y() - from.y()) * i / steps);
            QTest::mouseMove(view, p);
            QTest::qWait(8);
        }
        QTest::mouseRelease(view, Qt::LeftButton, Qt::NoModifier, to);
        QTest::qWait(80);
    }

    // Builds the view + returns the located TreeView, or fails the test.
    QQuickItem *openVault(QQuickView *view, const QString &vaultPath, QQuickItem **sidebarOut = nullptr)
    {
        view->engine()->addImportPath(m_moduleDir.path());
        // AppIcon (used throughout Sidebar.qml) resolves svg packs through
        // this provider; without it the default material pack's icons can't
        // raster. Per-engine, so add it to each view.
        view->engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                          new IconImageProvider);
        view->rootContext()->setContextProperty(QStringLiteral("vaultUrl"),
                                                 QUrl::fromLocalFile(vaultPath));
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : view->errors())
                msg += e.toString() + QStringLiteral("\n");
            // A QML error here is the exact failure mode a green cmake build
            // does NOT catch (AOT/qmlcachegen can compile a file that still
            // fails to load) — report it verbatim rather than as "null item".
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

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        // Symlink (not copy) to the real checked-out qml/ — must reflect what's
        // on disk right now, never a stale build-time copy.
        QVERIFY2(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")),
                 "failed to symlink qml/ into the test's scratch QML module dir");

        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
        qmldir.write("module SuopTranscriber\n"
                      "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                      "HoverBorder 1.0 qml/HoverBorder.qml\n"
                      "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
                      "AppIcon 1.0 qml/AppIcon.qml\n"
                      "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                      "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                      "Sidebar 1.0 qml/Sidebar.qml\n");
        qmldir.close();

        // Sidebar.qml's AppIcon usages bind IconTheme.resolve(...); register
        // the singleton so they resolve instead of throwing at load.
        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterType<VaultTreeModel>("SuopTranscriber", 1, 0, "VaultTreeModel");
        qmlRegisterType<VaultSearchProxyModel>("SuopTranscriber", 1, 0, "VaultSearchProxyModel");
        qmlRegisterUncreatableType<TranscriptionControllerStub>(
            "SuopTranscriber", 1, 0, "TranscriptionController",
            QStringLiteral("stub — this harness never assigns one"));

        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    id: root\n"
            "    width: 320\n"
            "    height: 400\n"
            "    property string openPath: \"\"\n"
            "    VaultTreeModel { id: vaultModel; objectName: \"vaultModel\"; vaultPath: Qt.resolvedUrl(vaultUrl) }\n"
            "    Sidebar {\n"
            "        id: sidebar\n"
            "        objectName: \"sidebar\"\n"
            "        anchors.fill: parent\n"
            "        model: vaultModel\n"
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

    // The headline claim: dragging an audio row onto a folder row moves the
    // file — and carries its transcript sidecar with it — instead of scrolling
    // the tree.
    void dragNoteOntoFolder_movesFileAndSidecar()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        // "Interviews" holds a seed file only so this test drops onto a
        // realistic, populated folder — an EMPTY folder is a valid drop target
        // too now that the tree shows those (tst_sidebar_expand.cpp).
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3.md")), "---\nmodel: x\n---\n[0] hi\n"));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        QTest::qWait(50); // let the auto-expand pass settle

        const int folderRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Interviews")));
        const int noteRow = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY2(folderRow >= 0, "the Interviews folder must be a visible row");
        QVERIFY2(noteRow >= 0, "talk.mp3 must be a visible row");

        QQuickItem *from = rowItem(treeItem, noteRow);
        QQuickItem *to = rowItem(treeItem, folderRow);
        QVERIFY(from && to);

        dragRow(&view, rowCenter(from), rowCenter(to));

        // The move itself, on the real filesystem.
        QVERIFY2(!QFile::exists(vault.filePath(QStringLiteral("talk.mp3"))),
                 "dragging talk.mp3 onto the Interviews folder must move it out of the vault root");
        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("Interviews/talk.mp3"))),
                 "talk.mp3 must land inside Interviews/");
        // CLAUDE.md vault layout: the transcript is a sibling .md and must never
        // be left split from its audio.
        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("Interviews/talk.mp3.md"))),
                 "the sibling .md transcript must travel with the audio it belongs to");
        QVERIFY2(!QFile::exists(vault.filePath(QStringLiteral("talk.mp3.md"))),
                 "the .md must not be left behind in the vault root");
    }

    // An EMPTY folder is a drop target like any other. This is the case the
    // tree couldn't even express before (empty folders were pruned, so there
    // was no row to drop onto), and it's the obvious thing to do right after
    // making a folder: create it, then drag the first file in. Worth its own
    // test because the empty row is the one whose delegate differs — it's the
    // only folder row that never goes through TreeView's expand machinery.
    void dragNoteOntoEmptyFolder_movesFile()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath(QStringLiteral("Empty"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        QTest::qWait(50); // let the auto-expand pass settle

        const int folderRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Empty")));
        const int noteRow = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY2(folderRow >= 0, "the empty folder must be a visible row");
        QVERIFY2(noteRow >= 0, "talk.mp3 must be a visible row");

        dragRow(&view, rowCenter(rowItem(treeItem, noteRow)), rowCenter(rowItem(treeItem, folderRow)));

        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("Empty/talk.mp3"))),
                 "dragging a note onto an empty folder must move it in");
        QVERIFY2(!QFile::exists(vault.filePath(QStringLiteral("talk.mp3"))),
                 "the note must not be left behind in the vault root");
    }

    // The actual reported bug: a left-drag on a row used to be swallowed by the
    // TreeView's own flick-to-scroll ("merely drags the whole thing"). Needs a
    // tree tall enough to actually BE scrollable — in a vault of three rows
    // nothing can scroll and a "didn't scroll" assertion proves nothing — and a
    // drag long enough to clear the flick threshold several times over.
    void dragDoesNotFlickTheTree()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));
        for (int i = 0; i < 24; ++i)
            QVERIFY(touch(vault.filePath(QStringLiteral("note%1.mp3").arg(i, 2, 10, QLatin1Char('0')))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        // Self-check: if this ever stops holding, the no-scroll assertion below
        // has gone vacuous and the test is worthless.
        QVERIFY2(treeItem->property("contentHeight").toReal() > treeItem->property("height").toReal(),
                 "test vault/viewport assumption is stale — the tree must be scrollable for this "
                 "test to prove anything");

        const int folderRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Interviews")));
        QVERIFY(folderRow >= 0);
        // A row far enough down the viewport that dragging it to the folder is
        // a long vertical stroke — exactly what a flick would eat.
        const int bottomRow = treeItem->property("bottomRow").toInt();
        const int dragRowIdx = qMax(folderRow + 1, bottomRow - 1);
        QQuickItem *from = rowItem(treeItem, dragRowIdx);
        QQuickItem *to = rowItem(treeItem, folderRow);
        QVERIFY(from && to);

        QObject *modelObj = treeItem->property("model").value<QObject *>();
        auto *model = qobject_cast<QAbstractItemModel *>(modelObj);
        QVERIFY(model);
        int pathRole = -1;
        const QHash<int, QByteArray> roles = model->roleNames();
        for (auto it = roles.cbegin(); it != roles.cend(); ++it)
            if (it.value() == "path")
                pathRole = it.key();
        QVERIFY(pathRole >= 0);
        QModelIndex idx;
        QMetaObject::invokeMethod(treeItem, "index", Qt::DirectConnection,
                                   Q_RETURN_ARG(QModelIndex, idx), Q_ARG(int, dragRowIdx), Q_ARG(int, 0));
        const QString draggedPath = model->data(idx, pathRole).toString();
        const QString draggedName = QFileInfo(draggedPath).fileName();

        const qreal contentYBefore = treeItem->property("contentY").toReal();
        dragRow(&view, rowCenter(from), rowCenter(to));

        QVERIFY2(qFuzzyCompare(treeItem->property("contentY").toReal() + 1, contentYBefore + 1),
                 "a row drag must not scroll the tree — the DragHandler has to hold the grab "
                 "against the TreeView's flick (grabPermissions)");
        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("Interviews/") + draggedName)),
                 qPrintable(QStringLiteral("the long drag must still have moved %1 into Interviews/")
                                .arg(draggedName)));
    }

    // A plain click must still open the file — the DragHandler sharing the row
    // with treeRowTap must not eat it (the drag only starts past the threshold).
    void plainClickStillActivatesNote()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        QTest::qWait(50);

        const int noteRow = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(noteRow >= 0);
        QQuickItem *row = rowItem(treeItem, noteRow);
        QVERIFY(row);

        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, rowCenter(row));
        QTest::qWait(30);

        QCOMPARE(view.rootObject()->property("openPath").toString(),
                 vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("talk.mp3"))),
                 "a plain click must not move anything");
    }

    // Dropping a row back onto the tree's empty space below the rows moves it to
    // the vault root — the only gesture that gets a file back OUT of a folder
    // (the root isn't a row of its own).
    void dragNoteOntoEmptySpace_movesToVaultRoot()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/talk.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int noteRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Interviews/talk.mp3")));
        QVERIFY2(noteRow >= 0, "the nested note must be a visible row");
        QQuickItem *row = rowItem(treeItem, noteRow);
        QVERIFY(row);

        // Well below the last row (2 rows in a 400px-tall view), still inside
        // the tree — i.e. genuinely rootDropTarget's empty area.
        const QPoint empty = treeItem->mapToScene(QPointF(treeItem->width() / 2,
                                                            treeItem->height() - 30)).toPoint();
        dragRow(&view, rowCenter(row), empty);

        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("talk.mp3"))),
                 "dropping on the tree's empty space must move the file to the vault root");
        QVERIFY2(!QFile::exists(vault.filePath(QStringLiteral("Interviews/talk.mp3"))),
                 "the file must be gone from its old folder");
    }

    // Dragging a folder row onto another folder moves the whole folder —
    // VaultTreeModel::moveFolder(), which NO other UI path reaches ("Move to…"
    // is note-only, gated on sidebar.ctxIsNote).
    void dragFolderOntoFolder_movesFolder()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Archive/old.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        QTest::qWait(50);

        const int srcRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Archive")));
        const int destRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Interviews")));
        QVERIFY(srcRow >= 0 && destRow >= 0);
        QQuickItem *from = rowItem(treeItem, srcRow);
        QQuickItem *to = rowItem(treeItem, destRow);
        QVERIFY(from && to);

        dragRow(&view, rowCenter(from), rowCenter(to));

        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("Interviews/Archive/old.mp3"))),
                 "dragging the Archive folder onto Interviews must move the folder and its contents");
        QVERIFY2(!QDir(vault.filePath(QStringLiteral("Archive"))).exists(),
                 "Archive must be gone from the vault root");
    }

    // A drag that lands somewhere the move can't legally go must do NOTHING.
    // Dropping a folder into its own child is the impossible target a user can
    // most easily aim at. Both Sidebar's canDropRowsOn() pre-check (which also
    // keeps the target from lighting up) and VaultTreeModel::moveFolder()'s own
    // descendant guard back this claim — the test asserts the user-visible
    // outcome rather than which of the two did the refusing.
    void dragFolderIntoOwnChild_isRefused()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Outer/Inner/deep.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int outerRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Outer")));
        const int innerRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Outer/Inner")));
        QVERIFY(outerRow >= 0 && innerRow >= 0);
        QQuickItem *from = rowItem(treeItem, outerRow);
        QQuickItem *to = rowItem(treeItem, innerRow);
        QVERIFY(from && to);

        dragRow(&view, rowCenter(from), rowCenter(to));

        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("Outer/Inner/deep.mp3"))),
                 "a folder dropped into its own descendant must not move anywhere");
        QVERIFY2(!QDir(vault.filePath(QStringLiteral("Outer/Inner/Outer"))).exists(),
                 "the folder must not have been moved inside itself");
    }

    // What the user sees WHILE the ghost is in the air, which every other test
    // here skips straight past to the filesystem result. Two claims, both from a
    // user report that the row under the ghost lighting up "kind of implies that
    // the item is being added to the other item":
    //   - no row shows its ordinary hover outline mid-drag (a vault note can't
    //     contain another note, so an outline that tracks the ghost from row to
    //     row is claiming a containment that doesn't exist), and
    //   - the destination FOLDER's whole subtree is lit instead — folder + every
    //     row inside it, as ONE region. Here the destination is resolved from a
    //     note being hovered INSIDE it, which is what makes this discriminating:
    //     that note lights up because of where it lives, while a note outside
    //     the folder stays dark. ("The note under the ghost must not light up"
    //     was the older, narrower rule — the highlight covers the folder and all
    //     its items now, so the containment being claimed is a real one.)
    void dragOverNestedNote_litsFolderNotHoveredRow()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int folderRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Interviews")));
        const int seedRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Interviews/seed.mp3")));
        const int noteRow = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(folderRow >= 0 && seedRow >= 0 && noteRow >= 0);
        QQuickItem *folderItem = rowItem(treeItem, folderRow);
        QQuickItem *seedItem = rowItem(treeItem, seedRow);
        QQuickItem *dragItem = rowItem(treeItem, noteRow);
        QVERIFY(folderItem && seedItem && dragItem);

        const QPoint from = rowCenter(dragItem);
        const QPoint to = rowCenter(seedItem);

        // Baseline. Without it, "no outline mid-drag" would also pass if this
        // harness simply couldn't see hover outlines at all.
        QTest::mouseMove(&view, from);
        QQuickItem *dragRowBorder = rowHoverBorder(dragItem);
        QVERIFY2(dragRowBorder, "every tree row must carry its own HoverBorder");
        QTRY_VERIFY2(dragRowBorder->property("active").toBool(),
                     "hovering a row with NO drag in flight must still outline it");

        // Press and move, but deliberately do not release — everything asserted
        // below is about the in-flight state.
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, from);
        const int steps = 12;
        for (int i = 1; i <= steps; ++i) {
            QTest::mouseMove(&view, QPoint(from.x() + (to.x() - from.x()) * i / steps,
                                            from.y() + (to.y() - from.y()) * i / steps));
            QTest::qWait(8);
        }

        QVERIFY2(sidebarItem->property("rowDragActive").toBool(),
                 "the drag must actually be in flight, or the rest of this proves nothing");
        QCOMPARE(sidebarItem->property("rowDragDest").toString(),
                 vault.filePath(QStringLiteral("Interviews")));

        const int rowCount = treeItem->property("rows").toInt();
        for (int r = 0; r < rowCount; ++r) {
            QQuickItem *item = rowItem(treeItem, r);
            if (!item)
                continue;
            QQuickItem *border = rowHoverBorder(item);
            QVERIFY(border);
            QVERIFY2(!border->property("active").toBool(),
                     qPrintable(QStringLiteral("row %1 must not show its hover outline while a row "
                                                "drag is in flight").arg(r)));
        }

        // The destination folder's highlight covers the folder AND its
        // contents, so the whole landing zone lights up as one region. seed.mp3
        // lights because it lives INSIDE Interviews — not because it's the row
        // under the ghost. The discriminator is talk.mp3: a root-level note, so
        // it stays dark no matter that the pointer started on it.
        QQuickItem *folderHighlight = folderItem->findChild<QQuickItem *>(QStringLiteral("rowDropHighlight"));
        QQuickItem *seedHighlight = seedItem->findChild<QQuickItem *>(QStringLiteral("rowDropHighlight"));
        QQuickItem *outsideHighlight = dragItem->findChild<QQuickItem *>(QStringLiteral("rowDropHighlight"));
        QVERIFY(folderHighlight && seedHighlight && outsideHighlight);
        QVERIFY2(folderHighlight->isVisible(),
                 "the destination folder must light up while the ghost hovers a note inside it");
        QVERIFY2(seedHighlight->isVisible(),
                 "a note INSIDE the destination folder must light up too — the highlight covers the "
                 "folder and all its items");
        QVERIFY2(!outsideHighlight->isVisible(),
                 "a note OUTSIDE the destination folder must stay dark — the highlight is the "
                 "destination folder's subtree, not every row");

        // The run draws as ONE perimeter: the folder owns the run's real top
        // edge, seed.mp3 (its last item) owns the real bottom edge, and the
        // seam between them is suppressed on both sides. Without that, each row
        // would draw its own closed box and the "one region" reading is lost.
        QQuickItem *folderOutline = folderItem->findChild<QQuickItem *>(QStringLiteral("folderHighlightOutline"));
        QQuickItem *seedOutline = seedItem->findChild<QQuickItem *>(QStringLiteral("folderHighlightOutline"));
        QVERIFY(folderOutline && seedOutline);
        QVERIFY(folderOutline->isVisible() && seedOutline->isVisible());
        QVERIFY2(folderOutline->property("topRounded").toBool(),
                 "the folder row owns the run's real top edge");
        QVERIFY2(!folderOutline->property("bottomRounded").toBool(),
                 "the folder's bottom is an internal seam — it must not be drawn");
        QVERIFY2(!seedOutline->property("topRounded").toBool(),
                 "the contained note's top is that same internal seam");
        QVERIFY2(seedOutline->property("bottomRounded").toBool(),
                 "the last item in the folder owns the run's real bottom edge");

        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, to);
        QTest::qWait(80);
        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("Interviews/talk.mp3"))),
                 "self-check: the gesture asserted above must have been a real, completable drag");
    }

    // Multi-select: dragging a row that's part of a 2+ selection carries the
    // whole selection, not just the row under the cursor.
    void dragMultiSelection_movesEverySelectedNote()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        QTest::qWait(50);

        const int aRow = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int bRow = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        const int folderRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Interviews")));
        QVERIFY(aRow >= 0 && bRow >= 0 && folderRow >= 0);

        // Build the multi-selection through the real gesture: click, then
        // ctrl-click (Sidebar's handleNoteClick).
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, rowCenter(rowItem(treeItem, aRow)));
        QTest::qWait(20);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::ControlModifier, rowCenter(rowItem(treeItem, bRow)));
        QTest::qWait(20);
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList().size(), 2);

        dragRow(&view, rowCenter(rowItem(treeItem, bRow)), rowCenter(rowItem(treeItem, folderRow)));

        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("Interviews/a.mp3"))),
                 "dragging one row of a multi-selection must carry the whole selection");
        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("Interviews/b.mp3"))),
                 "the dragged row itself must move too");
    }

    // Scrolling a tree big enough to RECYCLE its delegates must not log
    // "AppIcon: no icon pack supplies 'collapse'" (user report: it fired
    // repeatedly whenever files were moved in a real, scroll-sized vault).
    //
    // The icon was never actually missing — every pack defines 'collapse'.
    // TableView/TreeView pool and reuse delegates, reassigning their required
    // properties one at a time, so the expand/collapse glyph's `name` can
    // already hold the incoming row's value while its `resolved`/`kind` still
    // reflect the outgoing one. AppIcon reported that momentary disagreement
    // as a missing icon (see AppIcon.qml's _warnIfStillUnresolved). It lives
    // in this file because reproducing it needs exactly what this file already
    // has: a tree tall enough to scroll, hence to recycle.
    void recyclingDelegates_doesNotWarnAboutMissingIcons()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));
        for (int i = 0; i < 40; ++i)
            QVERIFY(touch(vault.filePath(QStringLiteral("note%1.mp3").arg(i, 2, 10, QLatin1Char('0')))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        // Self-checks: without BOTH of these the test is vacuous — no reuse
        // means no transient means nothing to have regressed.
        QVERIFY2(treeItem->property("reuseItems").toBool(),
                 "this test is about pooled delegate REUSE; TreeView must have it enabled");
        QVERIFY2(treeItem->property("contentHeight").toReal() > treeItem->property("height").toReal(),
                 "the tree must be scrollable for delegates to recycle — fixture assumption is stale");

        QStringList messages;
        g_captured = &messages;
        g_prevHandler = qInstallMessageHandler(captureMessages);

        // Scroll the full height several times: every pass drags rows through
        // the reuse pool, and a folder row (the only one with an expand glyph)
        // in and out of the viewport.
        const qreal maxY = treeItem->property("contentHeight").toReal() - treeItem->property("height").toReal();
        for (int pass = 0; pass < 3; ++pass) {
            for (qreal y = 0; y <= maxY; y += 40) {
                treeItem->setProperty("contentY", y);
                QTest::qWait(6);   // let bindings settle + Qt.callLater fire
            }
            for (qreal y = maxY; y >= 0; y -= 40) {
                treeItem->setProperty("contentY", y);
                QTest::qWait(6);
            }
        }
        QTest::qWait(50);

        qInstallMessageHandler(g_prevHandler);
        g_captured = nullptr;

        const QStringList iconWarnings = messages.filter(QStringLiteral("no icon pack supplies"));
        QVERIFY2(iconWarnings.isEmpty(),
                 qPrintable(QStringLiteral("recycling delegates logged %1 spurious missing-icon "
                                            "warning(s), e.g. \"%2\" — the icon exists; this is a "
                                            "transient during delegate reuse")
                                .arg(iconWarnings.size())
                                .arg(iconWarnings.value(0))));
    }

    // The folder highlight's OTHER source: a plain mouse hover, no drag in
    // flight. Same run machinery as the drop highlight asserted above (one
    // perimeter over the folder + its items, seams suppressed), which is why it
    // lives beside it — only the trigger and the stroke color differ.
    void hoverFolder_litsFolderAndItsItems()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        QQuickItem *folderItem = rowItem(treeItem, rowForPath(treeItem, vault.filePath(QStringLiteral("Interviews"))));
        QQuickItem *seedItem = rowItem(treeItem, rowForPath(treeItem, vault.filePath(QStringLiteral("Interviews/seed.mp3"))));
        QQuickItem *talkItem = rowItem(treeItem, rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(folderItem && seedItem && talkItem);

        auto outlineOf = [](QQuickItem *row) {
            return row->findChild<QQuickItem *>(QStringLiteral("folderHighlightOutline"));
        };

        // Prime the pointer somewhere harmless first. QTest::mouseMove to the
        // coordinates the pointer already occupies (carried over from an
        // earlier test's window — these run in one process) delivers no move
        // event at all, so no HoverHandler activates and this test fails only
        // when run as part of the suite. Empty space below the rows, so this
        // move itself hovers nothing.
        QTest::mouseMove(&view, QPoint(5, view.height() - 5));
        QTest::qWait(20);

        // Hovering the FOLDER lights the folder and everything in it.
        QTest::mouseMove(&view, rowCenter(folderItem));
        QTRY_COMPARE(sidebarItem->property("hoveredFolderPath").toString(),
                     vault.filePath(QStringLiteral("Interviews")));
        QVERIFY2(outlineOf(folderItem)->isVisible(), "hovering a folder must outline the folder itself");
        QVERIFY2(outlineOf(seedItem)->isVisible(),
                 "hovering a folder must also outline its contents — the folder and all its items");
        QVERIFY2(!outlineOf(talkItem)->isVisible(),
                 "a row outside the hovered folder must stay dark");
        // One perimeter, not two stacked boxes.
        QVERIFY2(!outlineOf(folderItem)->property("bottomRounded").toBool(),
                 "the seam between the folder and its first item must be suppressed");
        QVERIFY2(!outlineOf(seedItem)->property("topRounded").toBool(),
                 "...on both sides of that seam");
        // The run's outline replaces the folder row's own single-row hover box,
        // rather than drawing a second outline on top of it.
        QQuickItem *folderBorder = rowHoverBorder(folderItem);
        QVERIFY(folderBorder);
        QVERIFY2(!folderBorder->property("active").toBool(),
                 "the folder's single-row hover outline must give way to the run's perimeter");

        // Hovering a plain NOTE is unchanged: its own one-row outline, and no
        // folder run anywhere.
        QTest::mouseMove(&view, rowCenter(talkItem));
        QTRY_VERIFY2(rowHoverBorder(talkItem)->property("active").toBool(),
                     "hovering a note must still outline just that note");
        QCOMPARE(sidebarItem->property("hoveredFolderPath").toString(), QString());
        QVERIFY2(!outlineOf(folderItem)->isVisible(),
                 "leaving the folder must drop its run highlight");
        QVERIFY2(!outlineOf(seedItem)->isVisible(), "...including over its contents");
    }

    // The post-drop "flash" (user report: "a graphical flash (rapid position
    // change, looks weird) as the UI adjusts to the new state", narrowed by
    // the user to moving a file from the vault root INTO a folder).
    //
    // A move rebuilds VaultTreeModel, and rebuild() is a full
    // beginResetModel/endResetModel — which invalidates every QModelIndex, so
    // TreeView drops its entire expansion state and falls back to root-level
    // rows. Sidebar.qml re-expands afterward; the bug was that it did so from
    // a 16ms polling Timer (~1 frame at 60Hz), so the COLLAPSED tree really
    // was painted: the destination folder's contents vanished and every row
    // below it slid up ~3 rows, then snapped back. Measured here before the
    // fix as ~22 consecutive samples of:
    //     Interviews@d0y0 note1@d0y28 note2@d0y56 ...   (a.mp3/b.mp3 gone)
    //
    // Two things this has to do that an earlier version of it did NOT, both
    // of which made it silently fail to reproduce the bug:
    //   - drive a REAL drop, not a direct model.moveFile() call. The direct
    //     call resets the model outside the event loop and the view never
    //     publishes the collapsed state at all — the flash simply doesn't
    //     happen on that path.
    //   - sample what each row SHOWS (path + depth), not just how many rows
    //     there are or where row N sits. Row y positions are index-based and
    //     uniform, so a collapse that removes rows leaves the surviving y
    //     values looking untouched; and a root->folder move is a DEPTH change
    //     (0 -> 1), which a y-only probe cannot see at all.
    //
    // The assertion is that the tree only ever shows one of exactly two
    // states — the one before the drop and the one after — so any third state
    // is a frame that flashed something the user never asked for.
    void dropIntoFolder_hasNoIntermediateState()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/b.mp3"))));
        for (int i = 0; i < 6; ++i)
            QVERIFY(touch(vault.filePath(QStringLiteral("note%1.mp3").arg(i))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        expandEverything(treeItem);
        // Interviews + a + b + 6 notes.
        const int kRows = 9;
        QTRY_COMPARE(treeItem->property("rows").toInt(), kRows);

        // One frame of truth: every row's file and its indent depth.
        auto sample = [&]() {
            QStringList row;
            const int n = treeItem->property("rows").toInt();
            for (int r = 0; r < n; ++r) {
                QQuickItem *it = rowItem(treeItem, r);
                if (!it) {
                    row << QStringLiteral("<unrealized>");
                    continue;
                }
                // path + indent depth + y: depth catches the root->folder
                // move, y catches any reflow (the trailing forceLayout after
                // re-expansion is deferred, so a reflow it caused would land
                // here as its own distinct frame).
                row << QStringLiteral("%1@d%2y%3")
                            .arg(it->property("path").toString().section(QLatin1Char('/'), -1))
                            .arg(it->property("depth").toInt())
                            .arg(qRound(it->y()));
            }
            return row.join(QLatin1Char(' '));
        };

        const QString before = sample();
        QVERIFY2(before.contains(QStringLiteral("a.mp3@d1")) && before.contains(QStringLiteral("note0.mp3@d0")),
                 qPrintable(QStringLiteral("fixture check: expected an expanded folder and a root-level "
                                            "note to drag, got: %1").arg(before)));

        QStringList frames;
        // Per-frame (what is about to be painted) AND a precise 1ms event-loop
        // sampler — a disturbance living in either is one a real compositor
        // could show.
        QObject::connect(&view, &QQuickWindow::afterAnimating, treeItem, [&]() { frames << sample(); });
        QTimer sampler;
        sampler.setTimerType(Qt::PreciseTimer);
        sampler.setInterval(1);
        QObject::connect(&sampler, &QTimer::timeout, treeItem, [&]() { frames << sample(); });

        const QPoint from = rowCenter(rowItem(treeItem, rowForPath(treeItem, vault.filePath(QStringLiteral("note0.mp3")))));
        const QPoint to = rowCenter(rowItem(treeItem, rowForPath(treeItem, vault.filePath(QStringLiteral("Interviews")))));

        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, from);
        for (int i = 1; i <= 12; ++i) {
            QTest::mouseMove(&view, QPoint(from.x() + (to.x() - from.x()) * i / 12,
                                            from.y() + (to.y() - from.y()) * i / 12));
            QTest::qWait(8);
        }
        // Sample from the instant of the drop onward: the drag itself is not
        // under test here (dragDoesNotFlickTheTree covers that), the aftermath is.
        sampler.start();
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, to);
        // A real event loop, not QTest::qWait: qWait pumps events in ~10ms
        // chunks, starving the 1ms sampler down to ~10ms — coarse enough to
        // step right over the ~16ms window being hunted. Long enough to cover
        // the move's own rebuild AND the QFileSystemWatcher-driven rebuild it
        // provokes (a second, asynchronous reset, so a second chance to flash).
        QEventLoop settle;
        QTimer::singleShot(400, &settle, &QEventLoop::quit);
        settle.exec();
        sampler.stop();

        QVERIFY2(QFile::exists(vault.filePath(QStringLiteral("Interviews/note0.mp3"))),
                 "self-check: the drag must actually have moved the file, or this measured nothing");
        const QString after = sample();
        QVERIFY2(after.contains(QStringLiteral("note0.mp3@d1")),
                 qPrintable(QStringLiteral("the moved note must end up rendered INSIDE the folder "
                                            "(depth 1), got: %1").arg(after)));
        QVERIFY2(frames.size() > 50,
                 qPrintable(QStringLiteral("only %1 samples — the samplers never really ran, so a flash "
                                            "could have slipped through unobserved").arg(frames.size())));

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
                                             "the after — the tree flashed it on the way:\n%3\nbefore: %4\nafter:  %5")
                                 .arg(i).arg(frames.size()).arg(timeline).arg(before).arg(after)));
        }
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppSettings.qml's Settings{} element
    // is loaded transitively by Sidebar.qml, and must never touch the real
    // ~/.config/suopTranscriber (CLAUDE.md: never let testing touch the user's
    // real vault or app config). Same setup as tst_sidebar_keynav.cpp.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_dragmove"));
    QGuiApplication app(argc, argv);
    TstSidebarDragMove tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_dragmove.moc"
