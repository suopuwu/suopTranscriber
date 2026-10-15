// Living spec + runtime verification for BottomBar.qml's bottom-left path
// (each segment is its OWN click target) and the row's slack distribution
// (path left, everything else right-aligned). Drives a REAL QQuickView
// (offscreen, sandboxed config — see main() below) loading the actual
// BottomBar.qml, and clicks the segments it actually renders.
//
// Same harness shape as tst_sidebar_expand.cpp / tst_sidebar_dragmove.cpp
// (read their top-of-file comments first). Two things are specific to this one:
//
//  - **DesktopIntegration is stubbed**, so a click RECORDS the call instead of
//    handing it to the desktop. The real singleton launches the user's file
//    manager and raises it over everything — a test that spawned a real Dolphin
//    window per click would be unusable, and would fail on any machine with no
//    file manager. The stub is what makes clicking these segments testable at
//    all, so the wiring is checked end-to-end (real tap -> real handler ->
//    which method, which path) rather than by reading the source.
//
//  - **Paths here are fictional** (/vault/...) and never touched on disk:
//    pathSegments is pure string splitting, so giving it real files would only
//    add I/O and a reason to flake.
//
// tst_styleguide.cpp keeps the cheap source-scan guards; this file is the one
// that proves what a click actually DOES.

#include <QtTest>

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QUrl>

#include "IconTheme.h"

namespace {

// See tst_sidebar_expand.cpp's identical stub: BottomBar.qml's every
// transcriptionController use site guards on it being non-null first (the
// bindings) or sits in a handler this harness never fires, so the property only
// needs its TYPE to resolve.
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
};

// Stands in for the real DesktopIntegration singleton. Records what a click
// asked for instead of doing it.
//
// The recording is STATIC, and the registration below hands each engine its own
// instance: every test builds a fresh QQuickView (hence a fresh QQmlEngine), and
// a singleton registered with qmlRegisterSingletonInstance may only ever be
// touched by ONE engine — the second engine to reach it gets a warning and a
// singleton whose methods aren't callable at all. A per-engine instance writing
// to shared static storage sidesteps that without making each test build its own
// engine plumbing.
class DesktopIntegrationStub : public QObject {
    Q_OBJECT

public:
    struct Call {
        QString method;
        QUrl url;
    };
    static QList<Call> s_calls;

    Q_INVOKABLE void revealInFileManager(const QUrl &url) { s_calls.append({QStringLiteral("reveal"), url}); }
    Q_INVOKABLE void openInFileManager(const QUrl &url) { s_calls.append({QStringLiteral("open"), url}); }
};

QList<DesktopIntegrationStub::Call> DesktopIntegrationStub::s_calls;

} // namespace

class TstBottomBarPathSegments : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // Walks the VISUAL tree (childItems), NOT the QObject tree (findChildren).
    // A Repeater's delegates are only ever given a visual parent —
    // QQuickItem::setParentItem() does not reparent them in QObject terms — so
    // findChildren() cannot see them and silently returns nothing at all.
    static void collectByObjectName(QQuickItem *root, const QString &name, QList<QQuickItem *> &out)
    {
        for (QQuickItem *child : root->childItems()) {
            if (child->objectName() == name)
                out.append(child);
            collectByObjectName(child, name, out);
        }
    }

    // The rendered segments, in LEFT-TO-RIGHT display order. Sorted by x rather
    // than trusting tree-walk order, so the ordering assertions below are about
    // what's on screen, not about Repeater internals.
    static QList<QQuickItem *> segmentsOf(QQuickItem *bar)
    {
        QList<QQuickItem *> found;
        collectByObjectName(bar, QStringLiteral("pathSegment"), found);
        QList<QQuickItem *> segments;
        for (QQuickItem *item : found) {
            if (item->isVisible())
                segments.append(item);
        }
        std::sort(segments.begin(), segments.end(), [](QQuickItem *a, QQuickItem *b) {
            return a->mapToScene(QPointF(0, 0)).x() < b->mapToScene(QPointF(0, 0)).x();
        });
        return segments;
    }

    static QQuickItem *itemNamed(QQuickItem *root, const QString &name)
    {
        QList<QQuickItem *> found;
        collectByObjectName(root, name, found);
        return found.isEmpty() ? nullptr : found.first();
    }

    static QStringList textsOf(const QList<QQuickItem *> &segments)
    {
        QStringList texts;
        for (QQuickItem *s : segments)
            texts.append(s->property("text").toString());
        return texts;
    }

    static void clickCenter(QQuickView *view, QQuickItem *item)
    {
        const QPoint pt = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        QTest::mouseClick(view, Qt::LeftButton, Qt::NoModifier, pt);
    }

    // Builds the view + returns the located BottomBar, or nullptr.
    QQuickItem *openBar(QQuickView *view, const QString &vaultLocalPath, const QString &currentFilePath,
                         bool shrinkPath = true)
    {
        view->engine()->addImportPath(m_moduleDir.path());
        view->engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                          new IconImageProvider);
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : view->errors())
                msg += e.toString() + QStringLiteral("\n");
            // A QML error here is the exact failure mode a green cmake build
            // does NOT catch (AOT/qmlcachegen can compile a file that still
            // fails to load) — report it verbatim rather than as "null item".
            qWarning("%s", qPrintable(msg.isEmpty() ? QStringLiteral("QML failed to load, no error text")
                                                     : msg));
            return nullptr;
        }
        QQuickItem *root = view->rootObject();
        if (!root)
            return nullptr;
        root->setProperty("vaultLocal", vaultLocalPath);
        root->setProperty("filePath", currentFilePath);
        root->setProperty("shrinkPath", shrinkPath);
        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        return root->findChild<QQuickItem *>(QStringLiteral("bottomBar"));
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
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
        qmlRegisterUncreatableType<TranscriptionControllerStub>(
            "SuopTranscriber", 1, 0, "TranscriptionController",
            QStringLiteral("stub — this harness never assigns one"));
        qmlRegisterSingletonType<DesktopIntegrationStub>(
            "SuopTranscriber", 1, 0, "DesktopIntegration",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new DesktopIntegrationStub; });

        // Wide enough that the whole path fits without eliding — this harness is
        // about which segment a click lands on, not about squeeze behavior.
        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    id: root\n"
            "    width: 1400\n"
            "    height: 40\n"
            "    property string vaultLocal: \"\"\n"
            "    property string filePath: \"\"\n"
            "    property bool shrinkPath: true\n"
            "    Binding { target: AppSettings; property: \"bottomBarPathShrinkToContent\"; value: root.shrinkPath }\n"
            "    BottomBar {\n"
            "        id: bar\n"
            "        objectName: \"bottomBar\"\n"
            "        anchors.fill: parent\n"
            "        vaultLocalPath: root.vaultLocal\n"
            "        currentFilePath: root.filePath\n"
            "    }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driverFile(m_driverQmlPath);
        QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
        driverFile.write(driverQml.toUtf8());
        driverFile.close();
    }

    void init() { DesktopIntegrationStub::s_calls.clear(); }

    // The headline claim: the path renders as one segment per clickable step —
    // vault root, then each subfolder, then the file — and the segment texts
    // still concatenate to exactly the path the user sees.
    void nestedFile_rendersOneSegmentPerStep()
    {
        QQuickView view;
        QQuickItem *bar = openBar(&view, QStringLiteral("/vault"),
                                   QStringLiteral("/vault/sub/deep/talk.mp3"));
        QVERIFY2(bar, "could not load BottomBar.qml / locate it");

        const QList<QQuickItem *> segments = segmentsOf(bar);
        QCOMPARE(textsOf(segments),
                 (QStringList{QStringLiteral("/vault"), QStringLiteral("/sub"),
                               QStringLiteral("/deep"), QStringLiteral("/talk.mp3")}));
        QCOMPARE(textsOf(segments).join(QString()), QStringLiteral("/vault/sub/deep/talk.mp3"));
    }

    // The vault root is ONE segment: the folders above it are outside the vault
    // and must not become targets of their own. A deep vault root is the case
    // that would break if it were ever split on "/".
    void deepVaultRoot_staysASingleSegment()
    {
        QQuickView view;
        QQuickItem *bar = openBar(&view, QStringLiteral("/home/user/media/recordings"),
                                   QStringLiteral("/home/user/media/recordings/talk.mp3"));
        QVERIFY2(bar, "could not load BottomBar.qml / locate it");

        const QList<QQuickItem *> segments = segmentsOf(bar);
        QCOMPARE(textsOf(segments), (QStringList{QStringLiteral("/home/user/media/recordings"),
                                                  QStringLiteral("/talk.mp3")}));

        clickCenter(&view, segments.at(0));
        QCOMPARE(DesktopIntegrationStub::s_calls.size(), 1);
        // The whole root opens the VAULT — not /home, not /home/user.
        QCOMPARE(DesktopIntegrationStub::s_calls.at(0).method, QStringLiteral("open"));
        QCOMPARE(DesktopIntegrationStub::s_calls.at(0).url, QUrl(QStringLiteral("file:///home/user/media/recordings")));
    }

    // Each segment opens ITS OWN folder — the point of the whole feature. A
    // middle segment must open the folder it names, not the vault and not the
    // file's own folder.
    void clickingEachFolderSegment_opensThatFolder()
    {
        QQuickView view;
        QQuickItem *bar = openBar(&view, QStringLiteral("/vault"),
                                   QStringLiteral("/vault/sub/deep/talk.mp3"));
        QVERIFY2(bar, "could not load BottomBar.qml / locate it");

        const QList<QQuickItem *> segments = segmentsOf(bar);
        QCOMPARE(segments.size(), 4);

        const QStringList expected{QStringLiteral("file:///vault"), QStringLiteral("file:///vault/sub"),
                                    QStringLiteral("file:///vault/sub/deep")};
        for (int i = 0; i < expected.size(); ++i) {
            DesktopIntegrationStub::s_calls.clear();
            clickCenter(&view, segments.at(i));
            QCOMPARE(DesktopIntegrationStub::s_calls.size(), 1);
            QCOMPARE(DesktopIntegrationStub::s_calls.at(0).method, QStringLiteral("open"));
            QCOMPARE(DesktopIntegrationStub::s_calls.at(0).url, QUrl(expected.at(i)));
        }
    }

    // The file segment keeps the original behavior: reveal it SELECTED inside
    // its folder (ShowItems), not merely open the folder.
    void clickingFileSegment_revealsTheFileSelected()
    {
        QQuickView view;
        QQuickItem *bar = openBar(&view, QStringLiteral("/vault"),
                                   QStringLiteral("/vault/sub/talk.mp3"));
        QVERIFY2(bar, "could not load BottomBar.qml / locate it");

        const QList<QQuickItem *> segments = segmentsOf(bar);
        QCOMPARE(segments.size(), 3);
        clickCenter(&view, segments.last());
        QCOMPARE(DesktopIntegrationStub::s_calls.size(), 1);
        QCOMPARE(DesktopIntegrationStub::s_calls.at(0).method, QStringLiteral("reveal"));
        QCOMPARE(DesktopIntegrationStub::s_calls.at(0).url, QUrl(QStringLiteral("file:///vault/sub/talk.mp3")));
    }

    // A tap must fire EXACTLY ONE action. This is the regression guard for the
    // nested-TapHandler trap (both fire, parent last): the row used to carry a
    // single whole-path TapHandler, and leaving one in place above the segments
    // would silently make every segment click also open the file.
    void clickingASegment_firesExactlyOneAction()
    {
        QQuickView view;
        QQuickItem *bar = openBar(&view, QStringLiteral("/vault"),
                                   QStringLiteral("/vault/sub/talk.mp3"));
        QVERIFY2(bar, "could not load BottomBar.qml / locate it");

        const QList<QQuickItem *> segments = segmentsOf(bar);
        QCOMPARE(segments.size(), 3);
        clickCenter(&view, segments.at(1));
        QCOMPARE(DesktopIntegrationStub::s_calls.size(), 1);
        QCOMPARE(DesktopIntegrationStub::s_calls.at(0).url, QUrl(QStringLiteral("file:///vault/sub")));
    }

    // With no file open the root is the only segment, and it still opens the
    // vault folder.
    void noFileOpen_rootIsTheOnlySegment()
    {
        QQuickView view;
        QQuickItem *bar = openBar(&view, QStringLiteral("/vault"), QString());
        QVERIFY2(bar, "could not load BottomBar.qml / locate it");

        const QList<QQuickItem *> segments = segmentsOf(bar);
        QCOMPARE(textsOf(segments), QStringList{QStringLiteral("/vault")});

        clickCenter(&view, segments.at(0));
        QCOMPARE(DesktopIntegrationStub::s_calls.size(), 1);
        QCOMPARE(DesktopIntegrationStub::s_calls.at(0).method, QStringLiteral("open"));
        QCOMPARE(DesktopIntegrationStub::s_calls.at(0).url, QUrl(QStringLiteral("file:///vault")));
    }

    // No vault: no segments at all, so nothing is clickable and nothing can be
    // sent to the file manager.
    void noVault_rendersNoClickableSegments()
    {
        QQuickView view;
        QQuickItem *bar = openBar(&view, QString(), QString());
        QVERIFY2(bar, "could not load BottomBar.qml / locate it");
        QCOMPARE(segmentsOf(bar).size(), 0);
        QCOMPARE(bar->property("pathSegments").toList().size(), 0);
    }

    // An open file outside the vault can't be broken down against the vault
    // root — splitting it would invent segments whose paths point nowhere. It
    // stays one whole-path segment that still reveals the real file.
    void fileOutsideVault_staysOneWholePathSegment()
    {
        QQuickView view;
        QQuickItem *bar = openBar(&view, QStringLiteral("/vault"),
                                   QStringLiteral("/elsewhere/stray.mp3"));
        QVERIFY2(bar, "could not load BottomBar.qml / locate it");

        const QList<QQuickItem *> segments = segmentsOf(bar);
        QCOMPARE(textsOf(segments), (QStringList{QStringLiteral("/vault"),
                                                  QStringLiteral("/elsewhere/stray.mp3")}));

        clickCenter(&view, segments.last());
        QCOMPARE(DesktopIntegrationStub::s_calls.size(), 1);
        QCOMPARE(DesktopIntegrationStub::s_calls.at(0).method, QStringLiteral("reveal"));
        QCOMPARE(DesktopIntegrationStub::s_calls.at(0).url, QUrl(QStringLiteral("file:///elsewhere/stray.mp3")));
    }

    // The segments must read as ONE continuous left-aligned
    // string. That holds only if exactly one segment claims Layout.fillWidth
    // and it's the LAST — an earlier claim takes the whole row and shoves
    // everything after it against the right edge, leaving a gap mid-path.
    // Asserted as geometry rather than by reading Layout.fillWidth back,
    // because the gap is the thing a stray claim actually breaks, and it stays
    // true however the layout is expressed.
    void segmentsRenderAsOneContinuousStringWithNoGaps()
    {
        QQuickView view;
        QQuickItem *bar = openBar(&view, QStringLiteral("/vault"),
                                   QStringLiteral("/vault/sub/deep/talk.mp3"));
        QVERIFY2(bar, "could not load BottomBar.qml / locate it");

        const QList<QQuickItem *> segments = segmentsOf(bar);
        QCOMPARE(segments.size(), 4);
        for (int i = 1; i < segments.size(); ++i) {
            const qreal prevRight = segments.at(i - 1)->mapToScene(
                QPointF(segments.at(i - 1)->width(), 0)).x();
            const qreal thisLeft = segments.at(i)->mapToScene(QPointF(0, 0)).x();
            QVERIFY2(qAbs(thisLeft - prevRight) < 1.0,
                     qPrintable(QStringLiteral("segment %1 starts at %2 but the one before it ends at "
                                                "%3 — the path has a gap in it")
                                    .arg(i).arg(thisLeft).arg(prevRight)));
        }
    }

    // Everything to the RIGHT of the path is one right-aligned status cluster,
    // and the "x/x tasks" counter is its last member — it must sit flush
    // against the bar's right margin, never immediately after the path. The
    // regression this guards: with bottomBarPathShrinkToContent on (the
    // default) the path yields its slack to the task label, but the task label
    // is HIDDEN when there's no job, an invisible Layout child claims no
    // fillWidth, and the whole cluster then packs left. Asserted as geometry,
    // because the misplacement is the thing a user sees however the layout is
    // expressed.
    void tasksCounter_sitsAtTheRightEdge_withNoJobShown()
    {
        QQuickView view;
        QQuickItem *bar = openBar(&view, QStringLiteral("/vault"),
                                   QStringLiteral("/vault/sub/talk.mp3"));
        QVERIFY2(bar, "could not load BottomBar.qml / locate it");
        QQuickItem *tasks = itemNamed(bar, QStringLiteral("bottomBarTasksCount"));
        QVERIFY2(tasks, "the x/x tasks counter must carry objectName \"bottomBarTasksCount\"");
        QVERIFY(tasks->isVisible());
        // The row's own right margin (BottomBar.qml's anchors.rightMargin).
        const qreal margin = 8.0;
        // QTRY: an item laid out this tick may not have settled its x yet.
        QTRY_VERIFY2(qAbs(bar->width() - margin
                          - tasks->mapToItem(bar, QPointF(tasks->width(), 0)).x()) < 1.5,
                     qPrintable(QStringLiteral("the tasks counter ends at %1 but the bar's "
                                                "right margin is at %2 — the right-hand "
                                                "cluster is not right-aligned")
                                    .arg(tasks->mapToItem(bar, QPointF(tasks->width(), 0)).x())
                                    .arg(bar->width() - margin)));
    }

    // Non-regression companion: the shrink-off mode is correct today (the
    // greedy path absorbs the slack) and the new spacer must not disturb it.
    void tasksCounter_sitsAtTheRightEdge_withNoJobShown_pathGreedyMode()
    {
        QQuickView view;
        QQuickItem *bar = openBar(&view, QStringLiteral("/vault"),
                                   QStringLiteral("/vault/sub/talk.mp3"), /*shrinkPath=*/false);
        QVERIFY2(bar, "could not load BottomBar.qml / locate it");
        QQuickItem *tasks = itemNamed(bar, QStringLiteral("bottomBarTasksCount"));
        QVERIFY2(tasks, "the x/x tasks counter must carry objectName \"bottomBarTasksCount\"");
        QVERIFY(tasks->isVisible());
        const qreal margin = 8.0;
        QTRY_VERIFY2(qAbs(bar->width() - margin
                          - tasks->mapToItem(bar, QPointF(tasks->width(), 0)).x()) < 1.5,
                     qPrintable(QStringLiteral("the tasks counter ends at %1 but the bar's "
                                                "right margin is at %2 — the right-hand "
                                                "cluster is not right-aligned")
                                    .arg(tasks->mapToItem(bar, QPointF(tasks->width(), 0)).x())
                                    .arg(bar->width() - margin)));
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppSettings.qml's Settings{} element
    // is loaded transitively by BottomBar.qml, and must never touch the real
    // ~/.config/suopTranscriber (tests must never touch the user's
    // real vault or app config). Same setup as tst_sidebar_expand.cpp.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_bottombar_pathsegments"));
    QGuiApplication app(argc, argv);
    TstBottomBarPathSegments tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_bottombar_pathsegments.moc"
