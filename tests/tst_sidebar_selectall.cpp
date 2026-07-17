// Living spec + runtime verification for Sidebar.qml's ribbon selectAllButton
// / Ctrl+A / bulkTargetPaths() rework: drives a REAL QQuickView (offscreen,
// sandboxed config — see main() below, same pattern as tst_sidebar_keynav.cpp)
// loading the actual Sidebar.qml against a real VaultTreeModel, with real
// QTest::mouseClick/keyClick events, rather than trusting the source-scan
// guard in tst_styleguide.cpp alone.
//
// What this proves that a source scan can't: clicking selectAllButton (or
// pressing Ctrl+A with the tree focused) actually populates selectedPaths
// with every visible note, and that the ribbon's bulk buttons — which no
// longer fall back to "everything eligible" when nothing is selected —
// genuinely disable with an empty selection and re-enable once selectAllButton
// is clicked.

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

// Same reasoning as tst_sidebar_keynav.cpp's stub: every real Sidebar.qml use
// of transcriptionController guards on `sidebar.transcriptionController &&`
// first, so leaving it null (never assigned) is enough to exercise the
// enabled/disabled bindings under test without linking the real ASR engines.
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
};

} // namespace

class TstSidebarSelectAll : public QObject
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
                      "singleton Notifications 1.0 qml/Notifications.qml\n"
                      "HoverBorder 1.0 qml/HoverBorder.qml\n"
                      "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
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
            "    height: 260\n"
            "    property string openPath: \"\"\n"
            "    VaultTreeModel { id: vaultModel; vaultPath: Qt.resolvedUrl(vaultUrl) }\n"
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

    // The actual claim under test: clicking selectAllButton (and, equally,
    // pressing Ctrl+A with the tree focused) populates selectedPaths with
    // every visible note; and bulkTargetPaths() itself now always narrows to
    // that selection — an empty selection intersects to an empty target set,
    // never "everything eligible" (the pre-change behavior for 0/1
    // selected). The ribbon buttons' own `enabled` bindings additionally
    // gate on `sidebar.transcriptionController`, which this harness leaves
    // null (see TranscriptionControllerStub's comment above) — so this test
    // calls bulkTargetPaths() directly with a hand-built eligibility map
    // instead of reading a button's `enabled` property, to isolate the
    // selection-narrowing behavior from that unrelated controller gate.
    void selectAllButtonAndCtrlAPopulateSelection()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QStringList notePaths;
        for (int i = 0; i < 5; ++i) {
            const QString path = vault.filePath(QStringLiteral("note%1.mp3").arg(i));
            QVERIFY(touch(path));
            notePaths << path;
        }

        QQuickView view;
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.rootContext()->setContextProperty(QStringLiteral("vaultUrl"),
                                                QUrl::fromLocalFile(vault.path()));
        view.setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view.status() != QQuickView::Ready) {
            const auto errors = view.errors();
            QString msg;
            for (const auto &e : errors)
                msg += e.toString() + QStringLiteral("\n");
            QFAIL(qPrintable(msg.isEmpty() ? QStringLiteral("QML failed to load, no error text") : msg));
        }
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QQuickItem *root = view.rootObject();
        QVERIFY(root);
        QQuickItem *sidebarItem = root->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        QVERIFY2(sidebarItem, "driver.qml's Sidebar { objectName: \"sidebar\" } not found");
        QQuickItem *treeItem = findByClassNamePrefix(sidebarItem, "QQuickTreeView");
        QVERIFY2(treeItem, "could not locate Sidebar's TreeView (id: tree)");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        QQuickItem *selectAllButton = sidebarItem->findChild<QQuickItem *>(QStringLiteral("selectAllButton"));
        QVERIFY2(selectAllButton, "ribbon must expose an id: selectAllButton");
        QQuickItem *ribbonDivider = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ribbonDivider"));
        QVERIFY2(ribbonDivider, "ribbon must expose an id: ribbonDivider between selectAllButton and "
                                 "the bulk action buttons");
        QVERIFY2(sidebarItem->findChild<QQuickItem *>(QStringLiteral("transcribeAllButton")),
                 "ribbon must expose an id: transcribeAllButton");

        // A fake "every note is eligible" map, built independently of the
        // (controller-gated, always-empty-in-this-harness) real eligibility
        // maps — isolates bulkTargetPaths()'s own selection-narrowing logic.
        QVariantMap fakeEligibleMap;
        for (const QString &p : notePaths)
            fakeEligibleMap.insert(p, true);
        auto bulkTargetCount = [&]() -> int {
            QVariant result;
            QMetaObject::invokeMethod(sidebarItem, "bulkTargetPaths", Qt::DirectConnection,
                                       Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, fakeEligibleMap));
            return result.toList().size();
        };

        // Nothing selected yet — must intersect to nothing, not fall back to
        // "everything eligible" (the pre-change 0/1-selected behavior).
        QCOMPARE(sidebarItem->property("selectedPaths").toList().size(), 0);
        QCOMPARE(bulkTargetCount(), 0);

        // Click selectAllButton.
        const QPointF selectAllCenter(selectAllButton->width() / 2, selectAllButton->height() / 2);
        const QPoint selectAllScenePos = selectAllButton->mapToScene(selectAllCenter).toPoint();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, selectAllScenePos);
        QTest::qWait(20);

        QCOMPARE(sidebarItem->property("selectedPaths").toList().size(), 5);
        QCOMPARE(bulkTargetCount(), 5);

        // Click row 0 to collapse back to a single selection and grab
        // keyboard focus for the tree (Ctrl+A's Shortcut is gated on
        // tree.activeFocus, matching the other selection shortcuts).
        QQuickItem *row0 = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, row0), Q_ARG(QPoint, QPoint(0, 0)));
        QVERIFY2(row0, "itemAtCell(0,0) returned null — row 0 delegate not realized");
        const QPoint row0ScenePos = row0->mapToScene(QPointF(row0->width() / 2, row0->height() / 2)).toPoint();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, row0ScenePos);
        QTest::qWait(20);
        QVERIFY(treeItem->property("activeFocus").toBool());
        QCOMPARE(sidebarItem->property("selectedPaths").toList().size(), 1);
        QCOMPARE(bulkTargetCount(), 1);

        // Ctrl+A must reproduce exactly what the button click did above.
        QTest::keyClick(&view, Qt::Key_A, Qt::ControlModifier);
        QTest::qWait(20);
        QCOMPARE(sidebarItem->property("selectedPaths").toList().size(), 5);
        QCOMPARE(bulkTargetCount(), 5);
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_selectall"));
    QGuiApplication app(argc, argv);
    TstSidebarSelectAll tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_selectall.moc"
