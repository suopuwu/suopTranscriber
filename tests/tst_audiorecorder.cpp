#include <QtTest>

#include <cmath>

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <QTime>
#include <QUrl>

#include "AudioRecorder.h"

#include <cstring>

namespace {

QByteArray toPcm16(const QVector<qint16> &samples)
{
    QByteArray out(samples.size() * 2, Qt::Uninitialized);
    std::memcpy(out.data(), samples.constData(), static_cast<size_t>(out.size()));
    return out;
}

QVector<qint16> fromPcm16(const QByteArray &pcm)
{
    QVector<qint16> out(pcm.size() / 2);
    std::memcpy(out.data(), pcm.constData(), static_cast<size_t>(pcm.size()));
    return out;
}

quint32 readLE32(const QByteArray &data, int offset)
{
    return static_cast<quint32>(static_cast<quint8>(data[offset]))
        | (static_cast<quint32>(static_cast<quint8>(data[offset + 1])) << 8)
        | (static_cast<quint32>(static_cast<quint8>(data[offset + 2])) << 16)
        | (static_cast<quint32>(static_cast<quint8>(data[offset + 3])) << 24);
}

quint16 readLE16(const QByteArray &data, int offset)
{
    return static_cast<quint16>(static_cast<quint8>(data[offset]))
        | (static_cast<quint16>(static_cast<quint8>(data[offset + 1])) << 8);
}

// A single filterAppStreams()-shaped {id, name} entry, for
// buildAppCaptureRows() tests that don't need a real pw-dump fixture.
QVariant appStream(const QString &id, const QString &name)
{
    QVariantMap map;
    map[QStringLiteral("id")] = id;
    map[QStringLiteral("name")] = name;
    return map;
}

// A single startRecording()-shaped {id, gain} entry, for
// pendingCaptureAdditions() tests.
QVariant captureEntry(const QString &id, double gain)
{
    QVariantMap map;
    map[QStringLiteral("id")] = id;
    map[QStringLiteral("gain")] = gain;
    return map;
}

}

class TstAudioRecorder : public QObject
{
    Q_OBJECT
private slots:
    void wavHeader_structureAndSizes();
    void patchWavHeader_rewritesSizeFieldsInPlace();
    void filenameTimestamp_matchesPattern();
    void outputPath_withAndWithoutSubfolder();
    void testOutputPath_livesUnderTempNotTheVault();
    void pruneTestRecordings_keepsTheNewestNAndNothingElse();
    void pruneTestRecordings_missingDirectoryIsANoOp();
    void pruneTestRecordings_keepMoreThanPresentRemovesNothing();
    void mixPcm16N_appliesIndependentGains();
    void mixPcm16N_clampsAtExtremes();
    void mixPcm16N_singleSourceScalesAndClips();
    void mixPcm16N_threeSourcesSumWithIndependentGains();
    void mixPcm16N_emptyBufferContributesSilenceAtFullLength();
    void rmsPcm16_knownSignals();
    void displayLevelFromRms_spreadsSpeechAcrossTheRange();
    void decayPeak_risesInstantlyToANewMaximum();
    void decayPeak_fallsByExactlyTheDecayWhenLevelIsLower();
    void decayPeak_neverFallsBelowTheCurrentLevelOrZero();
    void levelBand_splitsLowGoodAndHotAtTheStatedDbThresholds();
    void levelBand_isMonotonicAndTotal();
    void dbFromDisplayLevel_invertsTheDisplayMapping();
    void holdPeak_pinsTheMaxForTheWholeHoldThenReleases();
    void holdPeak_aNewMaximumRestartsTheHold();
    void holdPeak_holdsEvenWhenTheSignalStaysJustBelowIt();
    void decayPeak_negativeDecayCannotInflateThePeak();
    void startStopRecording_stateFlipsSynchronouslyOnTheCall();
    void patchWavHeader_midStreamCheckpointIsReadableAndPreservesPosition();
    void patchWavHeader_checkpointLeavesTheFileSelfConsistentForACrash();
    void parseSystemSources_malformedOrEmptyYieldsOnlyTheDefaultEntry();
    void parseSystemSources_realFixtureSurfacesMonitorsAndStreamsFilteringNonAudio();
    void planMixTick_equalQueuesConsumeTheMinWithNothingStalled();
    void planMixTick_raggedButWithinThresholdStillConsumesFrameAlignedMin();
    void planMixTick_sourceLaggingBeyondThresholdIsStalledAndExcluded();
    void planMixTick_stalledAndStillEmptySourceStaysStalled();
    void planMixTick_stalledSourceWithDataAgainUnstalls();
    void planMixTick_minBelowOneFrameYieldsZeroBytes();
    void planMixTick_emptyInputYieldsZeroAndNoStalls();
    void filterAppStreams_keepsOnlyApplicationStreams();
    void buildAppCaptureRows_groupsLiveStreamsByApplicationName();
    void buildAppCaptureRows_keepsARememberedAppWithNoLiveStream();
    void buildAppCaptureRows_marksARememberedAppActiveOnceItsStreamReturns();
    void buildAppCaptureRows_sortsByNameSoRowsDoNotMoveUnderTheCursor();
    void buildAppCaptureRows_skipsEntriesWithNoNameOrNoId();
    void buildAppCaptureRows_emptyInputsYieldNoRows();
    void captureCommand_mapsEachSourceIdToItsCapture();
    void isGenericProcessName_catchesTheChromiumShellNames();
    void streamDisplayName_prefersARealApplicationName();
    void streamDisplayName_rescuesElectronAppsFromTheAncestryChain();
    void streamDisplayName_keepsChromiumWhenNoAncestorDisagrees();
    void streamDisplayName_fallsBackToBinaryThenMediaName();
    void processAncestryNames_walksThisProcessUpToItsParents();
    void parseSystemSources_appliesTheInjectedAncestryToStreamNames();
    void pendingCaptureAdditions_returnsOnlyIdsNotAlreadyCapturing();
    void pendingCaptureAdditions_nothingPendingWhenAllAreActive();
    void pendingCaptureAdditions_dropsDuplicatesWithinDesired();
    void pendingCaptureAdditions_dropsEmptyIds();
    void pendingCaptureAdditions_emptyActiveListAddsEverything();
    void planMixTick_freshlyAddedStalledSourceDoesNotHaltTheMix();
};

void TstAudioRecorder::wavHeader_structureAndSizes()
{
    const quint32 dataSize = 1000;
    const QByteArray header = AudioRecorderIO::buildWavHeader(dataSize, 48000, 2, 16);

    QCOMPARE(header.size(), 44);
    QCOMPARE(header.mid(0, 4), QByteArray("RIFF"));
    QCOMPARE(header.mid(8, 4), QByteArray("WAVE"));
    QCOMPARE(header.mid(12, 4), QByteArray("fmt "));
    QCOMPARE(header.mid(36, 4), QByteArray("data"));

    QCOMPARE(readLE32(header, 4), 36u + dataSize); // RIFF chunk size
    QCOMPARE(readLE32(header, 16), 16u); // fmt subchunk size (PCM)
    QCOMPARE(readLE16(header, 20), quint16(1)); // audio format: PCM
    QCOMPARE(readLE16(header, 22), quint16(2)); // channel count
    QCOMPARE(readLE32(header, 24), 48000u); // sample rate
    QCOMPARE(readLE32(header, 28), 48000u * 2 * 2); // byte rate
    QCOMPARE(readLE16(header, 32), quint16(4)); // block align
    QCOMPARE(readLE16(header, 34), quint16(16)); // bits per sample
    QCOMPARE(readLE32(header, 40), dataSize); // data chunk size
}

void TstAudioRecorder::patchWavHeader_rewritesSizeFieldsInPlace()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("test.wav");

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(AudioRecorderIO::buildWavHeader(0));
    const QByteArray payload(400, '\x7f');
    file.write(payload);

    AudioRecorderIO::patchWavHeader(file, static_cast<quint32>(payload.size()));
    file.close();

    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray whole = file.readAll();
    file.close();

    QCOMPARE(whole.size(), 44 + payload.size());
    QCOMPARE(readLE32(whole, 4), 36u + static_cast<quint32>(payload.size()));
    QCOMPARE(readLE32(whole, 40), static_cast<quint32>(payload.size()));
    QCOMPARE(whole.mid(44), payload);
}

void TstAudioRecorder::filenameTimestamp_matchesPattern()
{
    const QUrl vaultUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/somevault"));
    const QDateTime ts(QDate(2026, 7, 13), QTime(9, 5, 3));
    const QString path = AudioRecorderIO::buildOutputFilePath(vaultUrl, QString(), ts);
    const QString fileName = QFileInfo(path).fileName();

    QVERIFY(QRegularExpression(QStringLiteral("^suopAudio_\\d{8}-\\d{6}\\.wav$")).match(fileName).hasMatch());
    QCOMPARE(fileName, QStringLiteral("suopAudio_20260713-090503.wav"));
}

void TstAudioRecorder::outputPath_withAndWithoutSubfolder()
{
    const QUrl vaultUrl = QUrl::fromLocalFile(QStringLiteral("/tmp/myvault"));
    const QDateTime ts(QDate(2026, 7, 13), QTime(14, 5, 9));

    const QString withFolder = AudioRecorderIO::buildOutputFilePath(vaultUrl, QStringLiteral("recordings"), ts);
    QCOMPARE(withFolder, QStringLiteral("/tmp/myvault/recordings/suopAudio_20260713-140509.wav"));

    const QString withoutFolder = AudioRecorderIO::buildOutputFilePath(vaultUrl, QString(), ts);
    QCOMPARE(withoutFolder, QStringLiteral("/tmp/myvault/suopAudio_20260713-140509.wav"));
}

void TstAudioRecorder::testOutputPath_livesUnderTempNotTheVault()
{
    const QDateTime ts(QDate(2026, 7, 26), QTime(9, 5, 3));
    const QString path = AudioRecorderIO::buildTestOutputFilePath(QStringLiteral("/tmp"), ts);
    QCOMPARE(path, QStringLiteral("/tmp/suopTranscriber-test/suopAudio_20260726-090503.wav"));

    // Shares the suopAudio_ + timestamp naming filenameTimestamp_matchesPattern
    // already pins for the vault path — the two must not drift apart.
    const QString fileName = QFileInfo(path).fileName();
    QVERIFY(QRegularExpression(QStringLiteral("^suopAudio_\\d{8}-\\d{6}\\.wav$")).match(fileName).hasMatch());
}

void TstAudioRecorder::pruneTestRecordings_keepsTheNewestNAndNothingElse()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    for (int i = 1; i <= 5; ++i) {
        QFile f(dir.filePath(QStringLiteral("suopAudio_20260101-00000%1.wav").arg(i)));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }
    QFile notes(dir.filePath(QStringLiteral("notes.md")));
    QVERIFY(notes.open(QIODevice::WriteOnly));
    QFile decoyWav(dir.filePath(QStringLiteral("other.wav")));
    QVERIFY(decoyWav.open(QIODevice::WriteOnly));

    const int removed = AudioRecorderIO::pruneTestRecordings(dir.path(), 3);
    QCOMPARE(removed, 2);

    const QDir directory(dir.path());
    const QStringList remainingAudio =
        directory.entryList({ QStringLiteral("suopAudio_*.wav") }, QDir::Files, QDir::Name);
    QCOMPARE(remainingAudio, QStringList({ QStringLiteral("suopAudio_20260101-000003.wav"),
                                            QStringLiteral("suopAudio_20260101-000004.wav"),
                                            QStringLiteral("suopAudio_20260101-000005.wav") }));

    QVERIFY2(QFile::exists(dir.filePath(QStringLiteral("notes.md"))), "unrelated file must survive pruning");
    QVERIFY2(QFile::exists(dir.filePath(QStringLiteral("other.wav"))),
             "a non-suopAudio_ .wav must survive pruning");
}

void TstAudioRecorder::pruneTestRecordings_missingDirectoryIsANoOp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString missing = dir.filePath(QStringLiteral("does-not-exist"));

    QCOMPARE(AudioRecorderIO::pruneTestRecordings(missing, 3), 0);
    QVERIFY2(!QDir(missing).exists(), "pruneTestRecordings must not create the directory");
}

void TstAudioRecorder::pruneTestRecordings_keepMoreThanPresentRemovesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    for (int i = 1; i <= 3; ++i) {
        QFile f(dir.filePath(QStringLiteral("suopAudio_20260101-00000%1.wav").arg(i)));
        QVERIFY(f.open(QIODevice::WriteOnly));
    }

    QCOMPARE(AudioRecorderIO::pruneTestRecordings(dir.path(), 10), 0);
    const QDir directory(dir.path());
    QCOMPARE(directory.entryList({ QStringLiteral("suopAudio_*.wav") }, QDir::Files).size(), 3);
}

void TstAudioRecorder::mixPcm16N_appliesIndependentGains()
{
    const QByteArray mic = toPcm16({ 1000, -1000, 32000, -32000 });
    const QByteArray sys = toPcm16({ 500, 500, 500, 500 });

    const QByteArray mixed = AudioRecorderIO::mixPcm16N({ mic, sys }, { 0.5, 1.0 });
    const QVector<qint16> samples = fromPcm16(mixed);

    QCOMPARE(samples.size(), 4);
    QCOMPARE(samples[0], qint16(1000)); // 1000*0.5 + 500 = 1000
    QCOMPARE(samples[1], qint16(0)); // -1000*0.5 + 500 = 0
    QCOMPARE(samples[2], qint16(16500)); // 32000*0.5 + 500 = 16500
    QCOMPARE(samples[3], qint16(-15500)); // -32000*0.5 + 500 = -15500
}

void TstAudioRecorder::mixPcm16N_clampsAtExtremes()
{
    const QByteArray a = toPcm16({ 32767, -32768 });
    const QByteArray b = toPcm16({ 32767, -32768 });

    const QByteArray mixed = AudioRecorderIO::mixPcm16N({ a, b }, { 1.0, 1.0 });
    const QVector<qint16> samples = fromPcm16(mixed);

    QCOMPARE(samples.size(), 2);
    QCOMPARE(samples[0], std::numeric_limits<qint16>::max());
    QCOMPARE(samples[1], std::numeric_limits<qint16>::min());
}

void TstAudioRecorder::mixPcm16N_singleSourceScalesAndClips()
{
    const QByteArray in = toPcm16({ 1000, -1000, 30000 });
    const QByteArray out = AudioRecorderIO::mixPcm16N({ in }, { 2.0 });
    const QVector<qint16> samples = fromPcm16(out);

    QCOMPARE(samples.size(), 3);
    QCOMPARE(samples[0], qint16(2000));
    QCOMPARE(samples[1], qint16(-2000));
    QCOMPARE(samples[2], std::numeric_limits<qint16>::max()); // 60000 clamps to 32767
}

void TstAudioRecorder::mixPcm16N_threeSourcesSumWithIndependentGains()
{
    const QByteArray a = toPcm16({ 1000, 1000 });
    const QByteArray b = toPcm16({ 2000, 2000 });
    const QByteArray c = toPcm16({ 3000, 3000 });

    const QByteArray mixed = AudioRecorderIO::mixPcm16N({ a, b, c }, { 1.0, 0.5, 2.0 });
    const QVector<qint16> samples = fromPcm16(mixed);

    QCOMPARE(samples.size(), 2);
    // 1000*1.0 + 2000*0.5 + 3000*2.0 = 1000 + 1000 + 6000 = 8000
    QCOMPARE(samples[0], qint16(8000));
    QCOMPARE(samples[1], qint16(8000));
}

void TstAudioRecorder::mixPcm16N_emptyBufferContributesSilenceAtFullLength()
{
    // A stalled source is mixed in as an EMPTY buffer (see planMixTick()) —
    // it must not shorten the output to zero, only contribute nothing at
    // every sample.
    const QByteArray live = toPcm16({ 1000, -1000, 2000, -2000 });
    const QByteArray stalled;

    const QByteArray mixed = AudioRecorderIO::mixPcm16N({ live, stalled }, { 1.0, 1.0 });
    const QVector<qint16> samples = fromPcm16(mixed);

    QCOMPARE(samples.size(), 4);
    QCOMPARE(samples[0], qint16(1000));
    QCOMPARE(samples[1], qint16(-1000));
    QCOMPARE(samples[2], qint16(2000));
    QCOMPARE(samples[3], qint16(-2000));
}

void TstAudioRecorder::rmsPcm16_knownSignals()
{
    QCOMPARE(AudioRecorderIO::rmsPcm16(QByteArray()), qreal(0.0));

    const QByteArray silence = toPcm16({ 0, 0, 0, 0 });
    QCOMPARE(AudioRecorderIO::rmsPcm16(silence), qreal(0.0));

    // A constant-amplitude signal at half of int16 magnitude has RMS == 0.5
    // exactly (sqrt of the mean of a constant square is that same constant).
    const QByteArray halfScale = toPcm16({ 16384, 16384, 16384, 16384 });
    QVERIFY(qAbs(AudioRecorderIO::rmsPcm16(halfScale) - 0.5) < 0.0001);

    // Full-scale alternating square wave has RMS approaching 1.0.
    const QByteArray fullScale = toPcm16({ 32767, -32767, 32767, -32767 });
    QVERIFY(AudioRecorderIO::rmsPcm16(fullScale) > 0.999);
}

void TstAudioRecorder::displayLevelFromRms_spreadsSpeechAcrossTheRange()
{
    // Endpoints: silence pins to 0, full scale to 1, and anything at or under
    // the -60 dBFS floor is clamped rather than going negative.
    QCOMPARE(AudioRecorderIO::displayLevelFromRms(0.0), qreal(0.0));
    QCOMPARE(AudioRecorderIO::displayLevelFromRms(0.001), qreal(0.0)); // exactly -60 dBFS
    QCOMPARE(AudioRecorderIO::displayLevelFromRms(0.0001), qreal(0.0)); // below the floor
    QVERIFY(qAbs(AudioRecorderIO::displayLevelFromRms(1.0) - 1.0) < 0.0001);
    QVERIFY(AudioRecorderIO::displayLevelFromRms(2.0) <= 1.0); // clamped, never over-range

    // The actual regression this fixes: conversational speech (RMS 0.01-0.10)
    // used to occupy the bottom ~10% of the canvas. It must now use a real
    // chunk of the height.
    const qreal quiet = AudioRecorderIO::displayLevelFromRms(0.01); // -40 dBFS
    const qreal normal = AudioRecorderIO::displayLevelFromRms(0.05); // ~-26 dBFS
    QVERIFY2(quiet > 0.25, "quiet speech must clear a quarter of the canvas height");
    QVERIFY2(normal > 0.5, "normal speech must clear half the canvas height");

    // Monotonic — a louder input can never render shorter.
    qreal previous = -1.0;
    for (const qreal rms : { 0.0, 0.002, 0.01, 0.05, 0.2, 0.5, 1.0 }) {
        const qreal mapped = AudioRecorderIO::displayLevelFromRms(rms);
        QVERIFY(mapped >= previous);
        previous = mapped;
    }
}

void TstAudioRecorder::decayPeak_risesInstantlyToANewMaximum()
{
    QCOMPARE(AudioRecorderIO::decayPeak(0.2, 0.9, 0.02), qreal(0.9));
}

void TstAudioRecorder::decayPeak_fallsByExactlyTheDecayWhenLevelIsLower()
{
    QVERIFY(qFuzzyCompare(AudioRecorderIO::decayPeak(0.9, 0.1, 0.02), qreal(0.88)));

    qreal peak = 0.9;
    for (int i = 0; i < 10; ++i)
        peak = AudioRecorderIO::decayPeak(peak, 0.1, 0.02);
    QVERIFY(qFuzzyCompare(peak, qreal(0.7)));
}

void TstAudioRecorder::decayPeak_neverFallsBelowTheCurrentLevelOrZero()
{
    // The decayed value would be 0 (0.05 - 0.4 clamped), but the current
    // level wins.
    QCOMPARE(AudioRecorderIO::decayPeak(0.05, 0.5, 0.4), qreal(0.5));
    // Never negative even with a silent current level and a near-zero peak.
    QCOMPARE(AudioRecorderIO::decayPeak(0.01, 0.0, 0.5), qreal(0.0));
}

// The colour bands the recorder's meters draw. Anchored on the dBFS values the
// header states, converted through the same -60 dB floor displayLevelFromRms
// uses, so a change to either the thresholds or the mapping breaks this rather
// than silently recolouring every meter.
void TstAudioRecorder::levelBand_splitsLowGoodAndHotAtTheStatedDbThresholds()
{
    const qreal goodFloor = AudioRecorderIO::levelBandGoodFloor();
    const qreal hotFloor = AudioRecorderIO::levelBandHotFloor();
    // -27 dBFS and -6 dBFS over a -60 dB floor.
    QCOMPARE(goodFloor, (-27.0 + 60.0) / 60.0);
    QCOMPARE(hotFloor, (-6.0 + 60.0) / 60.0);
    QVERIFY(goodFloor < hotFloor);

    QCOMPARE(AudioRecorderIO::levelBand(0.0), QStringLiteral("low"));
    QCOMPARE(AudioRecorderIO::levelBand(goodFloor - 0.01), QStringLiteral("low"));
    // Boundaries belong to the HIGHER band, so a level sitting exactly on the
    // threshold never reads as the problem band it just left.
    QCOMPARE(AudioRecorderIO::levelBand(goodFloor), QStringLiteral("good"));
    QCOMPARE(AudioRecorderIO::levelBand(hotFloor - 0.01), QStringLiteral("good"));
    QCOMPARE(AudioRecorderIO::levelBand(hotFloor), QStringLiteral("hot"));
    QCOMPARE(AudioRecorderIO::levelBand(1.0), QStringLiteral("hot"));

    // Ordinary conversational speech (~-40..-20 dBFS per displayLevelFromRms's
    // own note) must not be nagged as too quiet across its whole range — the
    // band exists to flag a source too faint to equalise, not normal talking.
    QCOMPARE(AudioRecorderIO::levelBand(AudioRecorderIO::displayLevelFromRms(0.08)),
             QStringLiteral("good"));
}

void TstAudioRecorder::levelBand_isMonotonicAndTotal()
{
    // Every level maps to exactly one of three names, and the sequence never
    // goes backwards as the level rises.
    QString previous = QStringLiteral("low");
    const QStringList order{QStringLiteral("low"), QStringLiteral("good"), QStringLiteral("hot")};
    for (int i = 0; i <= 100; ++i) {
        const QString band = AudioRecorderIO::levelBand(i / 100.0);
        QVERIFY2(order.contains(band), qPrintable(QStringLiteral("unknown band: ") + band));
        QVERIFY2(order.indexOf(band) >= order.indexOf(previous), "levelBand went backwards");
        previous = band;
    }
}

// The meters are labelled in dBFS, so the conversion back out of the display
// scale has to be the exact inverse of displayLevelFromRms's remap — if the two
// ever disagree, the number beside a meter stops describing the meter.
void TstAudioRecorder::dbFromDisplayLevel_invertsTheDisplayMapping()
{
    QCOMPARE(AudioRecorderIO::displayFloorDb(), -60.0);
    // Endpoints: the floor and full scale.
    QCOMPARE(AudioRecorderIO::dbFromDisplayLevel(0.0), -60.0);
    QCOMPARE(AudioRecorderIO::dbFromDisplayLevel(1.0), 0.0);
    // Out-of-range input is clamped, never extrapolated into nonsense dB.
    QCOMPARE(AudioRecorderIO::dbFromDisplayLevel(-0.5), -60.0);
    QCOMPARE(AudioRecorderIO::dbFromDisplayLevel(1.5), 0.0);

    // The band thresholds must report the dBFS values they were chosen as.
    QCOMPARE(AudioRecorderIO::dbFromDisplayLevel(AudioRecorderIO::levelBandGoodFloor()), -27.0);
    QCOMPARE(AudioRecorderIO::dbFromDisplayLevel(AudioRecorderIO::levelBandHotFloor()), -6.0);

    // Round trip: a real RMS -> display -> dB must land back on its own dBFS.
    for (const qreal rms : { 0.5, 0.1, 0.05, 0.01 }) {
        const qreal expectedDb = 20.0 * std::log10(rms);
        const qreal roundTripped =
            AudioRecorderIO::dbFromDisplayLevel(AudioRecorderIO::displayLevelFromRms(rms));
        QVERIFY2(std::abs(roundTripped - expectedDb) < 1e-9,
                 qPrintable(QStringLiteral("rms %1: expected %2 dB, got %3 dB")
                                .arg(rms).arg(expectedDb).arg(roundTripped)));
    }
}

// The headline claim: for the whole hold the line does not move at all, and
// only then does it start coming down. Driven at the recorder's real cadence
// (~75ms per level tick) so the assertion is about seconds, not ticks.
void TstAudioRecorder::holdPeak_pinsTheMaxForTheWholeHoldThenReleases()
{
    constexpr qint64 kTickMs = 75;
    constexpr qint64 kHoldMs = 3000;
    constexpr qreal kDecay = 1.0 / 40.0;

    // A peak of 0.7 over a signal that immediately drops to 0.5 — the case the
    // old decay-only rule collapsed in under a second, because it bled toward
    // the live level rather than holding.
    QVariantMap state = AudioRecorderIO::holdPeak(0.0, 0, 0.7, 0, kHoldMs, kDecay);
    QCOMPARE(state.value(QStringLiteral("peak")).toReal(), 0.7);
    QCOMPARE(state.value(QStringLiteral("holdUntil")).toLongLong(), qint64(3000));

    for (qint64 now = kTickMs; now < kHoldMs; now += kTickMs) {
        state = AudioRecorderIO::holdPeak(state.value(QStringLiteral("peak")).toReal(),
                                           state.value(QStringLiteral("holdUntil")).toLongLong(),
                                           0.5, now, kHoldMs, kDecay);
        QVERIFY2(qFuzzyCompare(state.value(QStringLiteral("peak")).toReal(), 0.7),
                 qPrintable(QStringLiteral("peak moved at %1ms, inside the 3s hold").arg(now)));
    }

    // One tick past the hold it finally releases, by exactly one decay step.
    state = AudioRecorderIO::holdPeak(state.value(QStringLiteral("peak")).toReal(),
                                       state.value(QStringLiteral("holdUntil")).toLongLong(),
                                       0.5, kHoldMs, kHoldMs, kDecay);
    QVERIFY2(state.value(QStringLiteral("peak")).toReal() < 0.7, "the peak never released");
    QVERIFY2(qFuzzyCompare(state.value(QStringLiteral("peak")).toReal(), 0.7 - kDecay),
             "release must be exactly one decay step per tick");
}

void TstAudioRecorder::holdPeak_aNewMaximumRestartsTheHold()
{
    constexpr qint64 kHoldMs = 3000;
    QVariantMap state = AudioRecorderIO::holdPeak(0.0, 0, 0.4, 0, kHoldMs, 1.0 / 40.0);
    QCOMPARE(state.value(QStringLiteral("holdUntil")).toLongLong(), qint64(3000));

    // A louder moment 2s in must jump the line AND buy another full 3s.
    state = AudioRecorderIO::holdPeak(state.value(QStringLiteral("peak")).toReal(),
                                       state.value(QStringLiteral("holdUntil")).toLongLong(),
                                       0.9, 2000, kHoldMs, 1.0 / 40.0);
    QCOMPARE(state.value(QStringLiteral("peak")).toReal(), 0.9);
    QCOMPARE(state.value(QStringLiteral("holdUntil")).toLongLong(), qint64(5000));
}

// Equal-to-the-peak counts as a new maximum, so a signal sitting exactly at its
// own peak keeps renewing the hold rather than ageing out under it.
void TstAudioRecorder::holdPeak_holdsEvenWhenTheSignalStaysJustBelowIt()
{
    constexpr qint64 kHoldMs = 3000;
    QVariantMap state = AudioRecorderIO::holdPeak(0.0, 0, 0.6, 0, kHoldMs, 1.0 / 40.0);
    state = AudioRecorderIO::holdPeak(state.value(QStringLiteral("peak")).toReal(),
                                       state.value(QStringLiteral("holdUntil")).toLongLong(),
                                       0.6, 1000, kHoldMs, 1.0 / 40.0);
    QCOMPARE(state.value(QStringLiteral("peak")).toReal(), 0.6);
    QCOMPARE(state.value(QStringLiteral("holdUntil")).toLongLong(), qint64(4000));

    // And a zero hold degenerates to plain decay, never to a stuck peak.
    const QVariantMap noHold = AudioRecorderIO::holdPeak(0.6, 0, 0.1, 500, 0, 1.0 / 40.0);
    QVERIFY(noHold.value(QStringLiteral("peak")).toReal() < 0.6);
}

void TstAudioRecorder::decayPeak_negativeDecayCannotInflateThePeak()
{
    // A bad caller passing a negative decay must not make the meter climb
    // on its own.
    QCOMPARE(AudioRecorderIO::decayPeak(0.5, 0.0, -1.0), qreal(0.5));
}

void TstAudioRecorder::startStopRecording_stateFlipsSynchronouslyOnTheCall()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    AudioRecorder recorder;
    QSignalSpy changed(&recorder, &AudioRecorder::recordingChanged);
    QVERIFY(!recorder.isRecording());

    // The whole point: `recording` must be true the instant the call returns,
    // NOT when the worker's started() gets back. A stale flag here is what let
    // a second tap post a duplicate start, whose rejection then clobbered the
    // live recording's state. No event loop is spun between the call and the
    // assertion, so nothing from the worker thread can have landed yet.
    recorder.startRecording(QUrl::fromLocalFile(dir.path()), QString(), /*useMic=*/true, QString(),
                            /*useSystemAudio=*/false, 1.0, 1.0);
    QVERIFY2(recorder.isRecording(), "startRecording() must reflect the click synchronously");
    QCOMPARE(changed.count(), 1);

    // A duplicate start (the impatient second click) must be a no-op that
    // neither posts to the worker nor disturbs the state.
    recorder.startRecording(QUrl::fromLocalFile(dir.path()), QString(), /*useMic=*/true, QString(),
                            /*useSystemAudio=*/false, 1.0, 1.0);
    QVERIFY(recorder.isRecording());
    QCOMPARE(changed.count(), 1);

    recorder.stopRecording();
    QVERIFY2(!recorder.isRecording(), "stopRecording() must reflect the click synchronously");
    QCOMPARE(changed.count(), 2);

    // And a stop with nothing running stays a no-op.
    recorder.stopRecording();
    QCOMPARE(changed.count(), 2);

    // The destructor posts a queued stop and joins the worker thread, so
    // whatever the worker managed to open here is torn down cleanly.
}

void TstAudioRecorder::patchWavHeader_midStreamCheckpointIsReadableAndPreservesPosition()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("checkpoint.wav");

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(AudioRecorderIO::buildWavHeader(0));

    const QByteArray first(400, '\x11');
    file.write(first);

    // Periodic checkpoint, mid-capture.
    AudioRecorderIO::patchWavHeader(file, static_cast<quint32>(first.size()));
    QCOMPARE(file.pos(), qint64(44 + first.size())); // cursor back at EOF, not 44

    // The crash case: a reader opening the file RIGHT NOW, with capture still
    // notionally in progress, must see a valid WAV describing exactly what has
    // been captured so far — not the zero-length file it used to see.
    {
        QFile snapshot(path);
        QVERIFY(snapshot.open(QIODevice::ReadOnly));
        const QByteArray partial = snapshot.readAll();
        QCOMPARE(partial.size(), 44 + first.size());
        QCOMPARE(readLE32(partial, 40), static_cast<quint32>(first.size()));
        QCOMPARE(readLE32(partial, 4), 36u + static_cast<quint32>(first.size()));
        QCOMPARE(partial.mid(44), first);
    }

    // Capture continues: the checkpoint must not have overwritten the payload
    // from byte 44 (which is what a non-position-preserving patch would do).
    const QByteArray second(300, '\x22');
    file.write(second);
    AudioRecorderIO::patchWavHeader(file, static_cast<quint32>(first.size() + second.size()));
    file.close();

    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray whole = file.readAll();
    file.close();

    QCOMPARE(whole.size(), 44 + first.size() + second.size());
    QCOMPARE(readLE32(whole, 40), static_cast<quint32>(first.size() + second.size()));
    QCOMPARE(whole.mid(44, first.size()), first);
    QCOMPARE(whole.mid(44 + first.size()), second);
}

void TstAudioRecorder::patchWavHeader_checkpointLeavesTheFileSelfConsistentForACrash()
{
    // Stands in for a process that gets killed mid-recording: everything is
    // asserted through a SECOND QFile opened read-only on the same path,
    // without ever closing (or destructing) the writer — only bytes the
    // flush inside patchWavHeader() actually pushed to the kernel are
    // visible here, exactly like what a killed process leaves behind.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("crash.wav");

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(AudioRecorderIO::buildWavHeader(0));

    quint32 bytesSoFar = 0;
    QByteArray payload;
    const QByteArray chunk(400, '\x33');

    // Three chunks, checkpointed after each — the exact sequence mixOnce()
    // performs: write the payload, then patch the header over it.
    for (int i = 0; i < 3; ++i) {
        file.write(chunk);
        payload += chunk;
        bytesSoFar += static_cast<quint32>(chunk.size());
        AudioRecorderIO::patchWavHeader(file, bytesSoFar);

        QFile reader(path);
        QVERIFY(reader.open(QIODevice::ReadOnly));
        const QByteArray seen = reader.readAll();
        QCOMPARE(seen.size(), 44 + static_cast<qint64>(bytesSoFar));
        QCOMPARE(readLE32(seen, 40), bytesSoFar);
        QCOMPARE(seen.mid(44), payload);
    }

    // A fourth chunk with NO checkpoint: the header must still describe no
    // more than what actually made it to disk. Under-claiming (still
    // reporting the last checkpointed size) merely loses the unflushed
    // tail, which is the acceptable failure mode; over-claiming would make
    // the file unplayable.
    file.write(chunk);

    QFile reader(path);
    QVERIFY(reader.open(QIODevice::ReadOnly));
    const QByteArray seen = reader.readAll();
    const quint32 claimedSize = readLE32(seen, 40);
    QVERIFY(claimedSize <= bytesSoFar);
    QVERIFY(seen.size() >= 44 + static_cast<qint64>(claimedSize));
    QCOMPARE(seen.mid(44, claimedSize), payload.left(static_cast<int>(claimedSize)));
}

void TstAudioRecorder::parseSystemSources_malformedOrEmptyYieldsOnlyTheDefaultEntry()
{
    // Empty input (e.g. pw-dump missing/timed out — AudioRecorder::
    // availableSystemSources() passes an empty QByteArray in that case).
    QVariantList empty = AudioRecorderIO::parseSystemSources(QByteArray());
    QCOMPARE(empty.size(), 1);
    QCOMPARE(empty[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("@DEFAULT_MONITOR@"));
    QCOMPARE(empty[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Default output"));

    // Malformed JSON must never crash — same one-entry degrade.
    const QVariantList malformed = AudioRecorderIO::parseSystemSources(QByteArrayLiteral("{ not valid json"));
    QCOMPARE(malformed.size(), 1);
    QCOMPARE(malformed[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("@DEFAULT_MONITOR@"));

    // Valid JSON that isn't an array (pw-dump always emits an array; guard
    // against a single-object response anyway).
    const QVariantList notArray = AudioRecorderIO::parseSystemSources(QByteArrayLiteral("{\"id\": 1}"));
    QCOMPARE(notArray.size(), 1);

    // An empty array is valid JSON and parses fine — still just the default.
    const QVariantList emptyArray = AudioRecorderIO::parseSystemSources(QByteArrayLiteral("[]"));
    QCOMPARE(emptyArray.size(), 1);
}

void TstAudioRecorder::parseSystemSources_realFixtureSurfacesMonitorsAndStreamsFilteringNonAudio()
{
    QFile fixture(QStringLiteral(SUOP_REPO_DIR "/tests/fixtures/pw_dump_sample.json"));
    QVERIFY2(fixture.open(QIODevice::ReadOnly), "tests/fixtures/pw_dump_sample.json must exist");
    const QByteArray json = fixture.readAll();
    fixture.close();

    const QVariantList sources = AudioRecorderIO::parseSystemSources(json);

    // default + 2 sinks + 2 well-formed streams ("aplay" falls back to
    // media.name) — the mic input, the client node, and the stream missing
    // both application.name/media.name/object.serial-plus-node.name-only
    // partial case are all filtered/skipped.
    QCOMPARE(sources.size(), 5);

    QCOMPARE(sources[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("@DEFAULT_MONITOR@"));

    // Monitors: id is "monitor:<node.name>" (the raw sink name, NOT
    // "<name>.monitor" — AudioRecorder::startSystemAudio() appends that
    // suffix itself when building parec's args), name is node.description.
    const QVariantMap headset = sources[1].toMap();
    QCOMPARE(headset.value(QStringLiteral("id")).toString(),
             QStringLiteral("monitor:alsa_output.usb-Acme_USB_Headset_0000000000000000-00.analog-stereo"));
    QCOMPARE(headset.value(QStringLiteral("name")).toString(), QStringLiteral("Acme USB Headset"));

    const QVariantMap speakers = sources[2].toMap();
    QCOMPARE(speakers.value(QStringLiteral("id")).toString(),
             QStringLiteral("monitor:alsa_output.usb-Generic_USB_AUDIO-00.analog-stereo"));
    QCOMPARE(speakers.value(QStringLiteral("name")).toString(), QStringLiteral("Desk Speakers LT"));

    // App streams: id is "stream:<object.serial>" (pw-record --target's
    // handle), name prefers application.name, falling back to media.name.
    const QVariantMap firefox = sources[3].toMap();
    QCOMPARE(firefox.value(QStringLiteral("id")).toString(), QStringLiteral("stream:120"));
    QCOMPARE(firefox.value(QStringLiteral("name")).toString(), QStringLiteral("Firefox"));

    const QVariantMap aplay = sources[4].toMap();
    QCOMPARE(aplay.value(QStringLiteral("id")).toString(), QStringLiteral("stream:130"));
    QCOMPARE(aplay.value(QStringLiteral("name")).toString(), QStringLiteral("ALSA Playback")); // media.name fallback

    // Nothing named "unnamed-stream-node", the mic (Audio/Source), or the
    // bare client node made it in.
    for (const QVariant &entry : sources) {
        const QString id = entry.toMap().value(QStringLiteral("id")).toString();
        QVERIFY(!id.contains(QStringLiteral("Condenser")));
        QVERIFY(id != QStringLiteral("stream:unnamed-stream-node"));
    }
}

void TstAudioRecorder::planMixTick_equalQueuesConsumeTheMinWithNothingStalled()
{
    const AudioRecorderIO::MixTickPlan plan =
        AudioRecorderIO::planMixTick({ 400, 400, 400 }, { false, false, false }, /*stallThresholdBytes=*/100,
                                     /*frameSize=*/4);
    QCOMPARE(plan.bytes, 400);
    QCOMPARE(plan.stalled, QList<bool>({ false, false, false }));
}

void TstAudioRecorder::planMixTick_raggedButWithinThresholdStillConsumesFrameAlignedMin()
{
    const AudioRecorderIO::MixTickPlan plan =
        AudioRecorderIO::planMixTick({ 400, 350, 390 }, { false, false, false }, 100, 4);
    QCOMPARE(plan.bytes, 348); // min is 350, frame-aligned down to a multiple of 4
    QCOMPARE(plan.stalled, QList<bool>({ false, false, false }));
}

void TstAudioRecorder::planMixTick_sourceLaggingBeyondThresholdIsStalledAndExcluded()
{
    // Source 1 is 350 bytes behind the best-supplied source (400), well past
    // the 100-byte threshold — it must be excluded from the min so it doesn't
    // hold up the other two.
    const AudioRecorderIO::MixTickPlan plan =
        AudioRecorderIO::planMixTick({ 400, 50, 390 }, { false, false, false }, 100, 4);
    QCOMPARE(plan.stalled, QList<bool>({ false, true, false }));
    QCOMPARE(plan.bytes, 388); // min over the two non-stalled sources (390), frame-aligned
}

void TstAudioRecorder::planMixTick_stalledAndStillEmptySourceStaysStalled()
{
    // Source 1 is already flagged stalled and STILL has nothing queued — it
    // must stay stalled even though its distance from the best source (50) is
    // within the threshold on its own. Without this stickiness a dead
    // source's gap collapses as the live queue drains and un-stalls it.
    const AudioRecorderIO::MixTickPlan plan = AudioRecorderIO::planMixTick({ 50, 0 }, { false, true }, 100, 4);
    QCOMPARE(plan.stalled, QList<bool>({ false, true }));
    QCOMPARE(plan.bytes, 48); // min over the one non-stalled source (50), frame-aligned
}

void TstAudioRecorder::planMixTick_stalledSourceWithDataAgainUnstalls()
{
    // Source 1 was stalled but now has data again, and its distance from the
    // best source is back within the threshold — it must un-stall
    // immediately regardless of the previous verdict.
    const AudioRecorderIO::MixTickPlan plan = AudioRecorderIO::planMixTick({ 200, 150 }, { false, true }, 100, 4);
    QCOMPARE(plan.stalled, QList<bool>({ false, false }));
    QCOMPARE(plan.bytes, 148); // min over both sources (150), frame-aligned
}

void TstAudioRecorder::planMixTick_minBelowOneFrameYieldsZeroBytes()
{
    const AudioRecorderIO::MixTickPlan plan = AudioRecorderIO::planMixTick({ 3, 3 }, { false, false }, 100, 4);
    QCOMPARE(plan.bytes, 0);
    QCOMPARE(plan.stalled, QList<bool>({ false, false }));
}

void TstAudioRecorder::planMixTick_emptyInputYieldsZeroAndNoStalls()
{
    const AudioRecorderIO::MixTickPlan plan = AudioRecorderIO::planMixTick({}, {}, 100, 4);
    QCOMPARE(plan.bytes, 0);
    QVERIFY(plan.stalled.isEmpty());
}

void TstAudioRecorder::filterAppStreams_keepsOnlyApplicationStreams()
{
    QFile fixture(QStringLiteral(SUOP_REPO_DIR "/tests/fixtures/pw_dump_sample.json"));
    QVERIFY2(fixture.open(QIODevice::ReadOnly), "tests/fixtures/pw_dump_sample.json must exist");
    const QByteArray json = fixture.readAll();
    fixture.close();

    const QVariantList sources = AudioRecorderIO::parseSystemSources(json);
    const QVariantList appStreams = AudioRecorderIO::filterAppStreams(sources);

    // Only the two "stream:" entries survive — the synthetic default and
    // both sink monitors are gone.
    QCOMPARE(appStreams.size(), 2);
    QCOMPARE(appStreams[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("stream:120"));
    QCOMPARE(appStreams[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Firefox"));
    QCOMPARE(appStreams[1].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("stream:130"));
    QCOMPARE(appStreams[1].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("ALSA Playback"));
}

void TstAudioRecorder::buildAppCaptureRows_groupsLiveStreamsByApplicationName()
{
    const QVariantList streams = { appStream(QStringLiteral("stream:1"), QStringLiteral("Firefox")),
                                    appStream(QStringLiteral("stream:2"), QStringLiteral("Discord")),
                                    appStream(QStringLiteral("stream:3"), QStringLiteral("Firefox")) };
    const QVariantList rows = AudioRecorderIO::buildAppCaptureRows(streams, {});
    QCOMPARE(rows.size(), 2);

    const QVariantMap discord = rows[0].toMap();
    QCOMPARE(discord.value(QStringLiteral("name")).toString(), QStringLiteral("Discord"));
    QCOMPARE(discord.value(QStringLiteral("ids")).toStringList(), QStringList{ QStringLiteral("stream:2") });
    QCOMPARE(discord.value(QStringLiteral("active")).toBool(), true);

    const QVariantMap firefox = rows[1].toMap();
    QCOMPARE(firefox.value(QStringLiteral("name")).toString(), QStringLiteral("Firefox"));
    QCOMPARE(firefox.value(QStringLiteral("ids")).toStringList(),
             (QStringList{ QStringLiteral("stream:1"), QStringLiteral("stream:3") }));
    QCOMPARE(firefox.value(QStringLiteral("active")).toBool(), true);
}

void TstAudioRecorder::buildAppCaptureRows_keepsARememberedAppWithNoLiveStream()
{
    // The whole feature: a remembered app with nothing currently live still
    // gets a row instead of vanishing.
    const QVariantList rows = AudioRecorderIO::buildAppCaptureRows({}, { QStringLiteral("Discord") });
    QCOMPARE(rows.size(), 1);
    const QVariantMap row = rows[0].toMap();
    QCOMPARE(row.value(QStringLiteral("name")).toString(), QStringLiteral("Discord"));
    QVERIFY(row.value(QStringLiteral("ids")).toStringList().isEmpty());
    QCOMPARE(row.value(QStringLiteral("active")).toBool(), false);
}

void TstAudioRecorder::buildAppCaptureRows_marksARememberedAppActiveOnceItsStreamReturns()
{
    const QVariantList streams = { appStream(QStringLiteral("stream:9"), QStringLiteral("Discord")) };
    const QVariantList rows = AudioRecorderIO::buildAppCaptureRows(streams, { QStringLiteral("Discord") });
    // One row, not two — the remembered name and the live stream are the
    // same application.
    QCOMPARE(rows.size(), 1);
    const QVariantMap row = rows[0].toMap();
    QCOMPARE(row.value(QStringLiteral("name")).toString(), QStringLiteral("Discord"));
    QCOMPARE(row.value(QStringLiteral("ids")).toStringList(), QStringList{ QStringLiteral("stream:9") });
    QCOMPARE(row.value(QStringLiteral("active")).toBool(), true);
}

void TstAudioRecorder::buildAppCaptureRows_sortsByNameSoRowsDoNotMoveUnderTheCursor()
{
    const QVariantList streams = { appStream(QStringLiteral("stream:1"), QStringLiteral("Discord")),
                                    appStream(QStringLiteral("stream:2"), QStringLiteral("firefox")) };
    const QVariantList rows = AudioRecorderIO::buildAppCaptureRows(streams, { QStringLiteral("Ardour") });
    QCOMPARE(rows.size(), 3);
    QCOMPARE(rows[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Ardour"));
    QCOMPARE(rows[1].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Discord"));
    QCOMPARE(rows[2].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("firefox"));
}

void TstAudioRecorder::buildAppCaptureRows_skipsEntriesWithNoNameOrNoId()
{
    const QVariantList streams = { appStream(QStringLiteral("stream:1"), QString()),
                                    appStream(QString(), QStringLiteral("Ghost")) };
    const QVariantList rows = AudioRecorderIO::buildAppCaptureRows(streams, { QString() });
    QVERIFY(rows.isEmpty());
}

void TstAudioRecorder::buildAppCaptureRows_emptyInputsYieldNoRows()
{
    QVERIFY(AudioRecorderIO::buildAppCaptureRows({}, {}).isEmpty());
}

void TstAudioRecorder::captureCommand_mapsEachSourceIdToItsCapture()
{
    // Empty and unrecognized ids both fall back to the default-sink monitor.
    for (const QString &id : { QString(), QStringLiteral("garbage-id") }) {
        const QStringList cmd = AudioRecorderIO::captureCommand(id);
        QCOMPARE(cmd.first(), QStringLiteral("parec"));
        QVERIFY(cmd.contains(QStringLiteral("@DEFAULT_MONITOR@")));
        QVERIFY(cmd.contains(QStringLiteral("--format=s16le")));
        QVERIFY(cmd.contains(QStringLiteral("--rate=48000")));
        QVERIFY(cmd.contains(QStringLiteral("--channels=2")));
        QVERIFY(cmd.contains(QStringLiteral("--raw")));
    }

    const QStringList monitorCmd = AudioRecorderIO::captureCommand(QStringLiteral("monitor:foo"));
    QCOMPARE(monitorCmd.first(), QStringLiteral("parec"));
    QVERIFY(monitorCmd.contains(QStringLiteral("foo.monitor")));
    QVERIFY(monitorCmd.contains(QStringLiteral("--format=s16le")));
    QVERIFY(monitorCmd.contains(QStringLiteral("--rate=48000")));
    QVERIFY(monitorCmd.contains(QStringLiteral("--channels=2")));
    QVERIFY(monitorCmd.contains(QStringLiteral("--raw")));

    const QStringList streamCmd = AudioRecorderIO::captureCommand(QStringLiteral("stream:195"));
    QCOMPARE(streamCmd.first(), QStringLiteral("pw-record"));
    QVERIFY(streamCmd.contains(QStringLiteral("--target")));
    QVERIFY(streamCmd.contains(QStringLiteral("195")));
    QVERIFY(streamCmd.contains(QStringLiteral("48000")));
    QVERIFY(streamCmd.contains(QStringLiteral("2")));
    QVERIFY(streamCmd.contains(QStringLiteral("s16")));
    QVERIFY(streamCmd.contains(QStringLiteral("--raw")));
}

void TstAudioRecorder::isGenericProcessName_catchesTheChromiumShellNames()
{
    QVERIFY(AudioRecorderIO::isGenericProcessName(QStringLiteral("Chromium")));
    QVERIFY(AudioRecorderIO::isGenericProcessName(QStringLiteral("chromium")));
    QVERIFY(AudioRecorderIO::isGenericProcessName(QStringLiteral("Electron")));
    QVERIFY(AudioRecorderIO::isGenericProcessName(QStringLiteral("google-chrome-stable")));
    // Launchers/runtimes, both observed on a real graph: an Electron re-exec
    // through /proc/self/exe reports "exe", and a node wrapper sits between
    // systemd and the app.
    QVERIFY(AudioRecorderIO::isGenericProcessName(QStringLiteral("exe")));
    QVERIFY(AudioRecorderIO::isGenericProcessName(QStringLiteral("node")));

    // Real applications whose names must survive — a false positive here
    // silently hides an app's actual label.
    QVERIFY(!AudioRecorderIO::isGenericProcessName(QStringLiteral("Firefox")));
    QVERIFY(!AudioRecorderIO::isGenericProcessName(QStringLiteral("Discord")));
    QVERIFY(!AudioRecorderIO::isGenericProcessName(QStringLiteral("Brave")));
    QVERIFY(!AudioRecorderIO::isGenericProcessName(QString()));
}

void TstAudioRecorder::streamDisplayName_prefersARealApplicationName()
{
    QVariantMap props;
    props[QStringLiteral("application.name")] = QStringLiteral("Firefox");
    props[QStringLiteral("media.name")] = QStringLiteral("Playback");
    // A non-generic application.name wins outright — the ancestry is not even
    // consulted, so a browser doesn't get relabelled with its launcher.
    QCOMPARE(AudioRecorderIO::streamDisplayName(props, {QStringLiteral("bash")}), QStringLiteral("Firefox"));
}

void TstAudioRecorder::streamDisplayName_rescuesElectronAppsFromTheAncestryChain()
{
    QVariantMap props;
    props[QStringLiteral("application.name")] = QStringLiteral("Chromium");
    props[QStringLiteral("media.name")] = QStringLiteral("Playback");

    // The whole point of the ancestry walk: an Electron app's audio stream
    // reports application.name "Chromium", and only a PARENT process knows
    // it's really Discord. Nearest non-generic ancestor wins, title-cased.
    const QStringList ancestry = {QStringLiteral("chromium"), QStringLiteral("discord"), QStringLiteral("systemd")};
    QCOMPARE(AudioRecorderIO::streamDisplayName(props, ancestry), QStringLiteral("Discord"));

    // The real chain measured for Vesktop (a Discord client): the stream's own
    // process re-execs through /proc/self/exe, so the nearest entry is the
    // junk "exe" and the app's name is one hop further up. Taking the nearest
    // entry blindly labelled this stream "Exe".
    const QStringList vesktop = {QStringLiteral("exe"), QStringLiteral("vesktop"), QStringLiteral("node"),
                                 QStringLiteral("systemd")};
    QCOMPARE(AudioRecorderIO::streamDisplayName(props, vesktop), QStringLiteral("Vesktop"));
}

void TstAudioRecorder::streamDisplayName_keepsChromiumWhenNoAncestorDisagrees()
{
    QVariantMap props;
    props[QStringLiteral("application.name")] = QStringLiteral("Chromium");

    // A real Chromium browser genuinely is Chromium: every ancestor is a
    // generic shell name too, so the generic label is the honest one and must
    // NOT be dropped or replaced by a shell/launcher name.
    const QStringList ancestry = {QStringLiteral("chromium"), QStringLiteral("chromium")};
    QCOMPARE(AudioRecorderIO::streamDisplayName(props, ancestry), QStringLiteral("Chromium"));
}

void TstAudioRecorder::streamDisplayName_fallsBackToBinaryThenMediaName()
{
    QVariantMap withBinary;
    withBinary[QStringLiteral("application.name")] = QStringLiteral("Chromium");
    withBinary[QStringLiteral("application.process.binary")] = QStringLiteral("/opt/slack/slack");
    withBinary[QStringLiteral("media.name")] = QStringLiteral("Playback");
    QCOMPARE(AudioRecorderIO::streamDisplayName(withBinary, {}), QStringLiteral("Slack"));

    // No application.name at all (aplay-style) — media.name is the only label
    // on offer, and the fixture-backed parser test depends on this rung.
    QVariantMap mediaOnly;
    mediaOnly[QStringLiteral("media.name")] = QStringLiteral("ALSA Playback");
    QCOMPARE(AudioRecorderIO::streamDisplayName(mediaOnly, {}), QStringLiteral("ALSA Playback"));

    // Nothing usable at all — the caller drops such a stream.
    QVERIFY(AudioRecorderIO::streamDisplayName(QVariantMap(), {}).isEmpty());
}

void TstAudioRecorder::processAncestryNames_walksThisProcessUpToItsParents()
{
    const QStringList names = AudioRecorderIO::processAncestryNames(QCoreApplication::applicationPid());
    if (!QFile::exists(QStringLiteral("/proc/self/status")))
        QSKIP("no /proc on this platform");

    QVERIFY2(!names.isEmpty(), "the walk must at least name the process it was given");
    // Element 0 is this test binary itself. Its name comes from comm, which
    // the kernel truncates to 15 chars ("tst_audiorecord") — argv[0] is used
    // ONLY to repair that, so the full name is what must come back here.
    QCOMPARE(names.first(), QStringLiteral("tst_audiorecorder"));
    QVERIFY2(names.size() > 1, "a test binary always has at least one ancestor short of init");

    QVERIFY2(AudioRecorderIO::processAncestryNames(0).isEmpty(), "pid 0 must not walk anywhere");
}

void TstAudioRecorder::parseSystemSources_appliesTheInjectedAncestryToStreamNames()
{
    // pw-dump's own shape, trimmed to one Electron-style stream: the parser
    // must route application.process.id through the INJECTED resolver, which
    // is what keeps it hermetic (a fixture pid must never hit real /proc).
    const QByteArray json = R"([
        {"info": {"props": {
            "media.class": "Stream/Output/Audio",
            "object.serial": 195,
            "application.process.id": 4242,
            "application.name": "Chromium",
            "media.name": "Playback"
        }}}
    ])";

    qint64 seenPid = 0;
    const auto ancestry = [&seenPid](qint64 pid) -> QStringList {
        seenPid = pid;
        return {QStringLiteral("chromium"), QStringLiteral("discord")};
    };

    const QVariantList sources = AudioRecorderIO::parseSystemSources(json, ancestry);
    const QVariantList apps = AudioRecorderIO::filterAppStreams(sources);
    QCOMPARE(apps.size(), 1);
    QCOMPARE(seenPid, qint64(4242));
    QCOMPARE(apps[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("stream:195"));
    QCOMPARE(apps[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Discord"));

    // Without a resolver the parse stays hermetic and falls back to the
    // node's own properties — no /proc access, whatever the pid says.
    const QVariantList noAncestry = AudioRecorderIO::filterAppStreams(AudioRecorderIO::parseSystemSources(json));
    QCOMPARE(noAncestry.size(), 1);
    QCOMPARE(noAncestry[0].toMap().value(QStringLiteral("name")).toString(), QStringLiteral("Playback"));
}

void TstAudioRecorder::pendingCaptureAdditions_returnsOnlyIdsNotAlreadyCapturing()
{
    const QVariantList desired = { captureEntry(QStringLiteral("stream:1"), 1.0),
                                    captureEntry(QStringLiteral("stream:2"), 0.5) };
    const QVariantList pending =
        AudioRecorderIO::pendingCaptureAdditions(desired, { QStringLiteral("stream:1") });
    QCOMPARE(pending.size(), 1);
    const QVariantMap entry = pending[0].toMap();
    QCOMPARE(entry.value(QStringLiteral("id")).toString(), QStringLiteral("stream:2"));
    QCOMPARE(entry.value(QStringLiteral("gain")).toDouble(), 0.5);
}

void TstAudioRecorder::pendingCaptureAdditions_nothingPendingWhenAllAreActive()
{
    const QVariantList desired = { captureEntry(QStringLiteral("stream:1"), 1.0),
                                    captureEntry(QStringLiteral("stream:2"), 1.0) };
    const QVariantList pending = AudioRecorderIO::pendingCaptureAdditions(
        desired, { QStringLiteral("stream:1"), QStringLiteral("stream:2") });
    QVERIFY(pending.isEmpty());
}

void TstAudioRecorder::pendingCaptureAdditions_dropsDuplicatesWithinDesired()
{
    // Double-adding an app comb-filters it against itself (a second capture
    // process for the same stream) — this is the failure the within-`desired`
    // de-dup guards against.
    const QVariantList desired = { captureEntry(QStringLiteral("stream:7"), 1.0),
                                    captureEntry(QStringLiteral("stream:7"), 1.0) };
    const QVariantList pending = AudioRecorderIO::pendingCaptureAdditions(desired, {});
    QCOMPARE(pending.size(), 1);
    QCOMPARE(pending[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("stream:7"));
}

void TstAudioRecorder::pendingCaptureAdditions_dropsEmptyIds()
{
    const QVariantList desired = { captureEntry(QString(), 1.0), captureEntry(QStringLiteral("stream:1"), 1.0) };
    const QVariantList pending = AudioRecorderIO::pendingCaptureAdditions(desired, {});
    QCOMPARE(pending.size(), 1);
    QCOMPARE(pending[0].toMap().value(QStringLiteral("id")).toString(), QStringLiteral("stream:1"));
}

void TstAudioRecorder::pendingCaptureAdditions_emptyActiveListAddsEverything()
{
    // The cold-start shape — same rule reused if start() ever routes through
    // this instead of building m_sources directly.
    const QVariantList desired = { captureEntry(QStringLiteral("stream:1"), 1.0),
                                    captureEntry(QStringLiteral("stream:2"), 1.0) };
    const QVariantList pending = AudioRecorderIO::pendingCaptureAdditions(desired, {});
    QCOMPARE(pending.size(), 2);
}

void TstAudioRecorder::planMixTick_freshlyAddedStalledSourceDoesNotHaltTheMix()
{
    // The late-join entry condition addAppSource() relies on: a brand-new
    // source starts wasStalled=true even at 0 queued bytes.
    const AudioRecorderIO::MixTickPlan joined =
        AudioRecorderIO::planMixTick({ 4800, 4800, 0 }, { false, false, true }, /*stallThresholdBytes=*/24000, 4);
    QCOMPARE(joined.stalled, QList<bool>({ false, false, true }));
    QCOMPARE(joined.bytes, 4800); // min over the other TWO sources, not dragged to 0 by the new one

    // The regression this guards: WITHOUT starting stalled, the new source's
    // 0 bytes drags the min to zero and the entire mix halts.
    const AudioRecorderIO::MixTickPlan withoutFix =
        AudioRecorderIO::planMixTick({ 4800, 4800, 0 }, { false, false, false }, 24000, 4);
    QCOMPARE(withoutFix.stalled, QList<bool>({ false, false, false }));
    QCOMPARE(withoutFix.bytes, 0);
}

QTEST_GUILESS_MAIN(TstAudioRecorder)
#include "tst_audiorecorder.moc"
