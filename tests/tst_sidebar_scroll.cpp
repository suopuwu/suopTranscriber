// Living spec + runtime verification for Sidebar.qml's scroll-offset
// preservation across a model reset. A reset drops every
// QModelIndex, so QQuickTreeView collapses to root rows, contentHeight shrinks,
// and the Flickable clamps contentY — the user's scroll position is silently
// lost by the time expandAllNow() puts the rows back. Sidebar.qml now samples
// the user's contentY continuously and restores it after the re-expand. That
// restore is pure runtime behaviour a source scan can't see, so this drives a
// REAL QQuickView.
//
// Same harness shape as tst_sidebar_expand.cpp (read its top-of-file comment
// first): offscreen QQuickView loading the real Sidebar.qml against a real
// VaultTreeModel + VaultSearchProxyModel, a null TranscriptionController stub,
// sandboxed config in main(). One wrinkle inherited from that file: handing
// VaultTreeModel its vaultPath at construction means the initial reset lands
// before Sidebar's Connections exist to hear it, so folders start collapsed.
// Re-setting vaultPath after load (clear then set) forces a reset the
// Connections DO hear, which expands the tree — this is also what the real app
// does (it opens AppSettings.lastVaultPath once loaded). forceExpandedReset()
// below does that and waits for a scrollable tree.
//
// Nothing here touches the user's vault or ~/.config/suopTranscriber.

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

class TranscriptionControllerStub : public QObject {
    Q_OBJECT
};

} // namespace

class TstSidebarScroll : public QObject
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

    // Build a vault of `folders` directories each holding `perFolder` files —
    // enough rows that the expanded tree scrolls in the 400px-high view.
    static void buildScrollableVault(const QString &root, int folders, int perFolder)
    {
        for (int d = 0; d < folders; ++d) {
            for (int f = 0; f < perFolder; ++f) {
                touch(QDir(root).filePath(QStringLiteral("dir%1/clip%2.wav").arg(d).arg(f)));
            }
        }
    }

    QQuickItem *openVault(QQuickView *view, const QString &vaultPath, QObject **modelOut)
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

    // Force a reset Sidebar's Connections actually hear, so the tree expands and
    // becomes scrollable. Returns once contentHeight exceeds the view height.
    static bool forceExpandedReset(VaultTreeModel *model, QQuickItem *tree, const QString &vaultPath)
    {
        model->setProperty("vaultPath", QUrl());
        model->setProperty("vaultPath", QUrl::fromLocalFile(vaultPath));
        // QTRY_VERIFY can't return a bool; poll by hand so callers can VERIFY.
        for (int i = 0; i < 200; ++i) {
            if (tree->property("contentHeight").toReal() > tree->property("height").toReal()
                && !tree->property("expandPending").toBool())
                return true;
            QTest::qWait(25);
        }
        return false;
    }

    static qreal contentY(QQuickItem *t) { return t->property("contentY").toReal(); }
    static qreal maxContentY(QQuickItem *t)
    {
        return qMax(qreal(0), t->property("contentHeight").toReal() - t->property("height").toReal());
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

    // The headline claim: a structural reset (here: deleting a file) must leave
    // the tree at the same scroll offset the user was viewing, not snap it back
    // toward the top.
    void scrollOffsetSurvivesAModelReset()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        buildScrollableVault(vault.path(), 12, 8);

        QQuickView view;
        QObject *modelObj = nullptr;
        QQuickItem *tree = openVault(&view, vault.path(), &modelObj);
        QVERIFY2(tree, "could not load Sidebar.qml / locate its TreeView");
        auto *model = qobject_cast<VaultTreeModel *>(modelObj);
        QVERIFY(model);
        QVERIFY2(forceExpandedReset(model, tree, vault.path()), "tree never became scrollable");

        // Scroll to a clearly non-zero mid offset and let the sampler record it.
        const qreal target = qMin<qreal>(600, maxContentY(tree));
        QVERIFY2(target > 100, "test vault wasn't tall enough to scroll meaningfully");
        tree->setProperty("contentY", target);
        QTest::qWait(50);
        QCOMPARE(contentY(tree), target);

        // A structural reset: delete one file (the last, so rows above the
        // offset are unaffected). trashFiles -> applyRemovalOutcome -> rebuild.
        const QVariantMap r = model->trashFiles(
            { QUrl::fromLocalFile(vault.filePath(QStringLiteral("dir11/clip7.wav"))) });
        QCOMPARE(r.value(QStringLiteral("trashed")).toInt(), 1);

        // Once recovery finishes, the offset is back where the user left it.
        QTRY_VERIFY(!tree->property("expandPending").toBool());
        QTRY_VERIFY2(qAbs(contentY(tree) - target) <= 2.0,
                     qPrintable(QStringLiteral("contentY landed at %1, expected ~%2")
                                    .arg(contentY(tree)).arg(target)));
    }

    // When the tree genuinely shrinks below the old offset (many rows removed),
    // the restore must clamp — an out-of-range contentY leaves blank space below
    // the last row.
    void scrollOffsetIsClampedWhenTheTreeShrinks()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        buildScrollableVault(vault.path(), 12, 8);

        QQuickView view;
        QObject *modelObj = nullptr;
        QQuickItem *tree = openVault(&view, vault.path(), &modelObj);
        QVERIFY2(tree, "could not load Sidebar.qml / locate its TreeView");
        auto *model = qobject_cast<VaultTreeModel *>(modelObj);
        QVERIFY(model);
        QVERIFY2(forceExpandedReset(model, tree, vault.path()), "tree never became scrollable");

        // Scroll to the very bottom.
        tree->setProperty("contentY", maxContentY(tree));
        QTest::qWait(50);
        QVERIFY(contentY(tree) > 100);

        // Delete almost everything, so contentHeight collapses far below the old
        // offset. Whole folders go in one trash op each (less trash litter than
        // per-file), leaving only dir0.
        for (int d = 1; d < 12; ++d) {
            const QVariantMap r = model->trashFolder(
                QUrl::fromLocalFile(vault.filePath(QStringLiteral("dir%1").arg(d))));
            QCOMPARE(r.value(QStringLiteral("trashed")).toInt(), 1);
        }

        QTRY_VERIFY(!tree->property("expandPending").toBool());
        // The offset is clamped to the (now much smaller) reachable range and
        // never exceeds it.
        QTRY_COMPARE(contentY(tree), maxContentY(tree));
        QVERIFY2(contentY(tree) <= maxContentY(tree) + 0.5, "contentY left the tree scrolled past its end");
    }

    // Opening a DIFFERENT vault is a reset too, but there is no previous
    // position to preserve — it must start at the top.
    void switchingVaultsStartsAtTheTop()
    {
        QTemporaryDir vaultA;
        QTemporaryDir vaultB;
        QVERIFY(vaultA.isValid() && vaultB.isValid());
        buildScrollableVault(vaultA.path(), 12, 8);
        buildScrollableVault(vaultB.path(), 12, 8);

        QQuickView view;
        QObject *modelObj = nullptr;
        QQuickItem *tree = openVault(&view, vaultA.path(), &modelObj);
        QVERIFY2(tree, "could not load Sidebar.qml / locate its TreeView");
        auto *model = qobject_cast<VaultTreeModel *>(modelObj);
        QVERIFY(model);
        QVERIFY2(forceExpandedReset(model, tree, vaultA.path()), "tree never became scrollable");

        tree->setProperty("contentY", qMin<qreal>(600, maxContentY(tree)));
        QTest::qWait(50);
        QVERIFY(contentY(tree) > 100);

        // Switch to the other vault.
        model->setProperty("vaultPath", QUrl::fromLocalFile(vaultB.path()));

        QTRY_VERIFY(!tree->property("expandPending").toBool());
        QTRY_COMPARE(contentY(tree), qreal(0));
    }

    // The restore must fire ONCE and then leave the user alone — a stray
    // pendingContentY re-firing would yank the view back after a deliberate
    // scroll.
    void restoreDoesNotFightASubsequentUserScroll()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        buildScrollableVault(vault.path(), 12, 8);

        QQuickView view;
        QObject *modelObj = nullptr;
        QQuickItem *tree = openVault(&view, vault.path(), &modelObj);
        QVERIFY2(tree, "could not load Sidebar.qml / locate its TreeView");
        auto *model = qobject_cast<VaultTreeModel *>(modelObj);
        QVERIFY(model);
        QVERIFY2(forceExpandedReset(model, tree, vault.path()), "tree never became scrollable");

        tree->setProperty("contentY", qMin<qreal>(500, maxContentY(tree)));
        QTest::qWait(50);

        const QVariantMap r = model->trashFiles(
            { QUrl::fromLocalFile(vault.filePath(QStringLiteral("dir11/clip7.wav"))) });
        QCOMPARE(r.value(QStringLiteral("trashed")).toInt(), 1);
        QTRY_VERIFY(!tree->property("expandPending").toBool());

        // Now the user scrolls somewhere new; the restore must not drag it back.
        const qreal newPos = qMin<qreal>(200, maxContentY(tree));
        tree->setProperty("contentY", newPos);
        QTest::qWait(200);
        QVERIFY2(qAbs(contentY(tree) - newPos) <= 2.0,
                 qPrintable(QStringLiteral("a stray restore moved contentY to %1, expected ~%2")
                                .arg(contentY(tree)).arg(newPos)));
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_scroll"));
    QGuiApplication app(argc, argv);
    TstSidebarScroll tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_scroll.moc"
