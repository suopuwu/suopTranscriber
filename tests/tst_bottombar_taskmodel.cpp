// Living spec + runtime verification for the Tasks-list "which model will
// this run" sub-line (see JobListFormatter::modelShortLabel()'s doc comment
// and DESIGN.md "Task system"): a queued task freezes its model at enqueue
// time, but until now the Tasks popup never showed it — with several tasks
// queued there was no way to tell which model any of them would run, or that
// two queued transcriptions were using different models. Every field this
// test asserts on (modelId / segmentationModelId / embeddingModelId) already
// flows through jobs()' toVariantMap() merge — this is a pure presentation
// change, so the test drives the REAL BottomBar.qml end to end rather than
// re-deriving the mapping in C++.
//
// ApplicationWindow + QQmlApplicationEngine, not the bare QQuickView the
// sibling tst_bottombar_pathsegments.cpp uses: the rows under test live
// inside jobListPopup, a modal Popup, and (per tst_snackbar.cpp's own
// hard-won lesson) a Popup's content only actually renders once it lands on
// a real window Overlay — a QQuickView root is a plain Item with no
// guaranteed overlay. See tst_snackbar.cpp's top-of-file comment for the
// fuller rationale; this harness mirrors its shape (fresh engine per test,
// singleton stub, visual-tree collectByObjectName from window->contentItem()).
//
// The stub's modelShortLabel() is wired straight to the REAL
// JobListFormatter::modelShortLabel() (backed by the REAL ModelManager
// catalogs) — a fake mapping would only prove the test agrees with itself.

#include <QtTest>

#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>

#include "IconTheme.h"
#include "JobListFormatter.h"

namespace {

// Answers exactly the surface BottomBar.qml's load-time bindings read off
// `transcriptionController` once it stops being null (see
// tst_sidebar_contextmenu.cpp's TranscriptionControllerStub doc comment for
// why every one of these has to exist: an unresolved property/call breaks
// its binding silently rather than failing visibly). `jobs`/`sortedJobs`
// both read the same hand-built list set per test via setJobs() — this
// harness never exercises collapseBatches()/sortJobsForDisplay(), so the two
// views don't need to differ.
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(double progress READ progress CONSTANT)
    Q_PROPERTY(double overallProgress READ overallProgress CONSTANT)
    Q_PROPERTY(int tasksCompleted READ tasksCompleted NOTIFY jobsChanged)
    Q_PROPERTY(int tasksTotal READ tasksTotal NOTIFY jobsChanged)
    Q_PROPERTY(QString currentJobKind READ currentJobKind CONSTANT)
    Q_PROPERTY(QUrl currentFile READ currentFile CONSTANT)
    Q_PROPERTY(QString currentJobKindLabel READ currentJobKindLabel CONSTANT)
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

    bool busy() const { return false; }
    double progress() const { return 0.0; }
    double overallProgress() const { return 0.0; }
    int tasksCompleted() const { return 0; }
    int tasksTotal() const { return m_jobs.size(); }
    QString currentJobKind() const { return QString(); }
    QUrl currentFile() const { return QUrl(); }
    QString currentJobKindLabel() const { return QString(); }
    QString currentPhase() const { return QString(); }
    bool activePaused() const { return false; }
    bool activeStopping() const { return false; }
    bool activeCanPause() const { return false; }
    QString activeBackendKind() const { return QString(); }
    QVariantList jobs() const { return m_jobs; }

    Q_INVOKABLE QString phaseLabel(const QString &) const { return QString(); }
    // The mapping under test — forwarded to the REAL implementation (see this
    // file's top-of-file comment).
    Q_INVOKABLE QString modelShortLabel(const QString &modelId) const
    {
        return JobListFormatter::modelShortLabel(modelId);
    }
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

private:
    QVariantList m_jobs;
};

} // namespace

class TstBottomBarTaskModel : public QObject
{
    Q_OBJECT

    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // Fresh engine per test: the TranscriptionController stub is a per-engine
    // singleton (see tst_snackbar.cpp's identical rationale for Notifications).
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    TranscriptionControllerStub *m_stub = nullptr;

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
        return QTest::qWaitForWindowExposed(m_window);
    }

    // Walks the VISUAL tree (childItems), not the QObject tree — the Tasks
    // popup's rows are ListView/Repeater delegates, which (like every other
    // dynamically-instantiated delegate in this codebase — see
    // tst_bottombar_pathsegments.cpp's identical note) are only ever given a
    // visual parent.
    static void collectByObjectName(QQuickItem *item, const QString &name, QList<QQuickItem *> &out)
    {
        if (!item)
            return;
        if (item->objectName() == name)
            out.append(item);
        for (QQuickItem *child : item->childItems())
            collectByObjectName(child, name, out);
    }

    QList<QQuickItem *> taskModelDetailLabels() const
    {
        QList<QQuickItem *> out;
        collectByObjectName(m_window->contentItem(), QStringLiteral("taskModelDetail"), out);
        return out;
    }

    // The non-fatal-warning glyph (BottomBar.qml's warningGlyph) —
    // one delegate per row, like taskModelDetailLabels() above, only its
    // `visible` flips on whether that row's "warning" field is non-empty.
    QList<QQuickItem *> taskWarningGlyphs() const
    {
        QList<QQuickItem *> out;
        collectByObjectName(m_window->contentItem(), QStringLiteral("taskWarningGlyph"), out);
        return out;
    }

    // jobListPopup has no objectName (nothing else needs one), so it's found
    // by C++ class rather than by name — a Popup is a QObject child of its
    // declaring Item regardless of open state (only its CONTENT reparents
    // onto the window overlay when opened — see QQuickPopup's own doc), so a
    // plain QObject-tree search from the bar finds it whether or not it has
    // ever been shown.
    QObject *jobListPopup() const
    {
        QQuickItem *bar = m_window->findChild<QQuickItem *>(QStringLiteral("bottomBar"));
        if (!bar)
            return nullptr;
        const auto children = bar->findChildren<QObject *>();
        for (QObject *child : children) {
            if (child->inherits("QQuickPopup"))
                return child;
        }
        return nullptr;
    }

    // Sets the stub's rows, opens the Tasks popup, and waits for exactly
    // `rowCount` taskModelDetail delegates (one per row, visible or not — see
    // BottomBar.qml's modelDetailLabel, always created, only its `visible`
    // flips) to materialize.
    bool showJobs(const QVariantList &jobs)
    {
        m_stub->setJobs(jobs);
        QObject *popup = jobListPopup();
        if (!popup)
            return false;
        QMetaObject::invokeMethod(popup, "open");
        return QTest::qWaitFor([&] { return taskModelDetailLabels().size() == jobs.size(); }, 5000);
    }

    static QVariantMap transcribeRow(int id, const QString &fileName, const QString &modelId)
    {
        return QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("kind"), QStringLiteral("transcribe")},
            {QStringLiteral("kindLabel"), QStringLiteral("Transcribe")},
            {QStringLiteral("status"), QStringLiteral("queued")},
            {QStringLiteral("fileUrl"), QUrl(QStringLiteral("file:///vault/") + fileName)},
            {QStringLiteral("modelId"), modelId},
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

        // Tall enough that jobListPopup (up to 600px, anchored above the bar)
        // fits fully inside the window.
        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import QtQuick.Controls\n"
            "import SuopTranscriber\n"
            "ApplicationWindow {\n"
            "    id: win\n"
            "    width: 900\n"
            "    height: 700\n"
            "    visible: true\n"
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
        m_engine.reset();
    }

    // The headline claim: two queued transcribe rows with different modelIds
    // render two DIFFERENT taskModelDetail texts — today, with several tasks
    // queued, there is no way to tell which model any of them will run; this
    // is the thing the user reported they couldn't see.
    void transcribeRowShowsItsModel()
    {
        QVERIFY(openWindow());
        const QVariantList jobs{transcribeRow(1, QStringLiteral("alpha.mp3"), QStringLiteral("tiny.en")),
                                transcribeRow(2, QStringLiteral("beta.mp3"), QStringLiteral("large-v3"))};
        QVERIFY2(showJobs(jobs), "the two transcribe rows never rendered a taskModelDetail label each");

        QSet<QString> texts;
        for (QQuickItem *label : taskModelDetailLabels()) {
            QVERIFY2(label->isVisible(), "a transcribe row's model line must be visible");
            const QString text = label->property("text").toString();
            QVERIFY2(!text.isEmpty(), "a transcribe row's model line must not be blank");
            texts.insert(text);
        }
        QCOMPARE(texts.size(), 2);
        QVERIFY2(texts.contains(QStringLiteral("Tiny (English)")),
                 qPrintable(QStringLiteral("expected \"Tiny (English)\" among: ")
                            + QStringList(texts.values()).join(QStringLiteral(", "))));
        QVERIFY2(texts.contains(QStringLiteral("Large (multilingual)")),
                 qPrintable(QStringLiteral("expected \"Large (multilingual)\" among: ")
                            + QStringList(texts.values()).join(QStringLiteral(", "))));
    }

    // A diarize row names BOTH sub-models — segmentation and embedding — on
    // the one line, joined so neither is silently omitted.
    void diarizeRowShowsBothModels()
    {
        QVERIFY(openWindow());
        const QVariantMap row{
            {QStringLiteral("id"), 1},
            {QStringLiteral("kind"), QStringLiteral("diarize")},
            {QStringLiteral("kindLabel"), QStringLiteral("Detect speakers")},
            {QStringLiteral("status"), QStringLiteral("queued")},
            {QStringLiteral("fileUrl"), QUrl(QStringLiteral("file:///vault/gamma.mp3"))},
            {QStringLiteral("segmentationModelId"), QStringLiteral("pyannote-segmentation-3.0")},
            {QStringLiteral("embeddingModelId"), QStringLiteral("wespeaker-resnet293-lm")},
            {QStringLiteral("diarizationThreshold"), 0.5},
        };
        QVERIFY2(showJobs({row}), "the diarize row never rendered a taskModelDetail label");

        const QList<QQuickItem *> labels = taskModelDetailLabels();
        QCOMPARE(labels.size(), 1);
        QVERIFY2(labels.first()->isVisible(), "a diarize row's model line must be visible");
        const QString expected =
            JobListFormatter::modelShortLabel(QStringLiteral("pyannote-segmentation-3.0"))
            + QStringLiteral(" · ")
            + JobListFormatter::modelShortLabel(QStringLiteral("wespeaker-resnet293-lm"));
        QCOMPARE(labels.first()->property("text").toString(), expected);
    }

    // A download row's TITLE already names the model ("tiny.en"), and a
    // modelIo (import/export) row is titled the same way — repeating it on a
    // second line under itself would read as a bug, not information.
    void downloadAndModelIoRowsShowNoDuplicateLine()
    {
        QVERIFY(openWindow());
        const QVariantMap downloadRow{
            {QStringLiteral("id"), 1},
            {QStringLiteral("kind"), QStringLiteral("download")},
            {QStringLiteral("kindLabel"), QStringLiteral("Download")},
            {QStringLiteral("status"), QStringLiteral("queued")},
            {QStringLiteral("downloadTarget"), QStringLiteral("model")},
            {QStringLiteral("modelId"), QStringLiteral("tiny.en")},
        };
        const QVariantMap modelIoRow{
            {QStringLiteral("id"), 2},
            {QStringLiteral("kind"), QStringLiteral("modelIo")},
            {QStringLiteral("kindLabel"), QStringLiteral("Import model")},
            {QStringLiteral("status"), QStringLiteral("queued")},
            {QStringLiteral("modelId"), QStringLiteral("tiny.en")},
        };
        QVERIFY2(showJobs({downloadRow, modelIoRow}),
                 "the download/modelIo rows never rendered a taskModelDetail label each");

        for (QQuickItem *label : taskModelDetailLabels()) {
            QVERIFY2(!label->isVisible(),
                     "a download/modelIo row's model line must stay hidden — the row title "
                     "already names the model");
            QVERIFY2(label->property("text").toString().isEmpty(),
                     "a download/modelIo row's model line must be blank");
        }
    }

    // A Done task carrying a non-fatal "warning" (e.g. TranscribeTask
    // surfacing a corrupt-audio repair) must render a DISTINCT indicator from
    // both a warning-free Done row and a genuinely Failed row — the task
    // succeeded, so this must never look like the failure state.
    void doneRowWithWarning_rendersDistinctlyFromCleanDoneAndFailedRows()
    {
        QVERIFY(openWindow());
        // "error"/"durationMs" are set on every real jobs() row unconditionally
        // (see TranscriptionController::jobs()) — set them explicitly here too
        // so an absent key never masquerades as a real gap in BottomBar's
        // bindings (modelData.error/.warning read undefined vs. "" the same
        // for THIS test's purposes, but the pre-existing error-tooltip binding
        // warns on assigning undefined to a QString property).
        QVariantMap warnedRow = transcribeRow(1, QStringLiteral("corrupt.mp3"), QStringLiteral("tiny.en"));
        warnedRow[QStringLiteral("status")] = QStringLiteral("done");
        warnedRow[QStringLiteral("error")] = QString();
        warnedRow[QStringLiteral("warning")] =
            QStringLiteral("196249 sample(s) were non-finite (NaN/Inf) and replaced with silence, starting at 12s");
        warnedRow[QStringLiteral("durationMs")] = 4200;

        QVariantMap cleanDoneRow = transcribeRow(2, QStringLiteral("clean.mp3"), QStringLiteral("tiny.en"));
        cleanDoneRow[QStringLiteral("status")] = QStringLiteral("done");
        cleanDoneRow[QStringLiteral("error")] = QString();
        cleanDoneRow[QStringLiteral("warning")] = QString();
        cleanDoneRow[QStringLiteral("durationMs")] = 3000;

        QVariantMap failedRow = transcribeRow(3, QStringLiteral("broken.mp3"), QStringLiteral("tiny.en"));
        failedRow[QStringLiteral("status")] = QStringLiteral("failed");
        failedRow[QStringLiteral("error")] = QStringLiteral("audio decode failed");
        failedRow[QStringLiteral("durationMs")] = 1000;

        QVERIFY2(showJobs({warnedRow, cleanDoneRow, failedRow}),
                 "the three rows never rendered their taskModelDetail labels");

        const QList<QQuickItem *> glyphs = taskWarningGlyphs();
        QCOMPARE(glyphs.size(), 3); // one delegate per row, like taskModelDetailLabels()
        int visibleCount = 0;
        for (QQuickItem *glyph : glyphs) {
            if (!glyph->isVisible())
                continue;
            ++visibleCount;
            QCOMPARE(glyph->property("text").toString(), QString::fromUtf8("\xE2\x9A\xA0")); // "⚠"
        }
        QVERIFY2(visibleCount == 1,
                 "exactly the warned row's glyph must be visible — not the clean Done row's, "
                 "and not the Failed row's (a warning must never look like a failure)");
    }

    // A stale or out-of-process (`engine:<id>`) modelId that matches nothing
    // in any catalog must still render as ITSELF, never as blank — the whole
    // point is to never misrepresent (or silently drop) which model a queued
    // task will run.
    void unknownModelIdFallsBackToTheRawId()
    {
        QVERIFY(openWindow());
        const QVariantList jobs{
            transcribeRow(1, QStringLiteral("delta.mp3"), QStringLiteral("engine:whatever"))};
        QVERIFY2(showJobs(jobs), "the unknown-model row never rendered a taskModelDetail label");

        const QList<QQuickItem *> labels = taskModelDetailLabels();
        QCOMPARE(labels.size(), 1);
        QVERIFY2(labels.first()->isVisible(), "an unrecognized model id must still show a line");
        QCOMPARE(labels.first()->property("text").toString(), QStringLiteral("engine:whatever"));
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppSettings.qml's Settings{}
    // element is loaded transitively by BottomBar.qml, and must never touch
    // the real ~/.config/suopTranscriber (tests must never touch the user's
    // real vault or app config).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_bottombar_taskmodel"));
    QGuiApplication app(argc, argv);
    TstBottomBarTaskModel tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_bottombar_taskmodel.moc"
