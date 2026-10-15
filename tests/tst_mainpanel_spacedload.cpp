// Living spec: a transcript loads on cold open regardless of whether its path
// has spaces, AND a very large diarized transcript renders fully without any
// virtualization.
//
// The bug: a large diarized file rendered BLANK on cold open.
// It looked like a scale problem (3469 eager gutter Repeater delegates + a
// ~380 KB monolithic TextArea), but a 2x2 probe — {small, large} x {plain,
// spaced} path — proved size was a red herring: LARGE/PLAIN loads in ~1.2s with
// all 3469 gutter rects laid out, while BOTH spaced-path cases blanked at any
// size. Root cause was the async load guard in MainPanel.onTranscriptLoaded,
// which compared a re-encoded QUrl.toString() ("%20") against the raw
// "file://"+orphanedMdPath load key (literal ' ') and so discarded EVERY load
// for a spaced path -> permanent blank. Fix: compare decoded local paths via
// isSameFile(), the same seam the live-reload guard already used.
//
// This test keeps the 2x2 as a regression pin: the spaced cases guard the fix,
// and LARGE/PLAIN documents that ~3469 segments render eagerly and fast (which
// is WHY the item's virtualization option was not taken). Harness cloned from
// tst_mainpanel_livescroll.cpp. Touches only a QTemporaryDir; never the user's
// vault/config.

#include <QtTest>

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "Clipboard.h"
#include "TranscriptDisplayBuilder.h"
#include "UndoStack.h"
#include "IconTheme.h"
#include "TranscriptionController.h"

namespace {

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

// A realistic diarized transcript: `speakers:` allowlist + `[t0] [Speaker N] text`
// lines, first segment at 7:03 like the real repro, ~3.4s/segment, alternating
// speaker turns, plus one mid-transcript silence span.
QString diarizedMd(int n)
{
    QString body = QStringLiteral(
        "---\nmodel: large-v3\nspeakerDetection: true\n"
        "speakers: Speaker 1, Speaker 2\nvad: true\n"
        "silences: 500-512\ntranscribedAt: 2026-07-22T10:00:00\n---\n\n");
    const double firstT0 = 423.0;
    const double step = 3.4;
    for (int i = 0; i < n; ++i) {
        const double t = firstT0 + i * step;
        const char *spk = (i % 8 < 4) ? "Speaker 1" : "Speaker 2";
        body += QStringLiteral("[%1] [%2] Transcript segment number %3 with several "
                               "words so it wraps nicely and looks realistic.\n")
                    .arg(t, 0, 'f', 1).arg(QLatin1String(spk)).arg(i);
    }
    return body;
}

} // namespace

class TstMainPanelSpacedLoad : public QObject
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

    QQuickItem *openPanel(QQuickView *view, TranscriptionController **cOut)
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
        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        return panel;
    }

    // Opens `mdPath` via the real async orphaned-md load path and prints a full
    // measurement line. Returns whether the body ever loaded (guard passed).
    bool probe(const QString &label, const QString &mdPath, int expectedSegments)
    {
        QQuickView view;
        QQuickItem *panel = openPanel(&view, nullptr);
        if (!panel) { qWarning("[%s] PANEL FAILED TO LOAD", qPrintable(label)); return false; }

        QElapsedTimer t; t.start();
        panel->setProperty("orphanedMdPath", mdPath);
        const bool bodyLoaded = QTest::qWaitFor([panel]() {
            return !panel->property("transcriptBody").toString().isEmpty();
        }, 8000);
        const qint64 bodyMs = t.elapsed();
        QTest::qWait(400); // let displayData + Repeaters + layout settle

        const int bodyLen = panel->property("transcriptBody").toString().size();
        const int sentences = panel->property("sentences").toList().size();
        const int displaySentences = panel->property("displaySentences").toList().size();

        QQuickItem *area = panel->findChild<QQuickItem *>(QStringLiteral("transcriptArea"));
        const int areaLen = area ? area->property("length").toInt() : -1;
        const bool areaVisible = area ? area->property("visible").toBool() : false;

        int rectCount = 0, nonZeroRects = 0;
        if (area) {
            const QVariantList rects = area->property("gutterLineRects").toList();
            rectCount = rects.size();
            for (const QVariant &v : rects) {
                const QRectF r = v.toRectF();
                if (r.width() > 0.0 || r.height() > 0.0) ++nonZeroRects;
            }
        }

        qInfo().noquote() << QStringLiteral(
            "[%1] bodyLoaded=%2 (%3ms) bodyLen=%4 sentences=%5 displaySentences=%6 "
            "expected=%7 | transcriptArea: len=%8 visible=%9 | gutterLineRects: n=%10 nonZero=%11")
            .arg(label).arg(bodyLoaded).arg(bodyMs).arg(bodyLen).arg(sentences)
            .arg(displaySentences).arg(expectedSegments)
            .arg(areaLen).arg(areaVisible).arg(rectCount).arg(nonZeroRects);

        return bodyLoaded && sentences >= expectedSegments;
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

        qmlRegisterSingletonType<IconTheme>("SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterSingletonType<Clipboard>("SuopTranscriber", 1, 0, "Clipboard",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new Clipboard; });
        qmlRegisterSingletonType<TranscriptDisplayBuilder>(
            "SuopTranscriber", 1, 0, "TranscriptDisplayBuilder",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new TranscriptDisplayBuilder; });
        qmlRegisterType<UndoStack>("SuopTranscriber", 1, 0, "UndoStack");
        qmlRegisterType<TranscriptionController>("SuopTranscriber", 1, 0, "TranscriptionController");
        qmlRegisterType<AudioPlayerStub>("SuopTranscriber", 1, 0, "AudioPlayer");
        qmlRegisterUncreatableType<AudioRecorderStub>("SuopTranscriber", 1, 0, "AudioRecorder", QStringLiteral("stub"));
        qmlRegisterUncreatableType<VaultTreeModelStub>("SuopTranscriber", 1, 0, "VaultTreeModel", QStringLiteral("stub"));
        qmlRegisterUncreatableType<UsbGrabberControllerStub>("SuopTranscriber", 1, 0, "UsbGrabberController", QStringLiteral("stub"));

        const QString driverQml = QStringLiteral(
            "import QtQuick\nimport SuopTranscriber\n"
            "Item { width: 900; height: 600\n"
            "  MainPanel { objectName: \"mainPanel\"; anchors.fill: parent }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QVERIFY(writeFile(m_driverQmlPath, driverQml));
    }

    // The 2x2. Each prints a measurement line; the final QVERIFY documents the
    // EXPECTED-good cases so a regression is loud, but every scenario is measured
    // regardless of pass/fail.
    void smallPlain()
    {
        const QString md = m_vaultDir.filePath(QStringLiteral("small_plain.wav.md"));
        QVERIFY(writeFile(md, diarizedMd(12)));
        QVERIFY2(probe(QStringLiteral("SMALL/PLAIN"), md, 12), "small diarized plain-path did not render");
    }

    void smallSpaced()
    {
        QVERIFY(QDir().mkpath(m_vaultDir.filePath(QStringLiteral("spaced dir"))));
        const QString md = m_vaultDir.filePath(QStringLiteral("spaced dir/small spaced.wav.md"));
        QVERIFY(writeFile(md, diarizedMd(12)));
        const bool ok = probe(QStringLiteral("SMALL/SPACED"), md, 12);
        if (!ok) qWarning("SMALL/SPACED BLANKED -> load guard regressed: onTranscriptLoaded must "
                          "compare decoded local paths (isSameFile), not toString()");
        QVERIFY2(ok, "diarized transcript at a SPACED path did not render (load-guard discard)");
    }

    void largePlain()
    {
        const QString md = m_vaultDir.filePath(QStringLiteral("large_plain.wav.md"));
        QVERIFY(writeFile(md, diarizedMd(3469)));
        const bool ok = probe(QStringLiteral("LARGE/PLAIN"), md, 3469);
        if (!ok) qWarning("LARGE/PLAIN BLANKED -> a ~3469-segment transcript no longer renders "
                          "eagerly; the no-virtualization assumption may no longer hold");
        QVERIFY2(ok, "large diarized plain-path did not render");
    }

    void largeSpaced()
    {
        QVERIFY(QDir().mkpath(m_vaultDir.filePath(QStringLiteral("spaced dir"))));
        const QString md = m_vaultDir.filePath(QStringLiteral("spaced dir/large spaced.wav.md"));
        QVERIFY(writeFile(md, diarizedMd(3469)));
        const bool ok = probe(QStringLiteral("LARGE/SPACED"), md, 3469);
        QVERIFY2(ok, "large diarized SPACED-path did not render");
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_mainpanel_spacedload"));
    QGuiApplication app(argc, argv);
    TstMainPanelSpacedLoad tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_mainpanel_spacedload.moc"
