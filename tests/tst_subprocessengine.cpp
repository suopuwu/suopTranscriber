// tst_subprocessengine — the NeMo-free keystone for the out-of-process engine
// path (DESIGN.md "Pluggable engines"). Drives tests/fixtures/fake_engine.py,
// which speaks the JSONL wire protocol with zero heavyweight deps, so this
// proves SubprocessEngine's parsing / callback dispatch / abort / graceful-
// degradation contract without any real ASR engine installed. It realizes the
// subprocess half of tst_engine_contract.cpp's living wire-protocol spec.

#include <QtTest>

#include <QElapsedTimer>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

#include "EngineManifest.h"
#include "SubprocessEngine.h"
#include "TranscriptionEngine.h"

class TstSubprocessEngine : public QObject
{
    Q_OBJECT

    QString m_python;
    QString m_fixture;
    QTemporaryDir m_scratch;

    // Builds a SubprocessEngine whose child is `python fake_engine.py --mode M`;
    // SubprocessEngine appends --audio/--offset/--lang after that.
    SubprocessEngine engineForMode(const QString &mode) const
    {
        EngineManifest manifest;
        manifest.name = QStringLiteral("fake");
        manifest.command = QStringList{m_python, m_fixture, QStringLiteral("--mode"), mode};
        return SubprocessEngine(manifest, QString(), m_scratch.path());
    }

    static AudioChunk oneSecondChunkAtOffset(double offset)
    {
        AudioChunk chunk;
        chunk.pcm16kMono.assign(16000, 0.0f);
        chunk.offsetSeconds = offset;
        return chunk;
    }

private slots:
    void initTestCase()
    {
        m_python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (m_python.isEmpty())
            m_python = QStandardPaths::findExecutable(QStringLiteral("python"));
        if (m_python.isEmpty())
            QSKIP("no python interpreter found — subprocess engine fixture cannot run");
        m_fixture = QStringLiteral(SUOP_REPO_DIR "/tests/fixtures/fake_engine.py");
        QVERIFY2(QFile::exists(m_fixture), qPrintable(m_fixture));
        QVERIFY(m_scratch.isValid());
    }

    // Spec: segments stream through onSegment as produced and match the return
    // value; progress is reported; and timestamps stay CHUNK-RELATIVE — the
    // engine must NOT add chunk.offsetSeconds (the app owns the merge). Same
    // contract tst_engine_contract asserts for the in-process path.
    void normalRun_streamsChunkRelativeSegments()
    {
        SubprocessEngine engine = engineForMode(QStringLiteral("normal"));

        std::vector<Segment> streamed;
        int progressCalls = 0;
        double lastTotal = 0.0;
        const std::vector<Segment> returned = engine.transcribe(
            oneSecondChunkAtOffset(60.0),
            [&](double, double total) {
                ++progressCalls;
                lastTotal = total;
            },
            [&](const Segment &s) { streamed.push_back(s); });

        QCOMPARE(returned.size(), std::size_t(2));
        QCOMPARE(streamed.size(), returned.size());
        for (std::size_t i = 0; i < returned.size(); ++i) {
            QCOMPARE(streamed[i].t0, returned[i].t0);
            QCOMPARE(streamed[i].text, returned[i].text);
        }
        // Chunk-relative: first segment starts at 0.0, NOT at the 60.0 offset.
        QCOMPARE(returned.front().t0, 0.0);
        QCOMPARE(returned.back().t1, 2.0);
        QCOMPARE(returned.front().text, std::string("hello world"));
        QVERIFY(progressCalls >= 1);
        QCOMPARE(lastTotal, 2.0);
    }

    // Spec: a recoverable error line is logged but does not stop the run — later
    // segments and the done still arrive.
    void recoverableError_continues()
    {
        SubprocessEngine engine = engineForMode(QStringLiteral("recoverable_then_done"));
        const std::vector<Segment> returned =
            engine.transcribe(oneSecondChunkAtOffset(0.0), {}, {});
        QCOMPARE(returned.size(), std::size_t(2));
    }

    // Spec: a non-recoverable error line degrades to "no segments" — never a
    // throw or crash (crash isolation is the point of out-of-process).
    void fatalError_degradesToEmpty()
    {
        SubprocessEngine engine = engineForMode(QStringLiteral("fatal_error"));
        std::vector<Segment> returned;
        try {
            returned = engine.transcribe(oneSecondChunkAtOffset(0.0), {}, {});
        } catch (...) {
            QFAIL("transcribe() must not throw on a fatal engine error");
        }
        QVERIFY(returned.empty());
    }

    // Spec: a child that exits non-zero mid-stream (no done line) still returns
    // whatever segments streamed before it died, with no throw.
    void nonzeroExit_returnsPartial()
    {
        SubprocessEngine engine = engineForMode(QStringLiteral("nonzero_exit"));
        const std::vector<Segment> returned =
            engine.transcribe(oneSecondChunkAtOffset(0.0), {}, {});
        QCOMPARE(returned.size(), std::size_t(1));
        QCOMPARE(returned.front().text, std::string("partial before crash"));
    }

    // Spec: a JSONL message split across two stdout writes is buffered and
    // parsed once complete, not dropped.
    void splitLine_isBuffered()
    {
        SubprocessEngine engine = engineForMode(QStringLiteral("split_lines"));
        const std::vector<Segment> returned =
            engine.transcribe(oneSecondChunkAtOffset(0.0), {}, {});
        QCOMPARE(returned.size(), std::size_t(1));
        QCOMPARE(returned.front().text, std::string("split line"));
    }

    // Spec: onAbort() returning true kills the child promptly and returns the
    // segments produced so far — the interface's "bail with partial results"
    // semantics. The fixture would otherwise stream forever.
    void abort_killsChildAndReturnsPartial()
    {
        SubprocessEngine engine = engineForMode(QStringLiteral("slow"));

        int seen = 0;
        QElapsedTimer timer;
        timer.start();
        const std::vector<Segment> returned = engine.transcribe(
            oneSecondChunkAtOffset(0.0), {}, [&](const Segment &) { ++seen; },
            [&] { return seen >= 1; });

        QVERIFY2(!returned.empty(), "should return the segment(s) seen before abort");
        QVERIFY2(timer.elapsed() < 5000, "abort must stop the child promptly, not hang");
    }
};

QTEST_GUILESS_MAIN(TstSubprocessEngine)
#include "tst_subprocessengine.moc"
