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
#include <QThread>

#include <atomic>
#include <cmath>
#include <limits>

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

// A ~60s synthetic mp3, corrupted throughout with a deterministic PRNG (every
// 3rd byte from 1% in), for hugeStderrDoesNotDeadlock_andIsStillReported()
// below. Measured on the dev machine (ffmpeg n8.1.2): this reliably pushes
// ffmpeg's mp3 decoder into ~90KB of repeated "Header missing" / "Error
// submitting packet to decoder" -loglevel=error lines — well past the 64KB
// pipe buffer — while still failing overall (nonzero exit), not limping to a
// "successful" decode. Confirmed independently (a throwaway Python
// subprocess.Popen that reads only stdout, never stderr, on this exact file)
// that this genuinely deadlocks without a stderr drain: it had to be killed
// by `timeout`, not that it just runs slow.
QString makeHeavilyCorruptedMp3(const QString &path)
{
    QProcess gen;
    gen.start("ffmpeg",
              {"-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=60", "-c:a", "libmp3lame", path});
    if (!gen.waitForFinished(30000) || gen.exitCode() != 0 || !QFileInfo::exists(path))
        return QString();

    QFile f(path);
    if (!f.open(QIODevice::ReadWrite))
        return QString();
    QByteArray data = f.readAll();
    const qint64 n = data.size();

    // xorshift32 — deterministic and reproducible across runs; no need for
    // real entropy, just enough to reliably break mp3 frame sync.
    quint32 state = 11;
    const auto nextByte = [&state]() -> char {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<char>(state & 0xFF);
    };
    for (qint64 i = n / 100; i < n; ++i) {
        if (i % 3 == 0)
            data[i] = nextByte();
    }

    f.seek(0);
    f.write(data);
    f.close();
    return path;
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

// Hand-rolled canonical IEEE-float WAV (format tag 3, with the "fact" chunk
// the format expects) — these tests' fixtures need exact raw bytes (a NaN, an
// Inf, a 2.5 peak) that no ffmpeg filter graph can express, so this writes
// them directly rather than shelling out to generate the source. Mirrors
// writeMono16kWav's header-write pattern in SubprocessEngine.cpp, at float32
// instead of int16 and without its [-1,1] clamp (the whole point here is
// values outside that range).
bool writeMonoFloatWav(const QString &path, quint32 sampleRate, const std::vector<float> &samples)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    constexpr quint16 channels = 1;
    constexpr quint16 bitsPerSample = 32;
    constexpr quint16 blockAlign = channels * bitsPerSample / 8;
    const quint32 byteRate = sampleRate * blockAlign;
    const quint32 dataBytes = static_cast<quint32>(samples.size()) * sizeof(float);
    constexpr quint32 factBytes = 4;
    const quint32 riffBytes = 4 + (8 + 16) + (8 + factBytes) + (8 + dataBytes);

    const auto putU32 = [&file](quint32 v) {
        char b[4] = {char(v & 0xff), char((v >> 8) & 0xff), char((v >> 16) & 0xff), char((v >> 24) & 0xff)};
        file.write(b, 4);
    };
    const auto putU16 = [&file](quint16 v) {
        char b[2] = {char(v & 0xff), char((v >> 8) & 0xff)};
        file.write(b, 2);
    };

    file.write("RIFF", 4);
    putU32(riffBytes);
    file.write("WAVE", 4);

    file.write("fmt ", 4);
    putU32(16);
    putU16(3); // WAVE_FORMAT_IEEE_FLOAT
    putU16(channels);
    putU32(sampleRate);
    putU32(byteRate);
    putU16(blockAlign);
    putU16(bitsPerSample);

    file.write("fact", 4);
    putU32(factBytes);
    putU32(static_cast<quint32>(samples.size()));

    file.write("data", 4);
    putU32(dataBytes);
    file.write(reinterpret_cast<const char *>(samples.data()), static_cast<qint64>(dataBytes));

    file.close();
    return true;
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

    // Spec: stderr is drained every poll iteration, exactly like
    // stdout — a pipe's kernel buffer is finite (~64KB on Linux), and ffmpeg
    // blocked writing a flood of per-frame decode errors past that would
    // never exit, wedging the poll loop even though decodeTo16kMono only
    // reads stdout for the actual PCM. The chosen proof: a
    // deliberately damaged input that provokes >64KB of -loglevel=error
    // stderr (see makeHeavilyCorruptedMp3's comment, including independent
    // confirmation via a stdout-only-reading Python subprocess that this
    // exact file genuinely deadlocks without a stderr drain) — this proves
    // the drain rather than just asserting a weaker property. The call must
    // RETURN (nullopt + non-empty error) well within a generous bound,
    // rather than hang.
    void hugeStderrDoesNotDeadlock_andIsStillReported()
    {
        REQUIRE_FFMPEG();

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString corrupt = makeHeavilyCorruptedMp3(tempDir.filePath(QStringLiteral("corrupt.mp3")));
        if (corrupt.isEmpty())
            QSKIP("could not generate the corrupted mp3 fixture via ffmpeg");

        QElapsedTimer timer;
        timer.start();
        QString error;
        const auto pcm = AudioDecoder::decodeTo16kMono(corrupt, &error);
        const qint64 elapsedMs = timer.elapsed();

        QVERIFY2(elapsedMs < 15000,
                 qPrintable(QStringLiteral("decode took %1ms — a stderr drain gap would wedge this "
                                            "on a full pipe, not just run slow")
                                .arg(elapsedMs)));
        QVERIFY2(!pcm.has_value(),
                 "this file's corruption is dense enough that ffmpeg exits non-zero overall — "
                 "if this starts failing, the fixture stopped reproducing a genuine failure");
        QVERIFY2(!error.isEmpty(), "errorOut must carry the accumulated stderr, not a post-exit "
                                    "readAllStandardError() that would now return almost nothing");
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

    // Pure-function bounds check on the decode stall watchdog (mirrors how
    // tst_transcription_queue.cpp tests TranscribeTask::isStalled directly) —
    // decode output is continuous, so the rule is a flat 60s bound, not
    // audio-length-relative like the engine's own watchdog.
    void isDecodeStalled_boundsNoOutputTime()
    {
        QVERIFY2(!AudioDecoder::isDecodeStalled(59'000), "59s of silence must not read as stalled");
        QVERIFY2(AudioDecoder::isDecodeStalled(61'000), "61s of silence must read as stalled");
    }

    // Spec: a NaN sample fed through ffmpeg's speechnorm filter
    // spins its rolling-peak tracker forever (NaN compares false against
    // everything, so the peak-advance loop never converges) — measured on a
    // real corrupt wireless-mic recording as a 678s kill with zero output, and
    // would run forever unkilled. The fix decodes WITHOUT speechnorm first
    // (NaN-safe), scrubs non-finite samples to silence in C++, then applies
    // speechnorm to the cleaned buffer. This test would have hung before the
    // fix; it must now both RETURN and yield all-finite output.
    //
    // The external timeout below is deliberately independent of the
    // in-process 60s stall watchdog — it uses DecodeControl's own
    // real cancel hook from a second thread, so this test still catches a
    // regression even if the watchdog itself were ever broken, rather than
    // silently relying on it as the only backstop.
    void nonFiniteSamplesDecodeInsteadOfHanging()
    {
        REQUIRE_FFMPEG();

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString wavPath = tempDir.filePath(QStringLiteral("nan.wav"));

        // 5s @ 48kHz mono tone (the minimal repro), with one
        // sample forced to NaN and another to +Inf partway through — the
        // exact combination measured to hang (Inf alone, or NaN without
        // speechnorm, are both harmless — also measured).
        constexpr quint32 sampleRate = 48000;
        constexpr int seconds = 5;
        std::vector<float> samples(sampleRate * seconds);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = 0.2f * std::sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / sampleRate);
        samples[samples.size() / 2] = std::numeric_limits<float>::quiet_NaN();
        samples[samples.size() / 2 + 1000] = std::numeric_limits<float>::infinity();
        QVERIFY(writeMonoFloatWav(wavPath, sampleRate, samples));

        constexpr qint64 kHardTimeoutMs = 30'000;
        std::atomic<bool> timedOut{false};
        AudioDecoder::DecodeControl control;
        control.cancelled = [&timedOut] { return timedOut.load(); };

        std::optional<std::vector<float>> pcm;
        QString error;
        QScopedPointer<QThread> worker(QThread::create([&] {
            pcm = AudioDecoder::decodeTo16kMono(wavPath, &error, control);
        }));

        QElapsedTimer timer;
        timer.start();
        worker->start();
        const bool joinedInTime = worker->wait(kHardTimeoutMs);
        if (!joinedInTime) {
            // Regression path: cancel for real so the worker thread (and its
            // ffmpeg child) actually unwinds instead of leaking past this test.
            timedOut = true;
            worker->wait(15'000);
        }
        const qint64 elapsedMs = timer.elapsed();

        QVERIFY2(joinedInTime,
                 qPrintable(QStringLiteral("decodeTo16kMono did not return within %1ms — regressed back to "
                                            "the speechnorm-on-NaN hang")
                                .arg(kHardTimeoutMs)));
        QVERIFY2(pcm.has_value(), qPrintable(QStringLiteral("expected the scrubbed decode to succeed, got "
                                                              "error: %1")
                                                  .arg(error)));
        QVERIFY2(elapsedMs < 15000,
                 qPrintable(QStringLiteral("decode took %1ms — should be fast (pass 1 has no speechnorm so "
                                            "NaN can't wedge it, pass 2 runs on a tiny scrubbed buffer)")
                                .arg(elapsedMs)));

        int nonFinite = 0;
        for (float v : *pcm) {
            if (!std::isfinite(v))
                ++nonFinite;
        }
        QCOMPARE(nonFinite, 0);
    }

    // Spec: the scrub route must not introduce any
    // NEW clamping of legitimate finite headroom beyond what
    // "speechnorm=e=25:p=0.95:r=0.05" already, unconditionally, does today.
    //
    // IMPORTANT — measured while writing this test, and worth recording
    // here rather than silently working around: the originally-drafted test
    // spec asked for "a synthetic f32 file peaking at 2.5 must not be
    // flattened to 1.0" i.e. the FINAL decodeTo16kMono() output should
    // contain values above 1.0. That is unattainable by this decode
    // function (or ANY decode function) once speechnorm is the last stage
    // — ffmpeg's own speechnorm `p` peak-target option is hard-bounded to
    // [0, 1] (`ffmpeg -h filter=speechnorm`), and empirically (three direct
    // ffmpeg CLI runs with today's UNCHANGED, already-shipped filter
    // string, no scrub involved at all) a quiet-only signal, a sustained
    // 2.5-amplitude tone, and a 50ms 2.5-amplitude burst in an otherwise
    // quiet bed ALL converge to peak ≈0.95 — e=25 is a deliberately
    // aggressive expansion factor (see the decode function's comment) that
    // normalizes essentially anything toward the target within
    // milliseconds. So today's shipped decodeTo16kMono has never returned
    // samples above ~0.95, corrupt input or not; the "1.56M samples above
    // 1.0" the real corrupt wireless-mic file was measured to have must
    // describe the raw/undecoded source, not this function's output.
    //
    // What the decision's "must survive" constraint actually rules out, and
    // what this test verifies instead: the scrub (replace non-finite with
    // 0) must not degrade a file's legitimate loud content any further than
    // speechnorm's own unavoidable normalization already does on an
    // otherwise-identical CLEAN file. Two near-identical fixtures — one
    // with a NaN forced into the quiet bed (exercises the scrub), one
    // without (nothing to scrub) — must decode to matching peaks near the
    // 2.5 burst; a materially LOWER peak on the corrupt one would mean the
    // scrub (or the two-pass split itself) is adding attenuation beyond
    // speechnorm's own, which is what an aformat/alimiter prefix would do
    // and is exactly what was rejected.
    void legitimateAboveUnityPeaksSurvive()
    {
        REQUIRE_FFMPEG();

        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        constexpr quint32 sampleRate = 48000;
        constexpr int seconds = 3;
        const auto makeSamples = [] {
            std::vector<float> samples(sampleRate * seconds);
            for (std::size_t i = 0; i < samples.size(); ++i)
                samples[i] = 0.05f * std::sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / sampleRate);
            const std::size_t burstStart = samples.size() / 2;
            const std::size_t burstLen = sampleRate / 20; // 50ms
            for (std::size_t i = burstStart; i < burstStart + burstLen && i < samples.size(); ++i)
                samples[i] = 2.5f * std::sin(2.0 * M_PI * 1000.0 * static_cast<double>(i) / sampleRate);
            return samples;
        };

        std::vector<float> corruptSamples = makeSamples();
        std::vector<float> cleanSamples = makeSamples();
        // A NaN far from the burst, in the quiet bed — nothing to scrub in
        // the control fixture, so any difference between the two decoded
        // peaks is attributable to the scrub/two-pass split, not the burst
        // itself or ordinary run-to-run ffmpeg variance.
        corruptSamples[100] = std::numeric_limits<float>::quiet_NaN();

        const QString corruptPath = tempDir.filePath(QStringLiteral("corrupt_with_headroom.wav"));
        const QString cleanPath = tempDir.filePath(QStringLiteral("clean_with_headroom.wav"));
        QVERIFY(writeMonoFloatWav(corruptPath, sampleRate, corruptSamples));
        QVERIFY(writeMonoFloatWav(cleanPath, sampleRate, cleanSamples));

        QString corruptError;
        QString cleanError;
        const auto corruptPcm = AudioDecoder::decodeTo16kMono(corruptPath, &corruptError);
        const auto cleanPcm = AudioDecoder::decodeTo16kMono(cleanPath, &cleanError);
        QVERIFY2(corruptPcm.has_value(), qPrintable(corruptError));
        QVERIFY2(cleanPcm.has_value(), qPrintable(cleanError));

        const auto peakOf = [](const std::vector<float> &pcm) {
            float peak = 0.0f;
            for (float v : pcm)
                peak = std::max(peak, std::fabs(v));
            return peak;
        };
        const float corruptPeak = peakOf(*corruptPcm);
        const float cleanPeak = peakOf(*cleanPcm);

        QVERIFY2(std::isfinite(corruptPeak) && corruptPeak > 0.5f,
                 qPrintable(QStringLiteral("corrupt-file peak was %1 — the burst should still dominate "
                                            "after scrubbing one unrelated NaN sample")
                                .arg(corruptPeak)));
        QVERIFY2(std::abs(corruptPeak - cleanPeak) < 0.02f,
                 qPrintable(QStringLiteral("scrubbing a NaN elsewhere in the file changed the burst's peak "
                                            "from %1 (clean) to %2 (corrupt) — the scrub is attenuating "
                                            "legitimate headroom beyond what speechnorm already does on its "
                                            "own, which the chosen route must not do")
                                .arg(cleanPeak)
                                .arg(corruptPeak)));
    }
};

QTEST_GUILESS_MAIN(TstAudioDecoder)
#include "tst_audiodecoder.moc"
