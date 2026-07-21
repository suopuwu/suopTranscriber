#include <QtTest>

#include <QFile>
#include <QRegularExpression>

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

    // `src` with its // line comments removed, so a "does the code do X" scan
    // can't be satisfied by prose merely MENTIONING X (a comment naming a moved
    // literal would otherwise keep a stale guard passing). Skips '//' inside
    // double-quoted strings so literals like "file://" survive. Mirrors
    // tst_styleguide.cpp's helper of the same name.
    static QString stripComments(const QString &src)
    {
        QString out;
        out.reserve(src.size());
        bool inString = false;
        for (int i = 0; i < src.size(); ++i) {
            const QChar c = src.at(i);
            if (inString) {
                if (c == QLatin1Char('\\') && i + 1 < src.size()) {
                    out += c;
                    out += src.at(++i);
                    continue;
                }
                if (c == QLatin1Char('"'))
                    inString = false;
                out += c;
                continue;
            }
            if (c == QLatin1Char('"')) {
                inString = true;
                out += c;
                continue;
            }
            if (c == QLatin1Char('/') && i + 1 < src.size() && src.at(i + 1) == QLatin1Char('/')) {
                while (i < src.size() && src.at(i) != QLatin1Char('\n'))
                    ++i;
                if (i < src.size())
                    out += QLatin1Char('\n');
                continue;
            }
            out += c;
        }
        return out;
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
        const QString settings = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml")));
        QVERIFY2(settings.contains(QStringLiteral("segmentationModelCombo")), "no segmentation picker");
        QVERIFY2(settings.contains(QStringLiteral("embeddingModelCombo")), "no embedding picker");
        // StyleGuide: interactive controls show a HoverBorder. The segmentation
        // picker is still inline, so its HoverBorder is asserted here directly.
        QVERIFY2(settings.contains(QStringLiteral("active: segmentationModelCombo.hovered")),
                 "segmentation picker missing HoverBorder");

        // The sensitivity row was collapsed into a SliderSetting. It must exist
        // as such in the popup...
        const QRegularExpression sensitivityRow(
            QStringLiteral("SliderSetting\\s*\\{[^}]*id:\\s*diarizationSensitivitySlider"));
        QVERIFY2(sensitivityRow.match(settings).hasMatch(),
                 "sensitivity control must be a SliderSetting { id: diarizationSensitivitySlider ... }");

        // ...and SliderSetting must give its Slider a HoverBorder driven by the
        // slider's own hovered state, so every SliderSetting row (this one
        // included) carries the StyleGuide hover treatment.
        const QString sliderSetting = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/SliderSetting.qml")));
        QVERIFY2(!sliderSetting.isEmpty(), "qml/SliderSetting.qml must exist");
        QVERIFY2(sliderSetting.contains(QStringLiteral("HoverBorder")), "SliderSetting missing HoverBorder");
        QVERIFY2(sliderSetting.contains(QStringLiteral("active: control.hovered")),
                 "SliderSetting's Slider must drive its HoverBorder from control.hovered (the slider's own "
                 "hover state) — otherwise the sensitivity row loses the StyleGuide hover treatment");
    }
};

QTEST_GUILESS_MAIN(TstBenchmarkCombos)
#include "tst_benchmark_combos.moc"
