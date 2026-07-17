// Living spec for two regressions the user hit while a transcription of the
// OPEN file was running/finishing:
//
//  1. The per-file action icons (regenerate label, generate chapters, detect
//     speakers, the delete trio) stayed greyed after the job finished — you had
//     to leave the file and re-enter to get them back. Root cause: their
//     `enabled` binding called transcriptionController.isQueued(url), a plain
//     Q_INVOKABLE. Reading an invokable inside a binding subscribes the binding
//     to NOTHING, so once it evaluated to greyed (file Active) it never re-ran
//     when the job left the queue. The fix routes them through
//     MainPanel.busyOrQueuedUrls — a property built from the reactive
//     queuedFileUrls/busy/currentFile Q_PROPERTYs, so the bindings re-evaluate
//     on queueChanged/busyChanged/currentFileChanged. This harness proves that
//     list reflects an active+queued job and, crucially, CLEARS reactively when
//     the jobs finish (the exact staleness that was broken) — no reopen needed.
//
//  2. onFinished() must reload the transcript BODY and follow to the bottom.
//     None of the metadata refreshes touch transcriptBody, and an engine that
//     emits a whole VAD window's segments in one post-decode burst (Parakeet
//     via sherpa-onnx) collapses the throttled live-tick to one update, so the
//     tail never appears until a reopen. Geometry-dependent scroll can't be
//     asserted offscreen and a real transcription needs a model, so this half
//     is a source-scan guard on MainPanel.qml (same technique as
//     tst_styleguide's guards / tst_transcription_queue's benchmarkJob guard).
//
// Harness shape is tst_mainpanel_chapterrefresh.cpp's (read its top comment):
// QQuickView, offscreen, scratch QML module dir symlinking the real qml/, a
// REAL TranscriptionController, sandboxed org/app name + test-mode QSettings in
// main(). The one addition is tst_transcription_queue's VaultFixture trick: a
// zero-byte fake model so transcribe() enqueues, goes Active, then fails fast
// (whisper can't load it) — driving the queue lifecycle with no GPU/model.
//
// Nothing here touches the user's vault or ~/.config/suopTranscriber.

#include <QtTest>

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "Clipboard.h"
#include "IconTheme.h"
#include "TranscriptionController.h"

namespace {

// See tst_mainpanel_chapterrefresh.cpp's identical stubs — each only needs its
// TYPE to resolve; MainPanel's use sites guard on non-null, so all stay null.
// TranscriptionController is deliberately the real class.
class AudioPlayerStub : public QObject { Q_OBJECT };
class AudioRecorderStub : public QObject { Q_OBJECT };
class VaultTreeModelStub : public QObject { Q_OBJECT };
class UsbGrabberControllerStub : public QObject { Q_OBJECT };

constexpr auto kModelId = "base.en";

} // namespace

class TstMainPanelButtonState : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QTemporaryDir m_vaultDir;
    QString m_driverQmlPath;

    // Reads the panel's busyOrQueuedUrls (a JS array of url strings) as a
    // QStringList for assertions.
    static QStringList busyOrQueuedUrls(QQuickItem *panel)
    {
        QStringList out;
        const QVariantList list = panel->property("busyOrQueuedUrls").toList();
        for (const QVariant &v : list)
            out.append(v.toString());
        return out;
    }

    QQuickItem *openPanel(QQuickView *view, TranscriptionController **controllerOut)
    {
        view->engine()->addImportPath(m_moduleDir.path());
        view->engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                          new IconImageProvider);
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : view->errors())
                msg += e.toString() + QStringLiteral("\n");
            // qmlcachegen AOT-compiles a file that still fails to LOAD; report
            // the load error verbatim (a green build does not catch it).
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

        auto *controller = new TranscriptionController(this);
        panel->setProperty("transcriptionController", QVariant::fromValue(controller));
        panel->setProperty("vaultPath", QUrl::fromLocalFile(m_vaultDir.path()));
        if (controllerOut)
            *controllerOut = controller;

        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        return panel;
    }

private slots:
    void initTestCase()
    {
        // TranscriptionController persists phase timings via a plain QSettings
        // (see tst_transcription_queue) — sandbox it away from the real config.
        QStandardPaths::setTestModeEnabled(true);

        QVERIFY(m_moduleDir.isValid());
        QVERIFY(m_vaultDir.isValid());

        // Zero-byte fake model so ensureModel() is satisfied offline and the
        // worker fails fast on load — the whole queue lifecycle, no GPU/model.
        const QString modelsDir = m_vaultDir.filePath(QStringLiteral(".transcriber/models"));
        QVERIFY(QDir().mkpath(modelsDir));
        QFile model(modelsDir + QStringLiteral("/ggml-base.en.bin"));
        QVERIFY(model.open(QIODevice::WriteOnly));
        model.close();

        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
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
        qmlRegisterUncreatableType<AudioPlayerStub>(
            "SuopTranscriber", 1, 0, "AudioPlayer", QStringLiteral("stub — this harness leaves it null"));
        qmlRegisterUncreatableType<AudioRecorderStub>(
            "SuopTranscriber", 1, 0, "AudioRecorder", QStringLiteral("stub — this harness leaves it null"));
        qmlRegisterUncreatableType<VaultTreeModelStub>(
            "SuopTranscriber", 1, 0, "VaultTreeModel", QStringLiteral("stub — this harness leaves it null"));
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

    // Bug 1, the functional heart: busyOrQueuedUrls must reflect an active+queued
    // job and then CLEAR when the jobs finish — the reactive-clearing the old
    // isQueued() bindings never got, which is why the buttons stayed greyed.
    void busyOrQueuedUrls_reflectsActiveAndQueued_thenClearsReactively()
    {
        QQuickView view;
        TranscriptionController *controller = nullptr;
        QQuickItem *panel = openPanel(&view, &controller);
        QVERIFY2(panel, "could not load MainPanel.qml / locate it");

        QVERIFY2(busyOrQueuedUrls(panel).isEmpty(), "should start with nothing queued");

        const QUrl fileA = QUrl::fromLocalFile(m_vaultDir.filePath(QStringLiteral("a.wav")));
        const QUrl fileB = QUrl::fromLocalFile(m_vaultDir.filePath(QStringLiteral("b.wav")));
        // Deterministic pre-event-loop snapshot (see tst_transcription_queue):
        // startJob() runs synchronously in transcribe(), so A is Active and B
        // Queued before the worker's cross-thread finished() can arrive.
        controller->transcribe(fileA, QUrl::fromLocalFile(m_vaultDir.path()), kModelId, false);
        controller->transcribe(fileB, QUrl::fromLocalFile(m_vaultDir.path()), kModelId, false);

        const QStringList active = busyOrQueuedUrls(panel);
        QVERIFY2(active.contains(fileA.toString()),
                 "the Active job's file must be in busyOrQueuedUrls (its buttons greyed)");
        QVERIFY2(active.contains(fileB.toString()),
                 "the Queued job's file must be in busyOrQueuedUrls (its buttons greyed)");

        // The regression: once the jobs finish, the list must empty itself with
        // no file switch / reopen. A stale (non-reactive) source would keep the
        // urls here forever and the buttons greyed.
        QTRY_VERIFY2(busyOrQueuedUrls(panel).isEmpty(),
                     "busyOrQueuedUrls did not clear after the jobs finished — "
                     "the per-file buttons would stay greyed until a reopen");
    }

    // Bug 1, structural: every per-file action button's `enabled` must gate on
    // busyOrQueuedUrls, and NONE may call the non-reactive isQueued(url) in an
    // `enabled` binding (that is the exact staleness above). Covers the buttons
    // whose visibility this offscreen harness can't satisfy (they need a live
    // AudioPlayer.source), so the guard pins all of them uniformly.
    void perFileButtons_gateOnReactiveBusyOrQueuedUrls_notInvokableIsQueued()
    {
        QFile src(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(src.open(QIODevice::ReadOnly), "MainPanel.qml must be readable");
        const QStringList lines = QString::fromUtf8(src.readAll()).split(QLatin1Char('\n'));

        const QStringList buttonIds = {
            QStringLiteral("regenerateLabelButton"), QStringLiteral("generateChaptersButton"),
            QStringLiteral("detectSpeakersButton"),  QStringLiteral("deleteTranscriptButton"),
            QStringLiteral("deleteAudioOnlyButton"),  QStringLiteral("deleteFileButton"),
        };
        QStringList gated;
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith(QStringLiteral("//")))
                continue; // comments may mention isQueued() historically
            // An `enabled:` binding must never reach for the invokable isQueued().
            if (trimmed.contains(QStringLiteral("enabled:")) && trimmed.contains(QStringLiteral(".isQueued(")))
                QFAIL(qPrintable(QStringLiteral("a per-file button gates enabled on the non-reactive "
                                                "isQueued(url) invokable — it goes stale and stays greyed: ")
                                 + trimmed));
            if (trimmed.contains(QStringLiteral("enabled:")) && trimmed.contains(QStringLiteral("busyOrQueuedUrls"))) {
                for (const QString &id : buttonIds)
                    if (trimmed.contains(id + QStringLiteral(".targetUrl")))
                        gated.append(id);
            }
        }
        for (const QString &id : buttonIds)
            QVERIFY2(gated.contains(id),
                     qPrintable(id + QStringLiteral(" must gate enabled on busyOrQueuedUrls.indexOf(targetUrl)")));
    }

    // Bug 2: onFinished() must reload the transcript BODY (nothing else does —
    // the metadata refreshes leave it stale) and, when the finished file is the
    // open one, follow to the bottom. Real transcription needs a model and the
    // scroll is geometry-dependent, so this is a source-scan guard on the
    // handler body (see this file's top comment for why).
    void onFinished_reloadsBodyAndFollowsToBottom()
    {
        QFile src(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(src.open(QIODevice::ReadOnly), "MainPanel.qml must be readable");
        const QString all = QString::fromUtf8(src.readAll());

        const int start = all.indexOf(QStringLiteral("function onFinished("));
        QVERIFY2(start >= 0, "an onFinished handler must exist");
        // Body ends at the next handler in the same Connections block.
        int end = all.indexOf(QStringLiteral("function onProgressChanged("), start);
        QVERIFY(end > start);
        const QString body = all.mid(start, end - start);

        QVERIFY2(body.contains(QStringLiteral("refreshTranscriptSync()")),
                 "onFinished must reload the transcript body — otherwise a burst-emitting engine's "
                 "tail (Parakeet) never appears until the file is reopened");
        QVERIFY2(body.contains(QStringLiteral("contentHeight - flick.height")),
                 "onFinished must follow to the bottom so the final segments scroll into view");
        QVERIFY2(body.contains(QStringLiteral("mainPanel.player.source.toString()")),
                 "onFinished's body reload/scroll must be guarded on the finished file still being open");
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: MainPanel.qml loads AppSettings, whose
    // Settings{} must NEVER touch the real ~/.config/suopTranscriber (CLAUDE.md).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_mainpanel_buttonstate"));
    QGuiApplication app(argc, argv);
    TstMainPanelButtonState tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_mainpanel_buttonstate.moc"
