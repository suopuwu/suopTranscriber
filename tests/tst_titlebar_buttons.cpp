// Living spec + runtime verification for TitleBar.qml's double-click-to-maximize
// vs. its own buttons — the frameless ("no system titlebar") mode only, since
// that's the only mode TitleBar.qml is visible in (Main.qml gates it on
// !AppSettings.useSystemTitlebar; a system-decorated window gets KWin's
// titlebar and none of this code runs).
//
// Same harness shape as tst_middledragscroll.cpp / tst_sidebar_expand.cpp (read
// their top-of-file comments first): a REAL offscreen QQuickView loading the
// actual checked-out TitleBar.qml, driven with real QTest mouse events.
//
// The claim this file exists for is NOT reviewable from source, and reading it
// wrong is what put the bug here in the first place: the bar-wide
// double-tap→maximize TapHandler and each button's own TapHandler are SEPARATE
// handlers on nested items, and Qt Quick does not "consume" a tap the way an
// accepted MouseArea press does. With both on the default DragThreshold
// gesturePolicy, both take only PASSIVE grabs, both match the same two clicks,
// and BOTH fire — so double-clicking the gear opened settings twice AND
// maximized the window. Whether an exclusive grab from press (ReleaseWithinBounds)
// is enough to keep the outer handler out is a Qt delivery detail with no
// answer in the docs; these tests pin it.
//
// The `window` here is a stand-in QtObject with a `visibility` property, not a
// real Window: TitleBar only ever reads/writes window.visibility for maximize,
// and driving a real QWindow's visibility offscreen proves nothing extra while
// making the assertions race the platform.
//
// tst_styleguide.cpp keeps the cheap source-scan guards; this file proves the
// behavior on screen.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QUrl>
#include <QWindow>

#include "IconTheme.h"

class TstTitleBarButtons : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    QQuickView *m_view = nullptr;

    QQuickItem *titleBar() const
    {
        return m_view->rootObject()->findChild<QQuickItem *>(QStringLiteral("titleBar"));
    }

    QQuickItem *button(const QString &objectName) const
    {
        return m_view->rootObject()->findChild<QQuickItem *>(objectName);
    }

    // Scene-coordinate centre of a named button, i.e. where a user aiming at it
    // actually clicks.
    QPoint centreOf(const QString &objectName) const
    {
        QQuickItem *item = button(objectName);
        if (!item)
            return {};
        const QPointF scene = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        return scene.toPoint();
    }

    int visibility() const
    {
        QObject *w = titleBar()->property("window").value<QObject *>();
        return w ? w->property("visibility").toInt() : -1;
    }

    int counter(const char *name) const
    {
        return m_view->rootObject()->property(name).toInt();
    }

    // A real double click: two clicks close enough together, at one spot, that
    // Qt counts them as tapCount 2. QTest::mouseDClick sends exactly that.
    void doubleClick(QPoint pos)
    {
        QTest::mouseMove(m_view, pos);
        QTest::mouseDClick(m_view, Qt::LeftButton, {}, pos);
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
                     "HoverBorder 1.0 qml/HoverBorder.qml\n"
                     "AppIcon 1.0 qml/AppIcon.qml\n"
                     "AppIconButton 1.0 qml/AppIconButton.qml\n"
                     "ConfirmPopup 1.0 qml/ConfirmPopup.qml\n"
                     "TitleBar 1.0 qml/TitleBar.qml\n");
        qmldir.close();

        // TitleBar.qml's AppIcon usages bind IconTheme.resolve(...); register
        // the singleton so they resolve instead of throwing at load.
        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });

        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    id: root\n"
            "    width: 600\n"
            "    height: 100\n"
            "    property int settingsRequests: 0\n"
            "    property int recordRequests: 0\n"
            "    property int usbGrabberRequests: 0\n"
            // Stand-in for the real Window — see this file's header comment.
            "    QtObject {\n"
            "        id: fakeWindow\n"
            "        property int visibility: Window.Windowed\n"
            "    }\n"
            "    TitleBar {\n"
            "        objectName: \"titleBar\"\n"
            "        width: parent.width\n"
            "        height: implicitHeight\n"
            "        window: fakeWindow\n"
            "        title: \"a note\"\n"
            "        onSettingsRequested: root.settingsRequests++\n"
            "        onRecordRequested: root.recordRequests++\n"
            "        onUsbGrabberRequested: root.usbGrabberRequests++\n"
            "    }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driverFile(m_driverQmlPath);
        QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
        driverFile.write(driverQml.toUtf8());
        driverFile.close();
    }

    void init()
    {
        m_view = new QQuickView;
        m_view->engine()->addImportPath(m_moduleDir.path());
        m_view->engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                           new IconImageProvider);
        m_view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (m_view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : m_view->errors())
                msg += e.toString() + QStringLiteral("\n");
            // A QML error here is the exact failure mode a green cmake build
            // does NOT catch (AOT/qmlcachegen can compile a file that still
            // fails to load) — report it verbatim rather than as "null item".
            qWarning("%s", qPrintable(msg.isEmpty() ? QStringLiteral("QML failed to load, no error text") : msg));
        }
        QCOMPARE(m_view->status(), QQuickView::Ready);
        m_view->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_view));
        QVERIFY(titleBar());
        QCOMPARE(visibility(), int(QWindow::Windowed));
    }

    void cleanup()
    {
        delete m_view;
        m_view = nullptr;
    }

    // The behavior the bar-wide handler is FOR, and the control for every test
    // below: double-clicking the bar itself (over the title text, where there's
    // no button) still toggles maximize. If this ever fails, the tests below
    // are passing for the wrong reason.
    void doubleClick_onBarItself_togglesMaximize()
    {
        const QPoint overTheTitleText(300, titleBar()->height() / 2);
        doubleClick(overTheTitleText);
        QCOMPARE(visibility(), int(QWindow::Maximized));

        // ...and back, i.e. it's a toggle, not a one-way latch.
        doubleClick(overTheTitleText);
        QCOMPARE(visibility(), int(QWindow::Windowed));
    }

    // THE headline test. A button is not part of the drag/maximize surface: a
    // double-click on one is two clicks OF THAT BUTTON, and must leave the
    // window size alone. Data-driven over the whole left cluster because the
    // fix is per-button — adding a fifth button and forgetting it is exactly
    // the regression this catches.
    void doubleClick_onLeftClusterButton_doesNotMaximize_data()
    {
        QTest::addColumn<QString>("objectName");
        QTest::addColumn<QByteArray>("counter");
        QTest::newRow("settings") << QStringLiteral("titleBarSettingsButton") << QByteArray("settingsRequests");
        QTest::newRow("record") << QStringLiteral("titleBarRecordButton") << QByteArray("recordRequests");
        QTest::newRow("usbGrabber") << QStringLiteral("titleBarUsbGrabberButton") << QByteArray("usbGrabberRequests");
    }

    void doubleClick_onLeftClusterButton_doesNotMaximize()
    {
        QFETCH(QString, objectName);
        QFETCH(QByteArray, counter);
        QVERIFY2(button(objectName), qPrintable(objectName + QStringLiteral(" not found")));

        doubleClick(centreOf(objectName));

        QVERIFY2(visibility() == int(QWindow::Windowed),
                 qPrintable(QStringLiteral("double-clicking %1 must not toggle maximize — the bar-wide "
                                           "double-tap handler has to stay out of the buttons")
                                .arg(objectName)));
        // The button itself still works, twice — the fix must keep the outer
        // handler out, not swallow the clicks.
        QCOMPARE(this->counter(counter.constData()), 2);
    }

    // The "more actions" button opens a menu rather than emitting a signal, so
    // it can't share the data-driven test above — but it's on the same bar and
    // needs the same guarantee.
    //
    // Worth knowing: this one already passed BEFORE the buttons got their
    // exclusive grab, because its first click opens a modal Popup whose block
    // plane swallows the second — so it never reached tapCount 2. It's kept as
    // a guarantee, not as a reproduction: it would go on passing if the menu
    // stopped being modal and the grab fix were reverted, which is why the
    // left-cluster rows above are the ones that pin the actual behavior.
    void doubleClick_onMoreActionsButton_doesNotMaximize()
    {
        QVERIFY(button(QStringLiteral("titleBarMoreActionsButton")));
        doubleClick(centreOf(QStringLiteral("titleBarMoreActionsButton")));
        QCOMPARE(visibility(), int(QWindow::Windowed));
    }

    // Single clicks were never broken; pin them so the fix can't "work" by
    // breaking the buttons.
    void singleClick_onSettingsButton_stillEmitsOnce_andDoesNotMaximize()
    {
        const QPoint pos = centreOf(QStringLiteral("titleBarSettingsButton"));
        QTest::mouseMove(m_view, pos);
        QTest::mouseClick(m_view, Qt::LeftButton, {}, pos);
        QCOMPARE(counter("settingsRequests"), 1);
        QCOMPARE(visibility(), int(QWindow::Windowed));
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name (tests must never touch the user's
    // real vault or app config — AppSettings.qml's Settings{}
    // element writes lastVaultPath the moment it loads). Same setup as
    // tst_middledragscroll.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_titlebar_buttons"));
    QGuiApplication app(argc, argv);
    TstTitleBarButtons tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_titlebar_buttons.moc"
