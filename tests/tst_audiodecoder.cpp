// tst_audiodecoder — living spec for DESIGN.md "Audio: decode vs playback".
//
// ffmpeg — never Qt Multimedia — decodes and resamples transcription input
// to 16 kHz mono float32 PCM (CLAUDE.md load-bearing constraint: "ffmpeg
// decodes/resamples audio; Qt Multimedia only plays it"). The PCM is
// streamed from ffmpeg's stdout pipe straight into memory: no temp WAV is
// ever written. Playback runs on the original file separately; AudioDecoder
// is transcription input only.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cmath>

#include "AudioDecoder.h"

namespace {

const QString kJfkWav = QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"); // ~11 s JFK sample
const QString kJfkMp3 = QStringLiteral(SUOP_SAMPLES_DIR "/jfk.mp3"); // same audio, lossy

bool ffmpegOnPath()
{
    return !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty();
}

// Snapshot a directory's file names — used to prove decoding leaves no
// artifacts behind (the no-temp-WAV design).
QStringList dirEntries(const QString &path)
{
    return QDir(path).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

} // namespace

// AudioDecoder shells out to ffmpeg by design; without it on PATH every
// decode test skips rather than fails (the dependency is external and
// documented, not a bug).
#define REQUIRE_FFMPEG()                                                                          \
    do {                                                                                          \
        if (!ffmpegOnPath())                                                                      \
            QSKIP("ffmpeg not on PATH — AudioDecoder shells out to ffmpeg by design "             \
                  "(DESIGN.md \"Audio: decode vs playback\")");                                   \
    } while (false)

class TstAudioDecoder : public QObject
{
    Q_OBJECT

private slots:
    // Spec: decode yields 16 kHz mono float PCM. The ~11 s jfk.wav must
    // produce roughly 11 * 16000 samples — the resample target is fixed at
    // 16 kHz regardless of the source rate — and every sample is a finite
    // float in the normalized range (f32le output; small overshoot allowed
    // for resampling ringing).
    void wavDecodesToSixteenKilohertzMonoFloat()
    {
        REQUIRE_FFMPEG();

        QString error;
        const auto pcm = AudioDecoder::decodeTo16kMono(kJfkWav, &error);
        QVERIFY2(pcm.has_value(), qPrintable(error));

        const std::size_t n = pcm->size();
        QVERIFY2(n >= std::size_t(10.5 * 16000) && n <= std::size_t(11.5 * 16000),
                 qPrintable(QStringLiteral("expected ~11s at 16kHz, got %1 samples").arg(n)));

        int badSamples = 0;
        for (float v : *pcm) {
            if (!std::isfinite(v) || v < -1.5f || v > 1.5f)
                ++badSamples;
        }
        QCOMPARE(badSamples, 0);
    }

    // Spec: decoding is format-agnostic normalization — any input container/
    // codec comes out as the same 16 kHz mono PCM. The mp3 variant of the
    // same recording must land within a few hundred ms of the wav's sample
    // count (mp3 encoder delay/padding accounts for the small difference).
    void mp3NormalizesToSameSampleCountAsWav()
    {
        REQUIRE_FFMPEG();

        QString error;
        const auto wav = AudioDecoder::decodeTo16kMono(kJfkWav, &error);
        QVERIFY2(wav.has_value(), qPrintable(error));
        const auto mp3 = AudioDecoder::decodeTo16kMono(kJfkMp3, &error);
        QVERIFY2(mp3.has_value(), qPrintable(error));

        const auto diff = std::llabs(static_cast<long long>(wav->size()) - static_cast<long long>(mp3->size()));
        const long long toleranceSamples = static_cast<long long>(0.3 * 16000); // 300 ms
        QVERIFY2(diff <= toleranceSamples,
                 qPrintable(QStringLiteral("wav %1 vs mp3 %2 samples differ by %3 (> %4)")
                                .arg(wav->size()).arg(mp3->size()).arg(diff).arg(toleranceSamples)));
    }

    // Spec: a missing input is a clean failure — std::nullopt plus a
    // non-empty error string (ffmpeg's stderr, thanks to -loglevel error),
    // never a crash or an empty-but-"successful" buffer.
    void nonexistentFileReturnsNulloptWithError()
    {
        REQUIRE_FFMPEG();

        QString error;
        const auto pcm = AudioDecoder::decodeTo16kMono(QStringLiteral("/nonexistent/audio/file.wav"), &error);
        QVERIFY(!pcm.has_value());
        QVERIFY2(!error.isEmpty(), "errorOut must carry ffmpeg's stderr for missing files");
    }

    // Spec: a file that exists but is not audio also fails cleanly with
    // nullopt + error — ffmpeg rejects it and the decoder surfaces that.
    void nonAudioFileReturnsNulloptWithError()
    {
        REQUIRE_FFMPEG();

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString textPath = tempDir.filePath(QStringLiteral("not-audio.txt"));
        {
            QFile f(textPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("this is plain text, not audio\n");
        }

        QString error;
        const auto pcm = AudioDecoder::decodeTo16kMono(textPath, &error);
        QVERIFY(!pcm.has_value());
        QVERIFY2(!error.isEmpty(), "errorOut must carry ffmpeg's stderr for non-audio input");
    }

    // Spec: NO TEMP WAV (DESIGN.md "Audio"): PCM streams over ffmpeg's stdout
    // pipe ("-f f32le ... -") directly into memory, so decoding leaves the
    // filesystem untouched — no sidecar next to the input, nothing in a work
    // dir. Light check: directory listings before/after must be identical.
    void decodingLeavesNoTempWavBehind()
    {
        REQUIRE_FFMPEG();

        // Decode from an isolated dir containing only a copy of the input —
        // any temp artifact would show up as a second entry.
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString wavCopy = tempDir.filePath(QStringLiteral("jfk.wav"));
        QVERIFY(QFile::copy(kJfkWav, wavCopy));

        const QStringList tempBefore = dirEntries(tempDir.path());
        QCOMPARE(tempBefore.size(), 1);

        QString error;
        const auto pcm = AudioDecoder::decodeTo16kMono(wavCopy, &error);
        QVERIFY2(pcm.has_value(), qPrintable(error));

        QCOMPARE(dirEntries(tempDir.path()), tempBefore);

        // And the shared samples dir is equally untouched by a decode.
        const QStringList samplesBefore = dirEntries(QStringLiteral(SUOP_SAMPLES_DIR));
        const auto pcm2 = AudioDecoder::decodeTo16kMono(kJfkWav, &error);
        QVERIFY2(pcm2.has_value(), qPrintable(error));
        QCOMPARE(dirEntries(QStringLiteral(SUOP_SAMPLES_DIR)), samplesBefore);
    }
};

QTEST_GUILESS_MAIN(TstAudioDecoder)
#include "tst_audiodecoder.moc"
