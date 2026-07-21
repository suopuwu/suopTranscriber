// Living spec: the transcript view follows the newest line as content streams
// in during transcription, even when a single update adds a lot at once.
//
// The bug: the follow measured "am I at the bottom?" against the flickable's
// contentHeight AFTER it had already grown, so any update taller than the 40px
// near-bottom threshold (a multi-segment whisper tick, or a whole-window
// Parakeet burst) read as "the user scrolled up" and following stopped — the
// transcript sat still while text piled up below the fold. A small per-tick
// growth (what an offscreen jfk decode produces) slipped under the threshold
// and hid the defect, which is why it only showed in the real app.
//
// The fix reacts to the flickable's OWN contentHeight change and tests
// at-bottom-ness against the PREVIOUS contentHeight (mainPanel.lastFollowContentHeight),
// gated by mainPanel.followingTail. These two tests drive that path directly
// with a big (>>40px) growth — no model needed, fully deterministic. A third,
// model-gated test runs a real whisper transcription end to end as a sanity
// check that the wiring holds with an actual job.
//
// Harness shape from tst_mainpanel_chapterrefresh.cpp; real-model location trick
// from tst_transcript_format.cpp. Nothing here touches the user's vault/config.

#include <QtTest>

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "AudioDecoder.h"
#include "Clipboard.h"
#include "TranscriptDisplayBuilder.h"
#include "UndoStack.h"
#include "IconTheme.h"
#include "TranscriptionController.h"

namespace {

// Minimal AudioPlayer replacement — MainPanel only reads source/position/
// duration/playing and calls togglePlay().
class AudioPlayerStub : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(qreal position READ position NOTIFY positionChanged)
    Q_PROPERTY(qreal duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
public:
    QUrl source() const { return m_source; }
    void setSource(const QUrl &s) { if (m_source != s) { m_source = s; emit sourceChanged(); } }
    qreal position() const { return 0; }
    qreal duration() const { return 0; }
    bool playing() const { return false; }
    Q_INVOKABLE void togglePlay() {}
signals:
    void sourceChanged();
    void positionChanged();
    void durationChanged();
    void playingChanged();
private:
    QUrl m_source;
};

class AudioRecorderStub : public QObject { Q_OBJECT };
class VaultTreeModelStub : public QObject { Q_OBJECT };
class UsbGrabberControllerStub : public QObject { Q_OBJECT };

QString locateWhisperModel()
{
    const QString env = qEnvironmentVariable("SUOP_TEST_MODEL");
    if (!env.isEmpty() && QFileInfo::exists(env))
        return env;
    const QString real = QStringLiteral("/home/user/recordings/.transcriber/models/ggml-base.en.bin");
    return QFileInfo::exists(real) ? real : QString();
}

QString mdWithLines(int n)
{
    QString body = QStringLiteral("---\nmodel: base\nspeakerDetection: false\n---\n");
    for (int i = 0; i < n; ++i)
        body += QStringLiteral("[%1] Transcript sentence number %2 with several words so it wraps nicely.\n")
                    .arg(i).arg(i);
    return body;
}

} // namespace

class TstMainPanelLiveScroll : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QTemporaryDir m_vaultDir;
    QString m_driverQmlPath;

    static bool writeFile(const QString &path, const QString &contents)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        f.write(contents.toUtf8());
        f.close();
        return true;
    }

    // Decodes jfk.wav to 16k mono, tiles it, writes a canonical 16k mono wav.
    static bool makeTiledWav(const QString &destPath, int n)
    {
        auto pcm = AudioDecoder::decodeTo16kMono(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"));
        if (!pcm || pcm->empty())
            return false;
        std::vector<float> tiled;
        tiled.reserve(pcm->size() * n);
        for (int i = 0; i < n; ++i)
            tiled.insert(tiled.end(), pcm->begin(), pcm->end());
        const quint32 dataLen = static_cast<quint32>(tiled.size() * 2);
        QFile out(destPath);
        if (!out.open(QIODevice::WriteOnly))
            return false;
        QDataStream s(&out);
        s.setByteOrder(QDataStream::LittleEndian);
        auto tag = [&](const char *t) { out.write(t, 4); };
        tag("RIFF"); s << quint32(36 + dataLen); tag("WAVE");
        tag("fmt "); s << quint32(16) << quint16(1) << quint16(1)
                       << quint32(16000) << quint32(32000) << quint16(2) << quint16(16);
        tag("data"); s << dataLen;
        for (float f : tiled) {
            int v = qBound(-32768, int(f * 32767.0f), 32767);
            s << qint16(v);
        }
        out.close();
        return true;
    }

    QQuickItem *openPanel(QQuickView *view, TranscriptionController **cOut, AudioPlayerStub **pOut)
    {
        view->engine()->addImportPath(m_moduleDir.path());
        view->engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                          new IconImageProvider);
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : view->errors())
                msg += e.toString() + QStringLiteral("\n");
            qWarning("%s", qPrintable(msg));
            return nullptr;
        }
        QQuickItem *root = view->rootObject();
        QQuickItem *panel = root ? root->findChild<QQuickItem *>(QStringLiteral("mainPanel")) : nullptr;
        if (!panel)
            return nullptr;
        auto *c = new TranscriptionController(this);
        auto *p = new AudioPlayerStub;
        panel->setProperty("transcriptionController", QVariant::fromValue(c));
        panel->setProperty("player", QVariant::fromValue(static_cast<QObject *>(p)));
        panel->setProperty("vaultPath", QUrl::fromLocalFile(m_vaultDir.path()));
        if (cOut) *cOut = c;
        if (pOut) *pOut = p;
        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        return panel;
    }

    static QObject *flickableOf(QQuickItem *panel)
    {
        QQuickItem *sv = panel->findChild<QQuickItem *>(QStringLiteral("transcriptScrollView"));
        return sv ? sv->property("contentItem").value<QObject *>() : nullptr;
    }
    static qreal cy(QObject *f) { return f->property("contentY").toReal(); }
    static qreal ch(QObject *f) { return f->property("contentHeight").toReal(); }
    static qreal vh(QObject *f) { return f->property("height").toReal(); }

    // Opens a transcribed .md (as an orphaned view — no audio/player needed),
    // waits for its body to land and layout to settle.
    QObject *openMdAndSettle(QQuickItem *panel, const QString &path)
    {
        panel->setProperty("orphanedMdPath", path);
        if (!QTest::qWaitFor([panel]() {
                return !panel->property("transcriptBody").toString().isEmpty();
            }, 5000))
            return nullptr;
        QTest::qWait(250);
        return flickableOf(panel);
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_moduleDir.isValid());
        QVERIFY(m_vaultDir.isValid());

        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        QVERIFY(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")));
        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
        qmldir.write("module SuopTranscriber\n"
                     "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                     "singleton Theme 1.0 qml/Theme.qml\n"
                     "singleton Notifications 1.0 qml/Notifications.qml\n"
                     "AppIcon 1.0 qml/AppIcon.qml\n"
                     "AppIconButton 1.0 qml/AppIconButton.qml\n"
                     "ConfirmPopup 1.0 qml/ConfirmPopup.qml\n"
                     "HoverBorder 1.0 qml/HoverBorder.qml\n"
                     "UnderlineField 1.0 qml/UnderlineField.qml\n"
                     "MiddleDragScroll 1.0 qml/MiddleDragScroll.qml\n"
                     "UsbRegexField 1.0 qml/UsbRegexField.qml\n"
                     "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                     "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                     "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
                     "MainPanel 1.0 qml/MainPanel.qml\n");
        qmldir.close();

        qmlRegisterSingletonType<IconTheme>("SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterSingletonType<Clipboard>("SuopTranscriber", 1, 0, "Clipboard",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new Clipboard; });
        // MainPanel's displayData binding calls into this (the transcript
        // display-model builder lifted out of the QML) — without it the whole
        // panel fails to load.
        qmlRegisterSingletonType<TranscriptDisplayBuilder>(
            "SuopTranscriber", 1, 0, "TranscriptDisplayBuilder",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new TranscriptDisplayBuilder; });
        // MainPanel now owns an instantiable `UndoStack { id: undoStack }`
        // (per-panel undo/redo storage); without this the .qml fails to load.
        qmlRegisterType<UndoStack>("SuopTranscriber", 1, 0, "UndoStack");
        qmlRegisterType<TranscriptionController>("SuopTranscriber", 1, 0, "TranscriptionController");
        qmlRegisterType<AudioPlayerStub>("SuopTranscriber", 1, 0, "AudioPlayer");
        qmlRegisterUncreatableType<AudioRecorderStub>("SuopTranscriber", 1, 0, "AudioRecorder", QStringLiteral("stub"));
        qmlRegisterUncreatableType<VaultTreeModelStub>("SuopTranscriber", 1, 0, "VaultTreeModel", QStringLiteral("stub"));
        qmlRegisterUncreatableType<UsbGrabberControllerStub>("SuopTranscriber", 1, 0, "UsbGrabberController", QStringLiteral("stub"));

        // Small viewport so even a modest transcript overflows and there's a
        // real "bottom" to follow.
        const QString driverQml = QStringLiteral(
            "import QtQuick\nimport SuopTranscriber\n"
            "Item { width: 900; height: 220\n"
            "  MainPanel { objectName: \"mainPanel\"; anchors.fill: parent }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QVERIFY(writeFile(m_driverQmlPath, driverQml));
    }

    // The fix: while following, a large single growth still pins to the bottom.
    // Before the fix this stranded the new content above the fold (the growth
    // exceeded the 40px near-bottom threshold, so it read as "scrolled up").
    void largeGrowthWhileFollowing_snapsToBottom()
    {
        QQuickView view;
        QQuickItem *panel = openPanel(&view, nullptr, nullptr);
        QVERIFY2(panel, "could not load MainPanel.qml");

        const QString md = m_vaultDir.filePath(QStringLiteral("grow.md"));
        QVERIFY(writeFile(md, mdWithLines(12)));
        QObject *flick = openMdAndSettle(panel, md);
        QVERIFY2(flick, "body never loaded / flickable missing");
        QVERIFY2(ch(flick) > vh(flick), "initial content must overflow the viewport");

        // Park at the bottom and arm the follow, exactly as a live transcription
        // does (followingTail set at job start; the user sitting at the bottom).
        flick->setProperty("contentY", ch(flick) - vh(flick));
        QTest::qWait(50);
        panel->setProperty("followingTail", true);

        // A big jump: rewrite the file much longer and poke the sync refresh the
        // way a live segment write does (sentencesRevision bump).
        QVERIFY(writeFile(md, mdWithLines(90)));
        panel->setProperty("sentencesRevision", panel->property("sentencesRevision").toInt() + 1);
        QVERIFY(QTest::qWaitFor([flick]() { return ch(flick) > 1000; }, 3000));
        QTest::qWait(200);

        const qreal bottom = ch(flick) - vh(flick);
        QVERIFY2(qAbs(bottom - cy(flick)) < 30.0,
                 qPrintable(QStringLiteral("after a big growth the view is NOT at the bottom "
                                           "(contentY=%1, bottom=%2) — the follow lost the tail")
                                .arg(cy(flick)).arg(bottom)));
    }

    // O6 is preserved: if the user scrolled UP, a growth must NOT yank them back
    // down (the previous-contentHeight test is what keeps this honest).
    void growthWhileScrolledUp_doesNotYankToBottom()
    {
        QQuickView view;
        QQuickItem *panel = openPanel(&view, nullptr, nullptr);
        QVERIFY2(panel, "could not load MainPanel.qml");

        const QString md = m_vaultDir.filePath(QStringLiteral("grow2.md"));
        QVERIFY(writeFile(md, mdWithLines(12)));
        QObject *flick = openMdAndSettle(panel, md);
        QVERIFY2(flick, "body never loaded / flickable missing");
        QVERIFY(ch(flick) > vh(flick));

        panel->setProperty("followingTail", true);
        flick->setProperty("contentY", 0); // user scrolled to the top
        QTest::qWait(50);

        QVERIFY(writeFile(md, mdWithLines(90)));
        panel->setProperty("sentencesRevision", panel->property("sentencesRevision").toInt() + 1);
        QVERIFY(QTest::qWaitFor([flick]() { return ch(flick) > 1000; }, 3000));
        QTest::qWait(200);

        QVERIFY2(cy(flick) < 200.0,
                 qPrintable(QStringLiteral("a growth yanked a scrolled-up user to the bottom "
                                           "(contentY=%1) — O6 violated").arg(cy(flick))));
    }

    // End-to-end sanity with a real whisper job (gated on a model being present).
    void realTranscription_endsAtBottom()
    {
        const QString model = locateWhisperModel();
        if (model.isEmpty())
            QSKIP("no whisper model — set SUOP_TEST_MODEL to a ggml .bin");

        const QString modelsDir = m_vaultDir.filePath(QStringLiteral(".transcriber/models"));
        QVERIFY(QDir().mkpath(modelsDir));
        qputenv("SUOP_MODELS_DIR", modelsDir.toLocal8Bit()); // app-global store -> this test's dir
        const QString modelDest = modelsDir + QStringLiteral("/ggml-base.en.bin");
        if (!QFileInfo::exists(modelDest))
            QVERIFY(QFile::link(model, modelDest));

        const QString wav = m_vaultDir.filePath(QStringLiteral("clip.wav"));
        QVERIFY(makeTiledWav(wav, 3));

        QQuickView view;
        TranscriptionController *c = nullptr;
        AudioPlayerStub *p = nullptr;
        QQuickItem *panel = openPanel(&view, &c, &p);
        QVERIFY2(panel, "could not load MainPanel.qml");

        const QUrl wavUrl = QUrl::fromLocalFile(wav);
        p->setSource(wavUrl);
        QTest::qWait(100);
        QObject *flick = flickableOf(panel);
        QVERIFY(flick);

        c->transcribe(wavUrl, QUrl::fromLocalFile(m_vaultDir.path()), QStringLiteral("base.en"));
        QElapsedTimer t; t.start();
        while (c->property("busy").toBool() && t.elapsed() < 60000)
            QTest::qWait(150);
        QTest::qWait(400); // onFinished reload + follow

        if (ch(flick) <= vh(flick))
            QSKIP("transcript did not overflow the viewport — nothing to follow");
        const qreal bottom = ch(flick) - vh(flick);
        QVERIFY2(qAbs(bottom - cy(flick)) < 40.0,
                 qPrintable(QStringLiteral("after a real transcription the view is not at the bottom "
                                           "(contentY=%1, bottom=%2)").arg(cy(flick)).arg(bottom)));
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_mainpanel_livescroll"));
    QGuiApplication app(argc, argv);
    TstMainPanelLiveScroll tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_mainpanel_livescroll.moc"
