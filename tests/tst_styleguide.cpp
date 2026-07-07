#include <QtTest>

#include <QDir>
#include <QFile>
#include <QRegularExpression>

// Source-scan checks for the conventions in StyleGuide.md and the environment
// constraints in CLAUDE.md ("force QT_MEDIA_BACKEND=ffmpeg"). These read the
// checked-in sources (paths injected by tests/CMakeLists.txt), so they guard
// the conventions without needing a running GUI.
class TstStyleGuide : public QObject
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
    // StyleGuide.md: the hover outline is the reusable HoverBorder.qml — a
    // transparent Rectangle, 1px white border, radius from AppSettings,
    // opacity animated in on hover.
    void hoverBorder_componentContract()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/HoverBorder.qml"));
        QVERIFY2(!src.isEmpty(), "qml/HoverBorder.qml must exist (StyleGuide.md hover convention)");
        QVERIFY(src.contains(QStringLiteral("color: \"transparent\"")));
        QVERIFY(src.contains(QStringLiteral("radius: AppSettings.borderRadius")));
        QVERIFY(src.contains(QStringLiteral("border.width: 1")));
        QVERIFY(src.contains(QStringLiteral("border.color: \"white\"")));
        QVERIFY2(src.contains(QStringLiteral("Behavior on opacity")),
                 "hover outline must fade, not pop (StyleGuide.md)");
    }

    // StyleGuide.md: one global AppSettings.borderRadius, used everywhere a
    // corner is rounded — never an ad-hoc literal. Exception: a radius equal
    // to half the item's width/height is drawing a circle (status dots,
    // slider handle), not rounding a corner.
    void borderRadius_noHardcodedCornerRadii()
    {
        const QDir qmlDir(QStringLiteral(SUOP_QML_DIR));
        const QRegularExpression radiusLiteral(QStringLiteral("^\\s*radius:\\s*(\\d+(?:\\.\\d+)?)\\s*$"));
        QStringList violations;
        const QStringList files = qmlDir.entryList({QStringLiteral("*.qml")}, QDir::Files);
        QVERIFY(!files.isEmpty());
        for (const QString &name : files) {
            const QStringList lines = readAll(qmlDir.filePath(name)).split(QLatin1Char('\n'));
            for (int i = 0; i < lines.size(); ++i) {
                const auto m = radiusLiteral.match(lines[i]);
                if (!m.hasMatch())
                    continue;
                const double r = m.captured(1).toDouble();
                // Circle idiom: nearby width/height == 2 * radius.
                bool isCircle = false;
                const QRegularExpression size(
                    QStringLiteral("^\\s*(?:width|height|implicitWidth|implicitHeight):\\s*(\\d+(?:\\.\\d+)?)\\s*$"));
                for (int j = qMax(0, i - 6); j < qMin<qsizetype>(lines.size(), i + 7); ++j) {
                    const auto s = size.match(lines[j]);
                    if (s.hasMatch() && qFuzzyCompare(s.captured(1).toDouble(), 2.0 * r))
                        isCircle = true;
                }
                if (!isCircle)
                    violations << QStringLiteral("%1:%2: %3").arg(name).arg(i + 1).arg(lines[i].trimmed());
            }
        }
        QVERIFY2(violations.isEmpty(),
                 qPrintable(QStringLiteral("hardcoded corner radius (use AppSettings.borderRadius):\n")
                            + violations.join(QLatin1Char('\n'))));
    }

    // StyleGuide.md: settings are properties on the AppSettings singleton
    // (no bespoke storage), edited via SettingsPopup bound to it, with the
    // defaults map as the single source of truth for reset buttons.
    void settings_appSettingsSingletonConvention()
    {
        const QString src = readAll(QStringLiteral(SUOP_QML_DIR "/AppSettings.qml"));
        QVERIFY(src.startsWith(QStringLiteral("pragma Singleton")));
        QVERIFY2(src.contains(QStringLiteral("import QtCore")) && src.contains(QStringLiteral("Settings {")),
                 "AppSettings persists via QtCore Settings (QSettings-backed), no bespoke storage");
        QVERIFY(src.contains(QStringLiteral("readonly property var defaults:")));
        QVERIFY(src.contains(QStringLiteral("function isDefault(")));
        QVERIFY(src.contains(QStringLiteral("function resetProperty(")));
        QVERIFY(src.contains(QStringLiteral("function resetAll(")));

        const QString popup = readAll(QStringLiteral(SUOP_QML_DIR "/SettingsPopup.qml"));
        QVERIFY2(popup.contains(QStringLiteral("AppSettings.")),
                 "settings UI lives in SettingsPopup.qml bound to AppSettings (StyleGuide.md)");
    }

    // CLAUDE.md load-bearing constraint: Qt Multimedia only *plays* audio and
    // must use the ffmpeg backend — forced before QGuiApplication exists.
    void mainCpp_forcesFfmpegMediaBackend()
    {
        const QString src = readAll(QStringLiteral(SUOP_SRC_DIR "/main.cpp"));
        const qsizetype setEnv = src.indexOf(QStringLiteral("qputenv(\"QT_MEDIA_BACKEND\", \"ffmpeg\")"));
        const qsizetype appCtor = src.indexOf(QStringLiteral("QGuiApplication app("));
        QVERIFY2(setEnv >= 0, "main.cpp must force QT_MEDIA_BACKEND=ffmpeg (CLAUDE.md)");
        QVERIFY2(appCtor > setEnv, "backend must be forced before QGuiApplication is constructed");
    }

    // Milestone 2 decision: the desktop's QQC2 theme (KDE Breeze) breaks this
    // app; the bundled Basic style is forced instead.
    void mainCpp_forcesBasicControlsStyle()
    {
        const QString src = readAll(QStringLiteral(SUOP_SRC_DIR "/main.cpp"));
        const qsizetype setEnv = src.indexOf(QStringLiteral("qputenv(\"QT_QUICK_CONTROLS_STYLE\", \"Basic\")"));
        const qsizetype appCtor = src.indexOf(QStringLiteral("QGuiApplication app("));
        QVERIFY(setEnv >= 0);
        QVERIFY(appCtor > setEnv);
    }

    // qt-window-effects constraint: transparency needs an 8-bit alpha surface
    // format installed as the default before QGuiApplication exists.
    void mainCpp_alphaSurfaceBeforeApp()
    {
        const QString src = readAll(QStringLiteral(SUOP_SRC_DIR "/main.cpp"));
        const qsizetype alpha = src.indexOf(QStringLiteral("setAlphaBufferSize(8)"));
        const qsizetype appCtor = src.indexOf(QStringLiteral("QGuiApplication app("));
        QVERIFY(alpha >= 0);
        QVERIFY(appCtor > alpha);
    }

    // StyleGuide.md: every interactable gets Qt.PointingHandCursor + a hover
    // outline. "Every interactable" is not statically enumerable, so this
    // documents the rule and sanity-checks that the convention is in use at
    // all rather than proving completeness.
    void hoverCursor_conventionInUse()
    {
        const QDir qmlDir(QStringLiteral(SUOP_QML_DIR));
        int cursorFiles = 0;
        int hoverBorderUses = 0;
        const QStringList files = qmlDir.entryList({QStringLiteral("*.qml")}, QDir::Files);
        for (const QString &name : files) {
            if (name == QLatin1String("HoverBorder.qml"))
                continue;
            const QString src = readAll(qmlDir.filePath(name));
            if (src.contains(QStringLiteral("PointingHandCursor")))
                ++cursorFiles;
            if (src.contains(QStringLiteral("HoverBorder {")))
                ++hoverBorderUses;
        }
        QVERIFY2(cursorFiles >= 4, "pointer-cursor convention should appear across the UI files");
        QVERIFY2(hoverBorderUses >= 3, "HoverBorder should be the reused hover-outline component");
    }
};

QTEST_GUILESS_MAIN(TstStyleGuide)
#include "tst_styleguide.moc"
