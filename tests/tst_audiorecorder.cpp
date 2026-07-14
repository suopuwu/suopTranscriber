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

QTEST_GUILESS_MAIN(TstAudioRecorder)
#include "tst_audiorecorder.moc"
