// Living spec for MainPanel's gutter speaker filter surviving speaker edits.
// Clicking a gutter name sets gutterSpeakerFilter and speakerDimModel dims every
// sentence whose DISPLAYED speaker differs. The reported bug: renaming the
// filtered speaker changed every displayed name out from under the filter, so
// nothing matched and the whole transcript went dark. The filter now follows a
// rename/merge, and a filter naming no present speaker counts as no filter.
//
// Same harness shape as tst_mainpanel_guttertimestamp.cpp, except the
// controller is a stub that records renameSpeaker/mergeSpeakers and lets the
// test emit their completion signals — the handlers under test are the
// panel's, not the controller's. Nothing here touches the user's vault or
// ~/.config.

#include <QtTest>

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>

#include "Clipboard.h"
#include "TranscriptDisplayBuilder.h"
#include "UndoStack.h"
#include "IconTheme.h"

namespace {

class AudioPlayerStub : public QObject { Q_OBJECT };
class AudioRecorderStub : public QObject { Q_OBJECT };
class VaultTreeModelStub : public QObject { Q_OBJECT };
class UsbGrabberControllerStub : public QObject { Q_OBJECT };

// Only the speaker-edit surface; other controller calls MainPanel makes just
// warn (TypeError) and are irrelevant to the filter.
class TranscriptionControllerStub : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    QStringList calls;
    Q_INVOKABLE void renameSpeaker(const QUrl &, const QString &from, const QString &to)
    {
        calls << QStringLiteral("rename:%1>%2").arg(from, to);
    }
    Q_INVOKABLE void mergeSpeakers(const QUrl &, const QString &loser, const QString &winner)
    {
        calls << QStringLiteral("merge:%1>%2").arg(loser, winner);
    }
signals:
    void speakerRenamed(const QUrl &fileUrl, const QString &oldName, bool ok, const QString &error);
    void speakersMerged(const QUrl &fileUrl, const QString &sourceName, const QString &targetName, bool ok,
                        const QString &error);
};

QVariantMap sentence(qreal t0, const QString &speaker)
{
    QVariantMap m;
    m.insert(QStringLiteral("t0"), t0);
    m.insert(QStringLiteral("text"), QStringLiteral("line %1").arg(t0));
    m.insert(QStringLiteral("speaker"), speaker);
    m.insert(QStringLiteral("repeatCount"), 1);
    return m;
}

// Alternating turns, 2 lines each speaker.
QVariantList turns(const QString &a, const QString &b)
{
    return {sentence(0, a), sentence(1, b), sentence(2, a), sentence(3, b)};
}

} // namespace

class TstMainPanelSpeakerFilter : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;
    QQuickItem *m_panel = nullptr;
    TranscriptionControllerStub *m_controller = nullptr;

    QQuickItem *openPanel(QQuickView *view, const QVariantList &sentences)
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

        panel->setProperty("transcriptBody", QStringLiteral("body"));
        panel->setProperty("sentences", sentences);
        panel->setProperty("chapters", QVariantList{});

        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        QTest::qWait(50);
        m_panel = panel;
        return panel;
    }

    QString filter() const { return m_panel->property("gutterSpeakerFilter").toString(); }
    int dimCount() const { return m_panel->property("speakerDimModel").toList().size(); }
    void call(const char *fn, const QVariant &a, const QVariant &b)
    {
        QVariant out;
        QMetaObject::invokeMethod(m_panel, fn, Q_RETURN_ARG(QVariant, out), Q_ARG(QVariant, a), Q_ARG(QVariant, b));
    }
    void call(const char *fn)
    {
        QVariant out;
        QMetaObject::invokeMethod(m_panel, fn, Q_RETURN_ARG(QVariant, out));
    }

    bool openWithController(QQuickView *view, const QVariantList &sentences)
    {
        if (!openPanel(view, sentences))
            return false;
        m_controller = new TranscriptionControllerStub(view);
        m_panel->setProperty("transcriptionController", QVariant::fromValue<QObject *>(m_controller));
        return m_panel->property("transcriptionController").value<QObject *>() == m_controller;
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
                      "CompressButton 1.0 qml/CompressButton.qml\n"
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
        // RecorderPage now owns an instantiable `AudioPlayer { id: recorderPlayer }`
        // (its own playback for the last recording, separate from MainPanel's
        // `player`), so the type must be creatable — the same reason UndoStack
        // is qmlRegisterType above, not uncreatable.
        qmlRegisterType<AudioPlayerStub>("SuopTranscriber", 1, 0, "AudioPlayer");
        qmlRegisterUncreatableType<AudioRecorderStub>(
            "SuopTranscriber", 1, 0, "AudioRecorder", QStringLiteral("stub — this harness leaves it null"));
        qmlRegisterUncreatableType<TranscriptionControllerStub>(
            "SuopTranscriber", 1, 0, "TranscriptionController", QStringLiteral("stub — set from C++"));
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


    // The general guard: a filter on a speaker no longer in the transcript
    // (re-diarize, external edit) must not dim everything.
    void dimModel_filterOnVanishedSpeaker_dimsNothing()
    {
        QQuickView view;
        QVERIFY(openPanel(&view, turns(QStringLiteral("A"), QStringLiteral("B"))));
        m_panel->setProperty("gutterSpeakerFilter", QStringLiteral("A"));
        QCOMPARE(dimCount(), 2);

        m_panel->setProperty("sentences", QVariant(turns(QStringLiteral("C"), QStringLiteral("B"))));
        QCOMPARE(dimCount(), 0);
    }

    // The reported bug: rename the filtered speaker. The optimistic overlay
    // changes the displayed name at once, so the filter must follow at once.
    void rename_filterFollowsNewName()
    {
        QQuickView view;
        QVERIFY(openWithController(&view, turns(QStringLiteral("A"), QStringLiteral("B"))));
        m_panel->setProperty("gutterSpeakerFilter", QStringLiteral("A"));

        call("commitSpeakerRename", QStringLiteral("A"), QStringLiteral("Alice"));
        QCOMPARE(m_controller->calls, QStringList{QStringLiteral("rename:A>Alice")});
        QCOMPARE(filter(), QStringLiteral("Alice"));
        QCOMPARE(dimCount(), 2); // only B's lines, not the whole transcript

        emit m_controller->speakerRenamed(QUrl(), QStringLiteral("A"), true, QString());
        m_panel->setProperty("sentences", QVariant(turns(QStringLiteral("Alice"), QStringLiteral("B"))));
        QCOMPARE(filter(), QStringLiteral("Alice"));
        QCOMPARE(dimCount(), 2);

        // Undo replays the reverse rename through the same overlay.
        call("undoLast");
        QCOMPARE(m_controller->calls.last(), QStringLiteral("rename:Alice>A"));
        QCOMPARE(filter(), QStringLiteral("A"));
        QCOMPARE(dimCount(), 2);
    }

    // Renaming a DIFFERENT speaker leaves the filter alone.
    void rename_otherSpeaker_filterUnchanged()
    {
        QQuickView view;
        QVERIFY(openWithController(&view, turns(QStringLiteral("A"), QStringLiteral("B"))));
        m_panel->setProperty("gutterSpeakerFilter", QStringLiteral("A"));
        call("commitSpeakerRename", QStringLiteral("B"), QStringLiteral("Bob"));
        QCOMPARE(filter(), QStringLiteral("A"));
        QCOMPARE(dimCount(), 2);
    }

    // A rejected rename reverts the display to the old name; the filter too.
    void renameFailure_filterReverts()
    {
        QQuickView view;
        QVERIFY(openWithController(&view, turns(QStringLiteral("A"), QStringLiteral("B"))));
        m_panel->setProperty("gutterSpeakerFilter", QStringLiteral("A"));
        call("commitSpeakerRename", QStringLiteral("A"), QStringLiteral("Alice"));
        QCOMPARE(filter(), QStringLiteral("Alice"));

        emit m_controller->speakerRenamed(QUrl(), QStringLiteral("A"), false, QStringLiteral("nope"));
        QCOMPARE(filter(), QStringLiteral("A"));
        QCOMPARE(dimCount(), 2);
    }

    // Merging the filtered speaker away: the filter follows to the winner.
    void merge_filterFollowsWinner()
    {
        QQuickView view;
        QVERIFY(openWithController(&view, turns(QStringLiteral("A"), QStringLiteral("B"))));
        m_panel->setProperty("gutterSpeakerFilter", QStringLiteral("A"));
        call("startSpeakerMerge", QStringLiteral("A"), QStringLiteral("B"));
        QCOMPARE(m_controller->calls, QStringList{QStringLiteral("merge:A>B")});

        emit m_controller->speakersMerged(QUrl(), QStringLiteral("A"), QStringLiteral("B"), true, QString());
        QCOMPARE(filter(), QStringLiteral("B"));
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_mainpanel_speakerfilter"));
    QGuiApplication app(argc, argv);
    TstMainPanelSpeakerFilter tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_mainpanel_speakerfilter.moc"
