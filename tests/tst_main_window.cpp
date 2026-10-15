// Living spec + runtime verification for qml/Main.qml — the app's root
// ApplicationWindow, and the ONLY instantiation site of SettingsPopup.qml
// (hence ModelsPage.qml and BenchmarkPage.qml). No other test loads any of
// these into a live QML engine: every other reference in tests/ is a
// readAll() SOURCE SCAN (tst_benchmark_combos.cpp), which cannot catch a
// load error, an unresolved type, a binding loop, or an AOT crash-on-load —
// a green cmake build does not prove a QML file loads (tst_snackbar.cpp
// makes the same point). This is the one place that actually drives Main.qml
// through a real QQmlApplicationEngine.
//
// Same harness shape as tst_snackbar.cpp (real ApplicationWindow, needed
// because SettingsPopup/ConfirmPopup/etc. reparent onto Overlay.overlay,
// which only exists for an ApplicationWindow — a QQuickView root is a plain
// Item and would silently null-parent every popup) plus tst_sidebar_keynav.cpp
// (hand C++ type registration; tree-row click + focus assertions). Loads the
// REAL Main.qml file directly (not a synthetic driver wrapping it, unlike
// tst_snackbar/tst_sidebar_*) — Main.qml already IS the ApplicationWindow, so
// there is nothing to wrap it in. Every C++ type it instantiates (VaultTreeModel,
// AudioPlayer, AudioRecorder, TranscriptionController, UsbGrabberController,
// UndoStack, VaultSearchProxyModel) is the REAL class, not a stub — proving the
// real wiring is the point of this file.
//
// Sandboxed org/app name (see main() below) is load-bearing, not cosmetic:
// qml/Main.qml's onVaultPathChanged writes AppSettings.lastVaultPath — a real
// app-scoped QtCore Settings{} element — on every vault open, and this test
// opens one on load. Without the sandbox this would clobber the user's real
// ~/.config/suopTranscriber (tests must never touch the user's real vault
// or app config).

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>

#include "AudioPlayer.h"
#include "AudioRecorder.h"
#include "Clipboard.h"
#include "DesktopIntegration.h"
#include "IconTheme.h"
#include "TranscriptDisplayBuilder.h"
#include "TranscriptionController.h"
#include "TreeOrdering.h"
#include "UndoStack.h"
#include "UsbGrabberController.h"
#include "VaultSearchProxyModel.h"
#include "VaultTreeModel.h"
#include "WindowEffects.h"

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

} // namespace

class TstMainWindow : public QObject
{
    Q_OBJECT

    QTemporaryDir m_moduleDir;
    QString m_mainQmlPath;

    // Fresh engine per test: Main.qml wires up AppSettings/Notifications
    // singleton state and a live VaultTreeModel, so reusing one engine would
    // leak state (and an already-open vault) between tests.
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    QStringList m_warnings;

    static QQuickItem *findByClassNamePrefix(QQuickItem *root, const QByteArray &prefix)
    {
        for (QQuickItem *child : root->findChildren<QQuickItem *>()) {
            if (QByteArray(child->metaObject()->className()).startsWith(prefix))
                return child;
        }
        return nullptr;
    }

    // Sidebar.qml is instantiated by Main.qml with no objectName (only an
    // `id:`), and its AOT-compiled class name isn't a safe prefix to guess at
    // ("SidebarRibbon_QMLTYPE_*" also starts with "Sidebar"). Found instead by
    // the properties only Sidebar.qml declares — renamingPath/selectedPaths —
    // which is robust regardless of how the engine names the generated type.
    QQuickItem *findSidebar() const
    {
        for (QQuickItem *item : m_window->findChildren<QQuickItem *>()) {
            if (item->property("renamingPath").isValid() && item->property("selectedPaths").isValid())
                return item;
        }
        return nullptr;
    }

    // SettingsPopup is a QQuickPopup, which is a plain QObject (not a
    // QQuickItem) — only its content reparents onto the overlay once opened,
    // so it must be found via the QObject tree, matching
    // tst_bottombar_taskmodel.cpp's identical jobListPopup() lookup. Matched by
    // class-name prefix (safe here: nothing else in the app is named
    // "SettingsPopup*").
    QObject *findSettingsPopup() const
    {
        for (QObject *child : m_window->findChildren<QObject *>()) {
            if (QByteArray(child->metaObject()->className()).startsWith("SettingsPopup"))
                return child;
        }
        return nullptr;
    }

    // Independent ground truth for "which visible TreeView row is this path
    // at" — reads the model's own roles via QAbstractItemModel's public API,
    // the same helper tst_sidebar_keynav.cpp/tst_sidebar_rename.cpp use.
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

    // Opens Main.qml against a scratch vault. Returns false (having already
    // reported the QML errors) on a load failure — the exact failure mode a
    // green cmake build does NOT catch, since AOT/qmlcachegen can compile a
    // file that still fails at load.
    bool openWindow(const QString &vaultPath)
    {
        m_engine = std::make_unique<QQmlApplicationEngine>();
        m_engine->addImportPath(m_moduleDir.path());
        m_engine->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                   new IconImageProvider);
        m_warnings.clear();
        connect(m_engine.get(), &QQmlApplicationEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const QQmlError &w : warnings)
                        m_warnings << w.toString();
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("initialVaultPath"),
                                                     QUrl::fromLocalFile(vaultPath));
        m_engine->load(QUrl::fromLocalFile(m_mainQmlPath));
        if (m_engine->rootObjects().isEmpty()) {
            qWarning("Main.qml failed to load — see the QML errors above");
            return false;
        }
        m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().first());
        if (!m_window) {
            qWarning("Main.qml's root object is not a QQuickWindow");
            return false;
        }
        return QTest::qWaitForWindowExposed(m_window);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        // Symlink (not copy) to the real checked-out qml/ — must reflect
        // what's on disk right now, never a stale build-time copy.
        QVERIFY2(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")),
                 "failed to symlink qml/ into the test's scratch QML module dir");
        m_mainQmlPath = suopDir + QStringLiteral("/qml/Main.qml");

        // Mirrors root CMakeLists.txt's QML_FILES list (see its qt_add_qml_module
        // block) so the module resolves types the same way the real, compiled
        // module does, rather than leaning on QML's implicit same-directory
        // import as an untested fallback — every entry except Main.qml itself
        // and the two .js files (files in the same directory as Main.qml
        // resolve those directly via relative "import" statements, not through
        // this module). Keep this in sync with CMakeLists.txt: a new .qml file
        // added to the app without a line added here makes this harness fail
        // to load (loudly — see mainWindow_loadsWithNoQmlErrors).
        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
        qmldir.write(
            "module SuopTranscriber\n"
            "singleton AppSettings 1.0 qml/AppSettings.qml\n"
            "singleton Notifications 1.0 qml/Notifications.qml\n"
            "singleton Theme 1.0 qml/Theme.qml\n"
            "Snackbar 1.0 qml/Snackbar.qml\n"
            "SnackbarHost 1.0 qml/SnackbarHost.qml\n"
            "HoverBorder 1.0 qml/HoverBorder.qml\n"
            "HoverUnderlineLabel 1.0 qml/HoverUnderlineLabel.qml\n"
            "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
            "UnderlineField 1.0 qml/UnderlineField.qml\n"
            "TextRedoShortcut 1.0 qml/TextRedoShortcut.qml\n"
            "FilterSuggestions 1.0 qml/FilterSuggestions.qml\n"
            "UsbRegexField 1.0 qml/UsbRegexField.qml\n"
            "UsbToggle 1.0 qml/UsbToggle.qml\n"
            "AppIcon 1.0 qml/AppIcon.qml\n"
            "AppIconButton 1.0 qml/AppIconButton.qml\n"
            "CompressButton 1.0 qml/CompressButton.qml\n"
            "SettingResetButton 1.0 qml/SettingResetButton.qml\n"
            "ToggleSetting 1.0 qml/ToggleSetting.qml\n"
            "SliderSetting 1.0 qml/SliderSetting.qml\n"
            "ConfirmPopup 1.0 qml/ConfirmPopup.qml\n"
            "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
            "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
            "TitleBar 1.0 qml/TitleBar.qml\n"
            "Sidebar 1.0 qml/Sidebar.qml\n"
            "TreeRowDelegate 1.0 qml/TreeRowDelegate.qml\n"
            "SearchResultDelegate 1.0 qml/SearchResultDelegate.qml\n"
            "SidebarRibbon 1.0 qml/SidebarRibbon.qml\n"
            "MainPanel 1.0 qml/MainPanel.qml\n"
            "SeekBar 1.0 qml/SeekBar.qml\n"
            "ChapterHeadingRow 1.0 qml/ChapterHeadingRow.qml\n"
            "PreludeEditor 1.0 qml/PreludeEditor.qml\n"
            "ChapterTocList 1.0 qml/ChapterTocList.qml\n"
            "SearchReplaceBar 1.0 qml/SearchReplaceBar.qml\n"
            "ReplaceRow 1.0 qml/ReplaceRow.qml\n"
            "BottomBar 1.0 qml/BottomBar.qml\n"
            "SettingsPopup 1.0 qml/SettingsPopup.qml\n"
            "BenchmarkPage 1.0 qml/BenchmarkPage.qml\n"
            "UsbGrabberPage 1.0 qml/UsbGrabberPage.qml\n"
            "RecorderPage 1.0 qml/RecorderPage.qml\n"
            "ModelsPage 1.0 qml/ModelsPage.qml\n"
            "AppComboBox 1.0 qml/AppComboBox.qml\n"
            "GainSlider 1.0 qml/GainSlider.qml\n"
            "GainPercentField 1.0 qml/GainPercentField.qml\n"
            "MiddleDragScroll 1.0 qml/MiddleDragScroll.qml\n"
            "ScrollPage 1.0 qml/ScrollPage.qml\n");
        qmldir.close();

        // C++ type registration by hand, since tests don't link the app's
        // qt_add_qml_module — following tst_sidebar_keynav.cpp's pattern. Every
        // type Main.qml instantiates directly, plus the ones its children need.
        // These are all REAL classes, not stubs.
        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterSingletonType<TreeOrdering>(
            "SuopTranscriber", 1, 0, "TreeOrdering",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new TreeOrdering; });
        qmlRegisterSingletonType<TranscriptDisplayBuilder>(
            "SuopTranscriber", 1, 0, "TranscriptDisplayBuilder",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new TranscriptDisplayBuilder; });
        qmlRegisterSingletonType<WindowEffects>(
            "SuopTranscriber", 1, 0, "WindowEffects",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new WindowEffects; });
        qmlRegisterSingletonType<DesktopIntegration>(
            "SuopTranscriber", 1, 0, "DesktopIntegration",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new DesktopIntegration; });
        qmlRegisterSingletonType<Clipboard>(
            "SuopTranscriber", 1, 0, "Clipboard",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new Clipboard; });
        qmlRegisterType<VaultTreeModel>("SuopTranscriber", 1, 0, "VaultTreeModel");
        qmlRegisterType<AudioPlayer>("SuopTranscriber", 1, 0, "AudioPlayer");
        qmlRegisterType<AudioRecorder>("SuopTranscriber", 1, 0, "AudioRecorder");
        qmlRegisterType<TranscriptionController>("SuopTranscriber", 1, 0, "TranscriptionController");
        qmlRegisterType<UsbGrabberController>("SuopTranscriber", 1, 0, "UsbGrabberController");
        qmlRegisterType<UndoStack>("SuopTranscriber", 1, 0, "UndoStack");
        qmlRegisterType<VaultSearchProxyModel>("SuopTranscriber", 1, 0, "VaultSearchProxyModel");
    }

    void cleanup()
    {
        m_window = nullptr;
        m_engine.reset();
    }

    // The core assertion: Main.qml (and everything under it — Sidebar,
    // MainPanel, SettingsPopup, BottomBar, TitleBar) loads with ZERO QML
    // warnings. A binding loop or unresolved property surfaces only as a
    // warning at load time, and this is the one place in the whole suite
    // that would ever see it.
    void mainWindow_loadsWithNoQmlErrors()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("note0.mp3"))));

        QVERIFY2(openWindow(vault.path()), "Main.qml failed to load into a live engine");
        QVERIFY2(m_warnings.isEmpty(),
                 qPrintable(QStringLiteral("Main.qml (or a component under it) emitted %1 QML "
                                           "warning(s) during load:\n")
                                .arg(m_warnings.size())
                            + m_warnings.join(QStringLiteral("\n"))));
    }

    // Forces SettingsPopup.qml — and therefore ModelsPage.qml and
    // BenchmarkPage.qml, its own only instantiation sites — through the
    // engine. Before this test, 2554 lines of SettingsPopup.qml + 287 of
    // ModelsPage.qml were parsed by zero QML engines in CI.
    void settingsPopup_instantiatesAndOpens()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("note0.mp3"))));

        QVERIFY2(openWindow(vault.path()), "Main.qml failed to load into a live engine");

        QObject *popup = findSettingsPopup();
        QVERIFY2(popup, "could not find the SettingsPopup instance under Main.qml's window");
        m_warnings.clear();
        QMetaObject::invokeMethod(popup, "open");
        QTRY_VERIFY2(popup->property("visible").toBool(), "SettingsPopup.open() must show the popup");
        QVERIFY2(m_warnings.isEmpty(),
                 qPrintable(QStringLiteral("opening SettingsPopup emitted %1 QML warning(s):\n")
                                .arg(m_warnings.size())
                            + m_warnings.join(QStringLiteral("\n"))));
    }

    // Regression pin (see qml/Main.qml's rootContent TapHandler doc comment):
    // the window-wide "click empty space blurs the focused text field"
    // TapHandler must never clobber a tree row's OWN focus grab — an
    // unconditional forceActiveFocus() here once killed sidebar arrow-key
    // navigation, because both handlers fire on the SAME tap. Driven here
    // against the REAL Main.qml, not the hand-copied mirror three other
    // harnesses carry (tst_sidebar_keynav.cpp/tst_sidebar_rename.cpp/
    // tst_sidebar_selectall.cpp) — a drift between that mirror and the real
    // handler would leave those harnesses green while the app breaks.
    void windowBlurTapHandler_leavesATreeRowsFocusAlone()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("note0.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("note1.mp3"))));

        QVERIFY2(openWindow(vault.path()), "Main.qml failed to load into a live engine");

        QQuickItem *sidebarItem = findSidebar();
        QVERIFY2(sidebarItem, "could not find the Sidebar instance under Main.qml's window");
        QQuickItem *treeItem = findByClassNamePrefix(sidebarItem, "QQuickTreeView");
        QVERIFY2(treeItem, "could not locate Sidebar's TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        QQuickItem *row0 = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, row0), Q_ARG(QPoint, QPoint(0, 0)));
        QVERIFY2(row0, "itemAtCell(0,0) returned null — row 0 delegate not realized");
        const QPoint clickPos = row0->mapToScene(QPointF(row0->width() / 2, row0->height() / 2)).toPoint();
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, clickPos);
        // TableView's topRow/bottomRow (and the focus grab) land via a
        // deferred polish pass, not synchronously within the click event.
        QTest::qWait(20);

        QVERIFY2(treeItem->property("activeFocus").toBool(),
                 "clicking a tree row must grab keyboard focus — the window-wide blur TapHandler "
                 "must not have clobbered it on the same tap");
        QCOMPARE(rowForPath(treeItem, sidebarItem->property("selectedPaths").toList().first().toString()),
                 0);
    }

    // The other half of the same regression: the very click that OPENS an
    // inline rename editor must not have the window-wide blur TapHandler
    // immediately blur (and thereby commit-close) it — gated on
    // sidebar.renamingPath === "" in qml/Main.qml. Without that gate the
    // rename editor closed on the same tap that opened it, and the rename
    // took several clicks to stick.
    void windowBlurTapHandler_leavesAnOpenRenameEditorAlone()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString notePath = vault.filePath(QStringLiteral("note0.mp3"));
        QVERIFY(touch(notePath));

        QVERIFY2(openWindow(vault.path()), "Main.qml failed to load into a live engine");

        QQuickItem *sidebarItem = findSidebar();
        QVERIFY2(sidebarItem, "could not find the Sidebar instance under Main.qml's window");
        QQuickItem *treeItem = findByClassNamePrefix(sidebarItem, "QQuickTreeView");
        QVERIFY2(treeItem, "could not locate Sidebar's TreeView");
        QTRY_VERIFY(treeItem->property("rows").toInt() > 0);

        // beginRename() is the same function the real click gesture calls
        // (see Sidebar.qml's treeRowTap) — tst_sidebar_rename.cpp exercises the
        // gesture itself; this test's whole point is what happens to an
        // ALREADY-OPEN editor on the very same tap under the real Main.qml.
        QMetaObject::invokeMethod(sidebarItem, "beginRename", Q_ARG(QVariant, notePath),
                                  Q_ARG(QVariant, false), Q_ARG(QVariant, QStringLiteral("note0")));
        QTRY_VERIFY2(sidebarItem->property("renamingPath").toString() == notePath,
                     "beginRename() must open the editor on the row");

        QQuickItem *row0 = nullptr;
        QMetaObject::invokeMethod(treeItem, "itemAtCell", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, row0), Q_ARG(QPoint, QPoint(0, 0)));
        QVERIFY2(row0, "itemAtCell(0,0) returned null — row 0 delegate not realized");
        QQuickItem *field = row0->findChild<QQuickItem *>(QStringLiteral("rowRenameField"));
        QVERIFY2(field && field->isVisible(), "the inline rename editor must be visible on row 0");

        // Click INSIDE the editor field itself — the exact tap the window-wide
        // blur TapHandler also observes (it's a passive handler spanning the
        // whole window).
        const QPoint clickPos = field->mapToScene(QPointF(field->width() / 2, field->height() / 2)).toPoint();
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, clickPos);
        QTest::qWait(20);

        QVERIFY2(sidebarItem->property("renamingPath").toString() == notePath,
                 "clicking inside the open rename editor must not close it — the window-wide blur "
                 "TapHandler is gated on sidebar.renamingPath === \"\" for exactly this reason");
    }
};

int main(int argc, char *argv[])
{
    // Offscreen: no real compositor/display needed. Must be set before
    // QGuiApplication reads it. Sandboxed org/app name so AppSettings.qml's
    // QtCore Settings{} element (loaded transitively by Main.qml — and written
    // to on every vault open, see this file's header comment) never touches
    // the real app's ~/.config/suopTranscriber (tests must never touch the
    // user's real vault or app config).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_main_window"));
    QGuiApplication app(argc, argv);
    TstMainWindow tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_main_window.moc"
