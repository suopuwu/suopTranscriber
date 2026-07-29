// Living spec + runtime verification for Sidebar.qml's inline rename and its
// multi-select delete: drives a REAL QQuickView (offscreen, sandboxed config —
// see main() below) loading the actual Sidebar.qml against a real
// VaultTreeModel, clicking real rows with real QTest mouse events and typing
// with real key events, then checking the actual filesystem for the result.
//
// Same harness shape as tst_sidebar_dragmove.cpp / tst_sidebar_keynav.cpp
// (read either's top-of-file comment first) and for the same reason: none of
// what this covers is answerable from source review. Specifically —
//   - whether a click on an already-selected row's NAME opens the editor,
//     while the same click on a not-yet-selected row still just opens the file
//     (one TapHandler decides between two outcomes; both halves are gestures),
//   - whether the deferred (Qt.callLater) rename commit survives the model
//     rebuild that destroys the very delegate whose handler started it,
//   - whether Escape abandons an edit without writing it, and
//   - whether deleteFiles() — a QList<QUrl> invokable handed a JS array of
//     "file://"-prefixed strings — actually converts and deletes, rather than
//     silently no-op'ing on a failed sequence conversion.
// tst_styleguide.cpp keeps the cheap source-scan guards over the same feature;
// this file is the one that proves the gesture RENAMES FILES.
//
// A green cmake build does NOT prove any of this: qmlcachegen/AOT happily
// compiles QML that still fails at load, so openVault() reports view->errors()
// verbatim rather than as a bare null-item failure.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QUrl>

#include "IconTheme.h"
#include "TreeOrdering.h"
#include "VaultSearchProxyModel.h"
#include "VaultTreeModel.h"

namespace {

QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

bool touch(const QString &path, const QByteArray &content = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content);
    return true;
}

// Sidebar.qml guards every transcriptionController use site on it being
// non-null, so most tests here assign none and this only needs its TYPE to
// resolve (see tst_sidebar_dragmove.cpp's identical stub).
//
// The label-rename test is the exception: a row shows — and therefore renames —
// its stored LABEL only when the controller hands one over, so that test needs
// a controller that actually answers. Hence the real implementations below.
// They're minimal by design: readMetadata()/setLabel() are the two the gesture
// runs through, and the rest exist only because Sidebar.qml's load-time
// bindings call them the moment this stops being null (bulkEligible ->
// matchesExistingTranscription, refreshJobStatus -> jobStatusForPath,
// refreshTreeOrdering -> detectCombineGroups) and an unresolved call would
// break those bindings rather than fail visibly. The two signals are here for
// the same reason: Sidebar's Connections block declares handlers for them, and
// Qt errors on a handler with no matching signal.
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
public:
    // What readMetadata() reports as this vault's label, for every path.
    QString label;
    // What setLabel() was last asked to write — the assertion target.
    QUrl setLabelUrl;
    QString setLabelValue;
    int setLabelCalls = 0;

    Q_INVOKABLE QVariantMap readMetadata(const QUrl &) const
    {
        return QVariantMap{{QStringLiteral("label"), label}};
    }
    Q_INVOKABLE bool setLabel(const QUrl &fileUrl, const QString &newLabel)
    {
        setLabelUrl = fileUrl;
        setLabelValue = newLabel;
        ++setLabelCalls;
        return true;
    }
    Q_INVOKABLE bool matchesExistingTranscription(const QUrl &, const QString &) const
    {
        return false;
    }
    Q_INVOKABLE int jobStatusForPath(const QString &) const { return -1; }
    Q_INVOKABLE QVariantList detectCombineGroups(const QVariantList &, const QStringList &) const
    {
        return {};
    }
    Q_INVOKABLE QStringList existingVaultTags(const QUrl &) const { return {}; }

    // The pending-rename store the Sidebar's rowNameText now reads.
    QHash<QString, QString> pendingRenames; // "file://"+localpath -> stem
    int pendingRenamesRev = 0;
    Q_PROPERTY(int pendingRenamesRevision READ pendingRenamesRevision NOTIFY pendingRenamesRevisionChanged)
    int pendingRenamesRevision() const { return pendingRenamesRev; }
    Q_INVOKABLE QString pendingRenameStem(const QUrl &fileUrl) const
    {
        return pendingRenames.value(fileUrl.toString());
    }
    Q_INVOKABLE void setPendingRename(const QUrl &fileUrl, const QString &stem)
    {
        pendingRenames.insert(fileUrl.toString(), stem);
        ++pendingRenamesRev;
        emit pendingRenamesRevisionChanged();
    }

signals:
    void finished(const QUrl &fileUrl, bool ok, const QString &error);
    void jobStatusesChanged();
    void pendingRenamesRevisionChanged();
};

} // namespace

class TstSidebarRename : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;
    // Assigned into the Sidebar by openVault(), and NULL for every test that
    // doesn't set it — which is the point. Sidebar.qml behaves differently the
    // moment a controller exists (rows start showing labels), so tests about
    // the plain filename gesture must keep seeing no controller at all.
    TranscriptionControllerStub *m_controller = nullptr;

    static QQuickItem *findByClassNamePrefix(QQuickItem *root, const QByteArray &prefix)
    {
        for (QQuickItem *child : root->findChildren<QQuickItem *>()) {
            if (QByteArray(child->metaObject()->className()).startsWith(prefix))
                return child;
        }
        return nullptr;
    }

    // Independent ground truth for "which visible row is this path at" — reads
    // the model's own roles through QAbstractItemModel's public API rather than
    // reusing any of Sidebar.qml's JS, so a passing test can't just be the
    // feature agreeing with itself. (Same helper as tst_sidebar_dragmove.cpp.)
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

    // See tst_sidebar_dragmove.cpp's identical helper: this harness hands
    // VaultTreeModel its vaultPath at CONSTRUCTION, so the Sidebar's
    // auto-expand Connections can miss that first reset. Expand explicitly
    // rather than depend on the race.
    static void expandEverything(QQuickItem *treeItem)
    {
        QMetaObject::invokeMethod(treeItem, "expandRecursively", Qt::DirectConnection,
                                   Q_ARG(int, -1), Q_ARG(int, -1));
        QTest::qWait(50);
    }

    static QQuickItem *rowItem(QQuickItem *treeItem, int row)
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, item), Q_ARG(QPoint, QPoint(0, row)));
        return item;
    }

    // A point ON the row's painted name text. Deliberately NOT the row centre:
    // the whole point of the gesture is that it's the NAME that's clickable-to-
    // rename, and the name Text fills the row's remaining width while painting
    // only its first implicitWidth pixels. Aims a few px into the glyphs.
    static QPoint nameTextPoint(QQuickItem *row)
    {
        QQuickItem *nameText = row->findChild<QQuickItem *>(QStringLiteral("rowNameText"));
        if (!nameText)
            return {};
        const qreal w = qMin(nameText->width(), nameText->property("implicitWidth").toReal());
        return nameText->mapToScene(QPointF(qMax(qreal(2), w / 2), nameText->height() / 2)).toPoint();
    }

    // A point on the folder row's expand/collapse glyph — the ONLY part of a
    // dir row that toggles it now that the name means "rename".
    static QPoint expandGlyphPoint(QQuickItem *row)
    {
        QQuickItem *glyph = row->findChild<QQuickItem *>(QStringLiteral("expandGlyph"));
        if (!glyph)
            return {};
        return glyph->mapToScene(QPointF(glyph->width() / 2, glyph->height() / 2)).toPoint();
    }

    // A point on the row but past the end of its painted name — the row's empty
    // right-hand space. The row's hitbox is far wider than a short name, so this
    // now begins the rename just as a click on the letters does (see
    // Sidebar.qml's tapIsOnRowName). It first toggled the folder, then was made
    // inert; it renames now.
    static QPoint pastNameTextPoint(QQuickItem *row)
    {
        QQuickItem *nameText = row->findChild<QQuickItem *>(QStringLiteral("rowNameText"));
        if (!nameText)
            return {};
        const qreal painted = qMin(nameText->width(), nameText->property("implicitWidth").toReal());
        return nameText->mapToScene(QPointF(painted + 20, nameText->height() / 2)).toPoint();
    }

    static QQuickItem *renameFieldFor(QQuickItem *row)
    {
        return row->findChild<QQuickItem *>(QStringLiteral("rowRenameField"));
    }

    // The editor is only "open" on a row when it's actually visible there —
    // every row owns a rowRenameField instance, hidden until that row is the
    // one being renamed (see Sidebar.qml's renamingPath).
    static bool renameFieldVisibleOn(QQuickItem *row)
    {
        QQuickItem *field = renameFieldFor(row);
        return field && field->isVisible();
    }

    static void clickAt(QQuickView *view, const QPoint &p, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QTest::mouseClick(view, Qt::LeftButton, mods, p);
        QTest::qWait(60);
    }

    // QTest::keyClicks() is a QWidget-only API — the QWindow overloads only
    // cover a single key, so type character by character into whatever has
    // focus. To REPLACE the whole name (rather than insert at the click caret),
    // callers select the field first (invokeMethod "selectAll") — a name-click
    // rename now seeds a plain caret, not a selected stem.
    static void typeText(QQuickView *view, const QString &text)
    {
        for (const QChar &c : text)
            QTest::keyClick(view, c.toLatin1());
    }

    QQuickItem *openVault(QQuickView *view, const QString &vaultPath, QQuickItem **sidebarOut = nullptr)
    {
        view->engine()->addImportPath(m_moduleDir.path());
        view->engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                          new IconImageProvider);
        view->rootContext()->setContextProperty(QStringLiteral("vaultUrl"),
                                                 QUrl::fromLocalFile(vaultPath));
        view->rootContext()->setContextProperty(
            QStringLiteral("controllerStub"), static_cast<QObject *>(m_controller));
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : view->errors())
                msg += e.toString() + QStringLiteral("\n");
            // The exact failure mode a green cmake build does NOT catch
            // (AOT/qmlcachegen can compile a file that still fails to load) —
            // report it verbatim rather than as "null item".
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
    // The stub is stack-allocated inside the one test that wants it, so it must
    // not stay pointed-at once that test's frame is gone.
    void cleanup() { m_controller = nullptr; }

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
                      "singleton Notifications 1.0 qml/Notifications.qml\n"
                      "HoverBorder 1.0 qml/HoverBorder.qml\n"
                      "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
                      "UnderlineField 1.0 qml/UnderlineField.qml\n"
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
            "        transcriptionController: controllerStub\n"
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

    // The headline claim: click a note once to select/open it, click its NAME
    // again, type, press Return — the file is renamed on disk, and its
    // transcript sidecar follows it (VaultTreeModel::renameFile's contract).
    //
    // This also proves the deferred commit survives: renameFile() rebuilds the
    // model, destroying the delegate whose TextField handler is mid-execution.
    void clickNameOfSelectedNote_rename_renamesFileAndSidecar()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3.md")), "---\nmodel: x\n---\n[0] hi\n"));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY2(row >= 0, "talk.mp3 must be a visible row");
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        // First click selects + opens; the editor must NOT open yet.
        clickAt(&view, nameTextPoint(item));
        QVERIFY2(!renameFieldVisibleOn(item),
                 "the FIRST click on an unselected row must open the file, not start a rename");

        // Second click, on the name of the now-selected row -> inline edit.
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY2(renameFieldVisibleOn(item),
                     "clicking the name of an already-selected row must open the inline rename editor");
        QQuickItem *field = renameFieldFor(item);
        QVERIFY(field);
        QTRY_VERIFY2(field->hasActiveFocus(), "the rename editor must take focus so the user can just type");
        // The editor now seeds what the ROW shows — the full
        // name+extension when extensions are visible (default) — with the stem
        // pre-selected, and strips the extension back to a stem on commit
        // (renameFile re-appends the real extension). Typing over the selected
        // stem replaces it and keeps ".mp3", so the final path is unchanged.
        QCOMPARE(field->property("text").toString(), QStringLiteral("talk.mp3"));

        // A name-click now drops a plain caret (no stem selection), so select
        // all before typing to replace the whole name.
        QMetaObject::invokeMethod(renameFieldFor(item), "selectAll");
        typeText(&view, QStringLiteral("interview"));
        QTest::keyClick(&view, Qt::Key_Return);
        QTest::qWait(150); // let the Qt.callLater commit + model rebuild run

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("interview.mp3"))),
                 "the audio file must be renamed on disk");
        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("interview.mp3.md"))),
                 "the transcript sidecar must follow the audio's new name");
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3.md"))));
    }

    // Regression: renaming the OPEN file "took 3-4 clicks". A
    // background model rebuild (vault watcher firing after the open, a
    // transcription finishing, …) runs Sidebar's onModelReset, which clears
    // selectedPaths but leaves currentPath. Before the fix, that reset landing
    // between the select-click and the name-click destroyed the sole-selection
    // precondition, so the next name-click only re-selected instead of renaming.
    // This drives that exact sequence — click to select+open, force a model
    // rebuild, then a SINGLE name-click must open the editor because the row is
    // still the open file (currentPath) even though the selection was wiped.
    void renameOfOpenFileSurvivesBackgroundModelReset()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const QString audioPath = vault.filePath(QStringLiteral("talk.mp3"));
        int row = rowForPath(treeItem, audioPath);
        QVERIFY2(row >= 0, "talk.mp3 must be a visible row");
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        // Click once: selects + opens the file (sets currentPath via onFileActivated).
        clickAt(&view, nameTextPoint(item));
        QVERIFY2(!renameFieldVisibleOn(item),
                 "the first click on an unselected row opens the file, not a rename");

        QObject *sidebarItem = view.rootObject()->findChild<QObject *>(QStringLiteral("sidebar"));
        QVERIFY(sidebarItem);
        QTRY_COMPARE(sidebarItem->property("currentPath").toString(), audioPath);

        // Reproduce the background structure-change rebuild that clears the
        // selection while a file stays open — e.g. an import / USB-grab / combine
        // landing another file in the vault while you work. rebuild() only emits
        // a real modelReset when the file set actually changed (an unchanged
        // rescan early-returns with content-only refreshes), so we add a sibling
        // file THEN refresh: onModelReset then clears selectedPaths but leaves
        // currentPath (the still-open file) intact.
        QVERIFY(touch(vault.filePath(QStringLiteral("other.mp3"))));
        QObject *vaultModel = view.rootObject()->findChild<QObject *>(QStringLiteral("vaultModel"));
        QVERIFY(vaultModel);
        QMetaObject::invokeMethod(vaultModel, "refresh", Qt::DirectConnection);
        QTRY_COMPARE(sidebarItem->property("selectedPaths").toList().size(), 0);
        QCOMPARE(sidebarItem->property("currentPath").toString(), audioPath);

        // The rebuild recreated every delegate; re-locate the row + item.
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);
        row = rowForPath(treeItem, audioPath);
        QVERIFY2(row >= 0, "talk.mp3 must still be a visible row after the rebuild");
        item = rowItem(treeItem, row);
        QVERIFY(item);

        // A SINGLE name-click on the still-open file must open the editor — no
        // extra re-select click, because currentPath outlived the reset.
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY2(renameFieldVisibleOn(item),
                     "renaming the open file must not require re-selecting it after a background model reset");
    }

    // The rename editor shows the FULL name+extension, but places a plain caret
    // where the user clicked rather than pre-selecting the stem — typing extends
    // the name (nothing is replaced), and there is no selection. It must also sit
    // flush with the rowNameText it replaces (leftPadding 0), not shifted right
    // by QQC2's default text-field padding.
    void renameSeed_showsExtension_placesCaretNoSelection_andIsLeftAligned()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);
        // Two clicks: select, then click the middle of the name to open the editor.
        clickAt(&view, nameTextPoint(item));
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY2(renameFieldVisibleOn(item), "the inline rename editor must open");
        QQuickItem *field = renameFieldFor(item);
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());

        // Full name shown, extension included.
        QCOMPARE(field->property("text").toString(), QStringLiteral("talk.mp3"));
        // No pre-selection — a plain caret, not a selected stem.
        QCOMPARE(field->property("selectedText").toString(), QString());
        // The caret landed inside the name near where the click fell (the middle
        // of the painted glyphs), not pinned to an end. Robust against exact
        // font metrics: it's simply somewhere strictly interior.
        const int caret = field->property("cursorPosition").toInt();
        QVERIFY2(caret > 0 && caret < QStringLiteral("talk.mp3").size(),
                 "a click in the middle of the name must drop the caret there, not at an end");
        // Flush with the row text it replaces — no QQC2 leftPadding shift.
        QCOMPARE(field->property("leftPadding").toInt(), 0);
    }

    // Clicking in the empty space to the RIGHT of the (short) name drops the
    // caret at the very end — positionAt clamps a past-the-text click to length.
    void renameCaret_clickPastNameGoesToEnd()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);
        clickAt(&view, nameTextPoint(item));     // select
        clickAt(&view, pastNameTextPoint(item)); // open, clicking past the name
        QTRY_VERIFY2(renameFieldVisibleOn(item), "the inline rename editor must open");
        QQuickItem *field = renameFieldFor(item);
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());
        QCOMPARE(field->property("selectedText").toString(), QString());
        QTRY_COMPARE(field->property("cursorPosition").toInt(), int(QStringLiteral("talk.mp3").size()));
    }

    // Source-scan guard for a fix with no QQuickView path here: the inline
    // rename editor was being closed the instant it opened because Main.qml's
    // window-spanning "click off a text field to blur it" TapHandler fired on the
    // very click that opened the editor, stealing its focus (its blur-commit then
    // closed it) — so renaming took several clicks. That handler lives in
    // Main.qml, which this Sidebar-only harness never loads, so guard the fix at
    // the source: the window blur must NOT fire while a sidebar rename is open,
    // i.e. its forceActiveFocus() is gated on sidebar.renamingPath.
    void windowClickBlur_isGatedOnActiveRename_sourceGuard()
    {
        QFile f(QStringLiteral(SUOP_QML_DIR) + QStringLiteral("/Main.qml"));
        QVERIFY2(f.open(QIODevice::ReadOnly | QIODevice::Text), "cannot read Main.qml");
        const QString src = QString::fromUtf8(f.readAll());

        // Strip // line comments so the doc comment that names renamingPath can't
        // satisfy the guard on its own (the tst_styleguide stripComments idiom).
        QString code;
        for (const QString &line : src.split(QLatin1Char('\n'))) {
            const int c = line.indexOf(QStringLiteral("//"));
            code += (c >= 0 ? line.left(c) : line) + QLatin1Char('\n');
        }

        const int call = code.indexOf(QStringLiteral("rootContent.forceActiveFocus()"));
        QVERIFY2(call >= 0, "Main.qml's window blur handler (rootContent.forceActiveFocus) is gone — "
                            "if it moved, move this guard with it");
        // The gate sits in the enclosing `if` just above the call.
        const QString before = code.mid(qMax(0, call - 400), qMin(400, call));
        QVERIFY2(before.contains(QStringLiteral("renamingPath")),
                 "Main.qml's window-wide blur must be gated on sidebar.renamingPath, or the inline "
                 "rename editor gets its focus stolen and closes the instant it opens (multi-click rename)");
    }

    // Escape abandons the edit: nothing is written, and the editor closes
    // (StyleGuide.md's commit convention — Escape reverts without committing).
    void escape_abandonsRenameWithoutWriting()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        clickAt(&view, nameTextPoint(item));
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY(renameFieldVisibleOn(item));

        typeText(&view, QStringLiteral("zzz"));
        QTest::keyClick(&view, Qt::Key_Escape);
        QTest::qWait(150);

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3"))),
                 "Escape must not rename the file");
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("zzz.mp3"))));
        QCOMPARE(sidebarItem->property("renamingPath").toString(), QString());
        QVERIFY2(!renameFieldVisibleOn(item), "Escape must close the inline editor");
    }

    // Blur commits, matching StyleGuide.md's single-line-field convention —
    // clicking away is not a cancel.
    void blur_commitsRename()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("other.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        clickAt(&view, nameTextPoint(item));
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY(renameFieldVisibleOn(item));

        QMetaObject::invokeMethod(renameFieldFor(item), "selectAll");
        typeText(&view, QStringLiteral("renamed"));
        // Click a different row -> the editor blurs.
        const int otherRow = rowForPath(treeItem, vault.filePath(QStringLiteral("other.mp3")));
        QVERIFY(otherRow >= 0);
        QQuickItem *otherItem = rowItem(treeItem, otherRow);
        QVERIFY(otherItem);
        clickAt(&view, nameTextPoint(otherItem));
        QTest::qWait(150);

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("renamed.mp3"))),
                 "blurring the rename editor must commit it, not silently discard the edit");
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3"))));
    }

    // Entering edit mode must not itself be read as a blur. The editor becomes
    // visible a frame before focus lands, so an unguarded blur-commit closes it
    // instantly — the user would see it flash and vanish.
    void openingTheEditor_doesNotInstantlyCommitAndClose()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        clickAt(&view, nameTextPoint(item));
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY(renameFieldVisibleOn(item));

        // Still open several frames later, with the row still in edit mode.
        QTest::qWait(200);
        QVERIFY2(renameFieldVisibleOn(item), "the editor must stay open after it opens");
        QCOMPARE(sidebarItem->property("renamingPath").toString(),
                 vault.filePath(QStringLiteral("talk.mp3")));
    }

    // Clicking the name of a row that ISN'T the sole selection must keep doing
    // what it always did — open the file. The rename gesture must not swallow
    // ordinary click-to-open.
    void clickNameOfUnselectedNote_opensFileAndDoesNotRename()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);
        QQuickItem *itemA = rowItem(treeItem, rowA);
        QQuickItem *itemB = rowItem(treeItem, rowB);
        QVERIFY(itemA && itemB);

        clickAt(&view, nameTextPoint(itemA)); // a.mp3 is now the selection
        // Clicking B's name: B is NOT selected, so this opens B rather than
        // renaming it.
        clickAt(&view, nameTextPoint(itemB));
        QVERIFY2(!renameFieldVisibleOn(itemB),
                 "clicking an unselected row's name must open it, not start a rename");
        QCOMPARE(sidebarItem->property("renamingPath").toString(), QString());
        QCOMPARE(view.rootObject()->property("openPath").toString(),
                 vault.filePath(QStringLiteral("b.mp3")));
    }

    // Ctrl-click builds a multi-selection; it must never trip the rename
    // gesture, even when it lands on the name of an already-selected row.
    void ctrlClickOnSelectedName_doesNotRename()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);
        QQuickItem *itemA = rowItem(treeItem, rowA);
        QQuickItem *itemB = rowItem(treeItem, rowB);
        QVERIFY(itemA && itemB);

        clickAt(&view, nameTextPoint(itemA));
        clickAt(&view, nameTextPoint(itemB), Qt::ControlModifier); // now 2 selected
        // Ctrl-click A's name: A IS selected, but this is a modified click
        // building a selection, not a rename request.
        clickAt(&view, nameTextPoint(itemA), Qt::ControlModifier);
        QVERIFY2(!renameFieldVisibleOn(itemA), "a Ctrl-click must never start a rename");
        QCOMPARE(sidebarItem->property("renamingPath").toString(), QString());
    }

    // Folder rows rename too (VaultTreeModel::renameFolder), keeping their
    // contents. Driven here through beginRename() — the function BOTH of a
    // folder's entry points (the context menu's "Rename", and clicking its
    // name — see clickFolderName_* below) call.
    void beginRename_onFolder_renamesFolderKeepingContents()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const QString folderPath = vault.filePath(QStringLiteral("Interviews"));
        const int row = rowForPath(treeItem, folderPath);
        QVERIFY2(row >= 0, "the Interviews folder must be a visible row");
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        QMetaObject::invokeMethod(sidebarItem, "beginRename", Q_ARG(QVariant, folderPath),
                                   Q_ARG(QVariant, true), Q_ARG(QVariant, QString()));
        QTRY_VERIFY2(renameFieldVisibleOn(item), "beginRename() must open the editor on the folder row");
        QQuickItem *field = renameFieldFor(item);
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());
        // A folder seeds with its own name, extension-stripping never applies.
        QCOMPARE(field->property("text").toString(), QStringLiteral("Interviews"));

        // The editor's caret/selection is placed a frame after it opens
        // (Qt.callLater — see placeCaret). The click-driven tests get this wait
        // for free from clickAt(); this programmatic entry must wait explicitly,
        // so the deferred selectAll can't land mid-typing and garble the name.
        QTest::qWait(60);
        QMetaObject::invokeMethod(renameFieldFor(item), "selectAll");
        typeText(&view, QStringLiteral("Talks"));
        QTest::keyClick(&view, Qt::Key_Return);
        QTest::qWait(150);

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("Talks/seed.mp3"))),
                 "the folder must be renamed with its contents intact");
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("Interviews"))));
    }

    // You rename what you can SEE. With AppSettings.useLabelsSidebar on (the
    // default), a transcribed row displays its stored label instead of its
    // filename — so the gesture must edit that LABEL, writing it back through
    // the controller and leaving the file on disk alone. Seeding the editor
    // with the filename here would be the visible bug: the editor would open
    // showing text the row never displayed.
    void renameOfLabelledRow_editsLabelNotFilename()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3.md")),
                      "---\nmodel: x\nlabel: Board meeting\n---\n[0] hi\n"));

        TranscriptionControllerStub controller;
        controller.label = QStringLiteral("Board meeting");
        m_controller = &controller;

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        QQuickItem *item = rowItem(treeItem, rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(item);
        QQuickItem *nameText = item->findChild<QQuickItem *>(QStringLiteral("rowNameText"));
        QVERIFY(nameText);
        QTRY_COMPARE_WITH_TIMEOUT(nameText->property("text").toString(),
                                  QStringLiteral("Board meeting"), 2000);

        clickAt(&view, nameTextPoint(item)); // select
        clickAt(&view, nameTextPoint(item)); // rename
        QTRY_VERIFY(renameFieldVisibleOn(item));
        QQuickItem *field = renameFieldFor(item);
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());
        QCOMPARE(field->property("text").toString(), QStringLiteral("Board meeting"));

        QMetaObject::invokeMethod(renameFieldFor(item), "selectAll");
        typeText(&view, QStringLiteral("Standup"));
        QTest::keyClick(&view, Qt::Key_Return);
        QTest::qWait(150);

        QCOMPARE(controller.setLabelCalls, 1);
        QCOMPARE(controller.setLabelValue, QStringLiteral("Standup"));
        QCOMPARE(controller.setLabelUrl, QUrl::fromLocalFile(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3"))),
                 "editing a displayed label must NOT rename the file on disk");
        QVERIFY(QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3.md"))));
    }

    // A file rename in flight (from ANY panel, e.g. MainPanel-
    // initiated) is recorded in the controller's pending-rename store and shows
    // in the Sidebar row AT ONCE, before the deferred RenameFileTask completes.
    void pendingRename_updatesRowNameOptimistically()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.m4a"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.m4a.md")),
                      "---\nmodel: x\n---\n[0] hi\n")); // transcribed, NO label

        TranscriptionControllerStub controller;
        m_controller = &controller;

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        QQuickItem *item = rowItem(treeItem, rowForPath(treeItem, vault.filePath(QStringLiteral("talk.m4a"))));
        QVERIFY(item);
        QQuickItem *nameText = item->findChild<QQuickItem *>(QStringLiteral("rowNameText"));
        QVERIFY(nameText);
        QTRY_COMPARE_WITH_TIMEOUT(nameText->property("text").toString(),
                                  QStringLiteral("talk.m4a"), 2000);

        controller.setPendingRename(QUrl::fromLocalFile(vault.filePath(QStringLiteral("talk.m4a"))),
                                    QStringLiteral("chat"));
        QCOMPARE(nameText->property("text").toString(), QStringLiteral("chat.m4a"));
    }

    // Guard: a LABEL-showing row (labels mode + a label) is LEFT ALONE
    // by a pending file rename — the store read sits AFTER the label branch in
    // rowNameText, so a file rename never disturbs a label-shown row.
    void pendingRename_leavesLabelRowUnchanged()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3.md")),
                      "---\nmodel: x\nlabel: My Talk\n---\n[0] hi\n"));

        TranscriptionControllerStub controller;
        controller.label = QStringLiteral("My Talk");
        m_controller = &controller;

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        QQuickItem *item = rowItem(treeItem, rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(item);
        QQuickItem *nameText = item->findChild<QQuickItem *>(QStringLiteral("rowNameText"));
        QVERIFY(nameText);
        QTRY_COMPARE_WITH_TIMEOUT(nameText->property("text").toString(),
                                  QStringLiteral("My Talk"), 2000);

        controller.setPendingRename(QUrl::fromLocalFile(vault.filePath(QStringLiteral("talk.mp3"))),
                                    QStringLiteral("chat"));
        QTest::qWait(50); // give a stray re-eval a chance to misfire
        QCOMPARE(nameText->property("text").toString(), QStringLiteral("My Talk"));
    }

    // Expand/collapse is the GLYPH's gesture. This is the half that pays for
    // the folder-name rename below: the row used to toggle from anywhere, so
    // its name could never mean anything else.
    void clickFolderGlyph_togglesExpanded()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const QString folderPath = vault.filePath(QStringLiteral("Interviews"));
        const QString childPath = vault.filePath(QStringLiteral("Interviews/seed.mp3"));
        QQuickItem *item = rowItem(treeItem, rowForPath(treeItem, folderPath));
        QVERIFY(item);
        // The child's visibility IS the expanded state, read off the model
        // rather than off the delegate's own `expanded` (a one-way latch for
        // childless nodes — see Sidebar.qml's collapsedEmptyDirs).
        QVERIFY2(rowForPath(treeItem, childPath) >= 0, "precondition: the folder starts expanded");

        clickAt(&view, expandGlyphPoint(item));
        QTRY_VERIFY2(rowForPath(treeItem, childPath) < 0,
                     "clicking the expand glyph must collapse the folder");

        clickAt(&view, expandGlyphPoint(item));
        QTRY_VERIFY2(rowForPath(treeItem, childPath) >= 0,
                     "clicking the expand glyph again must re-expand the folder");
    }

    // Clicking a folder's NAME renames it — no already-selected first click
    // needed, because a folder never enters selectedPaths and so has no
    // selected state to reach. It must NOT also toggle the folder.
    void clickFolderName_startsRenameAndDoesNotToggleExpanded()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));

        QQuickView view;
        QQuickItem *treeItem = openVault(&view, vault.path());
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const QString folderPath = vault.filePath(QStringLiteral("Interviews"));
        const QString childPath = vault.filePath(QStringLiteral("Interviews/seed.mp3"));
        QQuickItem *item = rowItem(treeItem, rowForPath(treeItem, folderPath));
        QVERIFY(item);

        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY2(renameFieldVisibleOn(item),
                     "clicking a folder's name must open the inline rename editor on the first click");
        QVERIFY2(rowForPath(treeItem, childPath) >= 0,
                 "the name-click must not ALSO collapse the folder — the glyph owns that now");

        QQuickItem *field = renameFieldFor(item);
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());
        QCOMPARE(field->property("text").toString(), QStringLiteral("Interviews"));

        QMetaObject::invokeMethod(renameFieldFor(item), "selectAll");
        typeText(&view, QStringLiteral("Talks"));
        QTest::keyClick(&view, Qt::Key_Return);
        QTest::qWait(150);

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("Talks/seed.mp3"))),
                 "the folder gesture must rename the folder on disk, contents intact");
        QVERIFY(!QFileInfo::exists(folderPath));
    }

    // The empty space right of a folder's name renames it, same as clicking the
    // name itself — the row's hitbox is far wider than the painted name, and a
    // click there must reach the name gesture (see tapIsOnRowName). It must
    // still NOT toggle the folder — only the glyph does that.
    void clickFolderPastName_startsRename()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("Interviews/seed.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const QString folderPath = vault.filePath(QStringLiteral("Interviews"));
        const QString childPath = vault.filePath(QStringLiteral("Interviews/seed.mp3"));
        QQuickItem *item = rowItem(treeItem, rowForPath(treeItem, folderPath));
        QVERIFY(item);

        clickAt(&view, pastNameTextPoint(item));
        QTRY_VERIFY2(renameFieldVisibleOn(item),
                     "a click in the empty space right of the folder's name must start a rename");
        QVERIFY2(rowForPath(treeItem, childPath) >= 0,
                 "the rename click must not ALSO toggle the folder — only the glyph does");
    }

    // The file counterpart of clickFolderPastName_startsRename: for a note the
    // rename needs the sole-selection first click (file-manager convention), but
    // the SECOND click may land in the empty space right of the name, not only
    // on the letters — the row's hitbox is far wider than a short name.
    void clickPastNameOfSoleSelectedFile_startsRename()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        clickAt(&view, nameTextPoint(item));     // select
        clickAt(&view, pastNameTextPoint(item)); // click the dead space -> rename
        QTRY_VERIFY2(renameFieldVisibleOn(item),
                     "clicking the empty space right of a sole-selected file's name must rename it");
        QCOMPARE(sidebarItem->property("renamingPath").toString(),
                 vault.filePath(QStringLiteral("talk.mp3")));
    }

    // A rename the model refuses (name already taken) must leave the file
    // alone and SAY so, rather than failing silently.
    void renameToExistingName_isRefusedAndReported()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("taken.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        clickAt(&view, nameTextPoint(item));
        clickAt(&view, nameTextPoint(item));
        QTRY_VERIFY(renameFieldVisibleOn(item));

        QMetaObject::invokeMethod(renameFieldFor(item), "selectAll");
        typeText(&view, QStringLiteral("taken"));
        QTest::keyClick(&view, Qt::Key_Return);
        QTest::qWait(150);

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("talk.mp3"))),
                 "a refused rename must leave the original file untouched");
        QCOMPARE(QFileInfo(vault.filePath(QStringLiteral("taken.mp3"))).size(), qint64(1));
        // The report moved from the sidebar's own `rowActionError` banner to the
        // app-wide Notifications queue (StyleGuide.md "Snackbars"), but the
        // behavior under test is unchanged: a refused rename must SAY so. Read
        // the singleton's queue rather than a panel property — this harness
        // renders Sidebar.qml alone, with no SnackbarHost to draw the result.
        QObject *notifications = view.engine()->singletonInstance<QObject *>(
            QStringLiteral("SuopTranscriber"), QStringLiteral("Notifications"));
        QVERIFY2(notifications, "the Notifications singleton must resolve");
        QObject *queue = notifications->property("queue").value<QObject *>();
        QVERIFY(queue);
        QVERIFY2(queue->property("count").toInt() > 0,
                 "a refused rename must surface a notification, not fail silently");
    }

    // Multi-select delete: the whole selection goes, in one call. This is the
    // half a source scan can't answer — deleteFiles() takes a QList<QUrl> and
    // QML hands it a JS array of "file://" strings, so a broken sequence
    // conversion would silently delete nothing.
    void confirmFileDelete_deletesWholeSelectionWithSidecars()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3.md")), "---\nmodel: x\n---\n[0] hi\n"));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("keep.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);
        QQuickItem *itemA = rowItem(treeItem, rowA);
        QQuickItem *itemB = rowItem(treeItem, rowB);
        QVERIFY(itemA && itemB);

        // Build a real 2-row selection with real clicks.
        clickAt(&view, nameTextPoint(itemA));
        clickAt(&view, nameTextPoint(itemB), Qt::ControlModifier);
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList().size(), 2);

        // Right-clicking a selected row targets the whole selection. The
        // widening lives at the call site now (selectionPathsFor) rather than
        // inside beginDeleteFile, so this replicates exactly what the context
        // menu's ctxDeleteFileRow does. Deleting no longer STAGES the paths for
        // a confirm — it trashes them immediately (recoverable, Undo offered in
        // a snackbar), and only stages the ones the trash refuses. Everything
        // trashes here, so the delete is done the moment this returns and
        // pendingDeletePaths stays empty.
        QVariant fileTargets;
        QMetaObject::invokeMethod(sidebarItem, "selectionPathsFor", Q_RETURN_ARG(QVariant, fileTargets),
                                   Q_ARG(QVariant, vault.filePath(QStringLiteral("a.mp3"))),
                                   Q_ARG(QVariant, false));
        QMetaObject::invokeMethod(sidebarItem, "beginDeleteFile", Q_ARG(QVariant, fileTargets));
        QTest::qWait(150);
        QVERIFY2(sidebarItem->property("pendingDeletePaths").toStringList().isEmpty(),
                 "a delete that trashed cleanly must not stage anything for confirmation");

        QVERIFY2(!QFileInfo::exists(vault.filePath(QStringLiteral("a.mp3"))), "every selected file must go");
        QVERIFY2(!QFileInfo::exists(vault.filePath(QStringLiteral("a.mp3.md"))),
                 "a deleted file's transcript sidecar must go with it");
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("b.mp3"))));
        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("keep.mp3"))),
                 "an unselected file must survive");
        // The selection is dropped — it would otherwise point at dead paths.
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList().size(), 0);
    }

    // A REAL right-click through the actual context menu. openContextMenu()
    // takes 9 positional args and JS silently passes `undefined` for a missed
    // one, so a mis-ordered call site would break the menu at runtime while
    // every source scan still passed — this drives the real gesture and reads
    // the real menu.
    void rightClickRow_menuOffersRenameAndNamesTheSelectionCount()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("c.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);
        QQuickItem *itemA = rowItem(treeItem, rowA);
        QQuickItem *itemB = rowItem(treeItem, rowB);
        QVERIFY(itemA && itemB);

        // Two rows selected, then right-click one of them.
        clickAt(&view, nameTextPoint(itemA));
        clickAt(&view, nameTextPoint(itemB), Qt::ControlModifier);
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList().size(), 2);

        QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier,
                           itemA->mapToScene(QPointF(itemA->width() / 2, itemA->height() / 2)).toPoint());
        QTest::qWait(80);

        // The right-click captured this row, as a note, with no label.
        QCOMPARE(sidebarItem->property("ctxPath").toString(), vault.filePath(QStringLiteral("a.mp3")));
        QVERIFY(sidebarItem->property("ctxIsNote").toBool());
        QVERIFY(!sidebarItem->property("ctxIsDir").toBool());
        QCOMPARE(sidebarItem->property("ctxLabel").toString(), QString());

        // Rename is offered...
        QQuickItem *renameRow = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxRenameRow"));
        QVERIFY2(renameRow, "the context menu must have a Rename row");
        QVERIFY2(renameRow->isVisible(), "Rename must be offered for a note row");

        // ...and the delete entry names the whole selection, not just the one
        // row that was right-clicked.
        QQuickItem *delLabel = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxDeleteFileLabel"));
        QVERIFY2(delLabel, "the context menu must have a delete row label");
        QCOMPARE(delLabel->property("text").toString(), QStringLiteral("Delete 2 items"));
    }

    // REGRESSION (user report, after the trash-first rework): "right click ...
    // seems to work once, then not again". Right-click must open the menu every
    // time, not just the first.
    void rightClickRow_opensTheMenuEveryTime_notJustOnce()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        // A Popup is a QObject, not a QQuickItem — findChild<QQuickItem*> misses it.
        QObject *menu = sidebarItem->findChild<QObject *>(QStringLiteral("contextMenu"));
        QVERIFY2(menu, "the context menu must be findable");

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);

        for (int pass = 0; pass < 3; ++pass) {
            const int row = (pass % 2 == 0) ? rowA : rowB;
            QQuickItem *item = rowItem(treeItem, row);
            QVERIFY(item);
            QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier,
                              item->mapToScene(QPointF(item->width() / 2,
                                                       item->height() / 2)).toPoint());
            QTRY_VERIFY2(menu->property("opened").toBool(),
                         qPrintable(QStringLiteral("right-click #%1 must open the context menu")
                                        .arg(pass + 1)));
            // Dismiss it the way a user would, then go again.
            QMetaObject::invokeMethod(menu, "close");
            QTRY_VERIFY(!menu->property("opened").toBool());
        }
    }

    // REGRESSION (user report, after the trash-first rework): "right click ...
    // works once, then not again". The failing sequence isn't right-click alone
    // (that passes) — it's right-click, USE the menu, then right-click again.
    // Deleting now mutates the model synchronously from inside the context
    // menu's own TapHandler, while that modal menu is mid-close.
    void rightClickAfterUsingTheDeleteMenuEntry_stillOpensTheMenu()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        QObject *menu = sidebarItem->findChild<QObject *>(QStringLiteral("contextMenu"));
        QVERIFY(menu);

        // 1. Right-click a row -> menu opens.
        int row = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);
        QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier,
                          item->mapToScene(QPointF(item->width() / 2,
                                                   item->height() / 2)).toPoint());
        QTRY_VERIFY2(menu->property("opened").toBool(), "the first right-click must open the menu");

        // 2. Actually CLICK "Delete file" in it, the way a user does.
        QQuickItem *delRow = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxDeleteFileRow"));
        QVERIFY2(delRow, "the context menu must have a Delete file row");
        QTRY_VERIFY(delRow->isVisible() && delRow->width() > 0);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                          delRow->mapToScene(QPointF(delRow->width() / 2,
                                                     delRow->height() / 2)).toPoint());
        QTest::qWait(200);
        QVERIFY2(!QFileInfo::exists(vault.filePath(QStringLiteral("a.mp3"))),
                 "precondition: the menu entry must actually have deleted the file");

        // 3. The menu must have fully closed...
        QTRY_VERIFY2(!menu->property("opened").toBool(),
                     "the context menu must finish closing after its entry ran — a modal popup "
                     "stuck open blocks every later click");
        QVERIFY2(!menu->property("visible").toBool(),
                 "a modal menu left visible keeps its overlay up and swallows all input");

        // 4. ...and a second right-click must open it again.
        row = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(row >= 0);
        item = rowItem(treeItem, row);
        QVERIFY(item);
        QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier,
                          item->mapToScene(QPointF(item->width() / 2,
                                                   item->height() / 2)).toPoint());
        QTRY_VERIFY2(menu->property("opened").toBool(),
                     "right-click must still work after using a menu entry once");
        QCOMPARE(sidebarItem->property("ctxPath").toString(), vault.filePath(QStringLiteral("b.mp3")));
    }

    // REGRESSION (same report): "It also fails the first time and onwards for
    // one with a transcript" — a transcribed row (status 2) shows the extra
    // "Delete transcript" entry, and right-clicking it must still open the menu.
    void rightClickTranscribedRow_opensTheMenu()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("talk.mp3.md")),
                      "---\nmodel: x\n---\n[0] hi\n"));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        QObject *menu = sidebarItem->findChild<QObject *>(QStringLiteral("contextMenu"));
        QVERIFY(menu);

        const int row = rowForPath(treeItem, vault.filePath(QStringLiteral("talk.mp3")));
        QVERIFY(row >= 0);
        QQuickItem *item = rowItem(treeItem, row);
        QVERIFY(item);

        QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier,
                          item->mapToScene(QPointF(item->width() / 2,
                                                   item->height() / 2)).toPoint());
        QTRY_VERIFY2(menu->property("opened").toBool(),
                     "right-clicking a TRANSCRIBED row must open the context menu");
        QCOMPARE(sidebarItem->property("ctxStatus").toInt(), 2);
        QVERIFY(sidebarItem->property("ctxIsAudio").toBool());

        QQuickItem *delTranscript =
            sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxDeleteTranscriptRow"));
        QVERIFY2(delTranscript, "the Delete transcript row must exist");
        QVERIFY2(delTranscript->isVisible(),
                 "Delete transcript must be offered for a transcribed audio row");
    }

    // REGRESSION (user report): the context menu spawned at the raw cursor
    // point, so a right-click low/far-right pushed it past the sidebar edge and
    // clipped it under the window frame. It's an in-scene popup, so Qt won't
    // flip/clamp it — Sidebar.qml's clampedMenuX/Y must keep the whole popup
    // inside the sidebar.
    void contextMenu_clampedInsideSidebarWhenAnchoredNearEdge()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        QObject *menu = sidebarItem->findChild<QObject *>(QStringLiteral("contextMenu"));
        QVERIFY(menu);

        const qreal sw = sidebarItem->width();
        const qreal sh = sidebarItem->height();
        QVERIFY(sw > 0 && sh > 0);

        // Anchor the menu at the very bottom-right corner of the sidebar — well
        // past where a 200px-wide, multi-row menu could fit if left unclamped.
        const QString path = vault.filePath(QStringLiteral("a.mp3"));
        const QString dir = QFileInfo(path).absolutePath();
        QMetaObject::invokeMethod(
            sidebarItem, "openContextMenu",
            Q_ARG(QVariant, path), Q_ARG(QVariant, false), Q_ARG(QVariant, true),
            Q_ARG(QVariant, true), Q_ARG(QVariant, 1), Q_ARG(QVariant, dir),
            Q_ARG(QVariant, QString()), Q_ARG(QVariant, sw - 2), Q_ARG(QVariant, sh - 2));
        QTRY_VERIFY(menu->property("opened").toBool());

        const qreal x = menu->property("x").toReal();
        const qreal y = menu->property("y").toReal();
        const qreal w = menu->property("width").toReal();
        const qreal h = menu->property("height").toReal();
        QVERIFY2(w > 0 && h > 0, "the menu must have laid out to a real size");
        QVERIFY2(x >= 0 && x + w <= sw + 0.5,
                 qPrintable(QStringLiteral("menu overflows sidebar horizontally: x=%1 w=%2 sidebar=%3")
                                .arg(x).arg(w).arg(sw)));
        QVERIFY2(y >= 0 && y + h <= sh + 0.5,
                 qPrintable(QStringLiteral("menu overflows sidebar vertically: y=%1 h=%2 sidebar=%3")
                                .arg(y).arg(h).arg(sh)));
    }

    // The other half of the same fix: right-clicking genuinely empty space
    // below the rows must STILL target the vault root, so "New folder" stays
    // reachable with no row to click. Guards against fixing the row case by
    // simply breaking this one.
    void rightClickEmptySpace_stillTargetsVaultRoot()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        // Well below the single row, but still inside the tree.
        const QPoint deep = treeItem->mapToScene(
            QPointF(treeItem->width() / 2, treeItem->height() - 10)).toPoint();
        QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier, deep);
        QTest::qWait(80);

        QCOMPARE(sidebarItem->property("ctxPath").toString(), QString());
        QVERIFY(!sidebarItem->property("ctxIsNote").toBool());
        QVERIFY(!sidebarItem->property("ctxIsDir").toBool());
        QCOMPARE(sidebarItem->property("ctxCreateInDir").toString(), vault.path());

        // "New folder" is the whole reason this handler exists.
        QQuickItem *newFolderRow = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxNewFolderRow"));
        QVERIFY2(newFolderRow, "the context menu must have a New folder row");
        QVERIFY(newFolderRow->isVisible());
        // ...and the row-only actions must not be offered for empty space.
        QQuickItem *renameRow = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxRenameRow"));
        QVERIFY(renameRow);
        QVERIFY2(!renameRow->isVisible(), "Rename must not be offered when no row was clicked");
    }

    // The same menu over a single unselected row reads as one file, not a count.
    void rightClickUnselectedRow_menuReadsAsSingleFile()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        QVERIFY(rowA >= 0);
        QQuickItem *itemA = rowItem(treeItem, rowA);
        QVERIFY(itemA);

        QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier,
                           itemA->mapToScene(QPointF(itemA->width() / 2, itemA->height() / 2)).toPoint());
        QTest::qWait(80);

        QQuickItem *delLabel = sidebarItem->findChild<QQuickItem *>(QStringLiteral("ctxDeleteFileLabel"));
        QVERIFY(delLabel);
        QCOMPARE(delLabel->property("text").toString(), QStringLiteral("Delete file"));
    }

    // "Delete transcript" widens to the whole selection when the right-clicked
    // row is part of one — same selectionPathsFor() rule "Delete file" uses,
    // and the same batch call. (This reverses an earlier single-target-only
    // design: now that the C++ takes batches and the ribbon has a bulk
    // delete-transcript button, the context menu widens for consistency, and
    // its label reads "Delete N transcripts" so the count is never silent.)
    // Every selected transcript goes; every selected AUDIO stays.
    void confirmTranscriptDelete_widensToSelectionAndKeepsAudio()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3.md")), "---\nmodel: x\n---\n[0] hi\n"));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3.md")), "---\nmodel: x\n---\n[0] yo\n"));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        const int rowA = rowForPath(treeItem, vault.filePath(QStringLiteral("a.mp3")));
        const int rowB = rowForPath(treeItem, vault.filePath(QStringLiteral("b.mp3")));
        QVERIFY(rowA >= 0 && rowB >= 0);
        clickAt(&view, nameTextPoint(rowItem(treeItem, rowA)));
        clickAt(&view, nameTextPoint(rowItem(treeItem, rowB)), Qt::ControlModifier);
        QCOMPARE(sidebarItem->property("selectedPaths").toStringList().size(), 2);

        // Right-clicking a selected row widens through selectionPathsFor, same
        // as ctxDeleteTranscriptRow does — trashed immediately, not staged.
        QVariant transcriptTargets;
        QMetaObject::invokeMethod(sidebarItem, "selectionPathsFor", Q_RETURN_ARG(QVariant, transcriptTargets),
                                   Q_ARG(QVariant, vault.filePath(QStringLiteral("a.mp3"))),
                                   Q_ARG(QVariant, false));
        QMetaObject::invokeMethod(sidebarItem, "beginDeleteTranscript", Q_ARG(QVariant, transcriptTargets));
        QTest::qWait(150);

        QVERIFY2(!QFileInfo::exists(vault.filePath(QStringLiteral("a.mp3.md"))),
                 "every selected transcript must be deleted");
        QVERIFY2(!QFileInfo::exists(vault.filePath(QStringLiteral("b.mp3.md"))),
                 "\"Delete transcript\" widens to the whole selection, like \"Delete file\"");
        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("a.mp3"))),
                 "every selected audio file must be kept");
        QVERIFY(QFileInfo::exists(vault.filePath(QStringLiteral("b.mp3"))));
    }

    // A single right-clicked row with no multi-selection deletes only itself,
    // even though it goes through the same batch call.
    void beginDeleteFile_onUnselectedRow_targetsOnlyThatRow()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("b.mp3"))));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        // No selection, so selectionPathsFor returns just this one row — the
        // batch is a single element. Same call site the context menu uses.
        QVariant oneTarget;
        QMetaObject::invokeMethod(sidebarItem, "selectionPathsFor", Q_RETURN_ARG(QVariant, oneTarget),
                                   Q_ARG(QVariant, vault.filePath(QStringLiteral("a.mp3"))),
                                   Q_ARG(QVariant, false));
        QMetaObject::invokeMethod(sidebarItem, "beginDeleteFile", Q_ARG(QVariant, oneTarget));
        QTest::qWait(150);

        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("a.mp3"))));
        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("b.mp3"))),
                 "deleting one row must not touch any other");
    }

    // THE point of the trash-first flow: a delete that trashed cleanly must NOT
    // prompt — it posts a snackbar carrying an Undo instead. The confirmation is
    // reserved for the one case that's genuinely irreversible (the trash
    // refusing), which can't be provoked without a specially-mounted filesystem
    // (see tst_vaulttreemodel's note), so this covers the reachable half: no
    // prompt, and an Undo that really puts the file back.
    void trashableDelete_promptsNothingAndOffersAWorkingUndo()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("gone.mp3")), "audio-bytes"));
        QVERIFY(touch(vault.filePath(QStringLiteral("gone.mp3.md")), "# transcript"));

        QQuickView view;
        QQuickItem *sidebarItem = nullptr;
        QQuickItem *treeItem = openVault(&view, vault.path(), &sidebarItem);
        QVERIFY2(treeItem, "could not load Sidebar.qml / locate its TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);
        expandEverything(treeItem);

        QObject *notifications = view.engine()->singletonInstance<QObject *>(
            QStringLiteral("SuopTranscriber"), QStringLiteral("Notifications"));
        QVERIFY(notifications);
        QObject *queue = notifications->property("queue").value<QObject *>();
        QVERIFY(queue);
        QCOMPARE(queue->property("count").toInt(), 0);

        QVariant goneTarget;
        QMetaObject::invokeMethod(sidebarItem, "selectionPathsFor", Q_RETURN_ARG(QVariant, goneTarget),
                                   Q_ARG(QVariant, vault.filePath(QStringLiteral("gone.mp3"))),
                                   Q_ARG(QVariant, false));
        QMetaObject::invokeMethod(sidebarItem, "beginDeleteFile", Q_ARG(QVariant, goneTarget));
        QTest::qWait(150);

        // Gone, with its sidecar...
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("gone.mp3"))));
        QVERIFY(!QFileInfo::exists(vault.filePath(QStringLiteral("gone.mp3.md"))));
        // ...with NO confirmation staged...
        QVERIFY2(sidebarItem->property("pendingDeletePaths").toStringList().isEmpty(),
                 "a recoverable delete must not ask for confirmation");
        // ...and a notification carrying an Undo.
        QCOMPARE(queue->property("count").toInt(), 1);

        QQmlExpression textExpr(qmlContext(sidebarItem), sidebarItem,
                                QStringLiteral("Notifications.queue.get(0).text"));
        QVERIFY2(!textExpr.evaluate().toString().isEmpty(),
                 "the snackbar must say what happened");

        // Fire action 0 exactly the way the snackbar's Undo button does.
        QQmlExpression undoExpr(
            qmlContext(sidebarItem), sidebarItem,
            QStringLiteral("Notifications.trigger(Notifications.queue.get(0).token, 0)"));
        undoExpr.evaluate();
        QVERIFY2(!undoExpr.hasError(), qPrintable(undoExpr.error().toString()));
        QTest::qWait(150);

        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("gone.mp3"))),
                 "Undo must bring the file back");
        QVERIFY2(QFileInfo::exists(vault.filePath(QStringLiteral("gone.mp3.md"))),
                 "Undo must bring the sidecar back too");
        QCOMPARE(readAll(vault.filePath(QStringLiteral("gone.mp3"))), QByteArray("audio-bytes"));
        QVERIFY2(queue->property("count").toInt() == 0,
                 "firing an action must dismiss its snackbar");
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppSettings.qml's Settings{} element
    // is loaded transitively by Sidebar.qml, and must never touch the real
    // ~/.config/suopTranscriber (tests must never touch the user's
    // real vault or app config). Same setup as tst_sidebar_dragmove.cpp.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_rename"));
    // These tests delete files, and deleting now TRASHES them — so the trash is
    // part of what's under test and must not be the developer's own (tests must
    // never touch real user state). Pointing XDG_DATA_HOME at a temp
    // dir makes the home trash <temp>/Trash, thrown away with it. Same reasoning
    // as tst_vaulttreemodel's initTestCase(). Must be set before the app reads
    // it. The QTemporaryDir is static so it outlives every test.
    static QTemporaryDir xdgDataHome;
    if (xdgDataHome.isValid())
        qputenv("XDG_DATA_HOME", xdgDataHome.path().toUtf8());
    QGuiApplication app(argc, argv);
    TstSidebarRename tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_rename.moc"
