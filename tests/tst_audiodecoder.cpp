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
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
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

// DecodeControl's pause/cancel is polled once per ~100ms loop iteration (see
// decodeTo16kMono()'s doc comment) — jfk.wav decodes in well under that on
// any reasonable machine, giving the control callbacks no realistic chance
// to fire more than once. A synthetic tone long enough to guarantee several
// poll iterations is the only reliable way to exercise mid-decode
// pause/cancel, hence generating one on the fly rather than shipping a
// multi-minute fixture.
QString makeLongSyntheticWav(const QString &path, int seconds = 180)
{
    QProcess gen;
    gen.start("ffmpeg", {"-y", "-f", "lavfi", "-i", QStringLiteral("sine=frequency=440:duration=%1").arg(seconds),
                          path});
    if (!gen.waitForFinished(30000) || gen.exitCode() != 0)
        return QString();
    return QFileInfo::exists(path) ? path : QString();
}

// A 10s tone: 5s at -3dB ("loud" half) concatenated with 5s at -30dB ("quiet"
// half) — a 27dB (~22x amplitude) gap, standing in for a quiet voice in an
// otherwise-loud recording (DESIGN.md/AudioDecoder.cpp's speechnorm comment).
QString makeLoudQuietWav(const QString &path)
{
    QProcess gen;
    gen.start("ffmpeg", {"-y",
                          "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=16000:duration=5",
                          "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=16000:duration=5",
                          "-filter_complex",
                          "[0:a]volume=-3dB[loud];[1:a]volume=-30dB[quiet];[loud][quiet]concat=n=2:v=0:a=1[out]",
                          "-map", "[out]", "-ac", "1", path});
    if (!gen.waitForFinished(30000) || gen.exitCode() != 0)
        return QString();
    return QFileInfo::exists(path) ? path : QString();
}

double rms(const std::vector<float> &pcm, std::size_t begin, std::size_t end)
{
    double sumSquares = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        sumSquares += static_cast<double>(pcm[i]) * pcm[i];
    return std::sqrt(sumSquares / static_cast<double>(end - begin));
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

    // Spec (2026-07-10 user report — pausing during "decoding" appeared to
    // do nothing): ffmpeg runs as a real subprocess here, not an in-process
    // library call, so — unlike whisper.cpp's model load — a cancel request
    // can kill it almost immediately rather than waiting for the whole
    // decode to finish naturally. Cancels on the 3rd poll (~200-300ms in);
    // the synthetic source is 180s, so a multi-second wait would mean the
    // cancel was ignored.
    void cancelledDuringDecode_stopsPromptlyWithCancelledError()
    {
        REQUIRE_FFMPEG();

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString longWav = makeLongSyntheticWav(tempDir.filePath(QStringLiteral("long.wav")));
        if (longWav.isEmpty())
            QSKIP("could not generate synthetic test audio via ffmpeg");

        AudioDecoder::DecodeControl control;
        int calls = 0;
        control.cancelled = [&calls] { return ++calls > 2; };

        QElapsedTimer timer;
        timer.start();
        QString error;
        const auto pcm = AudioDecoder::decodeTo16kMono(longWav, &error, control);
        QVERIFY(!pcm.has_value());
        QCOMPARE(error, QStringLiteral("Cancelled"));
        QVERIFY2(timer.elapsed() < 3000,
                 qPrintable(QStringLiteral("cancel took %1ms — should be near-instant, not wait out the decode")
                                .arg(timer.elapsed())));
    }

    // Spec: pause genuinely suspends the ffmpeg subprocess (SIGSTOP) rather
    // than just refusing to read from it — proven by control.paused()
    // getting polled repeatedly (the loop keeps ticking every ~100ms while
    // stopped) before it returns false and decode completes normally.
    void pausedDuringDecode_blocksUntilUnpaused_thenCompletesNormally()
    {
        REQUIRE_FFMPEG();

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString longWav = makeLongSyntheticWav(tempDir.filePath(QStringLiteral("long.wav")));
        if (longWav.isEmpty())
            QSKIP("could not generate synthetic test audio via ffmpeg");

        AudioDecoder::DecodeControl control;
        int pauseChecks = 0;
        bool stillPaused = true;
        control.paused = [&pauseChecks, &stillPaused] {
            ++pauseChecks;
            if (pauseChecks > 5)
                stillPaused = false;
            return stillPaused;
        };

        QString error;
        const auto pcm = AudioDecoder::decodeTo16kMono(longWav, &error, control);
        QVERIFY2(pcm.has_value(), qPrintable(error));
        QVERIFY2(pauseChecks > 5,
                 qPrintable(QStringLiteral("only %1 pause checks — pause didn't actually block the loop")
                                .arg(pauseChecks)));
    }

    // Spec (this feature): decodeTo16kMono applies ffmpeg's speechnorm filter
    // so a quiet passage in an otherwise-loud recording isn't left far below
    // whisper.cpp's mel-normalization clamp floor (see AudioDecoder.cpp's
    // speechnorm comment). Proof: a synthetic 27dB (~22x) loud/quiet gap must
    // come out of decodeTo16kMono() meaningfully narrower — genuine adaptive
    // leveling, not a no-op or a uniform gain that preserves the same ratio.
    // Thresholds are empirically grounded (see AudioDecoder.cpp's comment):
    // measured ~9.8x post-normalization ratio with generous slack either side.
    void speechnormNarrowsQuietPassageGap()
    {
        REQUIRE_FFMPEG();

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString wavPath = makeLoudQuietWav(tempDir.filePath(QStringLiteral("loud_quiet.wav")));
        if (wavPath.isEmpty())
            QSKIP("could not generate synthetic loud/quiet test audio via ffmpeg");

        QString error;
        const auto pcm = AudioDecoder::decodeTo16kMono(wavPath, &error);
        QVERIFY2(pcm.has_value(), qPrintable(error));

        // 10s at 16kHz split into two 5s halves; trim a small guard band
        // around the midpoint so speechnorm's own reaction lag at the
        // loud->quiet transition doesn't contaminate either measurement.
        const std::size_t half = pcm->size() / 2;
        const std::size_t guard = 8000; // 0.5s
        const double loudRms = rms(*pcm, 0, half - guard);
        const double quietRms = rms(*pcm, half + guard, pcm->size());

        QVERIFY2(quietRms > 1e-6, "quiet half must not be silence after normalization");
        const double ratio = loudRms / quietRms;
        QVERIFY2(ratio < 15.0,
                 qPrintable(QStringLiteral("post-speechnorm loud/quiet RMS ratio %1 — gap barely narrowed "
                                            "from the raw ~22x, leveling isn't actually working")
                                .arg(ratio)));
    }
};

QTEST_GUILESS_MAIN(TstAudioDecoder)
#include "tst_audiodecoder.moc"
