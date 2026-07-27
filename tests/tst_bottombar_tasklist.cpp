// Living spec + runtime verification for the Tasks-list per-row × (cancel/
// dismiss) button: a click on its RIGHT ~10px used to do nothing because the
// Basic style's overlay vertical ScrollBar (parented over the ScrollView, 10px
// wide, always hit-testable even when nothing is drawn — see
// qml/BottomBar.qml's scrollBarGutter doc comment) floated directly on top of
// it. This is the real-click proof; tst_styleguide.cpp keeps the cheap
// source-scan guard for the alignment intent a click test can't see.
//
// ApplicationWindow + QQmlApplicationEngine, not a bare QQuickView — same
// reason as the sibling tst_bottombar_taskmodel.cpp (whose top-of-file
// comment this mirrors): the rows under test live inside jobListPopup, a
// modal Popup, and a Popup's content only actually renders once it lands on a
// real window Overlay. A bare QQuickView root is a plain Item with no
// guaranteed overlay, so the popup and its rows never materialize there.

#include <QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>

#include "IconTheme.h"

namespace {

// Answers exactly the surface BottomBar.qml's load-time bindings and the
// close button's TapHandler read off `transcriptionController` — see
// tst_bottombar_taskmodel.cpp's identical stub for why every one of these has
// to exist (an unresolved property/call breaks its binding silently rather
// than failing visibly). Every Q_INVOKABLE the close button (or anything else
// reachable while the popup is open) can call records its name + argument
// into a static call list, same pattern as DesktopIntegrationStub in
// tst_bottombar_pathsegments.cpp.
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(double progress READ progress CONSTANT)
    Q_PROPERTY(double overallProgress READ overallProgress CONSTANT)
    Q_PROPERTY(int tasksCompleted READ tasksCompleted NOTIFY jobsChanged)
    Q_PROPERTY(int tasksTotal READ tasksTotal NOTIFY jobsChanged)
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(QString currentJobKind READ currentJobKind CONSTANT)
    Q_PROPERTY(QString currentJobKindLabel READ currentJobKindLabel CONSTANT)
    Q_PROPERTY(QUrl currentFile READ currentFile CONSTANT)
    Q_PROPERTY(QString currentPhase READ currentPhase CONSTANT)
    Q_PROPERTY(bool activePaused READ activePaused CONSTANT)
    Q_PROPERTY(bool activeStopping READ activeStopping CONSTANT)
    Q_PROPERTY(bool activeCanPause READ activeCanPause CONSTANT)
    Q_PROPERTY(QString activeBackendKind READ activeBackendKind CONSTANT)
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    Q_PROPERTY(QVariantList sortedJobs READ jobs NOTIFY jobsChanged)

public:
    struct Call {
        QString method;
        QVariant arg;
    };
    static QList<Call> s_calls;

    void setJobs(const QVariantList &jobs)
    {
        m_jobs = jobs;
        emit jobsChanged();
    }

    double progress() const { return 0.0; }
    double overallProgress() const { return 0.0; }
    int tasksCompleted() const { return 0; }
    int tasksTotal() const { return m_jobs.size(); }
    bool busy() const { return false; }
    QString currentJobKind() const { return QString(); }
    QString currentJobKindLabel() const { return QString(); }
    QUrl currentFile() const { return QUrl(); }
    QString currentPhase() const { return QString(); }
    bool activePaused() const { return false; }
    bool activeStopping() const { return false; }
    bool activeCanPause() const { return false; }
    QString activeBackendKind() const { return QString(); }
    QVariantList jobs() const { return m_jobs; }

    Q_INVOKABLE qint64 activeElapsedMs() const { return 0; }
    Q_INVOKABLE qint64 estimatedRemainingMs() const { return -1; }
    Q_INVOKABLE QString phaseLabel(const QString &) const { return QString(); }
    // Item 015's modelDetail binding calls this for a "transcribe"/"diarize"
    // row (jobRow.modelDetail in BottomBar.qml) — an unguarded read that
    // throws a QML TypeError without it, which breaks the whole delegate's
    // layout (not just the missing text). This harness doesn't assert on the
    // model line at all, so the identity mapping is enough to keep the
    // binding from throwing.
    Q_INVOKABLE QString modelShortLabel(const QString &modelId) const { return modelId; }
    Q_INVOKABLE void cancelJob(const QVariant &id) { s_calls.append({QStringLiteral("cancelJob"), id}); }
    Q_INVOKABLE void dismiss(const QUrl &url) { s_calls.append({QStringLiteral("dismiss"), url}); }
    Q_INVOKABLE void cancelBatch(const QVariant &id) { s_calls.append({QStringLiteral("cancelBatch"), id}); }
    Q_INVOKABLE void dismissBatch(const QVariant &id) { s_calls.append({QStringLiteral("dismissBatch"), id}); }
    Q_INVOKABLE void moveQueuedJob(const QVariant &id, int newIndex)
    {
        s_calls.append({QStringLiteral("moveQueuedJob"), QVariantList{id, newIndex}});
    }
    Q_INVOKABLE void undoTask(const QVariant &token) { s_calls.append({QStringLiteral("undoTask"), token}); }
    Q_INVOKABLE void cancelAll() { s_calls.append({QStringLiteral("cancelAll"), QVariant()}); }
    Q_INVOKABLE void clearFinished() { s_calls.append({QStringLiteral("clearFinished"), QVariant()}); }
    Q_INVOKABLE void togglePauseActive() { s_calls.append({QStringLiteral("togglePauseActive"), QVariant()}); }

signals:
    void jobsChanged();

private:
    QVariantList m_jobs;
};

QList<TranscriptionControllerStub::Call> TranscriptionControllerStub::s_calls;

} // namespace

class TstBottomBarTaskList : public QObject
{
    Q_OBJECT

    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // Fresh engine per test: the stub is a per-engine singleton (see
    // tst_bottombar_taskmodel.cpp's identical rationale).
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    TranscriptionControllerStub *m_stub = nullptr;

    bool openWindow()
    {
        m_engine = std::make_unique<QQmlApplicationEngine>();
        m_engine->addImportPath(m_moduleDir.path());
        m_engine->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                   new IconImageProvider);
        m_engine->load(QUrl::fromLocalFile(m_driverQmlPath));
        if (m_engine->rootObjects().isEmpty()) {
            qWarning("driver QML failed to load — see the QML errors above");
            return false;
        }
        m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().first());
        if (!m_window)
            return false;
        m_stub = qobject_cast<TranscriptionControllerStub *>(m_engine->singletonInstance<QObject *>(
            QStringLiteral("SuopTranscriber"), QStringLiteral("TranscriptionController")));
        if (!m_stub)
            return false;
        return QTest::qWaitForWindowExposed(m_window);
    }

    // Walks the VISUAL tree (childItems), not the QObject tree — the Tasks
    // popup's rows are ListView/Repeater delegates, which are only ever given
    // a visual parent (see tst_bottombar_pathsegments.cpp's identical note).
    static QQuickItem *findByObjectName(QQuickItem *item, const QString &name)
    {
        if (!item)
            return nullptr;
        if (item->objectName() == name)
            return item;
        for (QQuickItem *child : item->childItems()) {
            if (QQuickItem *found = findByObjectName(child, name))
                return found;
        }
        return nullptr;
    }

    static void collectByClass(QQuickItem *item, const char *className, QList<QQuickItem *> &out)
    {
        if (!item)
            return;
        if (item->inherits(className))
            out.append(item);
        for (QQuickItem *child : item->childItems())
            collectByClass(child, className, out);
    }

    static QVariantMap queuedTranscribeRow(int id, const QString &fileName)
    {
        return QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("status"), QStringLiteral("queued")},
            {QStringLiteral("error"), QString()},
            {QStringLiteral("kind"), QStringLiteral("transcribe")},
            {QStringLiteral("kindLabel"), QStringLiteral("Transcribe")},
            {QStringLiteral("displayName"), fileName},
            {QStringLiteral("fileUrl"), QUrl(QStringLiteral("file:///vault/") + fileName)},
            {QStringLiteral("durationMs"), -1},
            {QStringLiteral("undoable"), false},
            {QStringLiteral("undoStale"), false},
            {QStringLiteral("batchId"), 0},
            {QStringLiteral("batchLabel"), QString()},
        };
    }

    // Item 023: a member row of a collapsed batch — same shape as
    // queuedTranscribeRow's standalone row, but stamped with the shared
    // batchId TranscriptionController::collapseBatches() groups on (see
    // JobListFormatter.cpp), which is what jobListView.flatRows keys
    // expansion on.
    static QVariantMap batchMemberRow(int id, const QString &fileName, int batchId)
    {
        QVariantMap row = queuedTranscribeRow(id, fileName);
        row[QStringLiteral("batchId")] = batchId;
        return row;
    }

    static QVariantList makeBatchMembers(int batchId, int count)
    {
        QVariantList members;
        for (int i = 1; i <= count; ++i) {
            members.append(batchMemberRow(1000 + i,
                QStringLiteral("file%1.mp3").arg(i, 3, 10, QLatin1Char('0')), batchId));
        }
        return members;
    }

    // The aggregate row TranscriptionController::collapseBatches() would
    // build for `members` — this stub bypasses collapseBatches() entirely
    // (its jobs()/sortedJobs are whatever setJobs() was given verbatim), so
    // the fixture has to hand-assemble the same shape collapseBatches()
    // produces (see JobListFormatter.cpp) for jobListView.flatRows to fan
    // back out correctly.
    static QVariantMap batchHeaderRow(int batchId, const QVariantList &members)
    {
        return QVariantMap{
            {QStringLiteral("isBatch"), true},
            {QStringLiteral("batchId"), batchId},
            {QStringLiteral("id"), batchId},
            {QStringLiteral("kind"), QStringLiteral("batch")},
            {QStringLiteral("kindLabel"), QStringLiteral("Transcribe")},
            {QStringLiteral("status"), QStringLiteral("queued")},
            {QStringLiteral("error"), QString()},
            {QStringLiteral("fileUrl"), QUrl()},
            {QStringLiteral("batchCount"), members.size()},
            {QStringLiteral("batchDoneCount"), 0},
            {QStringLiteral("durationMs"), qint64(-1)},
            {QStringLiteral("undoable"), false},
            {QStringLiteral("undoStale"), false},
            {QStringLiteral("members"), members},
        };
    }

    // Unlike findByObjectName (first match only) — counts every REALIZED
    // delegate carrying `name`, which is how the virtualization tests below
    // measure how many task rows actually exist as live Items right now
    // (every row, header/member/standalone alike, has exactly one
    // taskCloseButton — see jobRow's RowLayout).
    static void collectByObjectName(QQuickItem *item, const QString &name, QList<QQuickItem *> &out)
    {
        if (!item)
            return;
        if (item->objectName() == name)
            out.append(item);
        for (QQuickItem *child : item->childItems())
            collectByObjectName(child, name, out);
    }

    // Text elides only in PAINTING — QQuickText/Label's own `text` property
    // always holds the full, unelided string — so this reliably finds a
    // specific row's filename among whatever delegates are currently
    // realized, regardless of how narrow the popup renders it.
    static bool anyDescendantTextContains(QQuickItem *item, const QString &needle)
    {
        if (!item)
            return false;
        if (item->inherits("QQuickText") && item->property("text").toString().contains(needle))
            return true;
        for (QQuickItem *child : item->childItems()) {
            if (anyDescendantTextContains(child, needle))
                return true;
        }
        return false;
    }

    // Opens the Tasks popup without seeding openPopupAndFindCloseButton()'s
    // one-job fixture — the batch/virtualization tests below need shapes
    // that helper doesn't produce, so they call setJobs() themselves first.
    QObject *openTasksPopup()
    {
        QQuickItem *bar = m_window->findChild<QQuickItem *>(QStringLiteral("bottomBar"));
        if (!bar)
            return nullptr;
        QObject *popup = bar->findChild<QObject *>(QStringLiteral("tasksPopup"));
        if (!popup)
            return nullptr;
        QMetaObject::invokeMethod(popup, "open");
        return popup;
    }

    // Locates the Tasks popup's single ListView (by class — it has no
    // objectName) once it has settled into a real, non-zero height, same
    // "the ScrollView/ListView needs a couple of layout ticks to catch up
    // from 0" trap openPopupAndFindCloseButton()'s doc comment explains.
    QQuickItem *waitForSettledListView()
    {
        QList<QQuickItem *> listViews;
        const bool settled = QTest::qWaitFor([&] {
            listViews.clear();
            collectByClass(m_window->contentItem(), "QQuickListView", listViews);
            return !listViews.isEmpty() && listViews.first()->height() > 0;
        }, 5000);
        return settled ? listViews.first() : nullptr;
    }

    // Sets the stub's one fixture row, opens the Tasks popup, and waits for
    // the close button to materialize AND settle into a stable, non-zero
    // scene position (see the QQuickView "an item made visible this tick
    // isn't laid out yet" trap — mapToScene can return the container origin
    // for a frame or two after visibility flips).
    //
    // The close button's OWN position settles on the very first layout pass
    // (it's positioned via the delegate Item, independent of its container's
    // reported size) — but the Tasks ListView's own Layout.preferredHeight
    // (bound to its contentHeight — item 023 dropped the wrapping ScrollView,
    // so the ListView is now directly what has to catch up) needs a couple
    // more polish/layout ticks to catch up from its initial 0. Until it does,
    // ListView's `clip: true` makes every row (including the button)
    // geometrically present but NOT part of the hit-test region — a click
    // lands on nothing and is silently swallowed. So "settled" has to mean
    // both: the button's position is stable AND the ListView has actually
    // grown to a non-zero height.
    QQuickItem *openPopupAndFindCloseButton()
    {
        m_stub->setJobs({queuedTranscribeRow(1, QStringLiteral("talk.mp3"))});
        QQuickItem *bar = m_window->findChild<QQuickItem *>(QStringLiteral("bottomBar"));
        if (!bar)
            return nullptr;
        QObject *popup = bar->findChild<QObject *>(QStringLiteral("tasksPopup"));
        if (!popup)
            return nullptr;
        QMetaObject::invokeMethod(popup, "open");

        QQuickItem *closeButton = nullptr;
        qreal lastX = -1;
        const bool settled = QTest::qWaitFor([&] {
            closeButton = findByObjectName(m_window->contentItem(), QStringLiteral("taskCloseButton"));
            if (!closeButton)
                return false;
            const qreal x = closeButton->mapToScene(QPointF(0, 0)).x();
            const bool buttonStable = x > 0 && qFuzzyCompare(x, lastX);
            lastX = x;

            QList<QQuickItem *> listViews;
            collectByClass(m_window->contentItem(), "QQuickListView", listViews);
            const bool listViewGrown = !listViews.isEmpty()
                && std::all_of(listViews.cbegin(), listViews.cend(),
                                [](QQuickItem *lv) { return lv->height() > 0; });
            return buttonStable && listViewGrown;
        }, 5000);
        return settled ? closeButton : nullptr;
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
                     "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                     "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                     "BottomBar 1.0 qml/BottomBar.qml\n");
        qmldir.close();

        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterSingletonType<TranscriptionControllerStub>(
            "SuopTranscriber", 1, 0, "TranscriptionController",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new TranscriptionControllerStub; });

        // A ~1000x700 root with BottomBar anchored to the BOTTOM (height 40):
        // jobListPopup sits at y: -height - 4 relative to the bar, so a
        // shallow driver would put it at negative scene coordinates where no
        // click can reach it.
        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import QtQuick.Controls\n"
            "import SuopTranscriber\n"
            "ApplicationWindow {\n"
            "    id: win\n"
            "    width: 1000\n"
            "    height: 700\n"
            "    visible: true\n"
            "    BottomBar {\n"
            "        id: bar\n"
            "        objectName: \"bottomBar\"\n"
            "        anchors.left: parent.left\n"
            "        anchors.right: parent.right\n"
            "        anchors.bottom: parent.bottom\n"
            "        height: 40\n"
            "        transcriptionController: TranscriptionController\n"
            "    }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driverFile(m_driverQmlPath);
        QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
        driverFile.write(driverQml.toUtf8());
        driverFile.close();
    }

    void init() { TranscriptionControllerStub::s_calls.clear(); }

    void cleanup()
    {
        m_window = nullptr;
        m_stub = nullptr;
        m_engine.reset();
    }

    // The regression test: a click on the close button's RIGHT edge used to
    // land on the ScrollBar overlay (which sat on the rightmost ~10px of the
    // 16px button) and do nothing. Fails before the four-edit fix (nothing
    // recorded), passes after (the bar now starts 2px to the right of the
    // button, per the gutter arithmetic in qml/BottomBar.qml).
    void clickingTheCloseButtonsRightEdge_cancelsTheTask()
    {
        QVERIFY(openWindow());
        QQuickItem *closeButton = openPopupAndFindCloseButton();
        QVERIFY2(closeButton, "the Tasks popup's × button never settled into a stable, clickable position");

        const QPoint pt = closeButton->mapToScene(
            QPointF(closeButton->width() - 2, closeButton->height() / 2)).toPoint();
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, pt);

        QCOMPARE(TranscriptionControllerStub::s_calls.size(), 1);
        QCOMPARE(TranscriptionControllerStub::s_calls.at(0).method, QStringLiteral("cancelJob"));
        QCOMPARE(TranscriptionControllerStub::s_calls.at(0).arg.toInt(), 1);
    }

    // The control: a click well inside the button's LEFT portion must cancel
    // both before and after the fix, so a failure of the right-edge test
    // above localizes specifically to the right-edge lane rather than the
    // button being broken outright.
    //
    // Deliberately NOT the literal geometric centre (width/2): the pre-fix
    // dead lane is 10px wide flush against the right edge of a 16px button —
    // i.e. it covers the button's right 62.5%, so width/2 falls INSIDE it
    // too (measured directly: pre-fix ScrollBar at scene x=[968,978], button
    // at [962,978] — only local x<6 is actually safe, matching the bug
    // report's own "left ~6px works"). A literal centre click would fail
    // before the fix exactly like the regression test, making it useless as
    // a control.
    void clickingTheCloseButtonsSafeLeftZone_cancelsTheTask()
    {
        QVERIFY(openWindow());
        QQuickItem *closeButton = openPopupAndFindCloseButton();
        QVERIFY2(closeButton, "the Tasks popup's × button never settled into a stable, clickable position");

        const QPoint pt = closeButton->mapToScene(
            QPointF(2, closeButton->height() / 2)).toPoint();
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, pt);

        QCOMPARE(TranscriptionControllerStub::s_calls.size(), 1);
        QCOMPARE(TranscriptionControllerStub::s_calls.at(0).method, QStringLiteral("cancelJob"));
        QCOMPARE(TranscriptionControllerStub::s_calls.at(0).arg.toInt(), 1);
    }

    // Geometry, style-independent: the ScrollView's vertical ScrollBar must
    // float entirely to the RIGHT of the × button — never overlapping it —
    // so this keeps holding even if the Basic style's bar width ever changes.
    void taskRowsClearTheScrollBarOverlay()
    {
        QVERIFY(openWindow());
        QQuickItem *closeButton = openPopupAndFindCloseButton();
        QVERIFY2(closeButton, "the Tasks popup's × button never settled into a stable, clickable position");

        QList<QQuickItem *> scrollBars;
        collectByClass(m_window->contentItem(), "QQuickScrollBar", scrollBars);
        QVERIFY2(!scrollBars.isEmpty(), "the Tasks popup's ScrollBars could not be found");

        // ScrollView also carries a horizontal ScrollBar (AlwaysOff, spanning
        // the full width at y near 0) — only the VERTICAL one is what floated
        // over the × button, so filter to it rather than asserting on both.
        QList<QQuickItem *> verticalBars;
        for (QQuickItem *bar : scrollBars) {
            if (bar->property("orientation").toInt() == Qt::Vertical)
                verticalBars.append(bar);
        }
        QVERIFY2(!verticalBars.isEmpty(), "the Tasks popup's vertical ScrollBar could not be found");

        const qreal closeButtonRight = closeButton->mapToScene(QPointF(closeButton->width(), 0)).x();
        for (QQuickItem *bar : verticalBars) {
            const qreal barLeft = bar->mapToScene(QPointF(0, 0)).x();
            QVERIFY2(barLeft >= closeButtonRight - 0.5,
                     qPrintable(QStringLiteral("ScrollBar starts at scene x=%1 but the × button ends at "
                                                "x=%2 — the bar still overlaps the button")
                                    .arg(barLeft).arg(closeButtonRight)));
        }
    }

    // Item 023: expanding/collapsing a batch must show exactly its header row
    // (collapsed) or header+members (expanded), and collapsing again must
    // return to just the header — proven by counting REALIZED taskCloseButton
    // rows, since flattening (jobListView.flatRows) now makes each member a
    // real top-level ListView row instead of a Repeater child living inside
    // one delegate (the structure this item removed).
    void batchExpansion_showsExactRowCountAndCollapsesBack()
    {
        QVERIFY(openWindow());
        const int batchId = 4200;
        const QVariantList members = makeBatchMembers(batchId, 4);
        m_stub->setJobs({batchHeaderRow(batchId, members)});
        QVERIFY(openTasksPopup());

        QQuickItem *listView = waitForSettledListView();
        QVERIFY2(listView, "the Tasks popup's ListView never settled into a non-zero height");

        QList<QQuickItem *> closeButtons;
        QVERIFY2(QTest::qWaitFor([&] {
            closeButtons.clear();
            collectByObjectName(m_window->contentItem(), QStringLiteral("taskCloseButton"), closeButtons);
            return closeButtons.size() == 1; // collapsed: header row only
        }, 5000), "a collapsed batch must render exactly its header row");
        QCOMPARE(listView->property("count").toInt(), 1);

        QMetaObject::invokeMethod(listView, "toggleBatchExpanded", Q_ARG(QVariant, QVariant(batchId)));

        QVERIFY2(QTest::qWaitFor([&] {
            closeButtons.clear();
            collectByObjectName(m_window->contentItem(), QStringLiteral("taskCloseButton"), closeButtons);
            return closeButtons.size() == 5; // header + 4 members
        }, 5000), "an expanded 4-member batch must render exactly 5 rows");
        QCOMPARE(listView->property("count").toInt(), 5);
        QVERIFY2(anyDescendantTextContains(m_window->contentItem(), QStringLiteral("file002.mp3")),
                 "an expanded batch's member row must render its own filename");

        QMetaObject::invokeMethod(listView, "toggleBatchExpanded", Q_ARG(QVariant, QVariant(batchId)));

        QVERIFY2(QTest::qWaitFor([&] {
            closeButtons.clear();
            collectByObjectName(m_window->contentItem(), QStringLiteral("taskCloseButton"), closeButtons);
            return closeButtons.size() == 1; // collapsed again
        }, 5000), "re-collapsing must drop back to exactly the header row");
        QCOMPARE(listView->property("count").toInt(), 1);
    }

    // The regression this item fixes: a 250-member expanded batch used to fan
    // every member out through a Repeater INSIDE one ListView delegate (see
    // qml/BottomBar.qml's flatRows doc comment), instantiating all 250 member
    // Items regardless of what was on screen. Flattening makes each member a
    // real top-level ListView row instead, so ListView's own virtualization
    // realizes only a small, bounded window of delegates — this is the actual
    // proof the fix works (a green popup-opens test alone wouldn't show it).
    void largeExpandedBatch_virtualizesRealizedDelegates_whileFullSetIsReachable()
    {
        QVERIFY(openWindow());
        const int batchId = 8100;
        const int memberCount = 250;
        const QVariantList members = makeBatchMembers(batchId, memberCount);
        m_stub->setJobs({batchHeaderRow(batchId, members)});
        QVERIFY(openTasksPopup());

        QQuickItem *listView = waitForSettledListView();
        QVERIFY2(listView, "the Tasks popup's ListView never settled into a non-zero height");

        QMetaObject::invokeMethod(listView, "toggleBatchExpanded", Q_ARG(QVariant, QVariant(batchId)));

        // flatRows must carry the full header+250-member set even though the
        // popup only ever shows a capped 320px viewport of it.
        QVERIFY2(QTest::qWaitFor([&] {
            return listView->property("count").toInt() == memberCount + 1;
        }, 5000), "the ListView's count must reflect the full flattened row set");
        const qreal contentHeight = listView->property("contentHeight").toReal();
        QVERIFY2(contentHeight > listView->height() * 3,
                 "contentHeight must reflect the full flattened row set, not just the capped viewport");

        // The actual virtualization proof: nowhere near 251 delegates are
        // ever realized at once — bounded by the viewport plus cacheBuffer,
        // not by the row count.
        QList<QQuickItem *> closeButtons;
        collectByObjectName(m_window->contentItem(), QStringLiteral("taskCloseButton"), closeButtons);
        QVERIFY2(closeButtons.size() < 60,
                 qPrintable(QStringLiteral("expected a bounded number of realized rows, got %1 of %2 total")
                                .arg(closeButtons.size()).arg(memberCount + 1)));
        QVERIFY2(!anyDescendantTextContains(m_window->contentItem(), QStringLiteral("file250.mp3")),
                 "the last member should not already be realized before scrolling to it");

        // Flick to the bottom — the last member must become realized on
        // demand, proving the list actually pages through the full set
        // rather than just reporting a count it structurally can't reach.
        listView->setProperty("contentY", contentHeight - listView->height());
        QVERIFY2(QTest::qWaitFor([&] {
            return anyDescendantTextContains(m_window->contentItem(), QStringLiteral("file250.mp3"));
        }, 5000), "flicking to the bottom must realize the last member row");
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppSettings.qml's Settings{}
    // element is loaded transitively by BottomBar.qml, and must never touch
    // the real ~/.config/suopTranscriber (CLAUDE.md: never let testing touch
    // the user's real vault or app config).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_bottombar_tasklist"));
    QGuiApplication app(argc, argv);
    TstBottomBarTaskList tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_bottombar_tasklist.moc"
