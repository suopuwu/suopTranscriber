// Living spec for "undoing a rename puts the panel's title back". The reported
// bug: rename the open file from the MainPanel (type into titleField, press
// Enter), then press the snackbar's Undo. The sidebar row reverts and the file
// plays fine — so the model and the player both took the revert — but the
// panel's title field keeps showing the NEW name until the user re-clicks the
// file in the tree.
//
// Root cause, and why the FOCUS detail below is load-bearing: titleField and
// filenameSubtitle are always-editable one-shot bindings (text:
// mainPanel.displayTitle() / currentFileStem()), severed by QML the first time
// the user types. refreshMetadata() is the only thing that ever puts them back,
// and it deliberately SKIPS a focused field so it never fights in-progress
// typing. titleField's onAccepted commits without blurring, and Snackbar's Undo
// is an Item + TapHandler that never takes focus — so at the moment undo lands,
// titleField still has focus and the resync is skipped. That is why the bug
// looks intermittent: renaming from the SIDEBAR and undoing works fine, because
// nothing in the panel has focus then. Every test below therefore holds focus on
// titleField across the undo; without that they would pass on the broken code.
//
// Same harness shape as tst_mainpanel_deleteundo.cpp (read its top-of-file
// comment first): QQuickView, offscreen, scratch QML module dir symlinking the
// real qml/, sandboxed org/app name in main(), REAL TranscriptionController +
// REAL VaultTreeModel over a QTemporaryDir vault, functional AudioPlayerStub.
// The controller is real because the undo path under test IS the controller's
// (undoTask enqueues the inverse RenameFileTask, which emits fileRenamed) — a
// stub emitting fileRenamed by hand would only prove the stub agrees with the
// handler, which is the half that was never broken.
//
// Nothing here touches the user's vault or ~/.config/suopTranscriber.

#include <QtTest>

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "Clipboard.h"
#include "IconTheme.h"
#include "TranscriptionController.h"
#include "VaultTreeModel.h"

namespace {

// See tst_mainpanel_deleteundo.cpp's identical stubs.
class AudioRecorderStub : public QObject { Q_OBJECT };
class UsbGrabberControllerStub : public QObject { Q_OBJECT };

// The AudioPlayer members MainPanel reads, and no more — `source` is the
// panel's "an audio file is open" flag, and the thing the rename repoints.
class AudioPlayerStub : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(qint64 position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)

public:
    using QObject::QObject;

    QUrl source() const { return m_source; }
    void setSource(const QUrl &url)
    {
        if (m_source == url)
            return;
        m_source = url;
        emit sourceChanged();
    }
    bool isPlaying() const { return false; }
    qint64 position() const { return 0; }
    void setPosition(qint64) {}
    qint64 duration() const { return 0; }

    Q_INVOKABLE void togglePlay() {}

signals:
    void sourceChanged();
    void playingChanged();
    void positionChanged();
    void durationChanged();

private:
    QUrl m_source;
};

QString transcript()
{
    return QStringLiteral("---\n"
                          "model: base\n"
                          "speakerDetection: false\n"
                          "---\n"
                          "[0] Hello world\n");
}

} // namespace

class TstMainPanelRenameUndo : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QTemporaryDir m_vaultDir;
    QString m_driverQmlPath;
    QString m_audioPath;
    QString m_mdPath;

    static bool writeFile(const QString &path, const QString &contents)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        f.write(contents.toUtf8());
        f.close();
        return true;
    }

public:
    QQuickItem *m_panel = nullptr;
    QQuickItem *m_titleField = nullptr;
    TranscriptionController *m_controller = nullptr;
    VaultTreeModel *m_model = nullptr;
    AudioPlayerStub *m_player = nullptr;

private:
    QString titleText() const { return m_titleField->property("text").toString(); }

    QQuickItem *openPanel(QQuickView *view)
    {
        m_panel = nullptr;
        m_titleField = nullptr;
        view->engine()->addImportPath(m_moduleDir.path());
        view->engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                          new IconImageProvider);
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : view->errors())
                msg += e.toString() + QStringLiteral("\n");
            // The exact failure a green cmake build does NOT catch: qmlcachegen
            // happily AOT-compiles a file that still fails to load.
            qWarning("%s", qPrintable(msg.isEmpty() ? QStringLiteral("MainPanel.qml failed to load, no error text")
                                                     : msg));
            return nullptr;
        }
        QQuickItem *root = view->rootObject();
        if (!root)
            return nullptr;
        QQuickItem *panel = root->findChild<QQuickItem *>(QStringLiteral("mainPanel"));
        if (!panel)
            return nullptr;

        m_controller = new TranscriptionController(this);
        m_model = new VaultTreeModel(this);
        m_player = new AudioPlayerStub(this);
        m_controller->setVaultTreeModel(m_model);
        m_model->setVaultPath(QUrl::fromLocalFile(m_vaultDir.path()));
        panel->setProperty("transcriptionController", QVariant::fromValue(m_controller));
        panel->setProperty("vaultTreeModel", QVariant::fromValue(m_model));
        panel->setProperty("player", QVariant::fromValue(m_player));
        panel->setProperty("vaultPath", QUrl::fromLocalFile(m_vaultDir.path()));
        // Main.qml sets BOTH — and displayTitle() goes through relativeToVault(),
        // which reads vaultLocalPath (the plain local path), not vaultPath.
        // Without this the title renders as the whole absolute path.
        panel->setProperty("vaultLocalPath", m_vaultDir.path());

        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        m_panel = panel;
        m_titleField = panel->findChild<QQuickItem *>(QStringLiteral("titleField"));
        return panel;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        QVERIFY(m_vaultDir.isValid());

        const QString modelsDir = m_vaultDir.filePath(QStringLiteral(".transcriber/models"));
        QVERIFY(QDir().mkpath(modelsDir));
        qputenv("SUOP_MODELS_DIR", modelsDir.toLocal8Bit());

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
                      "AppIcon 1.0 qml/AppIcon.qml\n"
                      "HoverBorder 1.0 qml/HoverBorder.qml\n"
                      "UnderlineField 1.0 qml/UnderlineField.qml\n"
                      "MiddleDragScroll 1.0 qml/MiddleDragScroll.qml\n"
                      "UsbRegexField 1.0 qml/UsbRegexField.qml\n"
                      "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                      "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                      "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
                      "MainPanel 1.0 qml/MainPanel.qml\n");
        qmldir.close();

        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterSingletonType<Clipboard>(
            "SuopTranscriber", 1, 0, "Clipboard",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new Clipboard; });
        qmlRegisterType<TranscriptionController>("SuopTranscriber", 1, 0, "TranscriptionController");
        qmlRegisterType<VaultTreeModel>("SuopTranscriber", 1, 0, "VaultTreeModel");
        qmlRegisterUncreatableType<AudioPlayerStub>(
            "SuopTranscriber", 1, 0, "AudioPlayer", QStringLiteral("stub — this harness constructs it in C++"));
        qmlRegisterUncreatableType<AudioRecorderStub>(
            "SuopTranscriber", 1, 0, "AudioRecorder", QStringLiteral("stub — this harness leaves it null"));
        qmlRegisterUncreatableType<UsbGrabberControllerStub>(
            "SuopTranscriber", 1, 0, "UsbGrabberController", QStringLiteral("stub — this harness leaves it null"));

        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    width: 900\n"
            "    height: 700\n"
            "    MainPanel {\n"
            "        objectName: \"mainPanel\"\n"
            "        anchors.fill: parent\n"
            "    }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driverFile(m_driverQmlPath);
        QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
        driverFile.write(driverQml.toUtf8());
        driverFile.close();
    }

    // Fresh fixture per test — each one renames files on disk.
    void init()
    {
        m_audioPath = m_vaultDir.filePath(QStringLiteral("talk.m4a"));
        // The sidecar is the FULL audio filename + ".md" — see
        // renameRecordingFile().
        m_mdPath = m_vaultDir.filePath(QStringLiteral("talk.m4a.md"));
        QVERIFY(writeFile(m_audioPath, QStringLiteral("not-really-audio")));
        QVERIFY(writeFile(m_mdPath, transcript()));
    }

    void cleanup()
    {
        for (const QString &name : {QStringLiteral("talk.m4a"), QStringLiteral("talk.m4a.md"),
                                     QStringLiteral("chat.m4a"), QStringLiteral("chat.m4a.md"),
                                     QStringLiteral("taken.m4a"), QStringLiteral("taken.m4a.md")}) {
            QFile::remove(m_vaultDir.filePath(name));
        }
    }

    // THE regression. Renaming while titleField holds focus, then undoing, must
    // put the ORIGINAL name back in the panel — not just in the sidebar. Before
    // the fix the undo's refreshMetadata() skipped the focused field and the
    // panel kept showing "chat".
    void undoingARename_revertsTheTitleEvenWhileTheFieldHasFocus()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        QVERIFY2(m_titleField, "titleField must expose objectName for this test");

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        // The load-bearing detail: committing a rename from titleField leaves it
        // focused (onAccepted does not blur), and the snackbar's Undo never takes
        // focus away. Hold focus across BOTH halves or this passes on the bug.
        m_titleField->forceActiveFocus();
        QVERIFY2(m_titleField->hasActiveFocus(), "titleField did not take focus");

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        QSignalSpy undoSpy(m_controller, &TranscriptionController::undoAvailable);
        m_controller->renameFile(QUrl::fromLocalFile(m_audioPath), QStringLiteral("chat"));

        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);
        QVERIFY2(renameSpy.constFirst().at(2).toBool(), "the forward rename itself failed");
        // Forward direction: the panel follows the rename even while focused.
        QTRY_COMPARE_WITH_TIMEOUT(titleText(), QStringLiteral("chat.m4a"), 2000);
        QVERIFY(QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("chat.m4a"))));

        // Now the undo, with focus still held.
        QVERIFY2(m_titleField->hasActiveFocus(), "precondition: titleField must still be focused");
        QTRY_COMPARE(undoSpy.count(), 1);
        m_controller->undoTask(undoSpy.constFirst().at(0).toInt());

        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(titleText(), QStringLiteral("talk.m4a"), 2000);
        QCOMPARE(m_player->source(), QUrl::fromLocalFile(m_audioPath));
    }

    // The fix must be a forced RESYNC, not "blur the field" — blurring would
    // yank the caret out from under a user who is mid-edit elsewhere, and would
    // make the assertion above pass for the wrong reason.
    void undoingARename_doesNotStealFocusFromTheTitleField()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        QVERIFY(m_titleField);

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        m_titleField->forceActiveFocus();
        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        QSignalSpy undoSpy(m_controller, &TranscriptionController::undoAvailable);
        m_controller->renameFile(QUrl::fromLocalFile(m_audioPath), QStringLiteral("chat"));
        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);

        QTRY_COMPARE(undoSpy.count(), 1);
        m_controller->undoTask(undoSpy.constFirst().at(0).toInt());
        QTRY_COMPARE_WITH_TIMEOUT(titleText(), QStringLiteral("talk.m4a"), 5000);

        QVERIFY2(m_titleField->hasActiveFocus(),
                 "the resync blurred titleField — that would eat the caret of a user mid-edit");
    }

    // A rename that FAILS must put the real on-disk stem back on screen. Before
    // the fix only filenameSubtitle was reverted, so a bad name typed into
    // titleField (non-labels mode) stayed visible after the error.
    void aFailedRename_revertsTheTitleToTheOnDiskName()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        QVERIFY(m_titleField);

        // Occupy the target name so the rename is rejected.
        QVERIFY(writeFile(m_vaultDir.filePath(QStringLiteral("taken.m4a")),
                          QStringLiteral("not-really-audio")));

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        m_titleField->forceActiveFocus();
        // The optimistic edit the user typed, which must not survive the failure.
        m_titleField->setProperty("text", QStringLiteral("taken"));

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        m_controller->renameFile(QUrl::fromLocalFile(m_audioPath), QStringLiteral("taken"));
        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);
        QVERIFY2(!renameSpy.constFirst().at(2).toBool(),
                 "precondition: renaming onto an existing name must fail");

        QTRY_COMPARE_WITH_TIMEOUT(titleText(), QStringLiteral("talk.m4a"), 2000);
        QVERIFY2(QFileInfo::exists(m_audioPath), "the failed rename must leave the file alone");
    }

    // The guard: a rename of some OTHER file must not rewrite the open file's
    // title. resyncIdentityFields() is unconditional on focus, so if it ever ran
    // for the wrong file it would clobber whatever the user was typing.
    void renamingAnotherFile_leavesTheOpenFilesTitleAlone()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        QVERIFY(m_titleField);

        const QString other = m_vaultDir.filePath(QStringLiteral("taken.m4a"));
        QVERIFY(writeFile(other, QStringLiteral("not-really-audio")));

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        // The user is mid-edit on the open file's title.
        m_titleField->forceActiveFocus();
        m_titleField->setProperty("text", QStringLiteral("half-typed name"));

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        m_controller->renameFile(QUrl::fromLocalFile(other), QStringLiteral("chat"));
        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);

        // Give the handler a chance to misfire before declaring it innocent.
        QTest::qWait(100);
        QCOMPARE(titleText(), QStringLiteral("half-typed name"));
    }

    // Item 074: a rename fired from the SIDEBAR for the open file (nothing in
    // the panel focused) updates the title AT ONCE — synchronously, via the
    // controller's pending-rename store (pendingRenamesRevisionChanged), before
    // the deferred RenameFileTask completes.
    void sidebarRenameOfOpenFile_updatesTitleOptimistically()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));
        QVERIFY2(!m_titleField->hasActiveFocus(), "precondition: no field focused");

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        m_controller->renameFile(QUrl::fromLocalFile(m_audioPath), QStringLiteral("chat"));
        // Optimistic: store populated + revision bumped synchronously in
        // renameFile(); the panel already reads the new name before completion.
        QCOMPARE(renameSpy.count(), 0);
        QCOMPARE(titleText(), QStringLiteral("chat.m4a"));

        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);
        QVERIFY2(renameSpy.constFirst().at(2).toBool(), "the rename itself failed");
        QTRY_COMPARE(titleText(), QStringLiteral("chat.m4a"));
    }

    // Queue item 018: the filename subtitle names the REAL on-disk file, so it
    // shows the extension (talk.m4a), not the bare stem (talk). The text binding
    // evaluates regardless of the labels-mode visibility gate.
    void filenameSubtitle_showsTheExtension()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        auto *sub = m_panel->findChild<QQuickItem *>(QStringLiteral("filenameSubtitle"));
        QVERIFY2(sub, "filenameSubtitle must expose objectName for this test");
        QTRY_COMPARE(sub->property("text").toString(), QStringLiteral("talk.m4a"));
    }

    // Queue item 018: the subtitle shows stem+ext, but renameFile() takes a stem
    // and re-appends the real extension — so committing "chat.m4a" must strip the
    // typed extension and yield chat.m4a, NOT chat.m4a.m4a.
    void committingTheSubtitle_stripsTheExtension_noDoubleExt()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        QMetaObject::invokeMethod(m_panel, "commitFilenameRename",
                                  Q_ARG(QVariant, QVariant(QStringLiteral("chat.m4a"))));
        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);
        QVERIFY2(renameSpy.constFirst().at(2).toBool(), "the forward rename itself failed");
        QVERIFY(QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("chat.m4a"))));
        QVERIFY2(!QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("chat.m4a.m4a"))),
                 "the typed extension must be stripped, not double-appended");
    }

    // Queue item 019, issue 1: a raw audio file's title edit must RENAME the
    // file (there's no label), never route to setLabel. No sidecar .md means no
    // label was invented either.
    void rawAudioTitleEditRenamesFile()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        QFile::remove(m_mdPath); // raw audio: no transcript, no label

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        QMetaObject::invokeMethod(m_panel, "commitRename",
                                  Q_ARG(QVariant, QVariant(QStringLiteral("chat"))));
        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);
        QVERIFY2(renameSpy.constFirst().at(2).toBool(), "the rename itself failed");
        QVERIFY(QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("chat.m4a"))));
        QVERIFY2(!QFileInfo::exists(m_audioPath), "the old audio file must be gone");
        QVERIFY2(!QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("chat.m4a.md"))),
                 "editing a raw audio title must rename, not invent a label sidecar");
    }

    // Queue item 019, issue 2: a transcribed-but-unlabeled file's title edit must
    // rename the file, not silently create a label.
    void unlabeledTranscribedTitleEditRenamesNotLabels()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        // init() wrote talk.m4a + talk.m4a.md (transcript(), no label:).

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        QMetaObject::invokeMethod(m_panel, "commitRename",
                                  Q_ARG(QVariant, QVariant(QStringLiteral("chat"))));
        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);
        QVERIFY2(renameSpy.constFirst().at(2).toBool(), "the rename itself failed");
        QVERIFY(QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("chat.m4a"))));
        QVERIFY(QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("chat.m4a.md"))));
        QVERIFY2(!QFileInfo::exists(m_audioPath), "the old audio file must be gone");
        QFile md(m_vaultDir.filePath(QStringLiteral("chat.m4a.md")));
        QVERIFY(md.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY2(!QString::fromUtf8(md.readAll()).contains(QStringLiteral("label:")),
                 "editing an unlabeled title must rename, not invent a label");
    }

    // Queue item 019, issue 1b: clicking empty background (deselectTextSelections)
    // must blur a focused titleField so its edit commits.
    void clickOutsideBlursTitleField()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        m_titleField->forceActiveFocus();
        QVERIFY(m_titleField->hasActiveFocus());
        QMetaObject::invokeMethod(m_panel, "deselectTextSelections");
        QTRY_VERIFY2(!m_titleField->hasActiveFocus(),
                     "an empty-space click must blur the focused title field");
    }

    // Queue item 019, issue 3: the confirm checkmark is visible only while the
    // field is focused AND edited, and its tap action (titleField.focus = false)
    // commits the rename and hides it again.
    void confirmCheckmarkVisibilityAndCommit()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        auto *check = m_panel->findChild<QQuickItem *>(QStringLiteral("titleConfirmButton"));
        QVERIFY2(check, "titleConfirmButton must expose objectName for this test");
        QVERIFY2(!check->isVisible(), "checkmark must be hidden while unfocused/unchanged");

        m_titleField->forceActiveFocus();
        m_titleField->setProperty("text", QStringLiteral("chat"));
        QTRY_VERIFY2(check->isVisible(), "checkmark must show when the title is focused and edited");

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        m_titleField->setProperty("focus", false); // what the checkmark's TapHandler does
        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);
        QVERIFY2(renameSpy.constFirst().at(2).toBool(), "the rename itself failed");
        QTRY_VERIFY2(!check->isVisible(), "checkmark must hide again after commit");
    }

    // Queue item 023: the filename-subtitle field commits on BOTH Enter and blur.
    // Two commits fired before the first rename result repoints the panel must
    // collapse to ONE rename (renameInFlight guard) — without it the second
    // fires against the already-moved old path and hits "File does not exist."
    void renameFilenameSubtitle_doubleCommitFiresOnlyOneRename()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        // Back-to-back (Enter then blur) before the event loop delivers the first
        // fileRenamed — the second must be swallowed by renameInFlight.
        QMetaObject::invokeMethod(m_panel, "commitFilenameRename",
                                  Q_ARG(QVariant, QVariant(QStringLiteral("chat.m4a"))));
        QMetaObject::invokeMethod(m_panel, "commitFilenameRename",
                                  Q_ARG(QVariant, QVariant(QStringLiteral("chat.m4a"))));
        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);
        QTest::qWait(200); // give a stray second rename a chance to fire
        QCOMPARE(renameSpy.count(), 1);
        QVERIFY(QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("chat.m4a"))));
    }

    // Queue item 023: same double-commit collapse for titleField's file-rename
    // branch (unlabeled file, non-labels routing) — that branch had no guard.
    void renameTitleField_doubleCommitFiresOnlyOneRename()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        QFile::remove(m_mdPath); // raw audio, no label -> commitRename renames
        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        QMetaObject::invokeMethod(m_panel, "commitRename",
                                  Q_ARG(QVariant, QVariant(QStringLiteral("chat"))));
        QMetaObject::invokeMethod(m_panel, "commitRename",
                                  Q_ARG(QVariant, QVariant(QStringLiteral("chat"))));
        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);
        QTest::qWait(200);
        QCOMPARE(renameSpy.count(), 1);
        QVERIFY(QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("chat.m4a"))));
    }

    // Queue item 029: the title names the FILE, never its location. A file in a
    // sub-folder must show just its base filename + extension, not the vault-
    // relative path (displayTitle() used to render the whole relative path).
    void title_showsBaseName_notContainingFolders()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        QVERIFY(QDir(m_vaultDir.path()).mkpath(QStringLiteral("sub")));
        QVERIFY(writeFile(m_vaultDir.filePath(QStringLiteral("sub/nested.m4a")),
                          QStringLiteral("not-really-audio")));
        QVERIFY(writeFile(m_vaultDir.filePath(QStringLiteral("sub/nested.m4a.md")), transcript()));

        m_player->setSource(QUrl::fromLocalFile(m_vaultDir.filePath(QStringLiteral("sub/nested.m4a"))));
        QTRY_COMPARE(titleText(), QStringLiteral("nested.m4a")); // NOT sub/nested.m4a

        QDir(m_vaultDir.filePath(QStringLiteral("sub"))).removeRecursively();
    }

    // Queue item 029 companion: the title shows stem+ext, but renameFile() takes
    // a stem and re-appends the real extension — committing "chat.m4a" must strip
    // the typed extension and yield chat.m4a, NOT chat.m4a.m4a. (Default config
    // is labels-mode ON and transcript() writes no label:, so commitRename routes
    // to the file-rename branch.)
    void committingTheTitle_stripsExtension_noDoubleExt()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        QMetaObject::invokeMethod(m_panel, "commitRename",
                                  Q_ARG(QVariant, QVariant(QStringLiteral("chat.m4a"))));
        QTRY_COMPARE_WITH_TIMEOUT(renameSpy.count(), 1, 5000);
        QVERIFY2(renameSpy.constFirst().at(2).toBool(), "the forward rename itself failed");
        QVERIFY(QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("chat.m4a"))));
        QVERIFY2(!QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("chat.m4a.m4a"))),
                 "typed extension must be stripped, not double-appended");
    }

    // Queue item 029 companion: merely focusing+blurring the title (committing the
    // unchanged name, now WITH its extension) must fire NO rename — an unguarded
    // commit would rename talk.m4a -> talk.m4a.m4a. Proves the blur-to-double-ext
    // bug (pre-existing for root files, newly exposed for sub-folders) is closed.
    void committingTheTitleUnchanged_firesNoRename()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(titleText(), QStringLiteral("talk.m4a"));

        QSignalSpy renameSpy(m_controller, &TranscriptionController::fileRenamed);
        QMetaObject::invokeMethod(m_panel, "commitRename",
                                  Q_ARG(QVariant, QVariant(QStringLiteral("talk.m4a"))));
        QTest::qWait(200);
        QCOMPARE(renameSpy.count(), 0);
        QVERIFY2(QFileInfo::exists(m_audioPath), "unchanged commit must not rename the file");
        QVERIFY(!QFileInfo::exists(m_vaultDir.filePath(QStringLiteral("talk.m4a.m4a"))));
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: MainPanel.qml loads the AppSettings
    // singleton, whose Settings{} element must NEVER touch the real
    // ~/.config/suopTranscriber (CLAUDE.md: never let testing touch the user's
    // real vault or app config).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_mainpanel_renameundo"));
    QGuiApplication app(argc, argv);
    TstMainPanelRenameUndo tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_mainpanel_renameundo.moc"
