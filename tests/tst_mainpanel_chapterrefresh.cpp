// Living spec for "the chapters/prelude on screen belong to the file that's
// actually open". `mainPanel.chapters` is a single shared property (MainPanel is
// one panel showing whichever file is current) with NO file key — nothing tags
// the array with the URL it was read from. So the ONLY thing keeping it honest
// is that every path which opens a file re-populates it. That invariant is
// exactly what regressed: refreshPreludeAndChapters()'s own doc comment claimed
// the transcriptLoaded handler called it after a cold open; it did not, and
// onSourceChanged/onOrphanedMdPathChanged deliberately skip it (Item P23, so a
// large file's body read can't block the GUI thread). Net effect: opening a file
// left the PREVIOUS file's chapters on screen, and a file's own stored chapters
// never appeared until some unrelated finished()/progressChanged() happened to
// fire.
//
// tst_mainpanel_chapterselection.cpp cannot catch this: it injects `chapters`
// straight into the property to drive the selection math, so it never exercises
// the refresh wiring at all. Hence this separate harness.
//
// Same harness shape as tst_mainpanel_chapterselection.cpp (read its top-of-file
// comment first): QQuickView, offscreen, scratch QML module dir symlinking the
// real qml/, sandboxed org/app name in main(). Two things are specific here:
//
//  - **The TranscriptionController is REAL, not a stub.** The whole assertion is
//    "what did the panel read back off disk for THIS file", so stubbing
//    readChapters() would only prove the stub agrees with itself. Its
//    constructor spawns a worker thread but loads no models (same as the four
//    other tests that construct one), and no test here queues a job.
//
//  - **Files are switched via `orphanedMdPath`, not `player.source`.** Both are
//    "a different file was just opened" (see MainPanel's onSourceChanged /
//    onOrphanedMdPathChanged), but the orphaned path needs no audio and no
//    AudioPlayer — readChapters() only ever reads the .md's body. That keeps the
//    fixture to two text files.
//
// Nothing here touches the user's vault or ~/.config/suopTranscriber.

#include <QtTest>

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>

#include "Clipboard.h"
#include "IconTheme.h"
#include "TranscriptionController.h"

namespace {

// See tst_mainpanel_chapterselection.cpp's identical stubs: MainPanel's use
// sites of these guard on the property being non-null, so each only needs its
// TYPE to resolve. All four stay null for every test below. TranscriptionController
// is deliberately NOT among them — it is the real class here.
class AudioPlayerStub : public QObject { Q_OBJECT };
class AudioRecorderStub : public QObject { Q_OBJECT };
class VaultTreeModelStub : public QObject { Q_OBJECT };
class UsbGrabberControllerStub : public QObject { Q_OBJECT };

// A transcript exactly as the app writes one: `---` frontmatter, then one
// "[start] text" line per segment, with "## [start] title" chapter markers
// spliced in immediately before the segment line they start at (see
// spliceBodyChapters()). Timestamps are the compact zero-trimmed form
// formatTimestamp() produces, so 0 -> "0" and 5 -> "5".
QString transcriptWithChapters()
{
    return QStringLiteral("---\n"
                          "model: base\n"
                          "speakerDetection: false\n"
                          "---\n"
                          "## [0] Alpha\n"
                          "[0] Hello world\n"
                          "## [5] Beta\n"
                          "[5] Second line\n");
}

QString transcriptWithoutChapters()
{
    return QStringLiteral("---\n"
                          "model: base\n"
                          "speakerDetection: false\n"
                          "---\n"
                          "[0] Hello world\n"
                          "[5] Second line\n");
}

} // namespace

class TstMainPanelChapterRefresh : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QTemporaryDir m_vaultDir;
    QString m_driverQmlPath;
    QString m_chapteredMd;
    QString m_plainMd;

    static bool writeFile(const QString &path, const QString &contents)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        f.write(contents.toUtf8());
        f.close();
        return true;
    }

    // The chapter TITLES currently on the panel, in order — the readable form of
    // `chapters` (a QVariantList of {t0, title, manual} maps) for assertions.
    static QStringList chapterTitles(QQuickItem *panel)
    {
        QStringList titles;
        const QVariantList chapters = panel->property("chapters").toList();
        for (const QVariant &entry : chapters)
            titles.append(entry.toMap().value(QStringLiteral("title")).toString());
        return titles;
    }

    // Points the panel at an .md.
    static void openMd(QQuickItem *panel, const QString &path)
    {
        panel->setProperty("orphanedMdPath", path);
    }

    // Simulates the state the real app is in after a "generate chapters" run:
    // onFinished() populated `chapters` for the file that's open. Seeding the
    // property DIRECTLY (rather than relying on the open to populate it) is what
    // keeps the switch/close assertions below independent of the cold-open
    // refresh — otherwise a broken refresh leaves `chapters` empty, the "did the
    // previous file's chapters get cleared" assertion is trivially satisfied,
    // and the slot silently stops guarding what its name claims.
    static void seedChaptersAsIfJustGenerated(QQuickItem *panel)
    {
        QVariantMap alpha;
        alpha.insert(QStringLiteral("t0"), 0.0);
        alpha.insert(QStringLiteral("title"), QStringLiteral("Alpha"));
        alpha.insert(QStringLiteral("manual"), false);
        QVariantMap beta;
        beta.insert(QStringLiteral("t0"), 5.0);
        beta.insert(QStringLiteral("title"), QStringLiteral("Beta"));
        beta.insert(QStringLiteral("manual"), false);
        panel->setProperty("chapters", QVariantList{alpha, beta});
    }

    // Waits for the async body load to land — see loadTranscriptAsync(). Gates on
    // transcriptBody, NOT on `chapters`: the body is populated by the same
    // handler either way, so this settles deterministically even when the
    // chapters refresh under test is broken.
    static bool waitForBodyLoaded(QQuickItem *panel)
    {
        return QTest::qWaitFor([panel]() {
            return !panel->property("transcriptBody").toString().isEmpty();
        }, 5000);
    }

public:
    QQuickItem *m_panel = nullptr;
    TranscriptionController *m_controller = nullptr;

private:
    // Builds the view and wires a REAL controller in. Returns the located
    // MainPanel (also caching it in m_panel), or nullptr.
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

        // Must be set BEFORE any orphanedMdPath assignment — beginTranscriptLoad()
        // reads through this property the moment the path changes.
        m_controller = new TranscriptionController(this);
        panel->setProperty("transcriptionController", QVariant::fromValue(m_controller));
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

        m_chapteredMd = m_vaultDir.filePath(QStringLiteral("chaptered.md"));
        m_plainMd = m_vaultDir.filePath(QStringLiteral("plain.md"));
        QVERIFY(writeFile(m_chapteredMd, transcriptWithChapters()));
        QVERIFY(writeFile(m_plainMd, transcriptWithoutChapters()));

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
        // The real thing — see this file's top comment.
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

    // The "isn't correctly displaying previously generated chapters" half: a
    // cold open of a file that already HAS chapters must show them, with no
    // transcription/edit/finished() needed to shake them loose.
    void coldOpeningAChapteredFile_showsItsStoredChapters()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        openMd(m_panel, m_chapteredMd);
        QTRY_COMPARE(chapterTitles(m_panel),
                     (QStringList{QStringLiteral("Alpha"), QStringLiteral("Beta")}));
    }

    // The "chapters show up on every file" half — the user-facing sequence:
    // generate chapters on one file, then click a file that has none, and the
    // first file's chapters are still sitting there under the new file's name.
    void switchingToAnUnchapteredFile_clearsThePreviousFilesChapters()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        openMd(m_panel, m_chapteredMd);
        QVERIFY(waitForBodyLoaded(m_panel));
        seedChaptersAsIfJustGenerated(m_panel);

        openMd(m_panel, m_plainMd);
        QTRY_VERIFY2(chapterTitles(m_panel).isEmpty(),
                     "the previous file's chapters are still on screen after switching files");
    }

    // The same leak in the other direction — switching between two files that
    // BOTH have chapters must swap them, not keep the first file's.
    void switchingBetweenChapteredFiles_showsTheNewFilesChapters()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        const QString otherMd = m_vaultDir.filePath(QStringLiteral("other.md"));
        QVERIFY(writeFile(otherMd, QStringLiteral("---\n"
                                                   "model: base\n"
                                                   "---\n"
                                                   "## [0] Gamma\n"
                                                   "[0] Different file\n")));

        openMd(m_panel, m_chapteredMd);
        QVERIFY(waitForBodyLoaded(m_panel));
        seedChaptersAsIfJustGenerated(m_panel);

        openMd(m_panel, otherMd);
        QTRY_COMPARE(chapterTitles(m_panel), (QStringList{QStringLiteral("Gamma")}));
    }

    // Closing the file (back to "nothing open") must not leave the last file's
    // chapters behind either. This is the one path the transcriptLoaded refresh
    // CANNOT cover: beginTranscriptLoad()'s no-file branch returns early, so
    // transcriptLoaded never fires and the clear has to happen up front — which
    // makes this the slot that pins that clear.
    void closingTheFile_clearsChapters()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        openMd(m_panel, m_chapteredMd);
        QVERIFY(waitForBodyLoaded(m_panel));
        seedChaptersAsIfJustGenerated(m_panel);

        openMd(m_panel, QString());
        QTRY_VERIFY2(chapterTitles(m_panel).isEmpty(),
                     "chapters survived closing the file");
    }

    // `prelude` leaks exactly like `chapters` — same single writer
    // (refreshPreludeAndChapters()), same missing call on open — so the clear
    // covers both. Its own slot rather than a second assertion on the one above:
    // that slot's chapters check aborts on failure, which would leave this line
    // unreached and the prelude half of the fix silently unpinned.
    // Asserted on the CLOSE path for the same reason as above, and because every
    // other path here goes through the orphaned-.md view, where prelude is ""
    // regardless (refreshPreludeAndChapters() reads it from player.source, and
    // this harness leaves the AudioPlayer stub null).
    void closingTheFile_clearsPrelude()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view), "could not load MainPanel.qml / locate it");

        openMd(m_panel, m_chapteredMd);
        QVERIFY(waitForBodyLoaded(m_panel));
        m_panel->setProperty("prelude", QStringLiteral("a previous file's prelude"));

        openMd(m_panel, QString());
        QTRY_COMPARE(m_panel->property("prelude").toString(), QString());
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: MainPanel.qml loads the AppSettings
    // singleton, whose Settings{} element must NEVER touch the real
    // ~/.config/suopTranscriber (CLAUDE.md: never let testing touch the user's
    // real vault or app config). Same setup as tst_mainpanel_chapterselection.cpp.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_mainpanel_chapterrefresh"));
    QGuiApplication app(argc, argv);
    TstMainPanelChapterRefresh tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_mainpanel_chapterrefresh.moc"
