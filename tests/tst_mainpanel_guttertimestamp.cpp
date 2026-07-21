// Living spec + runtime verification for MainPanel.qml's gutter timestamp
// padding (queue item 024): each per-segment gutter timestamp is zero-padded to
// fit the file's LARGEST timestamp (gutterMaxT0) so the transcript's left-edge
// column reads as an aligned ruler. formatGutterTimestamp(t0) is a pure QML
// function whose padding tier + leading-unit width come from the file's own
// max, so this drives it directly through the `sentences` property (which feeds
// displaySentences -> gutterMaxT0) and QCOMPAREs its output.
//
// Same harness shape as tst_mainpanel_chapterselection.cpp (read its top-of-file
// comment): QQuickView, offscreen, scratch QML module dir symlinking the real
// qml/, sandboxed org/app name in main(), five empty C++ property stubs that
// stay null. Nothing here touches the user's vault or ~/.config.

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

// Same reasoning as tst_mainpanel_chapterselection.cpp: MainPanel guards every
// use site on these being non-null, so each property only needs its TYPE to
// resolve for the file to load; all five stay null here.
class AudioPlayerStub : public QObject { Q_OBJECT };
class AudioRecorderStub : public QObject { Q_OBJECT };
class TranscriptionControllerStub : public QObject { Q_OBJECT };
class VaultTreeModelStub : public QObject { Q_OBJECT };
class UsbGrabberControllerStub : public QObject { Q_OBJECT };

QVariantMap sentence(qreal t0, const QString &text)
{
    QVariantMap m;
    m.insert(QStringLiteral("t0"), t0);
    m.insert(QStringLiteral("text"), text);
    m.insert(QStringLiteral("speaker"), QString());
    m.insert(QStringLiteral("repeatCount"), 1);
    return m;
}

} // namespace

class TstMainPanelGutterTimestamp : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;
    QQuickItem *m_panel = nullptr;

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

    // The file's span is set by its max displayed segment t0 (gutterMaxT0).
    QString formatFor(qreal fileMax, qreal t0)
    {
        QVariant out;
        // A single sentence at the file's max fixes gutterMaxT0; the function's
        // own argument is what we're formatting.
        if (!m_panel)
            return QString();
        m_panel->setProperty("sentences", QVariantList{sentence(fileMax, QStringLiteral("x"))});
        QMetaObject::invokeMethod(m_panel, "formatGutterTimestamp",
                                  Q_RETURN_ARG(QVariant, out), Q_ARG(QVariant, QVariant(t0)));
        return out.toString();
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
        qmlRegisterUncreatableType<AudioPlayerStub>(
            "SuopTranscriber", 1, 0, "AudioPlayer", QStringLiteral("stub — this harness leaves it null"));
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

    // A 3-second row is padded to the width of the file's largest timestamp: its
    // TIER (seconds/minutes/hours) and the leading unit's digit-count both come
    // from the file max, not the row's own value.
    void formatGutterTimestamp_padsShortRowsToFileMax()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, QVariantList{sentence(0, QStringLiteral("a"))}),
                 "could not load MainPanel.qml / locate it");

        QCOMPARE(formatFor(36000.0, 3.0), QStringLiteral("00:00:03")); // >9h  -> 2-digit hours
        QCOMPARE(formatFor(3600.0, 3.0), QStringLiteral("0:00:03"));   // <=9h -> 1-digit hours
        QCOMPARE(formatFor(720.0, 3.0), QStringLiteral("00:03"));      // 10+ min -> 2-digit minutes
        QCOMPARE(formatFor(300.0, 3.0), QStringLiteral("0:03"));       // <10 min -> 1-digit minutes
        QCOMPARE(formatFor(45.0, 3.0), QStringLiteral("03"));          // sub-minute, 2-digit seconds
        QCOMPARE(formatFor(7.0, 3.0), QStringLiteral("3"));            // under 10s, 1-digit seconds
    }

    // A timestamp AT the file's max renders in full at that tier's width.
    void formatGutterTimestamp_maxRowRendersInFull()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, QVariantList{sentence(0, QStringLiteral("a"))}),
                 "could not load MainPanel.qml / locate it");

        QCOMPARE(formatFor(720.0, 720.0), QStringLiteral("12:00"));
        QCOMPARE(formatFor(36123.0, 36123.0), QStringLiteral("10:02:03"));
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
    QCoreApplication::setApplicationName(QStringLiteral("tst_mainpanel_guttertimestamp"));
    QGuiApplication app(argc, argv);
    TstMainPanelGutterTimestamp tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_mainpanel_guttertimestamp.moc"
