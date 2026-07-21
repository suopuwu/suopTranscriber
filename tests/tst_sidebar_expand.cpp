// Living spec + runtime verification for Sidebar.qml's expand/collapse glyph
// on EMPTY folders: drives a REAL QQuickView (offscreen, sandboxed config —
// see main() below) loading the actual Sidebar.qml against a real
// VaultTreeModel, and reads the glyph the delegate actually renders.
//
// Same harness shape as tst_sidebar_dragmove.cpp / tst_sidebar_keynav.cpp
// (read their top-of-file comments first). This file exists because the
// central question is NOT answerable from source review: the glyph used to be
// gated on `treeDelegate.hasChildren`, and dropping that gate only works if
// QQuickTreeView is willing to track expanded state for a node with no
// children at all. Nothing in Qt's public docs or installed headers says
// whether expand()/toggleExpanded() no-ops there — if it did, the glyph would
// be permanently stuck on "+" and clicking a folder would visibly do nothing.
// emptyFolder_toggleExpanded_flipsGlyph is the test that pins that answer.
//
// The answer, for the record: QQuickTreeView (6.11) REFUSES to expand a
// childless node — isExpanded(row) stays false after toggleExpanded()/expand(),
// and collapse() no-ops — but the delegate's `expanded` property still latches
// to true on the attempt and never clears. So `expanded` is unusable for an
// empty folder (it would read "−" once and stick), and Sidebar.qml tracks
// empty-folder state itself in sidebar.collapsedEmptyDirs. These tests are what
// keep that non-obvious workaround honest if a future Qt changes its mind.
//
// tst_styleguide.cpp keeps the cheap source-scan guards; this file is the one
// that proves what's ON SCREEN.

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

// See tst_sidebar_dragmove.cpp's identical stub: Sidebar.qml's every
// transcriptionController use site guards on it being non-null first, and this
// harness never assigns one — so the property only needs its TYPE to resolve.
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
};

} // namespace

class TstSidebarExpand : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // TreeView declared plainly in QML is a plain QQuickTreeView instance;
    // match by class-name prefix so this never has to name QQuickTreeView as a
    // C++ type (Qt private API). Same helper as tst_sidebar_dragmove.cpp.
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
    // reusing any of Sidebar.qml's JS. Same helper as tst_sidebar_dragmove.cpp.
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

    static QQuickItem *rowItem(QQuickItem *treeItem, int row)
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, item), Q_ARG(QPoint, QPoint(0, row)));
        return item;
    }

    // The glyph carries objectName "expandGlyph" purely so this harness can
    // read it — the delegate has no other stable handle on it. Since the
    // icon-pack refactor it's an AppIcon, so the disclosure state is its
    // semantic `name` ("expand"/"collapse", or "" for a non-dir row), not a
    // "+"/"−" glyph string. glyphName() reads that.
    static QQuickItem *glyphOfRow(QQuickItem *treeItem, int row)
    {
        QQuickItem *item = rowItem(treeItem, row);
        if (!item)
            return nullptr;
        return item->findChild<QQuickItem *>(QStringLiteral("expandGlyph"));
    }

    static QString glyphName(QQuickItem *treeItem, int row)
    {
        QQuickItem *glyph = glyphOfRow(treeItem, row);
        return glyph ? glyph->property("name").toString() : QStringLiteral("<no glyph>");
    }

    static QPoint rowCenter(QQuickItem *row)
    {
        return row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint();
    }

    // A point on the row's expand glyph — the only part of a dir row that
    // toggles it. The row at large deliberately does not: its name starts an
    // inline rename instead, and the space past the name is inert (see
    // Sidebar.qml's tapIsOnExpandGlyph, and tst_sidebar_rename's
    // clickFolderName_/clickFolderPastName_ tests).
    static QPoint glyphCenter(QQuickItem *treeItem, int row)
    {
        QQuickItem *glyph = glyphOfRow(treeItem, row);
        if (!glyph)
            return {};
        return glyph->mapToScene(QPointF(glyph->width() / 2, glyph->height() / 2)).toPoint();
    }

    // Builds the view + returns the located TreeView, or fails the test.
    QQuickItem *openVault(QQuickView *view, const QString &vaultPath,
                          QQuickItem **sidebarOut = nullptr, QObject **modelOut = nullptr)
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
        if (modelOut)
            *modelOut = root->findChild<QObject *>(QStringLiteral("vaultModel"));
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
                      "singleton Theme 1.0 qml/Theme.qml\n"
                      "singleton Notifications 1.0 qml/Notifications.qml\n"
                      "HoverBorder 1.0 qml/HoverBorder.qml\n"
                      "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
                      "AppIcon 1.0 qml/AppIcon.qml\n"
                      "AppIconButton 1.0 qml/AppIconButton.qml\n"
                      "ConfirmPopup 1.0 qml/ConfirmPopup.qml\n"
                      "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                      "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                      "Sidebar 1.0 qml/Sidebar.qml\n");
        qmldir.close();

        // Sidebar.qml's AppIcon usages bind IconTheme.resolve(...); register
        // the singleton so they resolve instead of throwing at load.
        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        // TreeOrdering is the C++ singleton refreshTreeOrdering() calls to
        // compute the tree's ordering (see src/core/TreeOrdering.h) — without
        // it registered, Sidebar.qml fails to load outright.
        qmlRegisterSingletonType<TreeOrdering>(
            "SuopTranscriber", 1, 0, "TreeOrdering",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new TreeOrdering; });
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
            "    VaultTreeModel { id: vaultModel; objectName: \"vaultModel\"\n"
            "                     vaultPath: Qt.resolvedUrl(vaultUrl) }\n"
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

    // The headline claim: an EMPTY folder still gets a glyph, in a real glyph
    // column with real width — it isn't silently rendered as a bare,
    // file-aligned name. It defaults to "collapse" (i.e. currently expanded),
    // matching the populated folders around it, since the tree auto-expands on
    // every model reset.
    void emptyFolder_showsExpandGlyph()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath(QStringLiteral("Empty"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("Empty")));
        QVERIFY2(row >= 0, "the empty folder never reached the tree at all");

        QQuickItem *glyph = glyphOfRow(treeItem, row);
        QVERIFY(glyph);
        QCOMPARE(glyph->property("name").toString(), QStringLiteral("collapse"));
        QVERIFY2(glyph->width() > 0, "glyph column collapsed to 0 width on an empty folder");
    }

    // The behavior the glyph exists for: clicking an empty folder's glyph
    // toggles it BOTH WAYS, repeatedly. Round-tripping twice is the whole point
    // — the naive implementation (reading treeDelegate.expanded) passes a
    // single flip and then sticks forever, because TreeView latches `expanded`
    // true on a childless node and never clears it.
    void emptyFolder_clickTogglesGlyphBothWays()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath(QStringLiteral("Empty"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("Empty")));
        QVERIFY(row >= 0);
        QCOMPARE(glyphName(treeItem, row), QStringLiteral("collapse"));

        // Through the REAL gesture (treeRowTap), not by poking TreeView's or
        // the sidebar's API directly — the click path is what users have.
        for (int i = 0; i < 2; ++i) {
            QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                              glyphCenter(treeItem, row));
            QTest::qWait(50);
            QCOMPARE(glyphName(treeItem, row), QStringLiteral("expand"));
            // Collapsing/expanding an empty folder reveals nothing either way —
            // the row count never moves. That's inherent, not a bug: this glyph
            // is an affordance for consistency, not a disclosure of content.
            QCOMPARE(treeItem->property("rows").toInt(), 1);

            QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                              glyphCenter(treeItem, row));
            QTest::qWait(50);
            QCOMPARE(glyphName(treeItem, row), QStringLiteral("collapse"));
            QCOMPARE(treeItem->property("rows").toInt(), 1);
        }
    }

    // Sidebar's expandAll() (which runs on EVERY model reset) must re-expand
    // collapsed empty folders, because it re-expands every populated folder —
    // an empty one keeping a stale "expand" glyph while everything around it
    // reopens is exactly the drift that made tracking this state separately a
    // risk.
    //
    // This is also the only test that executes expandAll() at all: the other
    // cases here hand VaultTreeModel its vaultPath at CONSTRUCTION, so the
    // reset lands before Sidebar's Connections exist to hear it (see
    // tst_sidebar_dragmove.cpp's note). Re-setting vaultPath after load is
    // what the REAL app does anyway — it opens its vault from
    // AppSettings.lastVaultPath once loaded — so this is the faithful path.
    void modelReset_reExpandsCollapsedEmptyFolder()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath(QStringLiteral("Empty"))));

        QQuickView view;
        QObject *model = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), nullptr, &model);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QVERIFY2(model, "could not locate the VaultTreeModel");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        int row = rowForPath(treeItem, vault.filePath(QStringLiteral("Empty")));
        QVERIFY(row >= 0);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                          glyphCenter(treeItem, row));
        QTest::qWait(50);
        QCOMPARE(glyphName(treeItem, row), QStringLiteral("expand"));

        // Force a real modelReset: setVaultPath() early-returns on an unchanged
        // URL, so clear it and set it back. The second assignment is the one
        // Sidebar's Connections hear.
        QVERIFY(model->setProperty("vaultPath", QUrl()));
        QVERIFY(model->setProperty("vaultPath", QUrl::fromLocalFile(vault.path())));

        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        QTRY_VERIFY((row = rowForPath(treeItem, vault.filePath(QStringLiteral("Empty")))) >= 0);
        QTRY_COMPARE(glyphName(treeItem, row), QStringLiteral("collapse"));
    }

    // Clicking an empty folder's glyph must stay a pure expand/collapse
    // gesture: it is NOT a note, so it must never enter the selection
    // (Sidebar's contract: "directories are never selectable").
    void emptyFolder_clickDoesNotSelect()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath(QStringLiteral("Empty"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("Empty")));
        QVERIFY(row >= 0);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                          glyphCenter(treeItem, row));
        QTest::qWait(50);

        QVERIFY(sidebarItem);
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList().size(), 0);
    }

    // The row at large is NOT a toggle — only the glyph is, which is what frees
    // a folder's name to start a rename instead. Worth asserting separately
    // from the populated-folder case (tst_sidebar_rename's
    // clickFolderPastName_doesNothing): an empty folder toggles through an
    // entirely different path (sidebar.collapsedEmptyDirs, not TreeView's own
    // expanded state), so it can regress on its own.
    void emptyFolder_clickPastGlyph_doesNotToggle()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(QDir().mkpath(vault.filePath(QStringLiteral("Empty"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("Empty")));
        QVERIFY(row >= 0);
        QCOMPARE(glyphName(treeItem, row), QStringLiteral("collapse"));

        // The row's centre is well past the painted name ("Empty" in a 320px
        // sidebar), i.e. the row's dead space.
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                          rowCenter(rowItem(treeItem, row)));
        QTest::qWait(50);
        QCOMPARE(glyphName(treeItem, row), QStringLiteral("collapse"));
    }

    // The glyph is a FOLDER affordance, not a "has stuff in it" hint: a
    // populated dir and an empty one must both carry it, and files must still
    // carry none (the 0-width leaf column that keeps names tight against the
    // left margin — see the delegate's comment).
    void glyphIsFolderOnly_notContentDependent()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Full/song.mp3"))));
        QVERIFY(QDir().mkpath(vault.filePath(QStringLiteral("Empty"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("loose.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() >= 3);

        const int fullRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Full")));
        const int emptyRow = rowForPath(treeItem, vault.filePath(QStringLiteral("Empty")));
        const int fileRow = rowForPath(treeItem, vault.filePath(QStringLiteral("loose.mp3")));
        QVERIFY(fullRow >= 0 && emptyRow >= 0 && fileRow >= 0);

        // Both folders carry a real glyph in a real column. Which glyph the
        // POPULATED one shows isn't asserted here — that's expandAll()'s
        // business (and it races this harness, see tst_sidebar_dragmove.cpp);
        // this test is about the folder/file distinction only.
        for (const int dirRow : {fullRow, emptyRow}) {
            QQuickItem *glyph = glyphOfRow(treeItem, dirRow);
            QVERIFY(glyph);
            const QString name = glyph->property("name").toString();
            QVERIFY2(name == QStringLiteral("expand") || name == QStringLiteral("collapse"),
                     qPrintable(QStringLiteral("folder glyph name was '%1'").arg(name)));
            QVERIFY(glyph->width() > 0);
        }

        QQuickItem *fileGlyph = glyphOfRow(treeItem, fileRow);
        QVERIFY(fileGlyph);
        // A non-dir row's glyph resolves to an empty name (draws nothing) and
        // collapses its column to zero width.
        QCOMPARE(fileGlyph->property("name").toString(), QString());
        QCOMPARE(fileGlyph->width(), 0.0);
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
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_expand"));
    QGuiApplication app(argc, argv);
    TstSidebarExpand tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_expand.moc"
