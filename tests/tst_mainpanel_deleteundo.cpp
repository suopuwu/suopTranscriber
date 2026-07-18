// Living spec for "undoing a delete puts the panel back where the delete moved
// it from". The reported bug: delete an open file's audio and the panel follows
// the surviving transcript into the orphaned-.md view (correct — that's the
// point of the action); press the snackbar's Undo and the audio comes back on
// disk and the sidebar row flips back to an audio row, but the panel stays in
// the text-only orphaned view with no player. Model and panel disagree about the
// same file until the user re-clicks it in the tree.
//
// Root cause, and why this harness is shaped the way it is: the delete side of
// VaultTreeModel pairs its rebuild() with a targeted signal MainPanel matches
// against the file it has open, while restoreFromTrash() only ever called
// rebuild(). rebuild() is enough for the tree (it re-reads everything) but the
// panel's view state is the panel's own — nothing walks it back unless a signal
// says to. So the assertions below deliberately go through the REAL
// VaultTreeModel and the REAL trash: a stub emitting the new signals by hand
// would only prove the stub agrees with the handler, which is precisely the half
// that was never broken.
//
// Same harness shape as tst_mainpanel_chapterrefresh.cpp (read its top-of-file
// comment first): QQuickView, offscreen, scratch QML module dir symlinking the
// real qml/, sandboxed org/app name in main(). Two differences:
//
//  - **VaultTreeModel is REAL** (see above), pointed at a QTemporaryDir vault.
//  - **AudioPlayer is a stub, but a functional one** — the panel's whole
//    orphaned-vs-audio distinction is "is player.source set", so unlike the
//    other MainPanel harnesses this one can't leave the player null. It carries
//    the six members MainPanel actually touches; nothing decodes, and no test
//    here plays anything (the .mp3 fixture is bytes with an audio extension,
//    which is all the vault's classification and the trash care about).
//
// Nothing here touches the user's vault or ~/.config/suopTranscriber. The trash
// is real, but only ever receives files from the QTemporaryDir vault.

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

// See tst_mainpanel_chapterrefresh.cpp's identical stubs: MainPanel's use sites
// guard on the property being non-null, so each only needs its TYPE to resolve.
class AudioRecorderStub : public QObject { Q_OBJECT };
class UsbGrabberControllerStub : public QObject { Q_OBJECT };

// The AudioPlayer members MainPanel reads, and no more — mirroring the real
// AudioPlayer.h's property signatures so the panel's bindings see the types they
// expect. `source` is the only one with any behavior: it is the panel's "an
// audio file is open" flag, which is the whole subject of this file.
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

// A transcript exactly as the app writes one — see
// tst_mainpanel_chapterrefresh.cpp's identical fixture.
QString transcript()
{
    return QStringLiteral("---\n"
                          "model: base\n"
                          "speakerDetection: false\n"
                          "---\n"
                          "[0] Hello world\n");
}

} // namespace

class TstMainPanelDeleteUndo : public QObject
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

    QString orphanedMdPath() const { return m_panel->property("orphanedMdPath").toString(); }
    QUrl playerSource() const { return m_player->source(); }

public:
    QQuickItem *m_panel = nullptr;
    TranscriptionController *m_controller = nullptr;
    VaultTreeModel *m_model = nullptr;
    AudioPlayerStub *m_player = nullptr;

private:
    // Builds the view and wires the real controller + real model in. Returns the
    // located MainPanel (also caching it in m_panel), or nullptr.
    QQuickItem *openPanel(QQuickView *view)
    {
        m_panel = nullptr;
        view->engine()->addImportPath(m_moduleDir.path());
        view->engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                         new IconImageProvider);
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : view->errors())
                msg += e.toString() + QStringLiteral("\n");
            // The exact failure a green cmake build does NOT catch: qmlcachegen
            // happily AOT-compiles a file that still fails to load. Verbatim,
            // not as "null item".
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

        // Must be set BEFORE any file is opened — beginTranscriptLoad() reads
        // through transcriptionController the moment a path changes.
        m_controller = new TranscriptionController(this);
        m_model = new VaultTreeModel(this);
        m_player = new AudioPlayerStub(this);
        // The live app wires this via Main.qml's `vaultTreeModel: vaultModel` on
        // the controller — it's what lets a trash/delete/restore TASK call back
        // to the model (applyRemovalOutcome/applyRestoreOutcome) so the per-file
        // delete/restore signals fire. Without it the ASYNC controller.trash()
        // path is silent (the bug the async test below guards).
        m_controller->setVaultTreeModel(m_model);
        m_model->setVaultPath(QUrl::fromLocalFile(m_vaultDir.path()));
        panel->setProperty("transcriptionController", QVariant::fromValue(m_controller));
        panel->setProperty("vaultTreeModel", QVariant::fromValue(m_model));
        panel->setProperty("player", QVariant::fromValue(m_player));
        panel->setProperty("vaultPath", QUrl::fromLocalFile(m_vaultDir.path()));

        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        m_panel = panel;
        return panel;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        QVERIFY(m_vaultDir.isValid());

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
        // The two real things — see this file's top comment.
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

    // Fresh fixture per test: each one trashes files, and a leftover trash entry
    // from the previous test would make the next one's restore ambiguous.
    void init()
    {
        m_audioPath = m_vaultDir.filePath(QStringLiteral("talk.mp3"));
        m_mdPath = m_vaultDir.filePath(QStringLiteral("talk.mp3.md"));
        QVERIFY(writeFile(m_audioPath, QStringLiteral("not-really-audio")));
        QVERIFY(writeFile(m_mdPath, transcript()));
    }

    void cleanup()
    {
        QFile::remove(m_audioPath);
        QFile::remove(m_mdPath);
    }

    // The reported bug, end to end and in the user's order: open the file, delete
    // its audio, press Undo. The panel must be back on the audio — not left in
    // the orphaned view it was only ever in because the audio was gone.
    void undoingAnAudioOnlyDelete_putsTheOpenFileBackOnItsAudio()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));

        const QVariantMap result = m_model->trashAudioOnly({ QUrl::fromLocalFile(m_audioPath) });
        QCOMPARE(result.value(QStringLiteral("trashed")).toInt(), 1);
        // Precondition, not the assertion: the delete half already worked, and if
        // it ever stops, the undo half below would pass for the wrong reason.
        QTRY_COMPARE(orphanedMdPath(), m_mdPath);
        QCOMPARE(playerSource(), QUrl());

        QCOMPARE(m_model->restoreFromTrash(result.value(QStringLiteral("restorable")).toList()), 1);

        QTRY_COMPARE_WITH_TIMEOUT(playerSource(), QUrl::fromLocalFile(m_audioPath), 2000);
        QVERIFY2(orphanedMdPath().isEmpty(),
                 "the panel is still in the orphaned-.md view of a file whose audio is back");
    }

    // The same asymmetry one action over: a transcript delete leaves the file
    // open and only reverts it to untranscribed, so undo has to re-read it.
    void undoingATranscriptDelete_showsTheTranscriptAgain()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_VERIFY(m_panel->property("hasStoredMetadata").toBool());

        const QVariantMap result = m_model->trashTranscript({ QUrl::fromLocalFile(m_audioPath) });
        QCOMPARE(result.value(QStringLiteral("trashed")).toInt(), 1);
        QTRY_VERIFY2(!m_panel->property("hasStoredMetadata").toBool(),
                     "precondition: deleting the transcript must revert the panel to untranscribed");

        QCOMPARE(m_model->restoreFromTrash(result.value(QStringLiteral("restorable")).toList()), 1);

        QTRY_VERIFY2_WITH_TIMEOUT(m_panel->property("hasStoredMetadata").toBool(),
                                  "the panel still reads as untranscribed after its transcript was restored",
                                  2000);
        // The file itself never moved — undo must not have switched views.
        QCOMPARE(playerSource(), QUrl::fromLocalFile(m_audioPath));
        QVERIFY(orphanedMdPath().isEmpty());
    }

    // Deleting the open file closes the panel, so undo has to reopen it — and
    // the panel's memory of having closed it (closedByDeleteUrl) is the only
    // thing left that knows it was ever open.
    void undoingAWholeFileDelete_reopensTheFileItClosed()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));

        const QVariantMap result = m_model->trashFiles({ QUrl::fromLocalFile(m_audioPath) });
        QCOMPARE(result.value(QStringLiteral("trashed")).toInt(), 1);
        QTRY_COMPARE(playerSource(), QUrl()); // precondition: the delete closed it

        QCOMPARE(m_model->restoreFromTrash(result.value(QStringLiteral("restorable")).toList()), 2);

        QTRY_COMPARE_WITH_TIMEOUT(playerSource(), QUrl::fromLocalFile(m_audioPath), 2000);
        QVERIFY(orphanedMdPath().isEmpty());
    }

    // The transcript-less case the audio-only path has no opinion about: nothing
    // survives, so the delete just closes the panel, and undo still owes it the
    // file back.
    void undoingADeleteOfUntranscribedAudio_reopensIt()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        QVERIFY(QFile::remove(m_mdPath)); // untranscribed: audio and nothing else
        m_player->setSource(QUrl::fromLocalFile(m_audioPath));

        const QVariantMap result = m_model->trashFiles({ QUrl::fromLocalFile(m_audioPath) });
        QCOMPARE(result.value(QStringLiteral("trashed")).toInt(), 1);
        QTRY_COMPARE(playerSource(), QUrl());

        QCOMPARE(m_model->restoreFromTrash(result.value(QStringLiteral("restorable")).toList()), 1);
        QTRY_COMPARE_WITH_TIMEOUT(playerSource(), QUrl::fromLocalFile(m_audioPath), 2000);
    }

    // The ASYNC controller path — what the live app actually does now: the
    // panel's delete action calls transcriptionController.trash(), a worker-thread
    // TrashTask whose completion calls back into the model (applyRemovalOutcome)
    // to fire fileDeleted. The sync tests above call m_model->trash*() directly
    // and so never exercised this hop; the "open file stays open after delete"
    // bug lived exactly here (a missing controller->model wiring makes the whole
    // path silent). Undo (the generic snackbar) then restores + reopens.
    void asyncControllerDelete_closesOpenFile_andUndoReopensIt()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QSignalSpy undoSpy(m_controller, &TranscriptionController::undoAvailable);

        // The panel's own "Delete file" — the async controller.trash() path.
        QVERIFY(QMetaObject::invokeMethod(m_panel, "requestDeleteFile"));

        // Task runs on the instant worker thread; applyRemovalOutcome then fires
        // fileDeleted on the GUI thread and the panel clears. If the controller
        // isn't wired to the model, this never happens (the reported bug).
        QTRY_COMPARE_WITH_TIMEOUT(playerSource(), QUrl(), 5000);

        // The completed trash advertised one undo; undoing it restores + reopens.
        QTRY_COMPARE(undoSpy.count(), 1);
        m_controller->undoTask(undoSpy.constFirst().at(0).toInt());
        QTRY_COMPARE_WITH_TIMEOUT(playerSource(), QUrl::fromLocalFile(m_audioPath), 5000);
    }

    // An orphaned .md is a file too, and reopening it needs the text-only view —
    // hence fileRestored's isOrphanedMd flag. Sending it to the player instead
    // would be a silent wrong-view bug, not a crash.
    void undoingAnOrphanedMdDelete_reopensItAsText()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        const QString notes = m_vaultDir.filePath(QStringLiteral("notes.md"));
        QVERIFY(writeFile(notes, transcript()));
        m_panel->setProperty("orphanedMdPath", notes);

        const QVariantMap result = m_model->trashFiles({ QUrl::fromLocalFile(notes) });
        QCOMPARE(result.value(QStringLiteral("trashed")).toInt(), 1);
        QTRY_VERIFY(orphanedMdPath().isEmpty()); // precondition: the delete closed it

        QCOMPARE(m_model->restoreFromTrash(result.value(QStringLiteral("restorable")).toList()), 1);

        QTRY_COMPARE_WITH_TIMEOUT(orphanedMdPath(), notes, 2000);
        QVERIFY2(playerSource().isEmpty(),
                 "an orphaned .md was reopened in the audio view — there is no audio");

        QFile::remove(notes);
    }

    // The guard on all of the above: a file deleted from the TREE, never opened
    // here, must not be opened by its undo. The panel had nothing on screen and
    // must still have nothing.
    void undoingADeleteOfAFileThatWasNeverOpen_opensNothing()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        // Nothing open at all — the state the panel starts a session in.
        QCOMPARE(playerSource(), QUrl());
        QVERIFY(orphanedMdPath().isEmpty());

        const QVariantMap result = m_model->trashFiles({ QUrl::fromLocalFile(m_audioPath) });
        QCOMPARE(result.value(QStringLiteral("trashed")).toInt(), 1);
        QCOMPARE(m_model->restoreFromTrash(result.value(QStringLiteral("restorable")).toList()), 2);

        // Give the handler a chance to misfire before declaring it innocent.
        QTest::qWait(100);
        QVERIFY2(playerSource().isEmpty(),
                 "undoing a delete opened a file the panel never had open");
        QVERIFY(orphanedMdPath().isEmpty());
    }

    // The other half of that guard: having MOVED ON to another file, an undo of
    // the earlier delete must put the file back in the vault WITHOUT dragging the
    // panel off what the user is now reading.
    void undoingADeleteAfterOpeningAnotherFile_leavesThatFileOpen()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        const QString otherAudio = m_vaultDir.filePath(QStringLiteral("other.mp3"));
        QVERIFY(writeFile(otherAudio, QStringLiteral("not-really-audio")));

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        const QVariantMap result = m_model->trashFiles({ QUrl::fromLocalFile(m_audioPath) });
        QCOMPARE(result.value(QStringLiteral("trashed")).toInt(), 1);
        QTRY_COMPARE(playerSource(), QUrl());

        // The user moves on rather than pressing Undo straight away.
        m_player->setSource(QUrl::fromLocalFile(otherAudio));

        QCOMPARE(m_model->restoreFromTrash(result.value(QStringLiteral("restorable")).toList()), 2);
        QTest::qWait(100);
        QCOMPARE(playerSource(), QUrl::fromLocalFile(otherAudio));
        QVERIFY2(QFileInfo::exists(m_audioPath), "the undo must still restore the file itself");

        QFile::remove(otherAudio);
    }

    // An undo of an UNRELATED file's delete must not yank the open file around —
    // the restore signals carry a url for exactly this reason, and the handlers
    // match it against what's open before touching anything.
    void undoingAnotherFilesDelete_leavesTheOpenFileAlone()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        const QString otherAudio = m_vaultDir.filePath(QStringLiteral("other.mp3"));
        const QString otherMd = m_vaultDir.filePath(QStringLiteral("other.mp3.md"));
        QVERIFY(writeFile(otherAudio, QStringLiteral("not-really-audio")));
        QVERIFY(writeFile(otherMd, transcript()));

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));

        const QVariantMap result = m_model->trashAudioOnly({ QUrl::fromLocalFile(otherAudio) });
        QCOMPARE(result.value(QStringLiteral("trashed")).toInt(), 1);
        QCOMPARE(m_model->restoreFromTrash(result.value(QStringLiteral("restorable")).toList()), 1);

        // Give the handlers a chance to misfire before declaring them innocent.
        QTest::qWait(100);
        QCOMPARE(playerSource(), QUrl::fromLocalFile(m_audioPath));
        QVERIFY(orphanedMdPath().isEmpty());

        QFile::remove(otherAudio);
        QFile::remove(otherMd);
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: MainPanel.qml loads the AppSettings
    // singleton, whose Settings{} element must NEVER touch the real
    // ~/.config/suopTranscriber (CLAUDE.md: never let testing touch the user's
    // real vault or app config). Same setup as tst_mainpanel_chapterrefresh.cpp.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_mainpanel_deleteundo"));
    QGuiApplication app(argc, argv);
    TstMainPanelDeleteUndo tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_mainpanel_deleteundo.moc"
