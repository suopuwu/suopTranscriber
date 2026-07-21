#include <QtTest>

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

}

class TstAudioRecorder : public QObject
{
    Q_OBJECT
private slots:
    void wavHeader_structureAndSizes();
    void patchWavHeader_rewritesSizeFieldsInPlace();
    void filenameTimestamp_matchesPattern();
    void outputPath_withAndWithoutSubfolder();
    void mixPcm16_appliesIndependentGains();
    void mixPcm16_clampsAtExtremes();
    void gainScalePcm16_singleSourceScalesAndClips();
    void rmsPcm16_knownSignals();
    void displayLevelFromRms_spreadsSpeechAcrossTheRange();
    void startStopRecording_stateFlipsSynchronouslyOnTheCall();
    void patchWavHeader_midStreamCheckpointIsReadableAndPreservesPosition();
    void parseSystemSources_malformedOrEmptyYieldsOnlyTheDefaultEntry();
    void parseSystemSources_realFixtureSurfacesMonitorsAndStreamsFilteringNonAudio();
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

void TstAudioRecorder::mixPcm16_appliesIndependentGains()
{
    const QByteArray mic = toPcm16({ 1000, -1000, 32000, -32000 });
    const QByteArray sys = toPcm16({ 500, 500, 500, 500 });

    const QByteArray mixed = AudioRecorderIO::mixPcm16(mic, 0.5, sys, 1.0);
    const QVector<qint16> samples = fromPcm16(mixed);

    QCOMPARE(samples.size(), 4);
    QCOMPARE(samples[0], qint16(1000)); // 1000*0.5 + 500 = 1000
    QCOMPARE(samples[1], qint16(0)); // -1000*0.5 + 500 = 0
    QCOMPARE(samples[2], qint16(16500)); // 32000*0.5 + 500 = 16500
    QCOMPARE(samples[3], qint16(-15500)); // -32000*0.5 + 500 = -15500
}

void TstAudioRecorder::mixPcm16_clampsAtExtremes()
{
    const QByteArray a = toPcm16({ 32767, -32768 });
    const QByteArray b = toPcm16({ 32767, -32768 });

    const QByteArray mixed = AudioRecorderIO::mixPcm16(a, 1.0, b, 1.0);
    const QVector<qint16> samples = fromPcm16(mixed);

    QCOMPARE(samples.size(), 2);
    QCOMPARE(samples[0], std::numeric_limits<qint16>::max());
    QCOMPARE(samples[1], std::numeric_limits<qint16>::min());
}

void TstAudioRecorder::gainScalePcm16_singleSourceScalesAndClips()
{
    const QByteArray in = toPcm16({ 1000, -1000, 30000 });
    const QByteArray out = AudioRecorderIO::gainScalePcm16(in, 2.0);
    const QVector<qint16> samples = fromPcm16(out);

    QCOMPARE(samples.size(), 3);
    QCOMPARE(samples[0], qint16(2000));
    QCOMPARE(samples[1], qint16(-2000));
    QCOMPARE(samples[2], std::numeric_limits<qint16>::max()); // 60000 clamps to 32767
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

QTEST_GUILESS_MAIN(TstAudioRecorder)
#include "tst_audiorecorder.moc"
