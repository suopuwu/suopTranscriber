// Living spec + runtime verification for Sidebar.qml's Up/Down note
// navigation (moveNoteSelection()): drives a REAL QQuickView (offscreen,
// sandboxed config — see main() below) loading the actual Sidebar.qml
// against a real VaultTreeModel, with real QTest::mouseClick/keyClick
// events, rather than trusting a source-scan alone. tst_styleguide.cpp
// still carries the plain source-scan regression guards (no QQuickView
// there); this file is the one place that actually renders Sidebar.qml and
// exercises the click-to-focus + Shortcut + TreeView-scroll interplay,
// since none of that is guessable with confidence from source review or
// Qt headers alone (in particular: whether TreeView's own
// keyNavigationEnabled default steals Up/Down before the Shortcut items
// see them).
//
// No ydotool/wtype/xdotool on this machine (see suopTranscriber's own GUI-
// verification notes) — QTest's own synthetic event injection needs none
// of that; it delivers real QKeyEvent/QMouseEvent objects directly into the
// QQuickWindow's event pipeline, same mechanism Qt's own autotests use.

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

// Sidebar.qml declares `property TranscriptionController transcriptionController`
// but every real use site guards on `sidebar.transcriptionController &&`
// first (see Sidebar.qml's treeDelegate.bulkEligible etc.) — this harness
// never assigns one (stays null), so an empty stub is enough to satisfy the
// property's TYPE resolution without linking the real engine
// (whisper/llama/sherpa-onnx, unrelated to keyboard navigation).
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
};

} // namespace

class TstSidebarKeyNav : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // Finds a descendant QQuickItem by its C++ class name prefix (TreeView/
    // ListView instances declared directly in QML, with no further custom
    // properties, are plain QQuickTreeView/QQuickListView instances — no
    // synthetic QML subclass — but match by prefix regardless in case the
    // QML engine ever does generate one, since QQuickTreeView/QQuickListView
    // are Qt private-API classes this test deliberately never links against
    // or names as a C++ type).
    static QQuickItem *findByClassNamePrefix(QQuickItem *root, const QByteArray &prefix)
    {
        for (QQuickItem *child : root->findChildren<QQuickItem *>()) {
            if (QByteArray(child->metaObject()->className()).startsWith(prefix))
                return child;
        }
        return nullptr;
    }

    // Independent ground truth for "what TreeView row is this path at right
    // now" — reads the exact same roles moveNoteSelection() does, but via
    // the TreeView's own "model" property + QAbstractItemModel's public
    // invokable API, entirely separate from moveNoteSelection()'s own JS
    // loop, so this can't just be confirming the same logic against itself.
    static int rowForPath(QQuickItem *treeItem, const QString &path)
    {
        QObject *modelObj = treeItem->property("model").value<QObject *>();
        auto *model = qobject_cast<QAbstractItemModel *>(modelObj);
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
                                       Q_RETURN_ARG(QModelIndex, idx), Q_ARG(int, r), Q_ARG(int, 0));
            if (model->data(idx, pathRole).toString() == path)
                return r;
        }
        return -1;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        // Symlink (not copy) straight to the real checked-out qml/ sources
        // — this must reflect whatever's on disk right now, never a stale
        // build-time copy.
        QVERIFY2(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")),
                 "failed to symlink qml/ into the test's scratch QML module dir");

        // Hand-written, intentionally minimal — only the QML-defined types
        // Sidebar.qml transitively needs (Sidebar itself, plus its
        // AppSettings singleton/HoverBorder/CombineGroupOutline children).
        // Keep in sync with CMakeLists.txt's qt_add_qml_module list if
        // Sidebar.qml starts pulling in another QML-defined component.
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

        // VaultTreeModel/VaultSearchProxyModel are real app C++ classes
        // (linked into this test binary directly, see tests/CMakeLists.txt)
        // — registering them here is exactly what qt_add_qml_module's
        // generated init function does for the real app, just scoped to
        // only the two types this harness actually needs.
        // Sidebar.qml's AppIcon usages bind IconTheme.resolve(...); register
        // the singleton so they resolve instead of throwing at load.
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

        // Driver QML: mirrors Main.qml's real currentPath wiring (signal
        // activates a file -> currentPath fed back down) exactly in shape,
        // just without MainPanel's audio-player/orphaned-md plumbing in
        // between — so the outline/focus behavior under test matches
        // production's actual signal path, not a simplified stand-in.
        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import QtQuick.Controls\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    id: root\n"
            "    width: 320\n"
            "    height: 260\n"
            "    property string openPath: \"\"\n"
            "    VaultTreeModel { id: vaultModel; vaultPath: Qt.resolvedUrl(vaultUrl) }\n"
            "    TextField { objectName: \"decoy\"; width: 1; height: 1 }\n"
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

    // The actual claim under test: Up/Down move the open note by one, in
    // on-screen order, scrolling an offscreen destination into view — and
    // stop working the instant something outside the tree has keyboard
    // focus (the "clicking into MainPanel unfocuses" half of the ask).
    void arrowKeysNavigateAndScrollIntoView()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        // 24 notes in a 260px-tall view (~28px/row) comfortably overflows
        // the viewport, so later rows start offscreen — the whole point of
        // this test is confirming positionViewAtRow actually reveals them.
        for (int i = 0; i < 24; ++i)
            QVERIFY(touch(vault.filePath(QStringLiteral("note%1.mp3").arg(i, 2, 10, QLatin1Char('0')))));
        QVERIFY(touch(vault.filePath(QStringLiteral("zzz_orphan.md"))));

        QQuickView view;
        view.engine()->addImportPath(m_moduleDir.path());
        // AppIcon (used throughout Sidebar.qml) resolves svg packs through
        // this provider; without it the default material pack's icons can't
        // raster.
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

        // TreeView needs at least one layout pass before rows/itemAtCell
        // are populated.
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        // Click row 0 — establishes both currentPath (via the real
        // TapHandler -> fileActivated -> onFileActivated chain) and
        // keyboard focus (the new forceActiveFocus() call), exactly as a
        // real user click would.
        QQuickItem *row0 = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, row0), Q_ARG(QPoint, QPoint(0, 0)));
        QVERIFY2(row0, "itemAtCell(0,0) returned null — row 0 delegate not realized");
        const QPoint clickPos = row0->mapToScene(QPointF(row0->width() / 2, row0->height() / 2)).toPoint();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, clickPos);
        // TableView recomputes topRow/bottomRow (and does the actual
        // positionViewAtRow scroll) via a deferred polish pass, not
        // synchronously within the event that triggered it — pump the event
        // loop briefly so that's actually happened before reading them.
        QTest::qWait(20);

        QVERIFY2(treeItem->property("activeFocus").toBool(),
                 "clicking a tree row must grab keyboard focus (TapHandler's forceActiveFocus())");
        const QString firstPath = root->property("openPath").toString();
        QVERIFY2(!firstPath.isEmpty(), "clicking row 0 must activate a file/orphaned-md");
        QCOMPARE(rowForPath(treeItem, firstPath), 0);

        // Walk Down far enough to force the selection offscreen, checking
        // after EVERY press that the newly-open note is still within
        // [topRow, bottomRow] — i.e. genuinely scrolled into view, not just
        // logically selected.
        QString prevPath = firstPath;
        for (int i = 0; i < 15; ++i) {
            QTest::keyClick(&view, Qt::Key_Down);
            QTest::qWait(20);
            const QString newPath = root->property("openPath").toString();
            QVERIFY2(newPath != prevPath || i == 0, "Down must move to a different note each press "
                                                     "(until the last note is reached)");
            const int row = rowForPath(treeItem, newPath);
            QVERIFY2(row >= 0, "the newly-activated path must still resolve to a real TreeView row");
            const int topRow = treeItem->property("topRow").toInt();
            const int bottomRow = treeItem->property("bottomRow").toInt();
            QVERIFY2(row >= topRow && row <= bottomRow,
                     qPrintable(QStringLiteral("Down-selected row %1 not within visible range [%2,%3] "
                                                "at step %4 — scroll-into-view did not happen")
                                    .arg(row)
                                    .arg(topRow)
                                    .arg(bottomRow)
                                    .arg(i)));
            prevPath = newPath;
        }
        // Row 0 (the very first note) must have scrolled OUT of the top by
        // now — otherwise this whole test was accidentally exercising a
        // viewport tall enough to never need scrolling at all.
        const int topRowAfterWalk = treeItem->property("topRow").toInt();
        QVERIFY2(topRowAfterWalk > 0, "expected the walk to have scrolled row 0 out of view by now — "
                                      "test viewport/row-count assumption is stale");

        // Walk back Up the same number of steps as the Down walk above;
        // must land exactly on the row we started from (clamped correctly,
        // no drift), and scroll row 0 back into view.
        for (int i = 0; i < 15; ++i) {
            QTest::keyClick(&view, Qt::Key_Up);
            QTest::qWait(20);
        }
        QCOMPARE(root->property("openPath").toString(), firstPath);
        QCOMPARE(rowForPath(treeItem, firstPath), 0);
        QVERIFY2(treeItem->property("topRow").toInt() == 0,
                 "moving back to row 0 must scroll it back into view at the top");

        // Up past the first note must clamp, not wrap or error.
        QTest::keyClick(&view, Qt::Key_Up);
        QTest::qWait(20);
        QCOMPARE(root->property("openPath").toString(), firstPath);

        // Losing focus (e.g. clicking into MainPanel in the real app) must
        // stop Up/Down from doing anything — the crux of the "clicking
        // into the main panel should unfocus it" half of the ask.
        root->setFocus(true);
        QVERIFY(!treeItem->property("activeFocus").toBool());
        QTest::keyClick(&view, Qt::Key_Down);
        QTest::qWait(20);
        QCOMPARE(root->property("openPath").toString(), firstPath);
    }

    // Bug (live test): after a Ctrl+Shift+arrow group-boundary EXTEND, a
    // following Shift-click / Ctrl-click with the MOUSE must still build a
    // multi-selection. The user reported it silently stopped after using
    // Ctrl+Shift+arrow (keyboard Shift+arrow kept working). Drives real key +
    // real modifier-bearing mouse events end-to-end so the whole
    // Shortcut->selection-state->TapHandler->handleNoteClick chain is exercised.
    void ctrlShiftArrow_thenMouseModifierClicksStillSelect()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QStringList notePaths;
        for (int i = 0; i < 6; ++i) {
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
        QVERIFY2(view.status() == QQuickView::Ready, "driver.qml failed to load");
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QQuickItem *root = view.rootObject();
        QVERIFY(root);
        QQuickItem *sidebarItem = root->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        QVERIFY(sidebarItem);
        QQuickItem *treeItem = findByClassNamePrefix(sidebarItem, "QQuickTreeView");
        QVERIFY(treeItem);
        QTRY_VERIFY(treeItem->property("rows").toInt() >= 6);

        auto rowScenePos = [&](int row) -> QPoint {
            QQuickItem *item = nullptr;
            QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                      Q_RETURN_ARG(QQuickItem *, item), Q_ARG(QPoint, QPoint(0, row)));
            if (!item)
                return QPoint(-1, -1);
            return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        };
        auto selectedPaths = [&]() -> QStringList {
            QStringList out;
            for (const QVariant &v : sidebarItem->property("selectedPaths").toList())
                out << v.toString();
            return out;
        };

        // Focus the tree + select row 0 with a real click.
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, rowScenePos(0));
        QTest::qWait(20);
        QVERIFY(treeItem->property("activeFocus").toBool());
        QCOMPARE(selectedPaths().size(), 1);

        // Ctrl+Shift+Down: one flat group (all notes share the vault-root
        // parent), so this extends the selection to the last note — all 6.
        QTest::keyClick(&view, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
        QTest::qWait(20);
        QCOMPARE(selectedPaths().size(), 6);

        // THE BUG: a Shift-click on row 2 with the mouse must now shrink the
        // range to note0..note2 (anchor still note0). If the extend left the
        // anchor unusable, this silently collapses to a single selection.
        QTest::mouseClick(&view, Qt::LeftButton, Qt::ShiftModifier, rowScenePos(2));
        QTest::qWait(20);
        QCOMPARE(selectedPaths().size(), 3);

        // And a Ctrl-click on row 4 must toggle it into the selection.
        QTest::mouseClick(&view, Qt::LeftButton, Qt::ControlModifier, rowScenePos(4));
        QTest::qWait(20);
        QVERIFY2(selectedPaths().contains(notePaths[4]),
                 "Ctrl-click after a Ctrl+Shift+arrow extend must still toggle a row in");
        QCOMPARE(selectedPaths().size(), 4);
    }

    // Queue item 055: the Delete key deletes the sidebar selection whenever one
    // exists and no text field is being edited — NOT gated on tree focus, since
    // the selection stays highlighted after focus drifts to the main panel.
    // Asserted at the property level (deleteShortcutEnabled) rather than driving
    // a real Delete, which would trash into the user's real XDG trash.
    void deleteGateFollowsSelectionNotTreeFocus()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        for (int i = 0; i < 5; ++i)
            QVERIFY(touch(vault.filePath(QStringLiteral("note%1.mp3").arg(i))));

        QQuickView view;
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.rootContext()->setContextProperty(QStringLiteral("vaultUrl"),
                                                QUrl::fromLocalFile(vault.path()));
        view.setSource(QUrl::fromLocalFile(m_driverQmlPath));
        QCOMPARE(view.status(), QQuickView::Ready);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QQuickItem *root = view.rootObject();
        QVERIFY(root);
        QQuickItem *sidebarItem = root->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        QVERIFY(sidebarItem);
        QQuickItem *treeItem = findByClassNamePrefix(sidebarItem, "QQuickTreeView");
        QVERIFY(treeItem);
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        // Click row 0: selection of 1, tree focused — Delete enabled (baseline).
        QQuickItem *row0 = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, row0), Q_ARG(QPoint, QPoint(0, 0)));
        QVERIFY(row0);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                          row0->mapToScene(QPointF(row0->width() / 2, row0->height() / 2)).toPoint());
        QTest::qWait(20);
        QCOMPARE(sidebarItem->property("selectedPaths").toList().size(), 1);
        QVERIFY(treeItem->property("activeFocus").toBool());
        QVERIFY2(sidebarItem->property("deleteShortcutEnabled").toBool(),
                 "Delete must be enabled with a selection and the tree focused");

        // Focus leaves the tree (like clicking into MainPanel): selection stays,
        // no text field is focused — Delete MUST stay enabled (the fix).
        root->setFocus(true);
        QVERIFY(!treeItem->property("activeFocus").toBool());
        QVERIFY(!sidebarItem->property("textFieldHasFocus").toBool());
        QVERIFY2(sidebarItem->property("deleteShortcutEnabled").toBool(),
                 "Delete must stay enabled after focus leaves the tree — the selection is still shown");

        // Focus a text field: Delete must be suppressed (must edit text, not
        // trash the selection).
        QQuickItem *decoy = root->findChild<QQuickItem *>(QStringLiteral("decoy"));
        QVERIFY(decoy);
        decoy->forceActiveFocus();
        QTest::qWait(20);
        QVERIFY2(sidebarItem->property("textFieldHasFocus").toBool(),
                 "focusing a TextField must register as text-field focus");
        QVERIFY2(!sidebarItem->property("deleteShortcutEnabled").toBool(),
                 "Delete must be suppressed while a text field is being edited");
    }

    // Queue item 052: Escape clears a multi-selection (mirroring the cancel
    // button and empty-space taps) even after keyboard focus has left the
    // tree — NOT gated on tree/search focus, since the selection stays
    // highlighted after focus drifts to the main panel. Under the old
    // `noteShortcutsEnabled` gate this shortcut went dead the instant the
    // tree blurred; this pins that regression.
    void escapeClearsSelectionWithoutTreeFocus()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        for (int i = 0; i < 5; ++i)
            QVERIFY(touch(vault.filePath(QStringLiteral("note%1.mp3").arg(i))));

        QQuickView view;
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.rootContext()->setContextProperty(QStringLiteral("vaultUrl"),
                                                QUrl::fromLocalFile(vault.path()));
        view.setSource(QUrl::fromLocalFile(m_driverQmlPath));
        QCOMPARE(view.status(), QQuickView::Ready);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        QQuickItem *root = view.rootObject();
        QVERIFY(root);
        QQuickItem *sidebarItem = root->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        QVERIFY(sidebarItem);
        QQuickItem *treeItem = findByClassNamePrefix(sidebarItem, "QQuickTreeView");
        QVERIFY(treeItem);
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        auto selectedPaths = [&]() -> QStringList {
            QStringList out;
            for (const QVariant &v : sidebarItem->property("selectedPaths").toList())
                out << v.toString();
            return out;
        };

        // Click row 0 to focus the tree and open the first note, then
        // Ctrl+Shift+Down to build a multi-selection (same pattern as
        // ctrlShiftArrow_thenMouseModifierClicksStillSelect above).
        QQuickItem *row0 = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, row0), Q_ARG(QPoint, QPoint(0, 0)));
        QVERIFY(row0);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                          row0->mapToScene(QPointF(row0->width() / 2, row0->height() / 2)).toPoint());
        QTest::qWait(20);
        QVERIFY(treeItem->property("activeFocus").toBool());

        QTest::keyClick(&view, Qt::Key_Down, Qt::ControlModifier | Qt::ShiftModifier);
        QTest::qWait(20);
        QVERIFY2(selectedPaths().size() > 1, "Ctrl+Shift+Down must build a multi-selection");

        // Blur the tree (like clicking into MainPanel) — the selection stays
        // highlighted, but keyboard focus is gone.
        root->setFocus(true);
        QVERIFY(!treeItem->property("activeFocus").toBool());

        // Escape must still collapse the selection down to at most the open
        // file, exactly as the cancel button / empty-space click would.
        QTest::keyClick(&view, Qt::Key_Escape);
        QTRY_VERIFY2(selectedPaths().size() <= 1,
                     "Escape must clear a multi-selection even without tree focus");
    }

};

int main(int argc, char *argv[])
{
    // Offscreen: no real compositor/display needed (this machine has none
    // of ydotool/wtype/xdotool to drive a real window anyway, so a real
    // window would gain nothing). Must be set before QGuiApplication reads
    // it. Sandboxed org/app name so AppSettings.qml's QtCore Settings{}
    // element (loaded transitively by Sidebar.qml) never touches the real
    // app's ~/.config/suopTranscriber (CLAUDE.md: never let testing touch
    // the user's real vault or app config).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_keynav"));
    QGuiApplication app(argc, argv);
    TstSidebarKeyNav tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_keynav.moc"
