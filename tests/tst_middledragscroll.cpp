// Living spec + runtime verification for MiddleDragScroll.qml — the shared
// Chrome-style middle-click autoscroll overlay used by MainPanel (transcript),
// Sidebar (file tree) and SettingsPopup (settings page).
//
// Same harness shape as tst_sidebar_expand.cpp / tst_sidebar_dragmove.cpp
// (read their top-of-file comments first): a REAL offscreen QQuickView loading
// the actual checked-out qml/, driven with real QTest mouse events.
//
// The headline claim these tests exist for: ONCE ARMED, THE SCROLL FOLLOWS THE
// POINTER ANYWHERE IN THE WINDOW — moving onto an element outside the region
// the gesture started in must NOT stop it (Chrome/Firefox behave that way, and
// every earlier panel-scoped copy of this overlay silently froze there). That
// is not answerable from source review: both halves of it rest on Qt Quick
// delivery behavior that is easy to get wrong in the reading —
//   - hover delivery is bounds-scoped per item, so gesture 1's free-scroll
//     tracking only survives because the armed phase is hoisted onto
//     Overlay.overlay (window-sized), not left on the arming overlay;
//   - gesture 2's held drag only survives leaving those bounds because
//     TapHandler.ReleaseWithinBounds — unlike DragWithinBounds, and unlike the
//     default DragThreshold — cancels its grab on neither drag distance nor
//     bounds exit.
// A driver with a deliberately SMALL arm region inside a bigger window is what
// pins both: every assertion below scrolls with the pointer parked well
// outside the arming overlay, over a sibling element that isn't it.
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

class TstMiddleDragScroll : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // The arming overlay covers the left panel only (0,0 200x300); `other`
    // (the stand-in for "an element that isn't the one you're scrolling" — a
    // sidebar, the bottom bar, another panel) is the right half. Every
    // scroll-while-outside assertion drives the pointer to kOutside.
    static constexpr QPoint kInside{100, 150};
    static constexpr QPoint kOutside{300, 250}; // 100px below origin => scrolls down
    static constexpr QPoint kInsideDeadzone{100, 155}; // 5px < deadzone(8)

    QQuickView *m_view = nullptr;

    QQuickItem *flick() const { return m_view->rootObject()->findChild<QQuickItem *>(QStringLiteral("flick")); }
    QQuickItem *mds() const { return m_view->rootObject()->findChild<QQuickItem *>(QStringLiteral("mds")); }
    qreal contentY() const { return flick()->property("contentY").toReal(); }
    bool armed() const { return mds()->property("armed").toBool(); }

    // Moves the pointer the way a user does: QTest's synthetic events do NOT
    // move the real cursor, and Qt re-syncs hover from QCursor::pos() whenever
    // a window appears under it — so a stale global cursor delivers a PHANTOM
    // hover at a position no test ever sent, silently rewriting the tracked
    // pointer mid-test. (Concretely: this harness's windows sit at global
    // (2,2) while the offscreen platform parks its cursor at (10,10), so every
    // view after the first got a phantom hover at local y=8 — enough to make an
    // armed scroll take off with the pointer standing still. That is a
    // synthetic-input artifact, not app behavior: a real pointer is wherever
    // QCursor::pos() says it is.) Keeping the two in step makes each test
    // independent of the ones before it.
    void moveMouse(QPoint pos)
    {
        QCursor::setPos(m_view->mapToGlobal(pos));
        QTest::mouseMove(m_view, pos);
    }

    // A quick middle click with no movement between press and release =
    // gesture 1 (arm free-scroll, keep running after release). A real click is
    // always preceded by the pointer being there, hence the move.
    void quickMiddleClick(QPoint pos)
    {
        moveMouse(pos);
        QTest::mousePress(m_view, Qt::MiddleButton, {}, pos);
        QTest::mouseRelease(m_view, Qt::MiddleButton, {}, pos);
    }

    // Long enough for several 16ms autoscroll ticks, short enough not to hit
    // the content-size clamp (~17px/tick at the offsets used here).
    void letItScroll() { QTest::qWait(150); }

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
        // MiddleDragScroll.qml imports nothing from this module, so unlike the
        // Sidebar harnesses this qmldir needs no singletons/other types.
        qmldir.write("module SuopTranscriber\n"
                     "MiddleDragScroll 1.0 qml/MiddleDragScroll.qml\n");
        qmldir.close();

        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    width: 400\n"
            "    height: 300\n"
            "    Item {\n"
            "        id: panel\n"
            "        width: 200\n"
            "        height: 300\n"
            "        Flickable {\n"
            "            id: flick\n"
            "            objectName: \"flick\"\n"
            "            anchors.fill: parent\n"
            "            contentWidth: 200\n"
            "            contentHeight: 3000\n"
            // Stands in for the hoverable content every real caller covers
            // (tree rows, segment text, settings buttons): its cursor is what
            // an overlay that wrongly claims one would steal.
            "            Item {\n"
            "                width: 200\n"
            "                height: 3000\n"
            "                HoverHandler { cursorShape: Qt.PointingHandCursor }\n"
            "            }\n"
            "        }\n"
            "        MiddleDragScroll {\n"
            "            objectName: \"mds\"\n"
            "            anchors.fill: parent\n"
            "            flickable: flick\n"
            "        }\n"
            "    }\n"
            // The element the pointer wanders onto. Its tap counter pins the
            // other half of Chrome's convention: a click that cancels an armed
            // scroll is CONSUMED, not also delivered as a normal click.
            "    Item {\n"
            "        id: other\n"
            "        objectName: \"other\"\n"
            "        x: 200\n"
            "        width: 200\n"
            "        height: 300\n"
            "        property int taps: 0\n"
            "        TapHandler {\n"
            "            acceptedButtons: Qt.LeftButton | Qt.MiddleButton\n"
            "            onTapped: other.taps++\n"
            "        }\n"
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
        // Park the real cursor somewhere known before each test — see
        // moveMouse().
        QCursor::setPos(m_view->mapToGlobal(kInside));
        QVERIFY(flick());
        QVERIFY(mds());
    }

    void cleanup()
    {
        delete m_view;
        m_view = nullptr;
    }

    // THE headline test — the bug this component was extracted to fix.
    // Gesture 1: quick middle click arms, then the pointer moves off the arm
    // region entirely and free-scrolls from there. A panel-scoped overlay
    // froze here, because hover delivery stops at the item's own bounds.
    void gesture1_freeScroll_continuesOverAnElementOutsideTheArmRegion()
    {
        // Start mid-content, NOT at the top: at contentY 0 a runaway UPWARD
        // scroll clamps to 0 and is indistinguishable from correctly holding
        // still, which is exactly how a phantom-hover bug hid here once.
        flick()->setProperty("contentY", 1000.0);

        quickMiddleClick(kInside);
        QVERIFY2(armed(), "a quick middle click (press+release, no drag) must arm free-scroll");
        // The pointer hasn't moved off the arm point: nothing moves, either way.
        letItScroll();
        QCOMPARE(contentY(), 1000.0);

        moveMouse(kOutside);
        letItScroll();
        QVERIFY2(contentY() > 1000.0,
                 "armed free-scroll must keep scrolling while the pointer sits over an element "
                 "outside the arm region (Chrome keeps scrolling wherever the cursor goes)");
        QVERIFY(armed());

        // Scrolling DOWN, i.e. in the direction the pointer actually moved.
        QVERIFY(contentY() > 1000.0);

        // And it keeps going from out there, rather than stalling after one
        // stale hover update.
        const qreal seen = contentY();
        letItScroll();
        QVERIFY(contentY() > seen);
    }

    // Gesture 2: press, drag off the arm region while held, and the scroll
    // must survive the pointer leaving those bounds — TapHandler's default
    // DragThreshold policy would have cancelled the grab at ~a few px, and
    // DragWithinBounds would have cancelled it at the panel edge.
    void gesture2_heldDrag_continuesOutsideTheArmRegion()
    {
        moveMouse(kInside);
        QTest::mousePress(m_view, Qt::MiddleButton, {}, kInside);
        moveMouse(kOutside);
        letItScroll();
        QVERIFY2(contentY() > 0.0,
                 "a held middle-drag must keep scrolling after the pointer leaves the arm region");
        QVERIFY(armed());

        // Release after a real drag = gesture 2: disarms immediately, no
        // second click needed, and the content stops where it was.
        QTest::mouseRelease(m_view, Qt::MiddleButton, {}, kOutside);
        QVERIFY2(!armed(), "releasing after a real drag must disarm (gesture 2)");
        const qreal stopped = contentY();
        letItScroll();
        QCOMPARE(contentY(), stopped);
    }

    // The deadzone travels with the gesture, not with the arm region: a small
    // move is still no scroll.
    void deadzone_suppressesScrollNearTheArmPoint()
    {
        flick()->setProperty("contentY", 1000.0); // see gesture1's note on the 0-clamp
        quickMiddleClick(kInside);
        moveMouse(kInsideDeadzone);
        letItScroll();
        QCOMPARE(contentY(), 1000.0);
        QVERIFY(armed());
    }

    // Chrome's cancel convention, from outside the arm region (where the
    // pointer actually is by the time a user cancels): any click disarms, and
    // is consumed rather than acting on whatever it landed on.
    void clickOutsideTheArmRegion_cancelsArmedScroll_andIsConsumed()
    {
        QQuickItem *other = m_view->rootObject()->findChild<QQuickItem *>(QStringLiteral("other"));
        QVERIFY(other);

        quickMiddleClick(kInside);
        moveMouse(kOutside);
        letItScroll();
        QVERIFY(contentY() > 0.0);

        QTest::mouseClick(m_view, Qt::LeftButton, {}, kOutside);
        QVERIFY2(!armed(), "any click while armed must cancel, wherever the pointer is");
        QCOMPARE(other->property("taps").toInt(), 0);

        const qreal stopped = contentY();
        letItScroll();
        QCOMPARE(contentY(), stopped);
    }

    // Escape disarms too (the other half of SettingsPopup's closePolicy
    // suspension: while armed, Escape is the autoscroll's, not the popup's).
    void escape_disarms()
    {
        quickMiddleClick(kInside);
        moveMouse(kOutside);
        letItScroll();
        QVERIFY(armed());

        QTest::keyClick(m_view, Qt::Key_Escape);
        QVERIFY(!armed());
        const qreal stopped = contentY();
        letItScroll();
        QCOMPARE(contentY(), stopped);
    }

    // While idle the overlay is inert: plain pointer movement scrolls nothing.
    void idle_doesNotScroll()
    {
        flick()->setProperty("contentY", 1000.0); // see gesture1's note on the 0-clamp
        moveMouse(kInside);
        moveMouse(kOutside);
        letItScroll();
        QCOMPARE(contentY(), 1000.0);
        QVERIFY(!armed());
    }

    // The armed phase can only span the window because the tracker is hoisted
    // out of this overlay and into the window overlay. Pins the trap that made
    // it silently inert: a bare `Overlay.overlay` resolves to null before the
    // item has a window, and that null unparents the very item whose
    // windowChanged would re-fire the binding — leaving it parentless and 0x0
    // forever, with no error, no warning, and an autoscroll that simply never
    // tracked.
    void tracker_isHoistedIntoTheWindowOverlay_atFullWindowSize()
    {
        QQuickItem *tracker = mds()->findChild<QQuickItem *>(QStringLiteral("middleDragTracker"));
        QVERIFY(tracker);
        QVERIFY2(tracker->parentItem() && tracker->parentItem() != mds(),
                 "the tracker must be reparented out of the arm overlay into the window overlay");
        QVERIFY2(tracker->window() == m_view, "the tracker must still be in the window");
        // Window-sized, i.e. far wider than the arm region it was declared in.
        QCOMPARE(tracker->width(), m_view->rootObject()->width());
        QCOMPARE(tracker->height(), m_view->rootObject()->height());
        QVERIFY(tracker->width() > mds()->width());
    }

    // The invariant the whole Item-not-MouseArea design exists for, and the
    // one this codebase keeps re-breaking: while idle, this overlay sits
    // PERMANENTLY topmost over the content it arms from, so it must not claim
    // the cursor away from it. Qt Quick resolves the cursor by a top-down
    // paint-order scan for the topmost hit item — a MouseArea there would win
    // that scan and force an ArrowCursor no matter how it was configured
    // (that's what made the Sidebar copy toggle its z), whereas a plain Item
    // whose only cursor-setting handler is DISABLED contributes none.
    void idle_doesNotClaimTheCursorFromCoveredContent()
    {
        moveMouse(kInside);
        QTRY_COMPARE(m_view->cursor().shape(), Qt::PointingHandCursor);
        QVERIFY(!armed());
    }

    // ...and once armed it owns the cursor everywhere, including out over the
    // elements it isn't scrolling — the visible half of "the gesture belongs
    // to the window now".
    void armed_ownsTheAutoscrollCursor_evenOutsideTheArmRegion()
    {
        quickMiddleClick(kInside);
        // Still within the deadzone: any direction is still possible.
        QTRY_COMPARE(m_view->cursor().shape(), Qt::SizeAllCursor);

        moveMouse(kOutside);
        QTRY_COMPARE(m_view->cursor().shape(), Qt::SizeVerCursor);

        // Disarming hands the cursor straight back to the content.
        QTest::keyClick(m_view, Qt::Key_Escape);
        moveMouse(kInside);
        QTRY_COMPARE(m_view->cursor().shape(), Qt::PointingHandCursor);
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name (CLAUDE.md: never let testing touch
    // the user's real vault or app config). Same setup as tst_sidebar_expand.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_middledragscroll"));
    QGuiApplication app(argc, argv);
    TstMiddleDragScroll tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_middledragscroll.moc"
