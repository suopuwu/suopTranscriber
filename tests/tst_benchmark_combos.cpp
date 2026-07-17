#include <QtTest>

#include <QFile>

// Source-scan guards for the benchmark overhaul + diarization tuning UI. These
// are QML-delegate behaviors with no QQuickView harness (same rationale as
// tst_styleguide.cpp): the diarization-sweep combo math, the >10-test warning,
// the benchmark's promotion to a top-level page, and the diarization pickers +
// sensitivity slider all live in QML wiring that a headless unit test can only
// check structurally. Paths injected by tests/CMakeLists.txt (SUOP_QML_DIR).
class TstBenchmarkCombos : public QObject
{
    Q_OBJECT

    static QString readAll(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        return QString::fromUtf8(f.readAll());
    }

private slots:
    // Benchmark is a TOP-LEVEL page (its own TitleBar button), not a Settings
    // sub-page (moved per the user request "like usb grabber").
    void benchmarkIsTopLevelPage_notSettingsSubPage()
    {
        const QString titleBar = readAll(QStringLiteral(SUOP_QML_DIR "/TitleBar.qml"));
        QVERIFY2(titleBar.contains(QStringLiteral("titleBarBenchmarkButton")), "no benchmark title-bar button");
        QVERIFY2(titleBar.contains(QStringLiteral("signal benchmarkRequested")), "no benchmarkRequested signal");

        const QString main = readAll(QStringLiteral(SUOP_QML_DIR "/Main.qml"));
        QVERIFY2(main.contains(QStringLiteral("onBenchmarkRequested")), "Main.qml doesn't handle benchmarkRequested");

        const QString mainPanel = readAll(QStringLiteral(SUOP_QML_DIR "/MainPanel.qml"));
        QVERIFY2(mainPanel.contains(QStringLiteral("benchmarkOpen")), "MainPanel has no benchmarkOpen flag");
        QVERIFY2(mainPanel.contains(QStringLiteral("BenchmarkPage")), "MainPanel doesn't mount BenchmarkPage");

        // No longer reachable through Settings' nav rail.
        const QString settings = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(!settings.contains(QStringLiteral("qsTr(\"Benchmark\")")),
                 "Benchmark should be removed from SettingsPopup pageNames");
    }

    // The diarization sweep multiplies chosen segmentation × embedding ×
    // sensitivity, and warns above ~10 tests before running.
    void diarizationSweep_hasComboMathAndWarning()
    {
        const QString page = readAll(QStringLiteral(SUOP_QML_DIR "/BenchmarkPage.qml"));
        QVERIFY2(!page.isEmpty(), "BenchmarkPage.qml not found");

        // Cartesian product over the three selection axes.
        QVERIFY2(page.contains(QStringLiteral("selectedSegIds")), "no segmentation selection");
        QVERIFY2(page.contains(QStringLiteral("selectedEmbedIds")), "no embedding selection");
        QVERIFY2(page.contains(QStringLiteral("selectedThresholds")), "no sensitivity selection");
        QVERIFY2(page.contains(QStringLiteral("buildDiarizationCombos")), "no combo builder");
        // count = segs * embeds * thresholds.
        QVERIFY2(page.contains(QStringLiteral("diarizationComboCount")), "no combo count");

        // The >10 gate + a confirm before a large sweep.
        QVERIFY2(page.contains(QStringLiteral("> 10")), "no >10-test threshold");
        QVERIFY2(page.contains(QStringLiteral("diarizationConfirmDialog")), "no large-sweep confirm dialog");

        // Actually dispatches to the backend sweep + defaults to the open file.
        QVERIFY2(page.contains(QStringLiteral("runDiarizationBenchmark")), "doesn't call runDiarizationBenchmark");
        QVERIFY2(page.contains(QStringLiteral("currentFilePath")), "benchmark doesn't use the open file");
    }

    // The diarization model pickers + the continuous sensitivity slider live in
    // Settings and carry the StyleGuide hover treatment (HoverBorder).
    void settingsHasDiarizationControls_withHoverTreatment()
    {
        const QString settings = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(settings.contains(QStringLiteral("segmentationModelCombo")), "no segmentation picker");
        QVERIFY2(settings.contains(QStringLiteral("embeddingModelCombo")), "no embedding picker");
        QVERIFY2(settings.contains(QStringLiteral("diarizationSensitivitySlider")), "no sensitivity slider");
        // StyleGuide: interactive controls show a HoverBorder.
        QVERIFY2(settings.contains(QStringLiteral("active: segmentationModelCombo.hovered")),
                 "segmentation picker missing HoverBorder");
        QVERIFY2(settings.contains(QStringLiteral("active: diarizationSensitivitySlider.hovered")),
                 "sensitivity slider missing HoverBorder");
    }
};

QTEST_GUILESS_MAIN(TstBenchmarkCombos)
#include "tst_benchmark_combos.moc"
