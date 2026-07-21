// Living spec + runtime verification for the sidebar file tree recovering its
// scrollability after a search is cleared (queue item 049). A non-empty text
// query hides the whole TreeView (visible: searchQuery === "") and shows the
// flat searchResultsView instead; a QQuickTableView releases its realized
// delegates while invisible, so contentHeight collapses toward the viewport
// height and — without a relayout AFTER it is visible again — the scrollbar
// thumb fills the track and the tree won't scroll to files below the fold.
// This drives the real bug through sidebar.searchQuery and checks contentHeight
// recovers, which no source scan can prove.
//
// Same harness shape as tst_sidebar_dragmove.cpp (read its top-of-file comment
// first): a REAL offscreen QQuickView loading the actual Sidebar.qml against a
// real VaultTreeModel, with a null TranscriptionController stub.

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

class TstSidebarSearchScroll : public QObject
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

    // A text search hides the TreeView; clearing it must not leave the tree's
    // contentHeight (and so its scrollbar) stuck at the collapsed-while-hidden
    // extent — the user must still be able to scroll to every file.
    void exitingSearch_restoresScrollableContentHeight()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));
        for (int i = 0; i < 24; ++i)
            QVERIFY(touch(vault.filePath(QStringLiteral("note%1.mp3").arg(i, 2, 10, QLatin1Char('0')))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QVERIFY(sidebarItem);
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        // Let the auto-expand pass settle so contentHeight is the full extent.
        QTRY_VERIFY(treeItem->property("contentHeight").toReal()
                        > treeItem->property("height").toReal());
        const qreal fullHeight = treeItem->property("contentHeight").toReal();

        // Run a search: tree hides, searchResultsView takes over.
        sidebarItem->setProperty("searchQuery", QStringLiteral("note"));
        QTRY_VERIFY(!treeItem->isVisible());
        QTest::qWait(50);

        // Exit search: tree returns.
        sidebarItem->setProperty("searchQuery", QString());
        QTRY_VERIFY(treeItem->isVisible());

        // The regression: contentHeight must recover to the full extent, so the
        // tree is scrollable again (contentHeight > height) rather than clamped
        // to ~viewport with a full-track scrollbar thumb.
        QTRY_VERIFY2(treeItem->property("contentHeight").toReal()
                         > treeItem->property("height").toReal(),
                     "tree is not scrollable after exiting search — contentHeight "
                     "stayed collapsed from while it was hidden");
        QTRY_VERIFY2(qAbs(treeItem->property("contentHeight").toReal() - fullHeight) < 2.0,
                     "contentHeight did not return to its pre-search full extent");
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_searchscroll"));
    QGuiApplication app(argc, argv);
    TstSidebarSearchScroll tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_searchscroll.moc"
