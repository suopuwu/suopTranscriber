// Right-click context menu, rendered the way the REAL APP renders it: inside an
// ApplicationWindow, with a real Overlay, and with SnackbarHost parented onto
// that overlay — exactly Main.qml's structure.
//
// Why this exists as its own harness rather than another tst_sidebar_rename
// case: that file drives Sidebar.qml in a bare QQuickView whose root is a plain
// Item. A QQuickView has no ApplicationWindow overlay, so a modal Popup's
// dimming, its stacking against other overlay children (SnackbarHost sits there
// at z:1001), and anything that depends on the two interacting simply cannot
// happen there. A user-reported "the screen shades but no menu appears" is
// precisely a modal-overlay symptom, and tst_sidebar_rename's right-click tests
// all PASS while the real app is broken — that gap is what this closes.
//
// Asserts the menu is genuinely SHOWN, not merely `opened`: a zero-sized or
// off-window popup still reports opened == true and still dims the screen,
// which looks exactly like "no menu".

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
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

// A controller that ANSWERS, unlike the null/empty stubs the other sidebar
// harnesses use. That distinction is the point of this file: with no controller,
// treeDelegate.refreshMetadata() leaves metadata {} and the right-click hands
// openContextMenu() a label of "" — so a "transcribed" row here behaves nothing
// like a transcribed row in the real app, which is exactly the case the user
// reports broken. readMetadata() is what treeRowContextTap reads for its label
// argument; the rest exist because Sidebar's load-time bindings call them the
// moment this stops being null, and an unresolved call breaks those bindings
// rather than failing visibly.
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QVariantMap readMetadata(const QUrl &fileUrl) const
    {
        // Only a transcribed file has a sidecar to read — mirror that.
        const QString md = fileUrl.toLocalFile() + QStringLiteral(".md");
        if (!QFileInfo::exists(md))
            return {};
        return QVariantMap{ { QStringLiteral("label"), QStringLiteral("A Recorded Talk") },
                            { QStringLiteral("tags"), QStringList{ QStringLiteral("meeting") } },
                            { QStringLiteral("speakers"), QStringList{} } };
    }
    Q_INVOKABLE bool matchesExistingTranscription(const QUrl &, const QString &, bool) const
    {
        return false;
    }
    Q_INVOKABLE int jobStatusForPath(const QString &) const { return -1; }
    Q_INVOKABLE QVariantList detectCombineGroups(const QVariantList &, const QStringList &) const
    {
        return {};
    }
    Q_INVOKABLE QStringList existingVaultTags(const QUrl &) const { return {}; }
    Q_INVOKABLE bool setLabel(const QUrl &, const QString &) { return true; }

signals:
    void finished(const QUrl &fileUrl, bool ok, const QString &error);
    void jobStatusesChanged();
};

} // namespace

class TstSidebarContextMenu : public QObject
{
    Q_OBJECT

    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;

    bool openVault(const QString &vaultPath)
    {
        m_engine = std::make_unique<QQmlApplicationEngine>();
        m_engine->addImportPath(m_moduleDir.path());
        m_engine->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                   new IconImageProvider);
        m_engine->rootContext()->setContextProperty(
            QStringLiteral("vaultUrl"), QUrl::fromLocalFile(vaultPath).toString());
        m_engine->load(QUrl::fromLocalFile(m_driverQmlPath));
        if (m_engine->rootObjects().isEmpty()) {
            qWarning("driver QML failed to load — see the QML errors above");
            return false;
        }
        m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().first());
        if (!m_window)
            return false;
        m_window->show();
        if (!QTest::qWaitForWindowExposed(m_window))
            return false;
        if (!sidebar()) {
            qWarning("Sidebar not found in the window");
            return false;
        }
        if (!tree()) {
            qWarning("TreeView (objectName vaultTree) not found inside the Sidebar");
            return false;
        }
        return true;
    }

    // Search from the WINDOW, not contentItem: ApplicationWindow QObject-parents
    // its declared children to the window itself, and findChild walks the
    // QObject tree — contentItem is the wrong root and silently finds nothing.
    QQuickItem *sidebar() const
    {
        return m_window->findChild<QQuickItem *>(QStringLiteral("sidebar"));
    }
    QQuickItem *tree() const
    {
        return m_window->findChild<QQuickItem *>(QStringLiteral("vaultTree"));
    }
    QObject *menu() const
    {
        return m_window->findChild<QObject *>(QStringLiteral("contextMenu"));
    }

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
                                      Q_RETURN_ARG(QModelIndex, idx), Q_ARG(int, r),
                                      Q_ARG(int, 0));
            if (model->data(idx, pathRole).toString() == path)
                return r;
        }
        return -1;
    }

    static QQuickItem *rowItem(QQuickItem *treeItem, int row)
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(QPoint, QPoint(0, row)));
        return item;
    }

    static void expandEverything(QQuickItem *treeItem)
    {
        QMetaObject::invokeMethod(treeItem, "expandRecursively", Qt::DirectConnection,
                                  Q_ARG(int, -1), Q_ARG(int, -1));
        QTest::qWait(50);
    }

    // The assertion that matters, and the one tst_sidebar_rename was missing:
    // `opened` only says open() ran. A popup with no size, or positioned off the
    // window, is still "opened" and still dims the screen — which is exactly the
    // reported symptom ("it shades the whole screen, but no menu shows up").
    void verifyMenuIsActuallyShown(const char *what)
    {
        QObject *m = menu();
        QVERIFY2(m, "the context menu must be findable");
        QTRY_VERIFY2(m->property("opened").toBool(),
                     qPrintable(QStringLiteral("%1: the menu must open").arg(what)));

        auto *popupItem = m->property("contentItem").value<QQuickItem *>();
        QVERIFY2(popupItem, "the menu must have a contentItem");

        const qreal w = m->property("width").toReal();
        const qreal h = m->property("height").toReal();

        QVERIFY2(w > 0 && h > 0,
                 qPrintable(QStringLiteral("%1: the menu has no size (%2x%3) — it would dim the "
                                           "screen and show nothing")
                                .arg(what).arg(w).arg(h)));

        // And it must actually land on the window, not off its edge.
        const QPointF scenePos = popupItem->mapToScene(QPointF(0, 0));
        const QRectF menuRect(scenePos, QSizeF(w, h));
        const QRectF windowRect(0, 0, m_window->width(), m_window->height());
        QVERIFY2(windowRect.intersects(menuRect),
                 qPrintable(QStringLiteral("%1: the menu is off-window at (%2,%3 %4x%5) in a "
                                           "%6x%7 window — invisible, but still dimming")
                                .arg(what).arg(menuRect.x()).arg(menuRect.y())
                                .arg(menuRect.width()).arg(menuRect.height())
                                .arg(m_window->width()).arg(m_window->height())));
    }

    void rightClickRowAt(int row)
    {
        QQuickItem *item = rowItem(tree(), row);
        QVERIFY(item);
        QTest::mouseClick(m_window, Qt::RightButton, Qt::NoModifier,
                          item->mapToScene(QPointF(item->width() / 2,
                                                   item->height() / 2)).toPoint());
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        QVERIFY(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")));

        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
        qmldir.write("module SuopTranscriber\n"
                     "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                     "singleton Notifications 1.0 qml/Notifications.qml\n"
                     "HoverBorder 1.0 qml/HoverBorder.qml\n"
                     "AppIcon 1.0 qml/AppIcon.qml\n"
                     "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                     "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                     "UnderlineField 1.0 qml/UnderlineField.qml\n"
                     "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
                     "Snackbar 1.0 qml/Snackbar.qml\n"
                     "SnackbarHost 1.0 qml/SnackbarHost.qml\n"
                     "Sidebar 1.0 qml/Sidebar.qml\n");
        qmldir.close();

        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterType<VaultTreeModel>("SuopTranscriber", 1, 0, "VaultTreeModel");
        qmlRegisterType<VaultSearchProxyModel>("SuopTranscriber", 1, 0, "VaultSearchProxyModel");
        qmlRegisterSingletonType<TranscriptionControllerStub>(
            "SuopTranscriber", 1, 0, "TranscriptionController",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new TranscriptionControllerStub; });

        // Main.qml's structure, reduced to what the context menu can touch: an
        // ApplicationWindow (so Overlay.overlay exists at all) with the Sidebar
        // AND the SnackbarHost that parents itself onto that same overlay.
        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import QtQuick.Controls\n"
            "import SuopTranscriber\n"
            "ApplicationWindow {\n"
            "    width: 900\n"
            "    height: 600\n"
            "    visible: true\n"
            "    property string openPath: \"\"\n"
            "    VaultTreeModel { id: vaultModel; vaultPath: Qt.resolvedUrl(vaultUrl) }\n"
            "    Component.onCompleted: AppSettings.useLabelsSidebar = true\n"
            "    Sidebar {\n"
            "        id: sidebar\n"
            "        objectName: \"sidebar\"\n"
            "        width: 320\n"
            "        height: parent.height\n"
            "        model: vaultModel\n"
            "        transcriptionController: TranscriptionController\n"
            "        currentPath: openPath\n"
            "        onFileActivated: (path) => openPath = path\n"
            "        onOrphanedMdActivated: (path) => openPath = path\n"
            "    }\n"
            "    SnackbarHost {}\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driver(m_driverQmlPath);
        QVERIFY(driver.open(QIODevice::WriteOnly | QIODevice::Text));
        driver.write(driverQml.toUtf8());
        driver.close();
    }

    void cleanup()
    {
        m_window = nullptr;
        m_engine.reset();
    }

    // User report: right-click "works once, then not again".
    void rightClick_showsTheMenuEveryTime()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));

        QVERIFY(openVault(vault.path()));
        QTRY_VERIFY(tree()->property("rows").toInt() > 0);
        expandEverything(tree());

        const int rowA = rowForPath(tree(), vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(tree(), vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);

        rightClickRowAt(rowA);
        verifyMenuIsActuallyShown("first right-click");
        QMetaObject::invokeMethod(menu(), "close");
        QTRY_VERIFY(!menu()->property("opened").toBool());

        rightClickRowAt(rowB);
        verifyMenuIsActuallyShown("second right-click");
    }

    // User report: "It also fails the first time and onwards for one with a
    // transcript." A transcribed row shows the extra "Delete transcript" entry,
    // which changes the Column's content height — and the Popup's height is
    // implicit from that Column.
    void rightClick_showsTheMenuOnATranscribedRow()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3.md")),
                      "---\nmodel: x\n---\n[0] hi\n"));

        QVERIFY(openVault(vault.path()));
        QTRY_VERIFY(tree()->property("rows").toInt() > 0);
        expandEverything(tree());

        const int row = rowForPath(tree(), vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        rightClickRowAt(row);

        QCOMPARE(sidebar()->property("ctxStatus").toInt(), 2);
        verifyMenuIsActuallyShown("transcribed row");
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_contextmenu"));
    QGuiApplication app(argc, argv);
    TstSidebarContextMenu tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_contextmenu.moc"
