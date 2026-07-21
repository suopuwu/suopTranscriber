// Living spec (queue item 028): opening a file from the sidebar (or the bottom
// bar) must drop out of ANY open top-level screen — the recorder, the USB
// grabber, or the benchmark page. Every file-open funnels through MainPanel's
// showAudioFile() / showOrphanedMd(), so those two functions call
// exitTopLevelScreens() which clears all three screen flags. A source-scan
// styleguide guard could see the call is SHAPED right but not that the flags
// actually clear when the funnel runs — so drive the real QML.
//
// Same harness shape as tst_mainpanel_chapterselection.cpp (read its top-of-
// file comment first): QQuickView, offscreen, scratch QML module dir symlinking
// the real qml/, sandboxed org/app name in main(). ONE difference: AudioPlayer
// is a REAL creatable stub with a settable `source` QUrl property (copied from
// tst_mainpanel_renameundo.cpp), because both show* funnels assign
// mainPanel.player.source and would null-deref against a bare QObject. The
// other four C++ property types stay bare/null — showOrphanedMd's
// refreshMetadata() guards on transcriptionController being non-null, so a null
// controller yields metadata={} and does not crash.
//
// Nothing here touches the user's vault or ~/.config/suopTranscriber.

#include <QtTest>

#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QUrl>

#include "Clipboard.h"
#include "TranscriptDisplayBuilder.h"
#include "UndoStack.h"
#include "IconTheme.h"

namespace {

// The AudioPlayer members MainPanel reads on a file open, and no more — `source`
// is the panel's "an audio file is open" flag. Copied from
// tst_mainpanel_renameundo.cpp's stub.
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

// The other four MainPanel property types only need to RESOLVE; the harness
// leaves them null. See tst_mainpanel_chapterselection.cpp.
class AudioRecorderStub : public QObject { Q_OBJECT };
class TranscriptionControllerStub : public QObject { Q_OBJECT };
class VaultTreeModelStub : public QObject { Q_OBJECT };
class UsbGrabberControllerStub : public QObject { Q_OBJECT };

} // namespace

class TstMainPanelExitTopLevel : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;
    QQuickItem *m_panel = nullptr;

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

        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        m_panel = panel;
        return panel;
    }

    void setAllScreensOpen()
    {
        m_panel->setProperty("recording", true);
        m_panel->setProperty("usbGrabberOpen", true);
        m_panel->setProperty("benchmarkOpen", true);
        QCOMPARE(m_panel->property("recording").toBool(), true);
        QCOMPARE(m_panel->property("usbGrabberOpen").toBool(), true);
        QCOMPARE(m_panel->property("benchmarkOpen").toBool(), true);
    }

    void assertAllScreensClosed()
    {
        QCOMPARE(m_panel->property("recording").toBool(), false);
        QCOMPARE(m_panel->property("usbGrabberOpen").toBool(), false);
        QCOMPARE(m_panel->property("benchmarkOpen").toBool(), false);
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
        // AudioPlayer is CREATABLE here (unlike chapterselection's harness): the
        // driver instantiates one and assigns it to MainPanel.player so the
        // show* funnels have a real `source` to write.
        qmlRegisterType<AudioPlayerStub>("SuopTranscriber", 1, 0, "AudioPlayer");
        qmlRegisterUncreatableType<AudioRecorderStub>(
            "SuopTranscriber", 1, 0, "AudioRecorder", QStringLiteral("stub — this harness leaves it null"));
        qmlRegisterUncreatableType<TranscriptionControllerStub>(
            "SuopTranscriber", 1, 0, "TranscriptionController", QStringLiteral("stub — this harness leaves it null"));
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
            "    AudioPlayer { id: playerStub }\n"
            "    MainPanel {\n"
            "        objectName: \"mainPanel\"\n"
            "        anchors.fill: parent\n"
            "        player: playerStub\n"
            "    }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driverFile(m_driverQmlPath);
        QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
        driverFile.write(driverQml.toUtf8());
        driverFile.close();
    }

    // Opening an audio file drops every open top-level screen and repoints the
    // player at the file.
    void showAudioFile_exitsAllTopLevelScreens()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        setAllScreensOpen();

        QMetaObject::invokeMethod(m_panel, "showAudioFile", Q_ARG(QVariant, QStringLiteral("/tmp/x.opus")));

        assertAllScreensClosed();
        const QObject *player = m_panel->property("player").value<QObject *>();
        QVERIFY(player);
        QCOMPARE(player->property("source").toUrl(), QUrl(QStringLiteral("file:///tmp/x.opus")));
    }

    // Opening an orphaned .md drops every open top-level screen too — it goes
    // through the second funnel.
    void showOrphanedMd_exitsAllTopLevelScreens()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        setAllScreensOpen();

        QMetaObject::invokeMethod(m_panel, "showOrphanedMd", Q_ARG(QVariant, QStringLiteral("/tmp/x.md")));

        assertAllScreensClosed();
        QCOMPARE(m_panel->property("orphanedMdPath").toString(), QStringLiteral("/tmp/x.md"));
    }

    // No screen is special-cased: with only ONE screen open, the funnel still
    // clears exactly that one (and leaves the already-false others false).
    void showAudioFile_clearsEachScreenIndividually_data()
    {
        QTest::addColumn<QString>("flag");
        QTest::newRow("recording") << QStringLiteral("recording");
        QTest::newRow("usbGrabberOpen") << QStringLiteral("usbGrabberOpen");
        QTest::newRow("benchmarkOpen") << QStringLiteral("benchmarkOpen");
    }

    void showAudioFile_clearsEachScreenIndividually()
    {
        QFETCH(QString, flag);
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        m_panel->setProperty(flag.toUtf8().constData(), true);
        QCOMPARE(m_panel->property(flag.toUtf8().constData()).toBool(), true);

        QMetaObject::invokeMethod(m_panel, "showAudioFile", Q_ARG(QVariant, QStringLiteral("/tmp/x.opus")));

        assertAllScreensClosed();
    }

    // Queue item 037: setTopLevelScreen() makes "two screens open at once"
    // unrepresentable — opening one closes every other, and "" closes them all.
    void setTopLevelScreen_openingOneClosesTheOthers()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");
        setAllScreensOpen();

        QMetaObject::invokeMethod(m_panel, "setTopLevelScreen", Q_ARG(QVariant, QStringLiteral("benchmark")));
        QCOMPARE(m_panel->property("benchmarkOpen").toBool(), true);
        QCOMPARE(m_panel->property("recording").toBool(), false);
        QCOMPARE(m_panel->property("usbGrabberOpen").toBool(), false);
        QCOMPARE(m_panel->property("topLevelScreenOpen").toBool(), true);

        QMetaObject::invokeMethod(m_panel, "setTopLevelScreen", Q_ARG(QVariant, QStringLiteral("")));
        assertAllScreensClosed();
        QCOMPARE(m_panel->property("topLevelScreenOpen").toBool(), false);
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_mainpanel_exittoplevel"));
    QGuiApplication app(argc, argv);
    TstMainPanelExitTopLevel tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_mainpanel_exittoplevel.moc"
