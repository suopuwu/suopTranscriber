// Living spec + runtime verification for Sidebar.qml's nesting guides: the
// per-row vertical line(s) drawn down a folder's expanded block plus the
// horizontal connector tick hanging each child off its parent's line — the
// only thing besides raw indentation that distinguishes a folder's contents
// from the sibling rows around it.
//
// Same harness shape as tst_sidebar_expand.cpp / tst_sidebar_dragmove.cpp /
// tst_sidebar_keynav.cpp (read their top-of-file comments first): a REAL
// QQuickView (offscreen, sandboxed config — see main() below) loading the
// actual Sidebar.qml against a real VaultTreeModel. This can't be a
// tst_styleguide source scan — the claims are geometric (guide count == row
// depth, lines land on the ancestor glyph columns, the last row of a block
// closes its line into a "└" instead of overshooting), which only the
// realized delegates can answer.
//
// Set SUOP_GRAB_DIR=<dir> to also dump the rendered tree to
// <dir>/sidebar_guides.png for eyeballing (offscreen grabWindow renders
// fine).

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QImage>
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

class TstSidebarGuides : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // Same helper as tst_sidebar_expand.cpp — match by class-name prefix so
    // this never has to name QQuickTreeView as a C++ type (Qt private API).
    static QQuickItem *findByClassNamePrefix(QQuickItem *root, const QByteArray &prefix)
    {
        for (QQuickItem *child : root->findChildren<QQuickItem *>()) {
            if (QByteArray(child->metaObject()->className()).startsWith(prefix))
                return child;
        }
        return nullptr;
    }

    static QQuickItem *rowItem(QQuickItem *treeItem, int row)
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, item), Q_ARG(QPoint, QPoint(0, row)));
        return item;
    }

    // TreeView's own depth() invokable — the same ground truth the delegate's
    // guides are derived FROM (via required property depth / tree.depth()),
    // but read independently through the view's public API.
    static int rowDepth(QQuickItem *treeItem, int row)
    {
        if (row < 0 || row >= treeItem->property("rows").toInt())
            return -1;
        int d = -1;
        QMetaObject::invokeMethod(treeItem, "depth", Qt::DirectConnection,
                                   Q_RETURN_ARG(int, d), Q_ARG(int, row));
        return d;
    }

    // childItems(), NOT findChildren(): a Repeater's created items get the
    // delegate as their VISUAL parent only — their QObject parent stays with
    // the Repeater's instance model, so the QObject-tree walk findChildren()
    // does never sees them.
    static QList<QQuickItem *> guidesOfRow(QQuickItem *treeItem, int row)
    {
        QQuickItem *item = rowItem(treeItem, row);
        if (!item)
            return {};
        QList<QQuickItem *> guides;
        for (QQuickItem *c : item->childItems()) {
            if (c->objectName() == QLatin1String("nestingGuide"))
                guides.append(c);
        }
        return guides;
    }

    static QQuickItem *tickOfRow(QQuickItem *treeItem, int row)
    {
        QQuickItem *item = rowItem(treeItem, row);
        return item ? item->findChild<QQuickItem *>(QStringLiteral("guideTick")) : nullptr;
    }

    // Builds the view + returns the located TreeView, or fails the test.
    QQuickItem *openVault(QQuickView *view, const QString &vaultPath, QObject **modelOut = nullptr)
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
        return findByClassNamePrefix(sidebarItem, "QQuickTreeView");
    }

    // The other harnesses' note applies here too: vaultPath handed to
    // VaultTreeModel at construction resets the model before Sidebar's
    // Connections exist, so expandAll() never runs and nested rows stay
    // hidden. Re-setting vaultPath (what the real app does on open) is the
    // faithful path to a fully expanded tree.
    static void reopenVault(QObject *model, const QString &vaultPath)
    {
        model->setProperty("vaultPath", QUrl());
        model->setProperty("vaultPath", QUrl::fromLocalFile(vaultPath));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
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

    // The headline claims, checked over EVERY visible row of a 3-deep vault
    // with no assumptions about the model's sort order:
    //  - a row carries exactly depth-many vertical guides, one per ancestor
    //    level, spaced exactly one indent step (16px) apart;
    //  - every nested row (depth > 0) shows the horizontal connector tick,
    //    anchored at the same x as its parent's (deepest) guide; top-level
    //    rows show neither;
    //  - the deepest guide closes into a "└" (stops at the tick) exactly on
    //    the rows that END their parent's block — i.e. the next display row
    //    is shallower — and runs the row's full height everywhere else.
    void guides_matchNestingDepth_andElbowOnBlockEnd()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Folder/inner.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("Folder/Sub/deep.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("loose.mp3"))));

        QQuickView view;
        QObject *model = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &model);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QVERIFY2(model, "could not locate the VaultTreeModel");
        reopenVault(model, vault.path());
        // Folder, inner.mp3, Sub, deep.mp3, loose.mp3 — all expanded.
        QTRY_COMPARE(treeItem->property("rows").toInt(), 5);

        const int rows = treeItem->property("rows").toInt();
        bool sawElbow = false, sawFullHeight = false, sawDepth2 = false;
        for (int r = 0; r < rows; ++r) {
            const int depth = rowDepth(treeItem, r);
            QVERIFY(depth >= 0);
            sawDepth2 = sawDepth2 || depth == 2;
            QQuickItem *row = rowItem(treeItem, r);
            QVERIFY(row);
            QQuickItem *tick = tickOfRow(treeItem, r);
            QVERIFY2(tick, "guideTick missing from a realized delegate");
            QCOMPARE(tick->isVisible(), depth > 0);

            QList<QQuickItem *> guides = guidesOfRow(treeItem, r);
            QCOMPARE(guides.size(), depth);
            std::sort(guides.begin(), guides.end(),
                      [](QQuickItem *a, QQuickItem *b) { return a->x() < b->x(); });
            for (int level = 0; level < guides.size(); ++level) {
                QQuickItem *guide = guides[level];
                // One line per ancestor level, one 16px indent step apart —
                // level 0's absolute x is the margins' business, not this
                // test's; the SPACING is what makes each line land on its
                // own ancestor's glyph column.
                if (level > 0)
                    QCOMPARE(guide->x() - guides[level - 1]->x(), 16.0);

                const bool deepest = level == depth - 1;
                if (deepest && depth > 0)
                    QCOMPARE(guide->x(), tick->x());
                const bool endsBlock = rowDepth(treeItem, r + 1) < depth;
                if (deepest && endsBlock) {
                    // "└": the line stops where the tick meets it.
                    QCOMPARE(guide->height(), tick->y() + tick->height());
                    sawElbow = true;
                } else {
                    // Continuation: full row height, so this segment fuses
                    // with the next row's into one unbroken line.
                    QCOMPARE(guide->height(), row->height());
                    if (depth > 0)
                        sawFullHeight = true;
                }
            }
        }
        // The loop's per-row expectations are only meaningful if the vault
        // actually exercised all three shapes.
        QVERIFY2(sawDepth2, "no depth-2 row realized — Sub/ never expanded?");
        QVERIFY(sawElbow);
        QVERIFY(sawFullHeight);

        if (qEnvironmentVariableIsSet("SUOP_GRAB_DIR")) {
            const QImage img = view.grabWindow();
            QVERIFY(img.save(QString::fromLocal8Bit(qgetenv("SUOP_GRAB_DIR"))
                             + QStringLiteral("/sidebar_guides.png")));
        }
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppSettings.qml's Settings{} element
    // is loaded transitively by Sidebar.qml, and must never touch the real
    // ~/.config/suopTranscriber (CLAUDE.md: never let testing touch the user's
    // real vault or app config). Same setup as tst_sidebar_expand.cpp.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_guides"));
    QGuiApplication app(argc, argv);
    TstSidebarGuides tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_guides.moc"
