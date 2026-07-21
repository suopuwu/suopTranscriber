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
    // every visible note. Item 041: the ribbon now derives eligibility from
    // the MODEL (refreshSelectionEligibility() narrowing searchResultNotes()
    // to the selection), not from a delegate-fed map, so there is no
    // bulkTargetPaths() to call here anymore — the selection→eligibility
    // narrowing over unrealized rows is covered by
    // bulkEligibilityCoversUnrealizedRows() below. This test pins the
    // selection-population half only.
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

        // Nothing selected yet.
        QCOMPARE(sidebarItem->property("selectedPaths").toList().size(), 0);

        // Click selectAllButton.
        const QPointF selectAllCenter(selectAllButton->width() / 2, selectAllButton->height() / 2);
        const QPoint selectAllScenePos = selectAllButton->mapToScene(selectAllCenter).toPoint();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, selectAllScenePos);
        QTest::qWait(20);

        QCOMPARE(sidebarItem->property("selectedPaths").toList().size(), 5);

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

        // Ctrl+A must reproduce exactly what the button click did above.
        QTest::keyClick(&view, Qt::Key_A, Qt::ControlModifier);
        QTest::qWait(20);
        QCOMPARE(sidebarItem->property("selectedPaths").toList().size(), 5);
    }

    // Item 041 regression: the ribbon's eligibility must cover EVERY selected
    // row, not just the handful TreeView has realized. The old delegate-fed
    // maps only described realized rows, so a select-all in a vault taller than
    // the viewport left most eligible files invisible to the ribbon — buttons
    // sat dim. Eligibility is now model-derived, so it must reach unrealized
    // rows too. This harness's controller is null, so use the relabel/chapters
    // rules (pure status/isOrphanedMd), not selectionTranscribePaths.
    void bulkEligibilityCoversUnrealizedRows()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const int n = 60;
        for (int i = 0; i < n; ++i) {
            const QString base = vault.filePath(QStringLiteral("clip%1.mp3").arg(i, 3, 10, QLatin1Char('0')));
            QVERIFY(touch(base));
            // A sibling .md with valid frontmatter → VaultTreeModel reports
            // TranscribedStatus (== 2), which the relabel/chapters rules need.
            QVERIFY(touch(base + ".md",
                          "---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n\n[0] hello\n"));
        }

        QQuickView view;
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.rootContext()->setContextProperty(QStringLiteral("vaultUrl"),
                                                QUrl::fromLocalFile(vault.path()));
        view.setSource(QUrl::fromLocalFile(m_driverQmlPath));
        QVERIFY(view.status() == QQuickView::Ready);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QQuickItem *root = view.rootObject();
        QVERIFY(root);
        QQuickItem *sidebarItem = root->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        QVERIFY(sidebarItem);
        QQuickItem *treeItem = findByClassNamePrefix(sidebarItem, "QQuickTreeView");
        QVERIFY(treeItem);
        // All 60 rows exist in the model even though the 260px-high driver only
        // realizes ~10 delegates at a time — that gap is the whole point.
        QTRY_COMPARE(treeItem->property("rows").toInt(), n);

        QQuickItem *selectAllButton = sidebarItem->findChild<QQuickItem *>(QStringLiteral("selectAllButton"));
        QVERIFY(selectAllButton);
        const QPoint selectAllScenePos = selectAllButton->mapToScene(
            QPointF(selectAllButton->width() / 2, selectAllButton->height() / 2)).toPoint();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, selectAllScenePos);

        QTRY_COMPARE(sidebarItem->property("selectedPaths").toList().size(), n);
        // The regression: every transcribed row is eligible, including the ~50
        // never realized. Under the old delegate-fed maps this was ~10.
        QTRY_COMPARE(sidebarItem->property("selectionRelabelPaths").toList().size(), n);
        QTRY_COMPARE(sidebarItem->property("selectionChaptersPaths").toList().size(), n);
    }

    // Bug (live test #3): a file restored by undoing its delete was NOT a true
    // selection — the restore rebuild()'s modelReset wiped the selection, and
    // MainPanel reopening the file only gave it the currentPath "open" OUTLINE,
    // which looks selected but is not a Shift-click anchor. So a Shift-click
    // from the restored file couldn't start a range. selectRestoredNote() (wired
    // to VaultTreeModel's fileRestored/transcriptRestored/audioRestoredForTranscript
    // — see the target: sidebar.model Connections) fixes it by making the
    // restored path a genuine selectedPaths member AND the lastClickedPath
    // anchor. This drives that end-to-end: reselect, then Shift-click another
    // row and assert a real RANGE results.
    void restoredNote_becomesShiftClickAnchor()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QStringList notePaths;
        for (int i = 0; i < 3; ++i) {
            const QString path = vault.filePath(QStringLiteral("note%1.mp3").arg(i));
            QVERIFY(touch(path));
            notePaths << path;
        }

        QQuickView view;
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.rootContext()->setContextProperty(QStringLiteral("vaultUrl"), QUrl::fromLocalFile(vault.path()));
        view.setSource(QUrl::fromLocalFile(m_driverQmlPath));
        QVERIFY2(view.status() == QQuickView::Ready, "driver.qml failed to load");
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QQuickItem *root = view.rootObject();
        QVERIFY(root);
        QQuickItem *sidebarItem = root->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        QVERIFY(sidebarItem);
        QQuickItem *treeItem = findByClassNamePrefix(sidebarItem, "QQuickTreeView");
        QVERIFY(treeItem);
        QTRY_VERIFY(treeItem->property("rows").toInt() >= 3);

        // Simulate what a restore does to the Sidebar: modelReset cleared the
        // selection, then the restore signal calls selectRestoredNote() onto the
        // brought-back file. (Calling it directly rather than round-tripping a
        // real system-trash delete keeps this deterministic; the wiring to the
        // model's restore signals is guarded separately in tst_styleguide.)
        QMetaObject::invokeMethod(sidebarItem, "selectRestoredNote", Q_ARG(QVariant, notePaths[1]));

        // It must now be a REAL selection + the Shift anchor — not just an outline.
        const QVariantList afterRestore = sidebarItem->property("selectedPaths").toList();
        QCOMPARE(afterRestore.size(), 1);
        QCOMPARE(afterRestore.first().toString(), notePaths[1]);
        QCOMPARE(sidebarItem->property("lastClickedPath").toString(), notePaths[1]);

        // Shift-click note0 (via handleNoteClick, the single entry point both
        // TapHandlers funnel into) — with a working anchor this yields the
        // contiguous range note0..note1, the thing the bug made impossible.
        QMetaObject::invokeMethod(sidebarItem, "handleNoteClick", Q_ARG(QVariant, notePaths[0]),
                                  Q_ARG(QVariant, false), Q_ARG(QVariant, int(Qt::ShiftModifier)));
        const QVariantList afterShift = sidebarItem->property("selectedPaths").toList();
        QStringList selected;
        for (const QVariant &v : afterShift)
            selected << v.toString();
        QCOMPARE(selected.size(), 2);
        QVERIFY2(selected.contains(notePaths[0]) && selected.contains(notePaths[1]),
                 "a Shift-click from the restored note must select the note0..note1 range");
    }

    // Deselect (Escape / a click on empty sidebar space / the deselect button)
    // collapses a multi-selection back to just the currently-open file — it
    // stays selected, everything else clears. Drives all three entry points
    // against a real view + real events, since each depends on runtime focus/
    // hit-testing a source scan can't verify.
    void deselectCollapsesToCurrentFile()
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
        // A taller-than-default window so the 5 short rows leave real dead
        // space below them for the empty-space click (step 3). The other tests
        // keep driver.qml's own 320x260; this one grows the root to the view.
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.rootContext()->setContextProperty(QStringLiteral("vaultUrl"),
                                                QUrl::fromLocalFile(vault.path()));
        view.setSource(QUrl::fromLocalFile(m_driverQmlPath));
        QVERIFY2(view.status() == QQuickView::Ready, "driver.qml failed to load");
        view.resize(360, 460);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QQuickItem *root = view.rootObject();
        QVERIFY(root);
        QQuickItem *sidebarItem = root->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        QVERIFY(sidebarItem);
        QQuickItem *treeItem = findByClassNamePrefix(sidebarItem, "QQuickTreeView");
        QVERIFY(treeItem);
        QTRY_VERIFY(treeItem->property("rows").toInt() >= 5);

        // Open row 0 with a real click: opens the file (currentPath == note0)
        // AND grabs tree focus, which Escape's Shortcut is gated on.
        QQuickItem *row0 = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                  Q_RETURN_ARG(QQuickItem *, row0), Q_ARG(QPoint, QPoint(0, 0)));
        QVERIFY2(row0, "itemAtCell(0,0) returned null — row 0 delegate not realized");
        const QPoint row0ScenePos = row0->mapToScene(QPointF(row0->width() / 2, row0->height() / 2)).toPoint();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, row0ScenePos);
        QTest::qWait(20);
        QVERIFY(treeItem->property("activeFocus").toBool());
        QCOMPARE(sidebarItem->property("currentPath").toString(), notePaths[0]);

        auto buildMultiSelection = [&]() {
            // Ctrl-add note1 and note2 without changing the open file, so the
            // "keep the current file" behavior is actually observable.
            QMetaObject::invokeMethod(sidebarItem, "handleNoteClick", Q_ARG(QVariant, notePaths[1]),
                                      Q_ARG(QVariant, false), Q_ARG(QVariant, int(Qt::ControlModifier)));
            QMetaObject::invokeMethod(sidebarItem, "handleNoteClick", Q_ARG(QVariant, notePaths[2]),
                                      Q_ARG(QVariant, false), Q_ARG(QVariant, int(Qt::ControlModifier)));
            QCOMPARE(sidebarItem->property("selectedPaths").toList().size(), 3);
        };
        auto assertCollapsedToNote0 = [&](const char *step) {
            const QVariantList after = sidebarItem->property("selectedPaths").toList();
            QStringList got;
            for (const QVariant &v : after)
                got << v.toString();
            QVERIFY2(after.size() == 1 && after.first().toString() == notePaths[0],
                     qPrintable(QStringLiteral("%1: expected selection collapsed to note0, got [%2]")
                                    .arg(QString::fromLatin1(step), got.join(QStringLiteral(", ")))));
            QCOMPARE(sidebarItem->property("lastClickedPath").toString(), notePaths[0]);
            // The open file is untouched — deselect only clears selection.
            QCOMPARE(sidebarItem->property("currentPath").toString(), notePaths[0]);
        };

        // 1) Escape.
        buildMultiSelection();
        QTest::keyClick(&view, Qt::Key_Escape, Qt::NoModifier);
        QTest::qWait(20);
        assertCollapsedToNote0("escape");

        // 2) The explicit deselect button beside the "N selected" indicator.
        buildMultiSelection();
        QQuickItem *deselectButton = sidebarItem->findChild<QQuickItem *>(QStringLiteral("deselectButton"));
        QVERIFY2(deselectButton, "ribbon must expose an id: deselectButton beside the N-selected label");
        QVERIFY2(deselectButton->isVisible(), "deselectButton must be visible while 2+ rows are selected");
        QQuickItem *selectAllButton = sidebarItem->findChild<QQuickItem *>(QStringLiteral("selectAllButton"));
        QVERIFY(selectAllButton);
        // The deselect button only just became visible (selection went 1→3 this
        // tick); wait for the ribbon Flow to actually lay it out, else its
        // stale position still coincides with selectAllButton at the Flow
        // origin and the click would hit select-all instead.
        const QPointF selectAllCenter = selectAllButton->mapToScene(
            QPointF(selectAllButton->width() / 2, selectAllButton->height() / 2));
        auto deselectCenter = [&]() {
            return deselectButton->mapToScene(
                QPointF(deselectButton->width() / 2, deselectButton->height() / 2));
        };
        QTRY_VERIFY((deselectCenter() - selectAllCenter).manhattanLength() > 4);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, deselectCenter().toPoint());
        QTest::qWait(20);
        assertCollapsedToNote0("deselect-button");

        // 3) A left-click on empty tree space below the last row. With 5 short
        // rows in a tall view there's dead space at the bottom; clicking it
        // must NOT be read as a row (cellAtPosition bail) and must deselect.
        buildMultiSelection();
        const QPointF treeBottom(treeItem->width() / 2, treeItem->height() - 6);
        // Guard: the chosen point is genuinely below every row (its content-y
        // exceeds the laid-out contentHeight), so the handler's cellAtPosition
        // bail can't fire and this is a true empty-space click.
        const qreal contentHeight = treeItem->property("contentHeight").toReal();
        const qreal clickContentY = treeItem->property("contentY").toReal() + treeBottom.y();
        QVERIFY2(clickContentY > contentHeight,
                 "test setup: chosen empty-space point unexpectedly landed within the rows");
        const QPoint emptyScenePos = treeItem->mapToScene(treeBottom).toPoint();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, emptyScenePos);
        QTest::qWait(20);
        assertCollapsedToNote0("empty-space-click");

        // 4) A left-click on empty RIBBON space to the RIGHT of the "N selected"
        // indicator / deselect button (user report: that gap didn't deselect).
        // ribbonEmptyLeftTap uses ribbonFlow.childAt() to bail on real controls;
        // find a point past the deselect button's right edge where childAt is
        // null (genuinely empty Flow space) and click it.
        buildMultiSelection();
        QQuickItem *ribbonFlow = deselectButton->parentItem();
        QVERIFY2(ribbonFlow, "deselectButton must live directly inside ribbonFlow");
        auto childAtFlow = [&](qreal x, qreal y) -> QQuickItem * {
            QQuickItem *hit = nullptr;
            QMetaObject::invokeMethod(ribbonFlow, "childAt", Qt::DirectConnection,
                                      Q_RETURN_ARG(QQuickItem *, hit),
                                      Q_ARG(qreal, x), Q_ARG(qreal, y));
            return hit;
        };
        const qreal rowY = deselectButton->y() + deselectButton->height() / 2;
        qreal emptyX = -1;
        for (qreal x = deselectButton->x() + deselectButton->width() + 2; x < ribbonFlow->width(); x += 2) {
            if (childAtFlow(x, rowY) == nullptr) { emptyX = x; break; }
        }
        QVERIFY2(emptyX > 0, "no empty ribbon space found to the right of the deselect button");
        // Sanity: the deselect button itself is NOT empty (childAt finds it),
        // proving the bail actually discriminates control vs. gap.
        QVERIFY(childAtFlow(deselectButton->x() + deselectButton->width() / 2, rowY) != nullptr);
        const QPoint ribbonEmptyScenePos = ribbonFlow->mapToScene(QPointF(emptyX, rowY)).toPoint();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, ribbonEmptyScenePos);
        QTest::qWait(20);
        assertCollapsedToNote0("ribbon-empty-space-click");
    }

    // Regression (real-vault bug): once the tree is SCROLLED DOWN, a Shift-click
    // (or Ctrl-click) on a real row was silently wiped back to just the open
    // file. Cause: treeEmptyLeftTap — the "click empty space to deselect"
    // handler — shares the click with the row's own treeRowTap (nested
    // TapHandlers aren't mutually exclusive), and its cellAtPosition() row-bail
    // mapped the tap into contentItem coordinates first, double-applying
    // contentY. Harmless at the top (contentY 0), but once scrolled every real
    // row hit-tested as -1 (empty space), so the row click fell through to
    // deselectExceptCurrent() and collapsed the selection the row tap had just
    // built. cellAtPosition() actually wants the tap in the view's own
    // coordinates (it accounts for the scroll itself) — i.e. point.position as
    // is. This drives a genuinely-scrolled TreeView and asserts a Shift-click
    // range on a low row SURVIVES rather than resetting to the single open file.
    // A plain source scan can't catch this — it's a runtime hit-test at a
    // non-zero contentY — so it lives here against a real QQuickView.
    void scrolledRowShiftClick_survivesEmptySpaceHandler()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QStringList notePaths;
        for (int i = 0; i < 40; ++i) {
            const QString path = vault.filePath(QStringLiteral("note%1.mp3").arg(i, 2, 10, QLatin1Char('0')));
            QVERIFY(touch(path));
            notePaths << path;
        }

        QQuickView view;
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.rootContext()->setContextProperty(QStringLiteral("vaultUrl"),
                                                QUrl::fromLocalFile(vault.path()));
        view.setSource(QUrl::fromLocalFile(m_driverQmlPath));
        QVERIFY2(view.status() == QQuickView::Ready, "driver.qml failed to load");
        view.resize(360, 420);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QQuickItem *root = view.rootObject();
        QVERIFY(root);
        QQuickItem *sidebarItem = root->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        QVERIFY(sidebarItem);
        QQuickItem *treeItem = findByClassNamePrefix(sidebarItem, "QQuickTreeView");
        QVERIFY(treeItem);
        QTRY_VERIFY(treeItem->property("rows").toInt() >= 40);

        auto rowScenePos = [&](int row) -> QPoint {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                      Q_RETURN_ARG(QQuickItem *, item), Q_ARG(QPoint, QPoint(0, row)));
            if (!item)
                return QPoint(-1, -1);
            return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        };

        // Open row 0 with a real click: sets currentPath + the Shift anchor +
        // keyboard focus, exactly as a user's first click would.
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, rowScenePos(0));
        QTest::qWait(20);
        QCOMPARE(sidebarItem->property("currentPath").toString(), notePaths[0]);
        QCOMPARE(sidebarItem->property("selectedPaths").toList().size(), 1);

        // Scroll to the bottom so contentY is genuinely non-zero — the exact
        // condition that broke the double-mapped cellAtPosition hit-test.
        const qreal contentH = treeItem->property("contentHeight").toReal();
        const qreal viewH = treeItem->property("height").toReal();
        treeItem->setProperty("contentY", contentH - viewH);
        QTRY_VERIFY2(treeItem->property("contentY").toReal() > 50,
                     "test setup: the tree did not actually scroll down");
        QTest::qWait(30); // let TableView re-realize the now-visible bottom rows

        // Pick a definitely-visible low row (well past the anchor at row 0) and
        // Shift-click it. With the anchor still note0 this is a large contiguous
        // range; the bug collapsed it to just the open file (length 1).
        const int topRow = treeItem->property("topRow").toInt();
        const int bottomRow = treeItem->property("bottomRow").toInt();
        QVERIFY2(topRow > 0, "expected the scroll to have pushed row 0 out of view");
        const int targetRow = qMax(topRow + 1, bottomRow - 1);
        const QPoint targetPos = rowScenePos(targetRow);
        QVERIFY2(targetPos.x() >= 0, "target row delegate not realized after scroll");
        QTest::mouseClick(&view, Qt::LeftButton, Qt::ShiftModifier, targetPos);
        QTest::qWait(20);

        const int selCount = sidebarItem->property("selectedPaths").toList().size();
        QVERIFY2(selCount > 1,
                 qPrintable(QStringLiteral("a Shift-click on a real row while the tree is scrolled must keep the "
                                            "range — the empty-space deselect handler must NOT fire on a real row. "
                                            "Got selCount=%1 (1 == wrongly collapsed to the open file)").arg(selCount)));
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
