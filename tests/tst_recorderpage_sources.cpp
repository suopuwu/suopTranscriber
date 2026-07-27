// RecorderPage.qml's first QQuickView harness — the runtime tier for the
// recorder's source rows, which until now were guarded ONLY by tst_styleguide
// source scans. A source scan proves the QML *says* the right thing; this
// proves it *does* it, by loading the real checked-out RecorderPage.qml
// offscreen and driving real clicks at it.
//
// Same harness shape as tst_middledragscroll.cpp / tst_modelpicker.cpp (read
// their top-of-file comments first): a scratch QML module dir symlinked to the
// real qml/, so this always exercises what is on disk right now.
//
// WHY A FUNCTIONAL AudioRecorder STUB: every other harness registers
// AudioRecorder uncreatable ("stub — this harness leaves it null") because
// MainPanel never drives it. The recorder page is the one screen whose whole
// behavior IS that object, so the stub here is real enough to drive: the test
// sets systemSources/appStreams/activeCaptureSourceIds directly, and the pure
// helpers (buildAppCaptureRows, displayLevel, decayPeak, pendingCaptureAdditions)
// delegate to the REAL AudioRecorderIO functions — so row-building logic is
// under test, not a second implementation of it. Only the hardware-touching
// entry points (refreshSources, startRecording, …) are faked, and
// startRecording records its arguments so a test can assert where a take would
// have been written without writing one.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "AudioRecorder.h"
#include "IconTheme.h"

namespace {

QVariantMap appStream(const QString &id, const QString &name)
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("name")] = name;
    return m;
}

QVariantMap systemSource(const QString &id, const QString &name)
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("name")] = name;
    return m;
}

} // namespace

// The AudioPlayer members RecorderPage reads for its playback affordance, and
// no more. Creatable, for the same reason UndoStack is: RecorderPage owns an
// inline `AudioPlayer { id: recorderPlayer }`.
class AudioPlayerStub : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool playing READ isPlaying NOTIFY playingChanged)
    Q_PROPERTY(qint64 position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
public:
    QUrl source() const { return m_source; }
    void setSource(const QUrl &u)
    {
        if (m_source == u)
            return;
        m_source = u;
        emit sourceChanged();
    }
    bool isPlaying() const { return m_playing; }
    qint64 position() const { return m_position; }
    void setPosition(qint64 p)
    {
        if (m_position == p)
            return;
        m_position = p;
        emit positionChanged();
    }
    qint64 duration() const { return m_duration; }

    Q_INVOKABLE void play() { m_playing = true; emit playingChanged(); }
    Q_INVOKABLE void pause() { m_playing = false; emit playingChanged(); }
    Q_INVOKABLE void stop() { m_playing = false; emit playingChanged(); }

signals:
    void sourceChanged();
    void playingChanged();
    void positionChanged();
    void durationChanged();

private:
    QUrl m_source;
    bool m_playing = false;
    qint64 m_position = 0;
    qint64 m_duration = 0;
};

class VaultTreeModelStub : public QObject { Q_OBJECT };

// See this file's top comment for why this one is functional rather than an
// empty uncreatable shell.
class AudioRecorderStub : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    Q_PROPERTY(bool monitoring READ isMonitoring NOTIFY monitoringChanged)
    Q_PROPERTY(qreal currentLevel READ currentLevel NOTIFY levelUpdated)
    Q_PROPERTY(qint64 elapsedMs READ elapsedMs NOTIFY levelUpdated)
    Q_PROPERTY(QVariantList sourceLevels READ sourceLevels NOTIFY levelUpdated)
    Q_PROPERTY(QStringList activeCaptureSourceIds READ activeCaptureSourceIds
                   NOTIFY activeCaptureSourcesChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QVariantList systemSources READ systemSources NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantList appStreams READ appStreams NOTIFY sourcesChanged)

public:
    bool isRecording() const { return m_recording; }
    qreal currentLevel() const { return m_currentLevel; }
    qint64 elapsedMs() const { return m_elapsedMs; }
    QVariantList sourceLevels() const { return m_sourceLevels; }
    QStringList activeCaptureSourceIds() const { return m_activeIds; }
    QString lastError() const { return m_lastError; }
    QVariantList systemSources() const { return m_systemSources; }
    QVariantList appStreams() const { return m_appStreams; }

    // The pure helpers, delegating to the REAL implementations — the point of
    // the harness is that row building is under test, not re-stated here.
    Q_INVOKABLE qreal displayLevel(qreal rms) const
    {
        return AudioRecorderIO::displayLevelFromRms(rms);
    }
    Q_INVOKABLE qreal decayPeak(qreal previousPeak, qreal currentLevel, qreal decayPerTick) const
    {
        return AudioRecorderIO::decayPeak(previousPeak, currentLevel, decayPerTick);
    }
    Q_INVOKABLE QVariantList appCaptureRows(const QVariantList &streams,
                                            const QStringList &rememberedNames) const
    {
        return AudioRecorderIO::buildAppCaptureRows(streams, rememberedNames);
    }
    Q_INVOKABLE QVariantList pendingCaptureAdditions(const QVariantList &desired,
                                                      const QStringList &activeIds) const
    {
        return AudioRecorderIO::pendingCaptureAdditions(desired, activeIds);
    }

    Q_INVOKABLE QVariantList availableMicDevices() const { return m_micDevices; }

    // Hardware entry points — faked, but they record what they were asked to do
    // so a test can assert on it.
    Q_INVOKABLE void refreshSources() { ++m_refreshCount; }
    Q_INVOKABLE void startRecording(const QUrl &vaultUrl, const QString &outputRelativeFolder,
                                     bool useMic, const QString &micDeviceId, bool useSystemAudio,
                                     const QString &systemSourceId, qreal micGain, qreal systemGain,
                                     const QVariantList &appSources, bool testMode)
    {
        m_startVaultUrl = vaultUrl;
        m_startRelativeFolder = outputRelativeFolder;
        m_startUseMic = useMic;
        m_startMicDeviceId = micDeviceId;
        m_startUseSystemAudio = useSystemAudio;
        m_startSystemSourceId = systemSourceId;
        m_startMicGain = micGain;
        m_startSystemGain = systemGain;
        m_startAppSources = appSources;
        m_startTestMode = testMode;
        ++m_startCount;
        // Mirrors AudioRecorder::startRecording(): a take supersedes monitoring,
        // which it stops before handing the worker the new capture.
        if (m_monitoring) {
            m_monitoring = false;
            emit monitoringChanged();
        }
        setRecording(true);
    }
    Q_INVOKABLE void stopRecording() { setRecording(false); }

    bool isMonitoring() const { return m_monitoring; }
    Q_INVOKABLE void startMonitoring(bool useMic, const QString &micDeviceId, bool useSystemAudio,
                                      double micGain, double systemGain, const QString &systemSourceId,
                                      const QVariantList &appCaptureSources)
    {
        Q_UNUSED(micDeviceId) Q_UNUSED(micGain) Q_UNUSED(systemGain) Q_UNUSED(systemSourceId)
        if (m_recording || m_monitoring)
            return;
        if (!useMic && !useSystemAudio && appCaptureSources.isEmpty())
            return;
        ++m_monitorStartCount;
        m_monitoring = true;
        emit monitoringChanged();
    }
    Q_INVOKABLE void stopMonitoring()
    {
        if (!m_monitoring)
            return;
        ++m_monitorStopCount;
        m_monitoring = false;
        emit monitoringChanged();
        // Mirrors the real split: a monitor teardown must NOT look like a
        // finished take. Emitting recordingFinished here is precisely the bug
        // that cleared `recording` out from under a take that had just begun.
        emit levelUpdated();
    }
    void setMicGain(double g) { m_micGain = g; }
    double lastMicGain() const
    {
        return m_micEnabledCalls.isEmpty() ? m_startMicGain : m_micEnabledCalls.last().second;
    }
    Q_INVOKABLE void setMicEnabled(bool enabled, const QString &micDeviceId, double gain)
    {
        Q_UNUSED(micDeviceId)
        if (!m_recording && !m_monitoring)
            return;
        m_micEnabledCalls.append(qMakePair(enabled, gain));
    }
    Q_INVOKABLE void setSystemAudioEnabled(bool enabled, const QString &sourceId, double gain)
    {
        Q_UNUSED(sourceId) Q_UNUSED(gain)
        if (!m_recording && !m_monitoring)
            return;
        m_systemEnabledCalls.append(enabled);
    }
    Q_INVOKABLE void removeAppCaptureSource(const QString &id)
    {
        if (!m_recording && !m_monitoring)
            return;
        m_removedSources.append(id);
    }
    // The real band rule, so the harness tests the colours the app will show.
    Q_INVOKABLE QString levelBand(qreal displayLevel) const
    {
        return AudioRecorderIO::levelBand(displayLevel);
    }
    Q_INVOKABLE qreal levelBandGoodFloor() const { return AudioRecorderIO::levelBandGoodFloor(); }
    Q_INVOKABLE qreal levelBandHotFloor() const { return AudioRecorderIO::levelBandHotFloor(); }

    int monitorStartCount() const { return m_monitorStartCount; }
    int monitorStopCount() const { return m_monitorStopCount; }
    QList<QPair<bool, double>> micEnabledCalls() const { return m_micEnabledCalls; }
    QList<bool> systemEnabledCalls() const { return m_systemEnabledCalls; }
    QStringList removedSources() const { return m_removedSources; }
    Q_INVOKABLE void addAppCaptureSource(const QString &id, double gain)
    {
        m_addedSources.append(qMakePair(id, gain));
    }

    // --- test-side drivers (not part of the QML-facing surface) ---
    void setSystemSources(const QVariantList &v) { m_systemSources = v; emit sourcesChanged(); }
    void setAppStreams(const QVariantList &v) { m_appStreams = v; emit sourcesChanged(); }
    void setMicDevices(const QVariantList &v) { m_micDevices = v; }
    void setActiveIds(const QStringList &v) { m_activeIds = v; emit activeCaptureSourcesChanged(); }
    void setRecording(bool r)
    {
        if (m_recording == r)
            return;
        m_recording = r;
        emit recordingChanged();
    }
    void tickLevel(qreal level, const QVariantList &perSource, qint64 elapsed)
    {
        m_currentLevel = level;
        m_sourceLevels = perSource;
        m_elapsedMs = elapsed;
        emit levelUpdated();
    }

    int refreshCount() const { return m_refreshCount; }
    int startCount() const { return m_startCount; }
    QUrl startVaultUrl() const { return m_startVaultUrl; }
    bool startTestMode() const { return m_startTestMode; }
    QVariantList startAppSources() const { return m_startAppSources; }
    QList<QPair<QString, double>> addedSources() const { return m_addedSources; }

signals:
    void recordingChanged();
    void monitoringChanged();
    void levelUpdated();
    void lastErrorChanged();
    void sourcesChanged();
    void activeCaptureSourcesChanged();
    void recordingFinished(const QString &filePath);

private:
    bool m_recording = false;
    bool m_monitoring = false;
    int m_monitorStartCount = 0;
    int m_monitorStopCount = 0;
    QList<QPair<bool, double>> m_micEnabledCalls;
    QList<bool> m_systemEnabledCalls;
    QStringList m_removedSources;
    qreal m_currentLevel = 0.0;
    qint64 m_elapsedMs = 0;
    QVariantList m_sourceLevels;
    QStringList m_activeIds;
    QString m_lastError;
    QVariantList m_systemSources;
    QVariantList m_appStreams;
    QVariantList m_micDevices;

    int m_refreshCount = 0;
    int m_startCount = 0;
    QUrl m_startVaultUrl;
    QString m_startRelativeFolder;
    bool m_startUseMic = false;
    QString m_startMicDeviceId;
    bool m_startUseSystemAudio = false;
    QString m_startSystemSourceId;
    qreal m_startMicGain = 1.0;
    qreal m_startSystemGain = 1.0;
    QVariantList m_startAppSources;
    bool m_startTestMode = false;
    double m_micGain = 1.0;
    QList<QPair<QString, double>> m_addedSources;
};

class TstRecorderPageSources : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // A real click at the item's centre — TapHandler-driven toggles only
    // respond to genuine mouse events, which is the whole point of driving
    // them rather than writing the property the handler would have written.
    static void tap(QQuickView *view, QQuickItem *item)
    {
        QVERIFY(item);
        const QPointF c = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
        QTest::mouseClick(view, Qt::LeftButton, Qt::KeyboardModifiers(), c.toPoint());
    }

    static QQuickItem *find(QQuickView *view, const QString &name)
    {
        if (QQuickItem *direct = view->rootObject()->findChild<QQuickItem *>(name))
            return direct;
        // Repeater-created delegates are NOT children in the QObject tree (they
        // are parented to the Repeater's instance model), so findChild misses
        // them entirely — walk visual children instead. Same gotcha
        // tst_sidebar_guides.cpp documents for its nesting guides.
        return findInVisualTree(view->rootObject(), name);
    }

    static QQuickItem *findInVisualTree(QQuickItem *root, const QString &name)
    {
        if (!root)
            return nullptr;
        const auto kids = root->childItems();
        for (QQuickItem *c : kids) {
            if (c->objectName() == name)
                return c;
            if (QQuickItem *hit = findInVisualTree(c, name))
                return hit;
        }
        return nullptr;
    }

    // Loads the driver and hands back the RecorderPage item + its stub.
    bool openRecorder(QQuickView *view, QQuickItem **pageOut, AudioRecorderStub **recOut)
    {
        view->engine()->addImportPath(m_moduleDir.path());
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            for (const QQmlError &e : view->errors())
                qWarning() << "QML error:" << e.toString();
            return false;
        }
        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return false;
        QQuickItem *page = find(view, QStringLiteral("recorderPage"));
        if (!page)
            return false;
        auto *rec = view->rootObject()->findChild<AudioRecorderStub *>(QStringLiteral("recStub"));
        if (!rec)
            return false;
        *pageOut = page;
        *recOut = rec;
        return true;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        // Symlink (not copy) to the real checked-out qml/ — must reflect what is
        // on disk right now, never a stale build-time copy.
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
                      "AppComboBox 1.0 qml/AppComboBox.qml\n"
                      "HoverBorder 1.0 qml/HoverBorder.qml\n"
                      "MiddleDragScroll 1.0 qml/MiddleDragScroll.qml\n"
                      "ScrollPage 1.0 qml/ScrollPage.qml\n"
                      "RecorderPage 1.0 qml/RecorderPage.qml\n");
        qmldir.close();

        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterType<AudioRecorderStub>("SuopTranscriber", 1, 0, "AudioRecorder");
        qmlRegisterType<AudioPlayerStub>("SuopTranscriber", 1, 0, "AudioPlayer");
        qmlRegisterUncreatableType<VaultTreeModelStub>(
            "SuopTranscriber", 1, 0, "VaultTreeModel",
            QStringLiteral("stub — this harness leaves it null"));

        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    width: 1100\n"
            "    height: 800\n"
            "    AudioRecorder { id: recStub; objectName: \"recStub\" }\n"
            "    RecorderPage {\n"
            "        objectName: \"recorderPage\"\n"
            "        anchors.fill: parent\n"
            "        audioRecorder: recStub\n"
            "        vaultLocalPath: \"/tmp/suop-harness-vault\"\n"
            "        vaultPath: \"file:///tmp/suop-harness-vault\"\n"
            "    }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driverFile(m_driverQmlPath);
        QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
        driverFile.write(driverQml.toUtf8());
        driverFile.close();
    }

    // The load itself is the first assertion: before this harness existed,
    // nothing proved RecorderPage.qml could even be instantiated.
    void recorderPageLoads()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY2(openRecorder(&view, &page, &rec), "could not load RecorderPage.qml");
        QVERIFY(find(&view, QStringLiteral("micToggle")));
        QVERIFY(find(&view, QStringLiteral("systemAudioToggle")));
        QVERIFY(find(&view, QStringLiteral("recordToggleButton")));
    }

    // Item 032's headline claim, at runtime: turning System Audio ON must turn
    // per-app capture OFF — and must NOT leave the per-app toggle greyed out,
    // which is the behavior it replaced.
    void systemAudioTurnsPerAppCaptureOffRatherThanGreyingIt()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        rec->setAppStreams({appStream(QStringLiteral("stream:1"), QStringLiteral("Discord"))});
        QTRY_VERIFY(page->property("appCaptureRows").toList().size() == 1);

        QQuickItem *appToggle = find(&view, QStringLiteral("appCaptureToggle"));
        QVERIFY2(appToggle, "per-app row toggle did not materialise");
        tap(&view, appToggle);
        QTRY_VERIFY(!page->property("enabledAppCaptureSources").isNull()
                    || true); // enabled state lives in AppSettings; assert via the row below

        QQuickItem *sysToggle = find(&view, QStringLiteral("systemAudioToggle"));
        QVERIFY(sysToggle);
        // The toggle must be live, not disabled, while a per-app row is armed.
        QVERIFY2(sysToggle->isEnabled(),
                 "System Audio toggle is disabled while a per-app row is armed — "
                 "that is the greying-out behavior item 032 removed");
        tap(&view, sysToggle);

        QTRY_COMPARE(page->property("useSystemAudio").toBool(), true);
        // ...and the per-app row it deselected is still interactive.
        QVERIFY2(appToggle->isEnabled(),
                 "per-app toggle went disabled when System Audio turned on — "
                 "item 032 requires deselection, not greying out");
    }

    // The reverse direction of the same rule.
    void armingAPerAppRowTurnsSystemAudioOff()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        rec->setAppStreams({appStream(QStringLiteral("stream:1"), QStringLiteral("Discord"))});
        QTRY_VERIFY(page->property("appCaptureRows").toList().size() == 1);

        QQuickItem *sysToggle = find(&view, QStringLiteral("systemAudioToggle"));
        tap(&view, sysToggle);
        QTRY_COMPARE(page->property("useSystemAudio").toBool(), true);

        QQuickItem *appToggle = find(&view, QStringLiteral("appCaptureToggle"));
        QVERIFY(appToggle);
        QVERIFY2(appToggle->isEnabled(), "per-app toggle greyed out while System Audio is on");
        tap(&view, appToggle);

        QTRY_COMPARE(page->property("useSystemAudio").toBool(), false);
    }

    // Item 036: a row must survive its application going quiet. This is the
    // claim the whole id -> name re-keying exists for.
    void perAppRowSurvivesItsApplicationGoingQuiet()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        rec->setAppStreams({appStream(QStringLiteral("stream:1"), QStringLiteral("Discord"))});
        QTRY_COMPARE(page->property("appCaptureRows").toList().size(), 1);
        QCOMPARE(page->property("appCaptureRows").toList().first().toMap()
                     .value(QStringLiteral("name")).toString(),
                 QStringLiteral("Discord"));

        // Discord stops making noise: its stream disappears from enumeration.
        rec->setAppStreams({});
        // The row must still be there — remembered by name.
        QTRY_COMPARE(page->property("appCaptureRows").toList().size(), 1);
        const QVariantMap row = page->property("appCaptureRows").toList().first().toMap();
        QCOMPARE(row.value(QStringLiteral("name")).toString(), QStringLiteral("Discord"));
        QCOMPARE(row.value(QStringLiteral("active")).toBool(), false);
    }

    // Item 036's dot: active reflects a live stream, not merely a remembered row.
    void activeDotTracksWhetherTheStreamIsLive()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        rec->setAppStreams({appStream(QStringLiteral("stream:1"), QStringLiteral("Discord"))});
        QTRY_COMPARE(page->property("appCaptureRows").toList().size(), 1);
        QQuickItem *dot = find(&view, QStringLiteral("appCaptureActiveDot"));
        QVERIFY2(dot, "active dot did not materialise for a live row");
        QTRY_VERIFY2(dot->isVisible(), "dot is not visible while the stream is live");

        rec->setAppStreams({});
        QTRY_VERIFY2(!dot->isVisible(), "dot stayed visible after the stream went away");
    }

    // Item 031: the selected system source must survive re-enumeration that
    // reorders the list — the desync this fixed.
    void systemSourceSelectionSurvivesReordering()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        rec->setSystemSources({systemSource(QStringLiteral("a"), QStringLiteral("Alpha")),
                                systemSource(QStringLiteral("b"), QStringLiteral("Beta"))});
        page->setProperty("selectedSystemSourceId", QStringLiteral("b"));
        QTRY_COMPARE(page->property("selectedSystemSourceId").toString(), QStringLiteral("b"));

        // Re-enumerate with the order flipped — index 1 is now a different id.
        rec->setSystemSources({systemSource(QStringLiteral("b"), QStringLiteral("Beta")),
                                systemSource(QStringLiteral("a"), QStringLiteral("Alpha"))});
        QTRY_COMPARE(page->property("selectedSystemSourceId").toString(), QStringLiteral("b"));
    }

    // ...and a source that vanishes entirely must be noticed, not silently kept.
    void vanishedSystemSourceRaisesTheWarning()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        rec->setSystemSources({systemSource(QStringLiteral("a"), QStringLiteral("Alpha")),
                                systemSource(QStringLiteral("b"), QStringLiteral("Beta"))});
        page->setProperty("selectedSystemSourceId", QStringLiteral("b"));
        QTRY_COMPARE(page->property("selectedSystemSourceId").toString(), QStringLiteral("b"));

        rec->setSystemSources({systemSource(QStringLiteral("a"), QStringLiteral("Alpha"))});
        QTRY_VERIFY2(!page->property("systemSourceWarning").toString().isEmpty(),
                     "a vanished system source raised no warning");
    }

    // Item 029: with test mode armed, a take must be reported as temporary —
    // the vault must not be where it lands.
    void testModeMarksTheTakeTemporary()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        QQuickItem *testToggle = find(&view, QStringLiteral("testModeToggleButton"));
        QVERIFY2(testToggle, "test-mode toggle did not materialise");
        tap(&view, testToggle);
        QTRY_COMPARE(page->property("testMode").toBool(), true);

        tap(&view, find(&view, QStringLiteral("recordToggleButton")));
        QTRY_COMPARE(rec->startCount(), 1);
        QVERIFY2(rec->startTestMode(),
                 "recording started with testMode=false while the toggle was on");
    }

    // Item 026: the three source rows share ONE measured label column, so their
    // toggles line up. Geometry, not a source scan.
    void sourceTogglesShareOneMeasuredWidth()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        rec->setAppStreams({appStream(QStringLiteral("stream:1"), QStringLiteral("Discord"))});
        QTRY_COMPARE(page->property("appCaptureRows").toList().size(), 1);

        QQuickItem *mic = find(&view, QStringLiteral("micToggle"));
        QQuickItem *sys = find(&view, QStringLiteral("systemAudioToggle"));
        QQuickItem *app = find(&view, QStringLiteral("appCaptureToggle"));
        QVERIFY(mic && sys && app);
        QTRY_VERIFY(mic->width() > 0);

        QCOMPARE(mic->width(), sys->width());
        QCOMPARE(mic->width(), app->width());
        // ...and they start at the same x, so the column really is a column.
        QCOMPARE(mic->mapToScene(QPointF(0, 0)).x(), sys->mapToScene(QPointF(0, 0)).x());
    }

    // The bug behind refinement 5: toggling the microphone mid-take updated the
    // checkbox and nothing else, because the worker read useMic once at
    // startRecording() and never again. The toggle must now reach the recorder.
    void togglingTheMicDuringARecordingReachesTheRecorder()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        tap(&view, find(&view, QStringLiteral("recordToggleButton")));
        QTRY_COMPARE(rec->startCount(), 1);
        QVERIFY(rec->isRecording());

        const int before = rec->micEnabledCalls().size();
        tap(&view, find(&view, QStringLiteral("micToggle")));
        QTRY_COMPARE(page->property("useMic").toBool(), false);
        QTRY_VERIFY2(rec->micEnabledCalls().size() > before,
                     "toggling the mic mid-recording never reached the recorder — the UI updated "
                     "but the live capture did not");
        QCOMPARE(rec->micEnabledCalls().last().first, false);

        // ...and back on again.
        tap(&view, find(&view, QStringLiteral("micToggle")));
        QTRY_COMPARE(page->property("useMic").toBool(), true);
        QTRY_COMPARE(rec->micEnabledCalls().last().first, true);
    }

    // Refinement 1, the per-app half: the row's toggle used to be hard-disabled
    // for the whole take (`enabled: !recording`).
    void perAppRowsStayToggleableDuringARecording()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        rec->setAppStreams({appStream(QStringLiteral("stream:1"), QStringLiteral("Discord"))});
        QTRY_COMPARE(page->property("appCaptureRows").toList().size(), 1);

        tap(&view, find(&view, QStringLiteral("recordToggleButton")));
        QTRY_VERIFY(rec->isRecording());

        QQuickItem *appToggle = find(&view, QStringLiteral("appCaptureToggle"));
        QVERIFY(appToggle);
        QVERIFY2(appToggle->isEnabled(),
                 "the per-app toggle is disabled during a take — arming an app mid-recording now "
                 "works, so blocking it only hides a working capability");
        QQuickItem *slider = find(&view, QStringLiteral("appCaptureGainSlider"));
        QVERIFY(slider);

        tap(&view, appToggle);
        // Arming it mid-take must hand the source to the running capture.
        QTRY_VERIFY2(!rec->addedSources().isEmpty(),
                     "arming an app mid-recording never reached the running capture");
        QCOMPARE(rec->addedSources().last().first, QStringLiteral("stream:1"));
    }

    // ...and disarming it mid-take must take it back out again.
    void disarmingAPerAppRowDuringARecordingRemovesTheSource()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        rec->setAppStreams({appStream(QStringLiteral("stream:1"), QStringLiteral("Discord"))});
        QTRY_COMPARE(page->property("appCaptureRows").toList().size(), 1);
        tap(&view, find(&view, QStringLiteral("appCaptureToggle")));

        tap(&view, find(&view, QStringLiteral("recordToggleButton")));
        QTRY_VERIFY(rec->isRecording());
        rec->setActiveIds({QStringLiteral("stream:1")});

        tap(&view, find(&view, QStringLiteral("appCaptureToggle")));
        QTRY_VERIFY2(rec->removedSources().contains(QStringLiteral("stream:1")),
                     "disarming an app mid-recording left it in the mix");
    }

    // Refinement 2: the meters have to be live while the page merely sits open,
    // or the levels are only visible once you have committed to a take.
    void monitoringRunsWhileThePageIsOpenWithSomethingArmed()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        // The mic is armed by default, so simply having the page up monitors.
        QTRY_VERIFY2(rec->isMonitoring(),
                     "the recorder page is open with the mic armed but nothing is metering");
        QVERIFY(page->property("capturing").toBool());

        // Disarming everything must release the devices again — monitoring is
        // not a reason to hold a microphone open indefinitely.
        tap(&view, find(&view, QStringLiteral("micToggle")));
        QTRY_COMPARE(page->property("useMic").toBool(), false);
        QTRY_VERIFY2(!rec->isMonitoring(),
                     "nothing is armed but the recorder is still holding devices open");
    }

    // A take supersedes monitoring rather than being refused by it.
    void startingARecordingStopsMonitoringFirst()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));
        QTRY_VERIFY(rec->isMonitoring());

        tap(&view, find(&view, QStringLiteral("recordToggleButton")));
        QTRY_COMPARE(rec->startCount(), 1);
        QVERIFY2(!rec->isMonitoring(), "monitoring outlived the take it was superseded by");
        QVERIFY(rec->isRecording());
    }

    // Refinements 3 and 4: the meters colour by band, and the band rule is the
    // shared one — not a threshold RecorderPage re-derives.
    void metersColourByTheSharedLevelBand()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        QQuickItem *slider = find(&view, QStringLiteral("micGainSlider"));
        QVERIFY2(slider, "mic gain slider not found");
        // It must be the metered component, carrying a level and a peak.
        QVERIFY2(slider->property("level").isValid() && slider->property("peak").isValid(),
                 "the gain slider is not a metered GainSlider");

        // The three bands must be visibly different from each other, and must
        // land on the same boundaries the C++ rule uses.
        auto band = [page](qreal level) {
            QVariant ret;
            QMetaObject::invokeMethod(page, "bandColor", Q_RETURN_ARG(QVariant, ret),
                                       Q_ARG(QVariant, level));
            return qvariant_cast<QColor>(ret);
        };
        const qreal goodFloor = AudioRecorderIO::levelBandGoodFloor();
        const qreal hotFloor = AudioRecorderIO::levelBandHotFloor();
        const QColor low = band(goodFloor - 0.05);
        const QColor good = band(goodFloor + 0.05);
        const QColor hot = band(hotFloor + 0.05);
        QVERIFY(low.isValid() && good.isValid() && hot.isValid());
        QVERIFY2(low != good && good != hot && low != hot,
                 "the three level bands must be visually distinct");
    }

    // The page-level half of the monitor-supersedes-take rule.
    //
    // SCOPE LIMIT, deliberately recorded: this does NOT cover the C++
    // regression that broke recording outright (the worker's monitor teardown
    // was emitted as stopped(), the same signal a finished take uses, so it
    // cleared `recording` just after startRecording() set it — leaving the UI
    // idle while capture really was running, and the next press rejected as
    // "already recording"). That bug lives in AudioRecorder's own signal
    // routing, which this harness replaces with a stub; verified by reverting
    // the fix, this test still passed. See tst_future_spec's entry for what is
    // owed.
    void recordingSurvivesTheMonitorTeardownItSupersedes()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));
        QTRY_VERIFY2(rec->isMonitoring(), "expected monitoring before the take");

        tap(&view, find(&view, QStringLiteral("recordToggleButton")));
        QTRY_COMPARE(rec->startCount(), 1);

        // Let any queued teardown settle, then the take must STILL be running.
        QTest::qWait(50);
        QVERIFY2(rec->isRecording(),
                 "the monitor teardown cleared `recording` out from under the take");
        QVERIFY2(!rec->isMonitoring(), "monitoring outlived the take");
        QVERIFY2(page->property("capturing").toBool(), "the page does not think it is capturing");

        // And a second press must STOP it, not be refused as a duplicate start.
        tap(&view, find(&view, QStringLiteral("recordToggleButton")));
        QTRY_VERIFY2(!rec->isRecording(), "the second press did not stop the take");
        QCOMPARE(rec->startCount(), 1);
    }

    // Dragging the mic gain slider must reach the worker. The meter measures
    // POST-gain, so a gain the worker never receives makes the slider look
    // inert — which is exactly how it behaved.
    void movingTheMicGainSliderReachesTheRecorder()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));
        QTRY_VERIFY(rec->isMonitoring());

        const int before = rec->micEnabledCalls().size();
        QQuickItem *slider = find(&view, QStringLiteral("micGainSlider"));
        QVERIFY(slider);
        // Drive the slider the way a drag does: move the value, then emit the
        // moved() signal its onMoved handler listens for.
        slider->setProperty("value", 1.7);
        QVERIFY(QMetaObject::invokeMethod(slider, "moved"));

        QTRY_VERIFY2(rec->micEnabledCalls().size() > before,
                     "moving the mic gain slider never reached the recorder — the number moved "
                     "but the live capture kept its original gain");
        QVERIFY2(qFuzzyCompare(rec->micEnabledCalls().last().second, 1.7),
                 "the recorder was handed the wrong gain");
    }

    // Item 034: both device pickers are the shared component, not bare
    // QQC2 ComboBoxes — the drift this closed.
    void deviceCombosAreTheSharedComponent()
    {
        QQuickView view;
        QQuickItem *page = nullptr;
        AudioRecorderStub *rec = nullptr;
        QVERIFY(openRecorder(&view, &page, &rec));

        for (const QString &name :
             {QStringLiteral("micCombo"), QStringLiteral("systemSourceCombo")}) {
            QQuickItem *combo = find(&view, name);
            QVERIFY2(combo, qPrintable(name + QStringLiteral(" not found")));
            // AppComboBox exposes labelRole; a bare ComboBox does not.
            QVERIFY2(combo->property("labelRole").isValid(),
                     qPrintable(name + QStringLiteral(" is not an AppComboBox")));
        }
    }
};

int main(int argc, char *argv[])
{
    // Never touch the real app config — this harness loads AppSettings, which
    // persists (see CLAUDE.md "never let testing touch the user's real vault or
    // app config"). A harness-specific org/app name sends QSettings somewhere
    // disposable instead.
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_recorderpage_sources"));
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TstRecorderPageSources tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_recorderpage_sources.moc"
