// Living spec for queue item 072: the app-wide "click outside a focused text
// field blurs (commits) it" behavior, made UNIVERSAL by a C++ application-level
// event filter (FocusBlurFilter) installed on qApp.
//
// Why this test and not a source scan: the whole point of the C++ filter is that
// it sees the press BEFORE any QML grab, so a Flickable/ScrollView/MouseArea
// swallowing the press can no longer defeat the blur. The reported bug was
// exactly this — clicking the empty transcript body (a ScrollView that arms
// scrolling on every press) never reached the QML root TapHandlers, so a focused
// title never committed. Only a real QQuickView that loads the REAL MainPanel.qml
// (whose body IS that always-present ScrollView) exercises the path a source scan
// structurally cannot see. So this harness installs the PRODUCTION FocusBlurFilter
// on qApp and drives a synthetic press over the ScrollView.
//
// Same harness shape as tst_mainpanel_renameundo.cpp (read its top-of-file
// comment first): QQuickView, offscreen, scratch QML module dir symlinking the
// real qml/, sandboxed org/app name in main(), REAL TranscriptionController +
// REAL VaultTreeModel over a QTemporaryDir vault, functional AudioPlayerStub.
//
// Nothing here touches the user's vault or ~/.config/suopTranscriber.

#include <QtTest>

#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>

#include "Clipboard.h"
#include "TranscriptDisplayBuilder.h"
#include "UndoStack.h"
#include "FocusBlurFilter.h"
#include "IconTheme.h"
#include "TranscriptionController.h"
#include "VaultTreeModel.h"

namespace {

// See tst_mainpanel_renameundo.cpp's identical stubs.
class AudioRecorderStub : public QObject { Q_OBJECT };
class UsbGrabberControllerStub : public QObject { Q_OBJECT };

// The AudioPlayer members MainPanel reads, and no more — `source` is the panel's
// "an audio file is open" flag.
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

class TstFocusBlurFilter : public QObject
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
        panel->setProperty("vaultLocalPath", m_vaultDir.path());

        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        m_panel = panel;
        m_titleField = panel->findChild<QQuickItem *>(QStringLiteral("titleField"));
        return panel;
    }

    // A synthetic LEFT press at `scenePt` delivered straight to the window, so
    // the qApp-installed filter sees it exactly as in production (before any QML
    // grab). scenePosition() equals the constructed localPos for a window event.
    static void pressAtScene(QWindow *win, const QPointF &scenePt)
    {
        QMouseEvent ev(QEvent::MouseButtonPress, scenePt, scenePt,
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QGuiApplication::sendEvent(win, &ev);
    }

    // Center of `item` in scene (== window) coordinates.
    static QPointF sceneCenter(QQuickItem *item)
    {
        return item->mapToScene(QPointF(item->width() / 2.0, item->height() / 2.0));
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

        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterSingletonType<Clipboard>(
            "SuopTranscriber", 1, 0, "Clipboard",
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
        qmlRegisterType<VaultTreeModel>("SuopTranscriber", 1, 0, "VaultTreeModel");
        // RecorderPage now owns an instantiable `AudioPlayer { id: recorderPlayer }`
        // (its own playback for the last recording, separate from MainPanel's
        // `player`, which this harness still constructs in C++ below), so the
        // type must ALSO be creatable from QML — the same reason UndoStack is
        // qmlRegisterType above, not uncreatable.
        qmlRegisterType<AudioPlayerStub>("SuopTranscriber", 1, 0, "AudioPlayer");
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

    void init()
    {
        m_audioPath = m_vaultDir.filePath(QStringLiteral("talk.m4a"));
        m_mdPath = m_vaultDir.filePath(QStringLiteral("talk.m4a.md"));
        QVERIFY(writeFile(m_audioPath, QStringLiteral("not-really-audio")));
        QVERIFY(writeFile(m_mdPath, transcript()));
    }

    void cleanup()
    {
        QFile::remove(m_audioPath);
        QFile::remove(m_mdPath);
    }

    // Gap A (the reported bug): a press in the empty transcript body — which is
    // an always-present ScrollView that grabs every press to arm scrolling, so
    // it never reached the QML root TapHandlers — must STILL blur the focused
    // title, because the app-level filter observes the press before any QML grab.
    void outsidePressOverScrollView_blursFocusedTitle()
    {
        FocusBlurFilter filter;
        qApp->installEventFilter(&filter);
        auto guard = qScopeGuard([&] { qApp->removeEventFilter(&filter); });

        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        QVERIFY2(m_titleField, "titleField must expose objectName for this test");

        m_player->setSource(QUrl::fromLocalFile(m_audioPath));
        QTRY_COMPARE(m_titleField->property("text").toString(), QStringLiteral("talk.m4a"));

        auto *scroll = m_panel->findChild<QQuickItem *>(QStringLiteral("transcriptScrollView"));
        QVERIFY2(scroll, "transcriptScrollView must be locatable (ScrollPage bodyObjectName alias)");

        m_titleField->forceActiveFocus();
        QVERIFY2(m_titleField->hasActiveFocus(), "titleField did not take focus");

        // A point over the empty ScrollView body, well below and outside titleField.
        pressAtScene(&view, sceneCenter(scroll));

        QTRY_VERIFY2(!m_titleField->hasActiveFocus(),
                     "a press over the transcript ScrollView must blur the focused title "
                     "despite the ScrollView eating the press (queue item 072, gap A)");
    }

    // The press-vs-bounds rule (a press INSIDE the focused field must KEEP focus,
    // so a drag-select starting in the field never self-blurs it) and the
    // genericity that closes completeness gap B (the filter keys on "is an
    // editable text primitive", NOT a specific id, so ANY field blurs).
    //
    // Proven against two explicitly-SIZED plain TextFields rather than MainPanel's
    // own fields: MainPanel's title/subtitle collapse to ~0 width in the offscreen
    // layout (a harness quirk unrelated to the filter), which makes a bounds-
    // precise inside/outside assertion meaningless there. The filter is generic C++
    // over any QQuickTextInput/QQuickTextEdit, so two real (deterministically
    // placed) TextFields exercise the exact same code path — see
    // outsidePressOverScrollView_blursFocusedTitle above for the real-MainPanel
    // ScrollView-swallow proof (gap A).
    QQuickItem *openSizedFields(QQuickView *view)
    {
        const QString qml = QStringLiteral(
            "import QtQuick\n"
            "import QtQuick.Controls\n"
            "Item {\n"
            "    width: 400\n"
            "    height: 400\n"
            "    TextField { objectName: \"fieldA\"; x: 20; y: 20; width: 150; height: 30 }\n"
            "    TextField { objectName: \"fieldB\"; x: 20; y: 300; width: 150; height: 30 }\n"
            "}\n");
        const QString path = m_moduleDir.filePath(QStringLiteral("sizedfields.qml"));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return nullptr;
        f.write(qml.toUtf8());
        f.close();
        view->setSource(QUrl::fromLocalFile(path));
        if (view->status() != QQuickView::Ready)
            return nullptr;
        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        return view->rootObject();
    }

    void insidePress_keepsFocus()
    {
        FocusBlurFilter filter;
        qApp->installEventFilter(&filter);
        auto guard = qScopeGuard([&] { qApp->removeEventFilter(&filter); });

        QQuickView view;
        QQuickItem *root = openSizedFields(&view);
        QVERIFY2(root, "sized-fields QML failed to load");
        auto *fieldA = root->findChild<QQuickItem *>(QStringLiteral("fieldA"));
        QVERIFY(fieldA);

        fieldA->forceActiveFocus();
        QVERIFY(fieldA->hasActiveFocus());

        // A press squarely inside fieldA's own bounds.
        pressAtScene(&view, sceneCenter(fieldA));

        // Give the filter a chance to (wrongly) blur before declaring it innocent.
        QTest::qWait(50);
        QVERIFY2(fieldA->hasActiveFocus(),
                 "a press inside the field must not self-blur (drag-select starts inside)");
    }

    void outsidePress_blursAnyField_notJustTitle()
    {
        FocusBlurFilter filter;
        qApp->installEventFilter(&filter);
        auto guard = qScopeGuard([&] { qApp->removeEventFilter(&filter); });

        QQuickView view;
        QQuickItem *root = openSizedFields(&view);
        QVERIFY2(root, "sized-fields QML failed to load");
        auto *fieldA = root->findChild<QQuickItem *>(QStringLiteral("fieldA"));
        auto *fieldB = root->findChild<QQuickItem *>(QStringLiteral("fieldB"));
        QVERIFY(fieldA && fieldB);

        // fieldA: a press over the OTHER field (outside fieldA) must blur it.
        fieldA->forceActiveFocus();
        QVERIFY(fieldA->hasActiveFocus());
        pressAtScene(&view, sceneCenter(fieldB));
        QTRY_VERIFY2(!fieldA->hasActiveFocus(),
                     "an outside press must blur the focused field (queue item 072)");

        // fieldB: same again for a DIFFERENT field — proving the blur isn't keyed
        // to one specific field id (completeness gap B).
        fieldB->forceActiveFocus();
        QVERIFY(fieldB->hasActiveFocus());
        pressAtScene(&view, sceneCenter(fieldA));
        QTRY_VERIFY2(!fieldB->hasActiveFocus(),
                     "an outside press must blur ANY focused field, not just one "
                     "(queue item 072, gap B)");
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: MainPanel.qml loads the AppSettings
    // singleton, whose Settings{} element must NEVER touch the real
    // ~/.config/suopTranscriber (CLAUDE.md).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_focusblurfilter"));
    QGuiApplication app(argc, argv);
    TstFocusBlurFilter tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_focusblurfilter.moc"
