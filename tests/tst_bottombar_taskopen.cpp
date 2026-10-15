// Living spec + runtime verification for the bottom-bar task label's
// click-to-open affordance (see BottomBar.qml's taskLabel: objectName
// "bottomBarTaskLabel"). The label shows "Kind: filename" for whatever job is
// currently displayed (busy → the active job; idle → the front queued job);
// this test proves a click opens THAT file via the same taskActivated signal
// Main.qml already wires to mainPanel.showAudioFile(path) for Tasks-popup
// rows, and that the click is properly gated (no file → inert, setting off →
// inert).
//
// Same harness shape as tst_bottombar_taskmodel.cpp (ApplicationWindow + a
// fresh QQmlApplicationEngine per test — read its top-of-file comment for why
// a real Window/Overlay and per-engine singleton stub are needed here rather
// than a bare QQuickView). Two differences from that file's stub:
//  - busy/currentFile/currentJobKind/currentJobKindLabel are SETTABLE here
//    (with NOTIFY signals) so the ACTIVE (busy) display path can be driven,
//    not just the queued path — the taskmodel stub hardcodes them CONSTANT.
//  - No popup involved: the task label is found directly by objectName from
//    the window's contentItem, and AppSettings is reached through a plain
//    QtObject property the driver QML exposes (a QML-file singleton isn't
//    retrievable via QQmlEngine::singletonInstance()).

#include <QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include "IconTheme.h"

namespace {

// See tst_bottombar_taskmodel.cpp's TranscriptionControllerStub doc comment
// for why every property BottomBar.qml's load-time bindings touch must exist.
// busy/currentFile/currentJobKind/currentJobKindLabel are the ones this test
// actually drives, so they're real settable properties with NOTIFY signals;
// everything else stays a harmless constant like the sibling stub.
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy WRITE setBusy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress CONSTANT)
    Q_PROPERTY(double overallProgress READ overallProgress CONSTANT)
    Q_PROPERTY(int tasksCompleted READ tasksCompleted NOTIFY jobsChanged)
    Q_PROPERTY(int tasksTotal READ tasksTotal NOTIFY jobsChanged)
    Q_PROPERTY(QString currentJobKind READ currentJobKind WRITE setCurrentJobKind NOTIFY currentJobKindChanged)
    Q_PROPERTY(QUrl currentFile READ currentFile WRITE setCurrentFile NOTIFY currentFileChanged)
    Q_PROPERTY(QString currentJobKindLabel READ currentJobKindLabel WRITE setCurrentJobKindLabel
                   NOTIFY currentJobKindLabelChanged)
    Q_PROPERTY(QString currentPhase READ currentPhase CONSTANT)
    Q_PROPERTY(bool activePaused READ activePaused CONSTANT)
    Q_PROPERTY(bool activeStopping READ activeStopping CONSTANT)
    Q_PROPERTY(bool activeCanPause READ activeCanPause CONSTANT)
    Q_PROPERTY(QString activeBackendKind READ activeBackendKind CONSTANT)
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    Q_PROPERTY(QVariantList sortedJobs READ jobs NOTIFY jobsChanged)

public:
    void setJobs(const QVariantList &jobs)
    {
        m_jobs = jobs;
        emit jobsChanged();
    }
    void setBusy(bool busy)
    {
        if (m_busy == busy)
            return;
        m_busy = busy;
        emit busyChanged();
    }
    void setCurrentFile(const QUrl &url)
    {
        if (m_currentFile == url)
            return;
        m_currentFile = url;
        emit currentFileChanged();
    }
    void setCurrentJobKind(const QString &kind)
    {
        if (m_currentJobKind == kind)
            return;
        m_currentJobKind = kind;
        emit currentJobKindChanged();
    }
    void setCurrentJobKindLabel(const QString &label)
    {
        if (m_currentJobKindLabel == label)
            return;
        m_currentJobKindLabel = label;
        emit currentJobKindLabelChanged();
    }

    bool busy() const { return m_busy; }
    double progress() const { return 0.0; }
    double overallProgress() const { return 0.0; }
    int tasksCompleted() const { return 0; }
    int tasksTotal() const { return m_jobs.size(); }
    QString currentJobKind() const { return m_currentJobKind; }
    QUrl currentFile() const { return m_currentFile; }
    QString currentJobKindLabel() const { return m_currentJobKindLabel; }
    QString currentPhase() const { return QString(); }
    bool activePaused() const { return false; }
    bool activeStopping() const { return false; }
    bool activeCanPause() const { return false; }
    QString activeBackendKind() const { return QString(); }
    QVariantList jobs() const { return m_jobs; }

    Q_INVOKABLE QString phaseLabel(const QString &) const { return QString(); }
    // Only reachable through the Tasks-popup delegate this harness never
    // opens — kept as a harmless stub rather than pulling in the real
    // JobListFormatter (this test's CMake target doesn't compile it).
    Q_INVOKABLE QString modelShortLabel(const QString &modelId) const { return modelId; }
    Q_INVOKABLE qint64 activeElapsedMs() const { return 0; }
    Q_INVOKABLE qint64 estimatedRemainingMs() const { return -1; }
    Q_INVOKABLE void togglePauseActive() { }
    Q_INVOKABLE void cancelAll() { }
    Q_INVOKABLE void clearFinished() { }
    Q_INVOKABLE void moveQueuedJob(int, int) { }
    Q_INVOKABLE void undoTask(int) { }
    Q_INVOKABLE void dismissBatch(int) { }
    Q_INVOKABLE void cancelBatch(int) { }
    Q_INVOKABLE void dismiss(const QUrl &) { }
    Q_INVOKABLE void cancelJob(int) { }

signals:
    void jobsChanged();
    void busyChanged();
    void currentFileChanged();
    void currentJobKindChanged();
    void currentJobKindLabelChanged();

private:
    QVariantList m_jobs;
    bool m_busy = false;
    QUrl m_currentFile;
    QString m_currentJobKind;
    QString m_currentJobKindLabel;
};

} // namespace

class TstBottomBarTaskOpen : public QObject
{
    Q_OBJECT

    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    TranscriptionControllerStub *m_stub = nullptr;
    QObject *m_appSettings = nullptr;

    bool openWindow()
    {
        m_engine = std::make_unique<QQmlApplicationEngine>();
        m_engine->addImportPath(m_moduleDir.path());
        m_engine->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                   new IconImageProvider);
        m_engine->load(QUrl::fromLocalFile(m_driverQmlPath));
        if (m_engine->rootObjects().isEmpty()) {
            qWarning("driver QML failed to load — see the QML errors above");
            return false;
        }
        m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().first());
        if (!m_window)
            return false;
        m_stub = qobject_cast<TranscriptionControllerStub *>(m_engine->singletonInstance<QObject *>(
            QStringLiteral("SuopTranscriber"), QStringLiteral("TranscriptionController")));
        if (!m_stub)
            return false;
        m_appSettings = qvariant_cast<QObject *>(m_window->property("appSettingsRef"));
        if (!m_appSettings)
            return false;
        return QTest::qWaitForWindowExposed(m_window);
    }

    static void collectByObjectName(QQuickItem *item, const QString &name, QList<QQuickItem *> &out)
    {
        if (!item)
            return;
        if (item->objectName() == name)
            out.append(item);
        for (QQuickItem *child : item->childItems())
            collectByObjectName(child, name, out);
    }

    QQuickItem *taskLabel() const
    {
        QList<QQuickItem *> found;
        collectByObjectName(m_window->contentItem(), QStringLiteral("bottomBarTaskLabel"), found);
        return found.isEmpty() ? nullptr : found.first();
    }

    void clickCenter(QQuickItem *item)
    {
        const QPoint pt = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, pt);
    }

    static QVariantMap queuedTranscribeRow(const QString &fileName)
    {
        return QVariantMap{
            {QStringLiteral("id"), 1},
            {QStringLiteral("kind"), QStringLiteral("transcribe")},
            {QStringLiteral("kindLabel"), QStringLiteral("Transcribe")},
            {QStringLiteral("status"), QStringLiteral("queued")},
            {QStringLiteral("fileUrl"), QUrl(QStringLiteral("file:///vault/") + fileName)},
            {QStringLiteral("modelId"), QStringLiteral("tiny.en")},
        };
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
                     "HoverBorder 1.0 qml/HoverBorder.qml\n"
                     "HoverUnderlineLabel 1.0 qml/HoverUnderlineLabel.qml\n"
                     "AppIcon 1.0 qml/AppIcon.qml\n"
                     "AppIconButton 1.0 qml/AppIconButton.qml\n"
                     "CompressButton 1.0 qml/CompressButton.qml\n"
                     "ConfirmPopup 1.0 qml/ConfirmPopup.qml\n"
                     "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                     "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                     "MiddleDragScroll 1.0 qml/MiddleDragScroll.qml\n"
                     "BottomBar 1.0 qml/BottomBar.qml\n");
        qmldir.close();

        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterSingletonType<TranscriptionControllerStub>(
            "SuopTranscriber", 1, 0, "TranscriptionController",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new TranscriptionControllerStub; });

        // appSettingsRef re-exposes the real AppSettings QML-file singleton as
        // a plain property: QQmlEngine::singletonInstance() only resolves
        // C++-registered singletons, not a qmldir "singleton ... qml/*.qml"
        // entry, so this is how the test toggles its four new settings.
        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import QtQuick.Controls\n"
            "import SuopTranscriber\n"
            "ApplicationWindow {\n"
            "    id: win\n"
            "    width: 900\n"
            "    height: 700\n"
            "    visible: true\n"
            "    property QtObject appSettingsRef: AppSettings\n"
            "    BottomBar {\n"
            "        id: bar\n"
            "        objectName: \"bottomBar\"\n"
            "        anchors.left: parent.left\n"
            "        anchors.right: parent.right\n"
            "        anchors.bottom: parent.bottom\n"
            "        transcriptionController: TranscriptionController\n"
            "    }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driverFile(m_driverQmlPath);
        QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
        driverFile.write(driverQml.toUtf8());
        driverFile.close();
    }

    void cleanup()
    {
        m_window = nullptr;
        m_stub = nullptr;
        m_appSettings = nullptr;
        m_engine.reset();
    }

    // The headline claim: clicking the task label while a job is ACTIVELY
    // running opens that job's file — exercising the busy branch of both
    // displayJobFileUrl and the label's `clickable` gate.
    void activeJob_clickOpensItsFile()
    {
        QVERIFY(openWindow());
        m_stub->setBusy(true);
        m_stub->setCurrentFile(QUrl(QStringLiteral("file:///vault/talk.wav")));
        m_stub->setCurrentJobKindLabel(QStringLiteral("Transcribe"));

        QQuickItem *label = taskLabel();
        QVERIFY2(label, "bottomBarTaskLabel not found");
        QVERIFY2(label->isVisible(), "the task label must be visible while a job is active");

        QSignalSpy spy(m_window->findChild<QQuickItem *>(QStringLiteral("bottomBar")),
                       SIGNAL(taskActivated(QString)));
        QVERIFY(spy.isValid());
        clickCenter(label);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("/vault/talk.wav"));
    }

    // Idle path: the label names the front QUEUED job instead of an active
    // one, and a click opens THAT file — exercising displayJobFileUrl's
    // frontQueuedJob branch.
    void queuedJob_clickOpensItsFile()
    {
        QVERIFY(openWindow());
        m_stub->setBusy(false);
        m_stub->setJobs({queuedTranscribeRow(QStringLiteral("beta.mp3"))});

        QQuickItem *label = taskLabel();
        QVERIFY2(label, "bottomBarTaskLabel not found");
        QVERIFY2(label->isVisible(), "the task label must be visible for a queued job (hasDisplayJob)");

        QSignalSpy spy(m_window->findChild<QQuickItem *>(QStringLiteral("bottomBar")),
                       SIGNAL(taskActivated(QString)));
        QVERIFY(spy.isValid());
        clickCenter(label);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("/vault/beta.mp3"));
    }

    // A display job with no file (e.g. a benchmark) must not be clickable —
    // guards the `clickable` gate's URL-emptiness check independent of the
    // click-to-open setting.
    void displayJobWithNoFile_clickDoesNothing()
    {
        QVERIFY(openWindow());
        m_stub->setBusy(true);
        m_stub->setCurrentFile(QUrl());
        m_stub->setCurrentJobKindLabel(QStringLiteral("Benchmark"));

        QQuickItem *label = taskLabel();
        QVERIFY2(label, "bottomBarTaskLabel not found");
        QVERIFY2(label->isVisible(), "a fileless display job (e.g. benchmark) is still shown");

        QSignalSpy spy(m_window->findChild<QQuickItem *>(QStringLiteral("bottomBar")),
                       SIGNAL(taskActivated(QString)));
        QVERIFY(spy.isValid());
        clickCenter(label);
        QCOMPARE(spy.count(), 0);
    }

    // Turning the setting off must make an otherwise-clickable label inert.
    void settingOff_clickDoesNothing()
    {
        QVERIFY(openWindow());
        m_appSettings->setProperty("bottomBarTaskClickOpensFile", false);
        m_stub->setBusy(true);
        m_stub->setCurrentFile(QUrl(QStringLiteral("file:///vault/talk.wav")));
        m_stub->setCurrentJobKindLabel(QStringLiteral("Transcribe"));

        QQuickItem *label = taskLabel();
        QVERIFY2(label, "bottomBarTaskLabel not found");

        QSignalSpy spy(m_window->findChild<QQuickItem *>(QStringLiteral("bottomBar")),
                       SIGNAL(taskActivated(QString)));
        QVERIFY(spy.isValid());
        clickCenter(label);
        QCOMPARE(spy.count(), 0);
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppSettings.qml's Settings{} element
    // is loaded transitively by BottomBar.qml, and must never touch the real
    // ~/.config/suopTranscriber (tests must never touch the user's real
    // vault or app config).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_bottombar_taskopen"));
    QGuiApplication app(argc, argv);
    TstBottomBarTaskOpen tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_bottombar_taskopen.moc"
