// Living spec + runtime verification for Sidebar.qml's inline rename and its
// multi-select delete: drives a REAL QQuickView (offscreen, sandboxed config —
// see main() below) loading the actual Sidebar.qml against a real
// VaultTreeModel, clicking real rows with real QTest mouse events and typing
// with real key events, then checking the actual filesystem for the result.
//
// Same harness shape as tst_sidebar_dragmove.cpp / tst_sidebar_keynav.cpp
// (read either's top-of-file comment first) and for the same reason: none of
// what this covers is answerable from source review. Specifically —
//   - whether a click on an already-selected row's NAME opens the editor,
//     while the same click on a not-yet-selected row still just opens the file
//     (one TapHandler decides between two outcomes; both halves are gestures),
//   - whether the deferred (Qt.callLater) rename commit survives the model
//     rebuild that destroys the very delegate whose handler started it,
//   - whether Escape abandons an edit without writing it, and
//   - whether deleteFiles() — a QList<QUrl> invokable handed a JS array of
//     "file://"-prefixed strings — actually converts and deletes, rather than
//     silently no-op'ing on a failed sequence conversion.
// tst_styleguide.cpp keeps the cheap source-scan guards over the same feature;
// this file is the one that proves the gesture RENAMES FILES.
//
// A green cmake build does NOT prove any of this: qmlcachegen/AOT happily
// compiles QML that still fails at load, so openVault() reports view->errors()
// verbatim rather than as a bare null-item failure.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
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

// See tst_sidebar_dragmove.cpp's identical stub: Sidebar.qml guards every
// transcriptionController use site on it being non-null, and this harness never
// assigns one — the property only needs its TYPE to resolve.
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
};

} // namespace

class TstSidebarRename : public QObject
{
    Q_OBJECT

private:
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
    // the model's own roles through QAbstractItemModel's public API rather than
    // reusing any of Sidebar.qml's JS, so a passing test can't just be the
    // feature agreeing with itself. (Same helper as tst_sidebar_dragmove.cpp.)
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

    // See tst_sidebar_dragmove.cpp's identical helper: this harness hands
    // VaultTreeModel its vaultPath at CONSTRUCTION, so the Sidebar's
    // auto-expand Connections can miss that first reset. Expand explicitly
    // rather than depend on the race.
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

    // A point ON the row's painted name text. Deliberately NOT the row centre:
    // the whole point of the gesture is that it's the NAME that's clickable-to-
    // rename, and the name Text fills the row's remaining width while painting
    // only its first implicitWidth pixels. Aims a few px into the glyphs.
    static QPoint nameTextPoint(QQuickItem *row)
    {
        QQuickItem *nameText = row->findChild<QQuickItem *>(QStringLiteral("rowNameText"));
        if (!nameText)
            return {};
        const qreal w = qMin(nameText->width(), nameText->property("implicitWidth").toReal());
        return nameText->mapToScene(QPointF(qMax(qreal(2), w / 2), nameText->height() / 2)).toPoint();
    }

    static QQuickItem *renameFieldFor(QQuickItem *row)
    {
        return row->findChild<QQuickItem *>(QStringLiteral("rowRenameField"));
    }

    // The editor is only "open" on a row when it's actually visible there —
    // every row owns a rowRenameField instance, hidden until that row is the
    // one being renamed (see Sidebar.qml's renamingPath).
    static bool renameFieldVisibleOn(QQuickItem *row)
    {
        QQuickItem *field = renameFieldFor(row);
        return field && field->isVisible();
    }

    static void clickAt(QQuickView *view, const QPoint &p, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QTest::mouseClick(view, Qt::LeftButton, mods, p);
        QTest::qWait(60);
    }

    // QTest::keyClicks() is a QWidget-only API — the QWindow overloads only
    // cover a single key, so type character by character into whatever has
    // focus. The first keystroke replaces the editor's initial selectAll().
    static void typeText(QQuickView *view, const QString &text)
    {
        for (const QChar &c : text)
            QTest::keyClick(view, c.toLatin1());
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
            // The exact failure mode a green cmake build does NOT catch
            // (AOT/qmlcachegen can compile a file that still fails to load) —
            // report it verbatim rather than as "null item".
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
                      "UnderlineField 1.0 qml/UnderlineField.qml\n"
                      "AppIcon 1.0 qml/AppIcon.qml\n"
                      "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                      "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                      "Sidebar 1.0 qml/Sidebar.qml\n");
        qmldir.close();

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

    // The headline claim: click a note once to select/open it, click its NAME
    // again, type, press Return — the file is renamed on disk, and its
    // transcript sidecar follows it (VaultTreeModel::renameFile's contract).
    //
    // This also proves the deferred commit survives: renameFile() rebuilds the
    // model, destroying the delegate whose TextField handler is mid-execution.
    void clickNameOfSelectedNote_rename_renamesFileAndSidecar()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3.md")), "---\nmodel: x\n---\n[0] hi\n"));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY2(row >= 0, "talk.mp3 must be a visible row");
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        // First click selects + opens; the editor must NOT open yet.
        clickAt(&view, nameTextPoint(item));
        QVERIFY2(!renameFieldVisibleOn(item),
                 "the FIRST click on an unselected row must open the file, not start a rename");

        // Second click, on the name of the now-selected row -> inline edit.
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY2(renameFieldVisibleOn(item),
                     "clicking the name of an already-selected row must open the inline rename editor");
        QQuickItem *field = renameFieldFor(item);
        QVERIFY(field);
        QTRY_VERIFY2(field->hasActiveFocus(), "the rename editor must take focus so the user can just type");
        // It seeds with the BASE name — renameFile() re-appends the extension
        // itself, so seeding "talk.mp3" would rename the file to "talk.mp3.mp3".
        QCOMPARE(field->property("text").toString(), QStringLiteral("talk"));

        typeText(&view, QStringLiteral("interview"));
        QTest::keyClick(&view, Qt::Key_Return);
        QTest::qWait(150); // let the Qt.callLater commit + model rebuild run

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("interview.mp3"))),
                 "the audio file must be renamed on disk");
        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("interview.mp3.md"))),
                 "the transcript sidecar must follow the audio's new name");
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3.md"))));
    }

    // Escape abandons the edit: nothing is written, and the editor closes
    // (StyleGuide.md's commit convention — Escape reverts without committing).
    void escape_abandonsRenameWithoutWriting()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        clickAt(&view, nameTextPoint(item));
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY(renameFieldVisibleOn(item));

        typeText(&view, QStringLiteral("zzz"));
        QTest::keyClick(&view, Qt::Key_Escape);
        QTest::qWait(150);

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3"))),
                 "Escape must not rename the file");
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("zzz.mp3"))));
        QCOMPARE(sidebarItem->property("renamingPath").toString(), QString());
        QVERIFY2(!renameFieldVisibleOn(item), "Escape must close the inline editor");
    }

    // Blur commits, matching StyleGuide.md's single-line-field convention —
    // clicking away is not a cancel.
    void blur_commitsRename()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("other.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        clickAt(&view, nameTextPoint(item));
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY(renameFieldVisibleOn(item));

        typeText(&view, QStringLiteral("renamed"));
        // Click a different row -> the editor blurs.
        const int otherRow = rowForPath(treeItem, vault.filePath(QStringLiteral("other.mp3")));
        QVERIFY(otherRow >= 0);
        QQuickItem *otherItem = rowItem(treeItem, otherRow);
        QVERIFY(otherItem);
        clickAt(&view, nameTextPoint(otherItem));
        QTest::qWait(150);

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("renamed.mp3"))),
                 "blurring the rename editor must commit it, not silently discard the edit");
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3"))));
    }

    // Entering edit mode must not itself be read as a blur. The editor becomes
    // visible a frame before focus lands, so an unguarded blur-commit closes it
    // instantly — the user would see it flash and vanish.
    void openingTheEditor_doesNotInstantlyCommitAndClose()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        clickAt(&view, nameTextPoint(item));
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY(renameFieldVisibleOn(item));

        // Still open several frames later, with the row still in edit mode.
        QTest::qWait(200);
        QVERIFY2(renameFieldVisibleOn(item), "the editor must stay open after it opens");
        QCOMPARE(sidebarItem->property("renamingPath").toString(),
                 vault.filePath(QStringLiteral("talk.mp3")));
    }

    // Clicking the name of a row that ISN'T the sole selection must keep doing
    // what it always did — open the file. The rename gesture must not swallow
    // ordinary click-to-open.
    void clickNameOfUnselectedNote_opensFileAndDoesNotRename()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);
        QQuickItem *itemA = rowItem(treeItem, rowA);
        QQuickItem *itemB = rowItem(treeItem, rowB);
        QVERIFY(itemA && itemB);

        clickAt(&view, nameTextPoint(itemA)); // a.mp3 is now the selection
        // Clicking B's name: B is NOT selected, so this opens B rather than
        // renaming it.
        clickAt(&view, nameTextPoint(itemB));
        QVERIFY2(!renameFieldVisibleOn(itemB),
                 "clicking an unselected row's name must open it, not start a rename");
        QCOMPARE(sidebarItem->property("renamingPath").toString(), QString());
        QCOMPARE(view.rootObject()->property("openPath").toString(),
                 vault.filePath(QStringLiteral("b.mp3")));
    }

    // Ctrl-click builds a multi-selection; it must never trip the rename
    // gesture, even when it lands on the name of an already-selected row.
    void ctrlClickOnSelectedName_doesNotRename()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);
        QQuickItem *itemA = rowItem(treeItem, rowA);
        QQuickItem *itemB = rowItem(treeItem, rowB);
        QVERIFY(itemA && itemB);

        clickAt(&view, nameTextPoint(itemA));
        clickAt(&view, nameTextPoint(itemB), Qt::ControlModifier); // now 2 selected
        // Ctrl-click A's name: A IS selected, but this is a modified click
        // building a selection, not a rename request.
        clickAt(&view, nameTextPoint(itemA), Qt::ControlModifier);
        QVERIFY2(!renameFieldVisibleOn(itemA), "a Ctrl-click must never start a rename");
        QCOMPARE(sidebarItem->property("renamingPath").toString(), QString());
    }

    // Folder rows rename too (VaultTreeModel::renameFolder), keeping their
    // contents. Driven through beginRename() — a folder is never part of
    // selectedPaths, so the click-the-selected-name gesture can't reach it;
    // the context menu's "Rename" is its only entry point, and that's the
    // function that entry calls.
    void beginRename_onFolder_renamesFolderKeepingContents()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const QString folderPath = vault.filePath(QStringLiteral("Interviews"));
        const int row = rowForPath(treeItem, folderPath);
        QVERIFY2(row >= 0, "the Interviews folder must be a visible row");
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        QMetaObject::invokeMethod(sidebarItem, "beginRename", Q_ARG(QVariant, folderPath),
                                   Q_ARG(QVariant, true), Q_ARG(QVariant, QString()));
        QTRY_VERIFY2(renameFieldVisibleOn(item), "beginRename() must open the editor on the folder row");
        QQuickItem *field = renameFieldFor(item);
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());
        // A folder seeds with its own name, extension-stripping never applies.
        QCOMPARE(field->property("text").toString(), QStringLiteral("Interviews"));

        typeText(&view, QStringLiteral("Talks"));
        QTest::keyClick(&view, Qt::Key_Return);
        QTest::qWait(150);

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("Talks/seed.mp3"))),
                 "the folder must be renamed with its contents intact");
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("Interviews"))));
    }

    // A rename the model refuses (name already taken) must leave the file
    // alone and SAY so, rather than failing silently.
    void renameToExistingName_isRefusedAndReported()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("taken.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        clickAt(&view, nameTextPoint(item));
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY(renameFieldVisibleOn(item));

        typeText(&view, QStringLiteral("taken"));
        QTest::keyClick(&view, Qt::Key_Return);
        QTest::qWait(150);

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3"))),
                 "a refused rename must leave the original file untouched");
        QCOMPARE(QFileInfo(vault.filePath(QStringLiteral("taken.mp3"))).size(), qint64(1));
        QVERIFY2(!sidebarItem->property("rowActionError").toString().isEmpty(),
                 "a refused rename must surface an error banner, not fail silently");
    }

    // Multi-select delete: the whole selection goes, in one call. This is the
    // half a source scan can't answer — deleteFiles() takes a QList<QUrl> and
    // QML hands it a JS array of "file://" strings, so a broken sequence
    // conversion would silently delete nothing.
    void confirmFileDelete_deletesWholeSelectionWithSidecars()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3.md")), "---\nmodel: x\n---\n[0] hi\n"));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("keep.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);
        QQuickItem *itemA = rowItem(treeItem, rowA);
        QQuickItem *itemB = rowItem(treeItem, rowB);
        QVERIFY(itemA && itemB);

        // Build a real 2-row selection with real clicks.
        clickAt(&view, nameTextPoint(itemA));
        clickAt(&view, nameTextPoint(itemB), Qt::ControlModifier);
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList().size(), 2);

        // Right-clicking a selected row targets the whole selection.
        QMetaObject::invokeMethod(sidebarItem, "beginDeleteFile",
                                   Q_ARG(QVariant, vault.filePath(QStringLiteral("a.mp3"))));
        QCOMPARE(sidebarItem->property("pendingDeletePaths").toStringList().size(), 2);

        QMetaObject::invokeMethod(sidebarItem, "confirmFileDelete");
        QTest::qWait(150);

        QVERIFY2(!QFileInfo::exists(vault.filePath(QStringLiteral("a.mp3"))), "every selected file must go");
        QVERIFY2(!QFileInfo::exists(vault.filePath(QStringLiteral("a.mp3.md"))),
                 "a deleted file's transcript sidecar must go with it");
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("b.mp3"))));
        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("keep.mp3"))),
                 "an unselected file must survive");
        // The selection is dropped — it would otherwise point at dead paths.
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList().size(), 0);
    }

    // A REAL right-click through the actual context menu. openContextMenu()
    // takes 9 positional args and JS silently passes `undefined` for a missed
    // one, so a mis-ordered call site would break the menu at runtime while
    // every source scan still passed — this drives the real gesture and reads
    // the real menu.
    void rightClickRow_menuOffersRenameAndNamesTheSelectionCount()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("c.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);
        QQuickItem *itemA = rowItem(treeItem, rowA);
        QQuickItem *itemB = rowItem(treeItem, rowB);
        QVERIFY(itemA && itemB);

        // Two rows selected, then right-click one of them.
        clickAt(&view, nameTextPoint(itemA));
        clickAt(&view, nameTextPoint(itemB), Qt::ControlModifier);
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList().size(), 2);

        QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier,
                           itemA->mapToScene(QPointF(itemA->width() / 2, itemA->height() / 2)).toPoint());
        QTest::qWait(80);

        // The right-click captured this row, as a note, with no label.
        QCOMPARE(sidebarItem->property("ctxPath").toString(), vault.filePath(QStringLiteral("a.mp3")));
        QVERIFY(sidebarItem->property("ctxIsNote").toBool());
        QVERIFY(!sidebarItem->property("ctxIsDir").toBool());
        QCOMPARE(sidebarItem->property("ctxLabel").toString(), QString());

        // Rename is offered...
        QQuickItem *renameRow = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxRenameRow"));
        QVERIFY2(renameRow, "the context menu must have a Rename row");
        QVERIFY2(renameRow->isVisible(), "Rename must be offered for a note row");

        // ...and the delete entry names the whole selection, not just the one
        // row that was right-clicked.
        QQuickItem *delLabel = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxDeleteFileLabel"));
        QVERIFY2(delLabel, "the context menu must have a delete row label");
        QCOMPARE(delLabel->property("text").toString(), QStringLiteral("Delete 2 items"));
    }

    // The other half of the same fix: right-clicking genuinely empty space
    // below the rows must STILL target the vault root, so "New folder" stays
    // reachable with no row to click. Guards against fixing the row case by
    // simply breaking this one.
    void rightClickEmptySpace_stillTargetsVaultRoot()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        // Well below the single row, but still inside the tree.
        const QPoint deep = treeItem->mapToScene(
            QPointF(treeItem->width() / 2, treeItem->height() - 10)).toPoint();
        QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier, deep);
        QTest::qWait(80);

        QCOMPARE(sidebarItem->property("ctxPath").toString(), QString());
        QVERIFY(!sidebarItem->property("ctxIsNote").toBool());
        QVERIFY(!sidebarItem->property("ctxIsDir").toBool());
        QCOMPARE(sidebarItem->property("ctxCreateInDir").toString(), vault.path());

        // "New folder" is the whole reason this handler exists.
        QQuickItem *newFolderRow = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxNewFolderRow"));
        QVERIFY2(newFolderRow, "the context menu must have a New folder row");
        QVERIFY(newFolderRow->isVisible());
        // ...and the row-only actions must not be offered for empty space.
        QQuickItem *renameRow = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxRenameRow"));
        QVERIFY(renameRow);
        QVERIFY2(!renameRow->isVisible(), "Rename must not be offered when no row was clicked");
    }

    // The same menu over a single unselected row reads as one file, not a count.
    void rightClickUnselectedRow_menuReadsAsSingleFile()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        QVERIFY(rowA >= 0);
        QQuickItem *itemA = rowItem(treeItem, rowA);
        QVERIFY(itemA);

        QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier,
                           itemA->mapToScene(QPointF(itemA->width() / 2, itemA->height() / 2)).toPoint());
        QTest::qWait(80);

        QQuickItem *delLabel = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxDeleteFileLabel"));
        QVERIFY(delLabel);
        QCOMPARE(delLabel->property("text").toString(), QStringLiteral("Delete file"));
    }

    // "Delete transcript" is single-target even when several rows are selected
    // — it's offered only for the one right-clicked transcribed audio row, and
    // must never be widened into the multi-select delete beside it.
    void confirmTranscriptDelete_isSingleTargetAndKeepsAudio()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3.md")), "---\nmodel: x\n---\n[0] hi\n"));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);
        clickAt(&view, nameTextPoint(rowItem(treeItem, rowA)));
        clickAt(&view, nameTextPoint(rowItem(treeItem, rowB)), Qt::ControlModifier);
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList().size(), 2);

        QMetaObject::invokeMethod(sidebarItem, "beginDeleteTranscript",
                                   Q_ARG(QVariant, vault.filePath(QStringLiteral("a.mp3"))));
        // Single-target even though two rows are selected.
        QCOMPARE(sidebarItem->property("pendingDeletePaths").toStringList().size(), 1);

        QMetaObject::invokeMethod(sidebarItem, "confirmFileDelete");
        QTest::qWait(150);

        QVERIFY2(!QFileInfo::exists(vault.filePath(QStringLiteral("a.mp3.md"))),
                 "the transcript must be deleted");
        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("a.mp3"))),
                 "the audio file must be kept");
        // The OTHER selected file is untouched — "Delete transcript" never
        // widens to the selection the way "Delete file" does.
        QVERIFY(QFileInfo::exists(vault.filePath(QStringLiteral("b.mp3"))));
    }

    // A single right-clicked row with no multi-selection deletes only itself,
    // even though it goes through the same batch call.
    void beginDeleteFile_onUnselectedRow_targetsOnlyThatRow()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        QMetaObject::invokeMethod(sidebarItem, "beginDeleteFile",
                                   Q_ARG(QVariant, vault.filePath(QStringLiteral("a.mp3"))));
        QCOMPARE(sidebarItem->property("pendingDeletePaths").toStringList(),
                 QStringList{vault.filePath(QStringLiteral("a.mp3"))});

        QMetaObject::invokeMethod(sidebarItem, "confirmFileDelete");
        QTest::qWait(150);

        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("b.mp3"))),
                 "deleting one row must not touch any other");
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppSettings.qml's Settings{} element
    // is loaded transitively by Sidebar.qml, and must never touch the real
    // ~/.config/suopTranscriber (CLAUDE.md: never let testing touch the user's
    // real vault or app config). Same setup as tst_sidebar_dragmove.cpp.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_rename"));
    QGuiApplication app(argc, argv);
    TstSidebarRename tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_rename.moc"
