// Living spec + runtime verification for the snackbar notification system
// (StyleGuide.md "Feedback: inline banner vs. snackbar"): drives a REAL
// ApplicationWindow (offscreen, sandboxed config — see main() below) loading
// the actual Notifications.qml / SnackbarHost.qml / Snackbar.qml, with real
// QTest mouse events, rather than trusting a source scan alone.
//
// Why an ApplicationWindow + QQmlApplicationEngine here, and not the
// QQuickView harness the tst_sidebar_* tests use: SnackbarHost reparents
// itself onto Overlay.overlay, which only exists for an ApplicationWindow. A
// QQuickView root is a plain Item, so the host's whole positioning contract
// would go untested (or silently null-parent) under that harness.
//
// tst_styleguide.cpp carries the plain source-scan guards (no engine there);
// this file is the one place that actually renders and clicks a snackbar. The
// two behaviors that most need real rendering, because neither is reviewable
// from source with confidence:
//   - dismissing one entry must not destroy another entry's delegate
//     (the reason Notifications.queue is a ListModel and not a JS array), and
//   - an action button must not steal focus from a TextField behind it
//     (the reason they're Item + TapHandler, not QQC2 Buttons) — this app
//     commits fields on blur, so a focus steal fires real writes.

#include <QtTest>

#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>

#include "IconTheme.h"

class TstSnackbar : public QObject
{
    Q_OBJECT

    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // Fresh engine per test: the Notifications singleton is per-engine state
    // (a live queue), so reusing one engine would leak entries between tests.
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    QObject *m_notifications = nullptr;

    bool openWindow()
    {
        m_engine = std::make_unique<QQmlApplicationEngine>();
        m_engine->addImportPath(m_moduleDir.path());
        m_engine->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                   new IconImageProvider);
        m_engine->load(QUrl::fromLocalFile(m_driverQmlPath));
        if (m_engine->rootObjects().isEmpty()) {
            // A QML load error here is the exact failure a green cmake build
            // does NOT catch (qmlcachegen can compile a file that still fails
            // to load) — the engine already printed it; say so plainly.
            qWarning("driver QML failed to load — see the QML errors above");
            return false;
        }
        m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().first());
        if (!m_window)
            return false;
        m_notifications = m_engine->singletonInstance<QObject *>(
            QStringLiteral("SuopTranscriber"), QStringLiteral("Notifications"));
        if (!m_notifications)
            return false;
        return QTest::qWaitForWindowExposed(m_window);
    }

    // Walks the VISUAL tree (childItems), not the QObject tree (findChildren),
    // and that distinction is the whole reason an earlier version of this file
    // found nothing: a Repeater's delegates are created by QQmlDelegateModel,
    // which owns them outright — they have NO QObject parent, so findChildren
    // from the window can never see a snackbar or its action buttons. The
    // statically-declared bits (the close button) DO have QObject parents, but
    // one lookup that works everywhere beats two that each work half the time.
    static void collectByName(QQuickItem *item, const QString &name, QList<QQuickItem *> &out)
    {
        if (!item)
            return;
        if (item->objectName() == name)
            out.append(item);
        const auto kids = item->childItems();
        for (QQuickItem *k : kids)
            collectByName(k, name, out);
    }

    QList<QQuickItem *> itemsNamed(const QString &name) const
    {
        QList<QQuickItem *> out;
        collectByName(m_window->contentItem(), name, out);
        return out;
    }

    static QQuickItem *childNamed(QQuickItem *from, const QString &name)
    {
        QList<QQuickItem *> out;
        collectByName(from, name, out);
        return out.isEmpty() ? nullptr : out.first();
    }

    QList<QQuickItem *> snackbars() const { return itemsNamed(QStringLiteral("snackbar")); }

    // The host is declared in the driver, so it IS QObject-parented — but it
    // reparents onto the overlay, which lives under contentItem, so the visual
    // walk finds it either way.
    QQuickItem *host() const
    {
        const auto found = itemsNamed(QStringLiteral("host"));
        return found.isEmpty() ? nullptr : found.first();
    }

    int queueCount() const
    {
        QObject *queue = m_notifications->property("queue").value<QObject *>();
        return queue ? queue->property("count").toInt() : -1;
    }

    QQuickItem *stack() const
    {
        const auto found = itemsNamed(QStringLiteral("snackbarStack"));
        return found.isEmpty() ? nullptr : found.first();
    }

    // A delegate that EXISTS is not yet a delegate that has been POSITIONED.
    // Column lays out during polish, which needs event-loop turns, so for a
    // brief window after a post the stack is still 0×0 and its children sit at
    // its un-laid-out origin — which, because the stack is anchored to the
    // bottom-right corner, is off the window entirely. Measuring or clicking in
    // that window reads garbage: every geometry assertion and every click below
    // must gate on this first. (This cost several hours of chasing "product
    // bugs" that were only ever this race.)
    bool layoutSettled() const
    {
        QQuickItem *s = stack();
        return s && s->width() > 0 && s->height() > 0;
    }

    // Clicks the item's center, but only once the layout has settled, and fails
    // loudly rather than letting QTest merely warn "occurs outside target
    // window" and carry on to a confusing downstream assertion.
    void clickCenter(QQuickItem *item)
    {
        QVERIFY(item);
        QTRY_VERIFY2(layoutSettled(), "the snackbar stack never laid out");
        const QPoint pt = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        QVERIFY2(QRectF(0, 0, m_window->width(), m_window->height()).contains(pt),
                 qPrintable(QStringLiteral("click point %1,%2 is outside the %3x%4 window")
                                .arg(pt.x()).arg(pt.y())
                                .arg(m_window->width()).arg(m_window->height())));
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, pt);
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
                     "singleton Notifications 1.0 qml/Notifications.qml\n"
                     "HoverBorder 1.0 qml/HoverBorder.qml\n"
                     "AppIcon 1.0 qml/AppIcon.qml\n"
                     "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                     "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                     "Snackbar 1.0 qml/Snackbar.qml\n"
                     "SnackbarHost 1.0 qml/SnackbarHost.qml\n");
        qmldir.close();

        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });

        // A TextField sits behind the host deliberately — focusStealing() needs
        // something focusable to lose focus, and that's the whole hazard the
        // Item+TapHandler action buttons exist to avoid.
        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import QtQuick.Controls\n"
            "import SuopTranscriber\n"
            "ApplicationWindow {\n"
            "    id: win\n"
            "    width: 900\n"
            "    height: 600\n"
            "    visible: true\n"
            "    property int undoCount: 0\n"
            "    property string lastUndoText: \"\"\n"
            "    TextField { id: field; objectName: \"field\"; anchors.centerIn: parent }\n"
            "    SnackbarHost { objectName: \"host\" }\n"
            "    function postPlain(text, timeout) {\n"
            "        return Notifications.post({ text: text, timeout: timeout })\n"
            "    }\n"
            "    function postWithUndo(text) {\n"
            "        return Notifications.post({\n"
            "            text: text, kind: \"error\", timeout: 0,\n"
            "            actions: [{ label: \"Undo\", onTriggered: function() {\n"
            "                win.undoCount += 1; win.lastUndoText = text\n"
            "            } }]\n"
            "        })\n"
            "    }\n"
            "    function dismissToken(t) { Notifications.dismiss(t) }\n"
            "    function focusField() { field.forceActiveFocus() }\n"
            "    function fieldHasFocus() { return field.activeFocus }\n"
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
        m_notifications = nullptr;
        m_engine.reset();
    }

    // The baseline: posting renders exactly one snackbar carrying the text.
    // The host's whole positioning contract: it must actually land on the
    // window's overlay. If Overlay.overlay resolved to null the host would be
    // unparented — findable but never rendered — and every click test below
    // would fail for a reason that has nothing to do with what it's testing.
    // Asserted first, and separately, so that failure is legible.
    void host_reparentsOntoTheWindowOverlay()
    {
        QVERIFY(openWindow());
        QQuickItem *h = host();
        QVERIFY2(h, "SnackbarHost must be findable in the window's QObject tree");
        QVERIFY2(h->parentItem(),
                 "SnackbarHost must have a visual parent — a null one means Overlay.overlay "
                 "didn't resolve, and nothing would ever render");
        // By class, not by comparing against ApplicationWindow.overlay: Qt 6
        // removed that plain property in favor of the Overlay.overlay attached
        // property, so property("overlay") reads back null and would make this
        // assertion compare a real pointer against a null of its own making.
        QVERIFY2(h->parentItem()->inherits("QQuickOverlay"),
                 qPrintable(QStringLiteral("SnackbarHost must parent onto the window's overlay, "
                                           "but landed on: ")
                            + QString::fromUtf8(h->parentItem()->metaObject()->className())));
        QVERIFY2(h->width() > 0 && h->height() > 0,
                 "the host must fill the overlay, not collapse to zero size");
    }

    // Regression: the stack must lay out INSIDE the window, hard against the
    // bottom-right. Two separate mistakes each produced a 0×0 Column anchored
    // at the corner, which rendered every snackbar outward past the window edge
    // (mostly invisible, entirely unclickable): a `y` binding on the delegate
    // fighting the positioner, and a `visible:` binding on the host making its
    // children non-positionable while the queue was empty. Neither showed up as
    // a QML warning — the only symptom was clicks landing off-window.
    void stack_laysOutInsideTheWindowAtBottomRight()
    {
        QVERIFY(openWindow());
        QVariant ret;
        QMetaObject::invokeMethod(m_window, "postPlain", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("Couldn't move: a.wav")),
                                  Q_ARG(QVariant, 0));
        QTRY_COMPARE(snackbars().size(), 1);

        QTRY_VERIFY2(layoutSettled(), "the snackbar stack never laid out");
        QQuickItem *bar = snackbars().first();
        QVERIFY2(bar->width() > 0 && bar->height() > 0, "a snackbar must have real size");
        const QRectF r = bar->mapRectToScene(QRectF(0, 0, bar->width(), bar->height()));
        QVERIFY2(r.left() >= 0 && r.top() >= 0
                     && r.right() <= m_window->width() && r.bottom() <= m_window->height(),
                 qPrintable(QStringLiteral("the snackbar must render fully inside the window, "
                                           "but its scene rect is ")
                            + QStringLiteral("(%1,%2 %3x%4) in a %5x%6 window")
                                  .arg(r.left()).arg(r.top()).arg(r.width()).arg(r.height())
                                  .arg(m_window->width()).arg(m_window->height())));
        // Hard against the bottom-right, within the 16px margins.
        QVERIFY2(m_window->width() - r.right() <= 17 && m_window->height() - r.bottom() <= 17,
                 "the stack must sit in the bottom-right corner");
    }

    void post_rendersOneSnackbarWithTheText()
    {
        QVERIFY(openWindow());
        QCOMPARE(snackbars().size(), 0);
        QVERIFY(host());

        QVariant ret;
        QMetaObject::invokeMethod(m_window, "postPlain", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("Couldn't move: a.wav")),
                                  Q_ARG(QVariant, 0));
        QTRY_COMPARE(snackbars().size(), 1);
        QCOMPARE(queueCount(), 1);
        QCOMPARE(snackbars().first()->property("text").toString(),
                 QStringLiteral("Couldn't move: a.wav"));
    }

    // The X button — the dismissability the whole component is named for.
    void dismissButton_removesTheEntry()
    {
        QVERIFY(openWindow());
        QVariant ret;
        QMetaObject::invokeMethod(m_window, "postPlain", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("gone soon")),
                                  Q_ARG(QVariant, 0));
        QTRY_COMPARE(snackbars().size(), 1);

        QQuickItem *close = childNamed(snackbars().first(), QStringLiteral("snackbarClose"));
        QVERIFY2(close, "every snackbar must carry a dismiss button");
        clickCenter(close);

        QTRY_COMPARE(queueCount(), 0);
        QTRY_COMPARE(snackbars().size(), 0);
    }

    // An action fires its callback AND dismisses — actions are terminal (you
    // undo once; a second Undo would re-trash).
    void action_firesCallbackThenDismisses()
    {
        QVERIFY(openWindow());
        QVariant ret;
        QMetaObject::invokeMethod(m_window, "postWithUndo", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("Moved 2 files to trash.")));
        QTRY_COMPARE(snackbars().size(), 1);

        QQuickItem *action = childNamed(snackbars().first(), QStringLiteral("snackbarAction"));
        QVERIFY2(action, "a posted action must render a button");
        QCOMPARE(action->property("label").toString(), QStringLiteral("Undo"));

        clickCenter(action);
        QTRY_COMPARE(m_window->property("undoCount").toInt(), 1);
        QCOMPARE(m_window->property("lastUndoText").toString(),
                 QStringLiteral("Moved 2 files to trash."));
        QTRY_COMPARE(queueCount(), 0);
        QVERIFY2(snackbars().isEmpty(), "firing an action must dismiss its snackbar");
    }

    // timeout: 0 is sticky; a real timeout expires on its own.
    void timeout_expiresOnItsOwn_andZeroIsSticky()
    {
        QVERIFY(openWindow());
        QVariant ret;
        QMetaObject::invokeMethod(m_window, "postPlain", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("sticky")),
                                  Q_ARG(QVariant, 0));
        QMetaObject::invokeMethod(m_window, "postPlain", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("fleeting")),
                                  Q_ARG(QVariant, 250));
        QTRY_COMPARE(queueCount(), 2);

        QTRY_COMPARE_WITH_TIMEOUT(queueCount(), 1, 3000);
        QCOMPARE(snackbars().size(), 1);
        QVERIFY2(snackbars().first()->property("text").toString() == QStringLiteral("sticky"),
                 "timeout: 0 must never auto-expire — the sticky entry is the survivor");
    }

    // StyleGuide: "An action never has to race its own timer." Reaching for
    // Undo must not let the entry expire out from under the cursor.
    void hoveringTheStack_pausesExpiry()
    {
        QVERIFY(openWindow());
        QVariant ret;
        QMetaObject::invokeMethod(m_window, "postPlain", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("hover me")),
                                  Q_ARG(QVariant, 300));
        QTRY_COMPARE(snackbars().size(), 1);

        // Park the pointer on the snackbar and hold it well past the timeout.
        QTRY_VERIFY2(layoutSettled(), "the snackbar stack never laid out");
        QQuickItem *bar = snackbars().first();
        const QPoint pt = bar->mapToScene(QPointF(bar->width() / 2, bar->height() / 2)).toPoint();
        QVERIFY(QRectF(0, 0, m_window->width(), m_window->height()).contains(pt));
        QTest::mouseMove(m_window, pt);
        QTRY_VERIFY2(m_notifications->property("paused").toBool(),
                     "the pointer resting over the stack must pause expiry");
        QTest::qWait(700);
        QCOMPARE(queueCount(), 1);
        QVERIFY2(!snackbars().isEmpty(), "a hovered snackbar must not expire");

        // Leaving resumes it.
        QTest::mouseMove(m_window, QPoint(5, 5));
        QTRY_VERIFY(!m_notifications->property("paused").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(queueCount(), 0, 3000);
    }

    // THE reason Notifications.queue is a ListModel rather than a `property var`
    // JS array: reassigning an array rebuilds every delegate, which would
    // destroy a snackbar mid-click on ANOTHER snackbar's action. Asserted on
    // delegate IDENTITY — a rebuild would hand back a different QQuickItem*.
    void dismissingOneEntry_doesNotRecreateTheOthersDelegates()
    {
        QVERIFY(openWindow());
        QVariant ret;
        QMetaObject::invokeMethod(m_window, "postPlain", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("first")), Q_ARG(QVariant, 0));
        const int firstToken = ret.toInt();
        QMetaObject::invokeMethod(m_window, "postPlain", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("second")), Q_ARG(QVariant, 0));
        QMetaObject::invokeMethod(m_window, "postPlain", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("third")), Q_ARG(QVariant, 0));
        QTRY_COMPARE(snackbars().size(), 3);

        // Identity of the survivors, captured before the removal.
        QQuickItem *secondBefore = nullptr;
        QQuickItem *thirdBefore = nullptr;
        for (QQuickItem *s : snackbars()) {
            const QString t = s->property("text").toString();
            if (t == QStringLiteral("second"))
                secondBefore = s;
            else if (t == QStringLiteral("third"))
                thirdBefore = s;
        }
        QVERIFY(secondBefore && thirdBefore);

        QMetaObject::invokeMethod(m_window, "dismissToken", Q_ARG(QVariant, firstToken));
        QTRY_COMPARE(snackbars().size(), 2);

        QQuickItem *secondAfter = nullptr;
        QQuickItem *thirdAfter = nullptr;
        for (QQuickItem *s : snackbars()) {
            const QString t = s->property("text").toString();
            if (t == QStringLiteral("second"))
                secondAfter = s;
            else if (t == QStringLiteral("third"))
                thirdAfter = s;
        }
        QCOMPARE(secondAfter, secondBefore);
        QCOMPARE(thirdAfter, thirdBefore);
    }

    // THE reason action/dismiss controls are Item + TapHandler and not QQC2
    // Buttons: this app commits TextFields on blur, so a focus steal here fires
    // real writes — and a failed write posts another snackbar, mutating the
    // queue mid-click.
    void actionAndDismissButtons_neverStealFocusFromAFieldBehind()
    {
        QVERIFY(openWindow());
        QMetaObject::invokeMethod(m_window, "focusField");
        QVariant hasFocus;
        QMetaObject::invokeMethod(m_window, "fieldHasFocus", Q_RETURN_ARG(QVariant, hasFocus));
        QVERIFY2(hasFocus.toBool(), "precondition: the field behind the snackbar is focused");

        QVariant ret;
        QMetaObject::invokeMethod(m_window, "postWithUndo", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("Moved 1 file to trash.")));
        QTRY_COMPARE(snackbars().size(), 1);

        clickCenter(childNamed(snackbars().first(), QStringLiteral("snackbarAction")));
        QTRY_COMPARE(m_window->property("undoCount").toInt(), 1);
        QMetaObject::invokeMethod(m_window, "fieldHasFocus", Q_RETURN_ARG(QVariant, hasFocus));
        QVERIFY2(hasFocus.toBool(),
                 "pressing a snackbar action must not blur the field behind it — this app "
                 "commits fields on blur, so a steal fires a real write");

        // Same for the X.
        QMetaObject::invokeMethod(m_window, "postPlain", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("dismiss me")),
                                  Q_ARG(QVariant, 0));
        QTRY_COMPARE(snackbars().size(), 1);
        clickCenter(childNamed(snackbars().first(), QStringLiteral("snackbarClose")));
        QTRY_COMPARE(queueCount(), 0);
        QMetaObject::invokeMethod(m_window, "fieldHasFocus", Q_RETURN_ARG(QVariant, hasFocus));
        QVERIFY2(hasFocus.toBool(), "the dismiss X must not steal focus either");
    }

    // Colors are StyleGuide-fixed per kind, and shared with the inline banners
    // so the same class of problem reads the same on either surface.
    void kind_selectsTheStyleGuideColor()
    {
        QVERIFY(openWindow());
        QVariant ret;
        QMetaObject::invokeMethod(m_window, "postWithUndo", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QStringLiteral("boom")));
        QTRY_COMPARE(snackbars().size(), 1);
        QCOMPARE(snackbars().first()->property("kind").toString(), QStringLiteral("error"));
        QCOMPARE(snackbars().first()->property("kindColor").value<QColor>(), QColor("#e07a7a"));
    }
};

int main(int argc, char *argv[])
{
    // Offscreen: no real compositor/display needed. Must be set before
    // QGuiApplication reads it. Sandboxed org/app name so AppSettings.qml's
    // QtCore Settings{} element (loaded transitively) never touches the real
    // app's ~/.config/suopTranscriber (CLAUDE.md: never let testing touch the
    // user's real vault or app config).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_snackbar"));
    QGuiApplication app(argc, argv);
    TstSnackbar tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_snackbar.moc"
