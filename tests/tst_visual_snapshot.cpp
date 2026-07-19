// OPT-IN pixel-snapshot harness — the coarse, flakier tier of visual testing,
// deliberately DORMANT by default. It renders real QML components offscreen to a
// PNG and diffs them against a golden image with a perceptual tolerance (a
// percentage of pixels may differ by a small per-channel delta, absorbing the
// font-antialiasing / GPU-vs-software-raster noise that makes exact-match
// screenshot tests miserable).
//
// WHY OPT-IN: goldens are environment-specific (a different machine's font
// hinting shifts pixels), so they are NOT committed, and this test does nothing
// unless you ask it to. It exists for the narrow set of *emergent rendering*
// cases that can't be expressed as a property/geometry assertion (see the
// property-based [VIS] tests in tst_mainpanel_deleteundo / tst_bottombar_* for
// the reliable tier this complements, and LIVE_TESTING.md's "Reducing this
// list" section).
//
// USAGE (from the build dir):
//   SUOP_SNAPSHOT=update  ./tests/tst_visual_snapshot   # (re)generate goldens
//   SUOP_SNAPSHOT=compare ./tests/tst_visual_snapshot   # diff against goldens
//   (unset)               ./tests/tst_visual_snapshot   # QSKIP everything
// Goldens live in $SUOP_SNAPSHOT_DIR (default: <repo>/tests/snapshots/, which
// is .gitignored — regenerate per machine). `compare` with a missing golden
// QSKIPs that case (run `update` first), never silently passes.
//
// Same offscreen + sandboxed-config setup as the other QQuickView harnesses.

#include <QtTest>

#include <QDir>
#include <QImage>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>

#include "IconTheme.h"

namespace {

enum class Mode { Dormant, Update, Compare };

Mode currentMode()
{
    const QByteArray v = qgetenv("SUOP_SNAPSHOT").toLower();
    if (v == "update")
        return Mode::Update;
    if (v == "compare")
        return Mode::Compare;
    return Mode::Dormant;
}

// Fraction of pixels that differ by more than kPerChannelDelta on any channel.
// The tolerance is what makes this usable across machines/Qt versions; an exact
// match would flake on sub-pixel AA alone.
constexpr int kPerChannelDelta = 24;      // 0..255; below this, treat as "same"
constexpr double kAllowedFraction = 0.02; // fail if >2% of pixels genuinely differ

double diffFraction(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return 1.0; // a size change is a total mismatch
    const QImage ia = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage ib = b.convertToFormat(QImage::Format_RGBA8888);
    qint64 differing = 0;
    const qint64 total = qint64(ia.width()) * ia.height();
    for (int y = 0; y < ia.height(); ++y) {
        const uchar *ra = ia.constScanLine(y);
        const uchar *rb = ib.constScanLine(y);
        for (int x = 0; x < ia.width() * 4; ++x) {
            if (qAbs(int(ra[x]) - int(rb[x])) > kPerChannelDelta) {
                ++differing;
                x = (x / 4) * 4 + 3; // count each pixel once, skip its remaining channels
            }
        }
    }
    return total > 0 ? double(differing) / double(total) : 0.0;
}

} // namespace

class TstVisualSnapshot : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_goldenDir;

    // Renders an inline QML scene (built against the real SuopTranscriber module
    // so AppIcon et al. paint exactly as in the app) to a QImage, offscreen.
    QImage render(const QString &sceneQml, const QSize &size)
    {
        QQuickView view;
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId), new IconImageProvider);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(size);

        const QString path = m_moduleDir.filePath(QStringLiteral("scene.qml"));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return {};
        f.write(sceneQml.toUtf8());
        f.close();

        view.setSource(QUrl::fromLocalFile(path));
        if (view.status() != QQuickView::Ready)
            return {};
        view.show();
        if (!QTest::qWaitForWindowExposed(&view))
            return {};
        // One extra event-loop turn so async AppIcon image-provider requests
        // have painted before the grab.
        QTest::qWait(50);
        return view.grabWindow();
    }

    // update: write the render as the golden. compare: diff vs the golden (QSKIP
    // if absent). Returns via QTest macros; caller QSKIPs in Dormant mode first.
    void checkSnapshot(const QString &name, const QString &sceneQml, const QSize &size)
    {
        const QImage shot = render(sceneQml, size);
        QVERIFY2(!shot.isNull(), qPrintable(QStringLiteral("failed to render scene '%1'").arg(name)));
        const QString goldenPath = m_goldenDir + QStringLiteral("/") + name + QStringLiteral(".png");

        if (currentMode() == Mode::Update) {
            QVERIFY(QDir().mkpath(m_goldenDir));
            QVERIFY2(shot.save(goldenPath, "PNG"), qPrintable(QStringLiteral("could not write golden %1").arg(goldenPath)));
            qInfo("wrote golden: %s", qPrintable(goldenPath));
            return;
        }

        // Compare.
        if (!QFileInfo::exists(goldenPath))
            QSKIP(qPrintable(QStringLiteral("no golden for '%1' — run with SUOP_SNAPSHOT=update first").arg(name)));
        QImage golden(goldenPath);
        QVERIFY2(!golden.isNull(), qPrintable(QStringLiteral("golden %1 failed to load").arg(goldenPath)));
        const double frac = diffFraction(shot, golden);
        QVERIFY2(frac <= kAllowedFraction,
                 qPrintable(QStringLiteral("snapshot '%1' differs from its golden in %2%% of pixels "
                                           "(allowed %3%%) — inspect %4 vs a fresh render")
                                .arg(name)
                                .arg(frac * 100.0, 0, 'f', 2)
                                .arg(kAllowedFraction * 100.0, 0, 'f', 2)
                                .arg(goldenPath)));
    }

private slots:
    void initTestCase()
    {
        if (currentMode() == Mode::Dormant)
            return; // nothing to set up; every case QSKIPs

        QVERIFY(m_moduleDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        QVERIFY2(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")),
                 "failed to symlink qml/ into the scratch QML module dir");
        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
        qmldir.write("module SuopTranscriber\n"
                     "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                     "AppIcon 1.0 qml/AppIcon.qml\n");
        qmldir.close();
        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });

        const QByteArray envDir = qgetenv("SUOP_SNAPSHOT_DIR");
        m_goldenDir = envDir.isEmpty() ? QStringLiteral(SUOP_REPO_DIR "/tests/snapshots")
                                       : QString::fromLocal8Bit(envDir);
    }

    // A representative scene: real AppIcons in the enabled + disabled delete-file
    // colors on the app's dark panel background. This is the kind of "does it
    // actually paint right" check that has no property proxy — it exercises the
    // SVG icon provider + compositing, which is exactly where pixel diffs earn
    // their keep. Add more scenes (whole screens, dialogs, both themes) the same
    // way once goldens are being generated on the target machine.
    void deleteIcon_enabledAndDisabledColors()
    {
        if (currentMode() == Mode::Dormant)
            QSKIP("SUOP_SNAPSHOT unset — visual snapshots are opt-in (set =update or =compare)");

        const QString scene = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Rectangle {\n"
            "    color: \"#2b2b2b\"\n"
            "    Row {\n"
            "        anchors.centerIn: parent\n"
            "        spacing: 24\n"
            "        AppIcon { name: \"delete-file\"; iconSize: 24; color: \"#e07a7a\" }\n"
            "        AppIcon { name: \"delete-file\"; iconSize: 24; color: \"#666666\" }\n"
            "    }\n"
            "}\n");
        checkSnapshot(QStringLiteral("delete_icon_colors"), scene, QSize(160, 80));
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_visual_snapshot"));
    QGuiApplication app(argc, argv);
    TstVisualSnapshot tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_visual_snapshot.moc"
