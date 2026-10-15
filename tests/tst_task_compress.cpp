// CompressAudioTask: a WAV or full-quality FLAC replaced by a verified FLAC
// voice encode — every way it must NOT lose the original, and the undo round
// trip (including the in-place FLAC → FLAC voice case).

#include <QDir>
#include <QFile>
#include <QLocale>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <memory>
#include <optional>
#include <vector>

#include "AudioDecoder.h"
#include "TaskQuery.h"
#include "tasks/CompressAudioTask.h"
#include "tasks/DetectSilenceTask.h"
#include "tasks/EngineCache.h"
#include "tasks/RevertCompressionTask.h"
#include "tasks/TaskContext.h"

namespace {

struct NullContextFixture {
    EngineCache engines;
    TaskControl control;
    TaskContext ctx{engines, control, [](bool) {}};
};

bool ffmpegAvailable()
{
    return !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty()
        && !QStandardPaths::findExecutable(QStringLiteral("ffprobe")).isEmpty();
}

// A 48 kHz stereo take in whatever container `name`'s extension implies —
// the recorder's capture shape (.wav) or a full-quality FLAC take (.flac).
QString makeTake(const QString &dir, const QString &name, double seconds = 3.0, int rate = 48000,
                 int channels = 2)
{
    const QString path = dir + QLatin1Char('/') + name;
    QProcess gen;
    gen.start(QStringLiteral("ffmpeg"),
              {QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-f"),
               QStringLiteral("lavfi"), QStringLiteral("-i"),
               QStringLiteral("anoisesrc=r=%1:a=0.1:d=%2").arg(rate).arg(seconds), QStringLiteral("-ac"),
               QString::number(channels), path});
    if (!gen.waitForFinished(15000) || gen.exitCode() != 0)
        return {};
    return path;
}

QStringList visibleAndHidden(const QString &dir)
{
    return QDir(dir).entryList(QDir::Files | QDir::Hidden, QDir::Name);
}

QString probeStream(const QString &path, const QString &entry)
{
    QProcess p;
    p.start(QStringLiteral("ffprobe"), {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
                                        QStringLiteral("a:0"), QStringLiteral("-show_entries"),
                                        QStringLiteral("stream=") + entry, QStringLiteral("-of"),
                                        QStringLiteral("csv=p=0"), path});
    p.waitForFinished(10000);
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

} // namespace

class TstTaskCompress : public QObject {
    Q_OBJECT

    // The original goes to the trash: redirect the home trash so the tests
    // never touch the developer's real one and it sits on the same filesystem
    // as the /tmp vaults (else moveToTrash() can't trash) — see the same
    // fixture in tst_vaulttreemodel.
    QTemporaryDir m_xdgDataHome;

    QString trashed(const QString &name) const { return m_xdgDataHome.filePath(QStringLiteral("Trash/files/") + name); }

    std::unique_ptr<CompressAudioTask> compress(const QString &vault, const QString &path,
                                                CompressAudioTask::Target target = CompressAudioTask::Target::Voice)
    {
        auto task = std::make_unique<CompressAudioTask>(QUrl::fromLocalFile(path), QUrl::fromLocalFile(vault), target);
        NullContextFixture fx;
        task->run(fx.ctx);
        return task;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_xdgDataHome.isValid());
        qputenv("XDG_DATA_HOME", m_xdgDataHome.path().toUtf8());
    }

    void identity()
    {
        CompressAudioTask t(QUrl::fromLocalFile(QStringLiteral("/v/take.wav")), QUrl(), CompressAudioTask::Target::Voice);
        QCOMPARE(t.kind(), QStringLiteral("compressAudio"));
        QCOMPARE(t.lane(), Task::Lane::Serial);
        QCOMPARE(t.presentation(), Task::Presentation::Background);
        QVERIFY(!t.isPersistable());
        QVERIFY(!t.isUndoable()); // nothing landed yet
        QCOMPARE(CompressAudioTask::destinationPath(QStringLiteral("/v/a b.wav")), QStringLiteral("/v/a b.flac"));
        QCOMPARE(CompressAudioTask::destinationPath(QStringLiteral("/v/a.flac")), QStringLiteral("/v/a.flac"));

        // Queued it must not dedup away a transcribe asked for after it; once
        // running it owns the file.
        QVERIFY(!t.occupiesFile());
        t.status = Task::Status::Active;
        QVERIFY(t.occupiesFile());
    }

    // Eligibility is by type, and for FLAC by its STREAMINFO: only a FLAC
    // that isn't already 16 kHz mono qualifies.
    void isCompressible_wavOrNonVoiceFlacOnly()
    {
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir dir;
        QVERIFY(CompressAudioTask::isCompressible(dir.filePath(QStringLiteral("missing.wav")), CompressAudioTask::Target::Voice));
        QVERIFY(!CompressAudioTask::isCompressible(dir.filePath(QStringLiteral("x.mp3")), CompressAudioTask::Target::Voice));
        QVERIFY(!CompressAudioTask::isCompressible(dir.filePath(QStringLiteral("x.opus")), CompressAudioTask::Target::Voice));
        QVERIFY(CompressAudioTask::isCompressible(makeTake(dir.path(), QStringLiteral("full.flac")), CompressAudioTask::Target::Voice));
        QVERIFY(CompressAudioTask::isCompressible(makeTake(dir.path(), QStringLiteral("stereo16k.flac"), 1, 16000, 2), CompressAudioTask::Target::Voice));
        QVERIFY(CompressAudioTask::isCompressible(makeTake(dir.path(), QStringLiteral("mono48k.flac"), 1, 48000, 1), CompressAudioTask::Target::Voice));
        QVERIFY(!CompressAudioTask::isCompressible(makeTake(dir.path(), QStringLiteral("voice.flac"), 1, 16000, 1), CompressAudioTask::Target::Voice));
    }

    // WAV → <stem>.flac at 16 kHz mono (everything AudioDecoder decodes to),
    // mtime kept, original in the trash, undo label carrying the saving.
    void wavBecomesFlacVoiceAndTheWavGoesToTrash()
    {
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir vault;
        const QString wav = makeTake(vault.path(), QStringLiteral("a take.wav"));
        QVERIFY(!wav.isEmpty());
        const QDateTime mtime = QFileInfo(wav).lastModified();
        const qint64 wavBytes = QFileInfo(wav).size();
        const std::optional<double> wavSeconds = AudioDecoder::probeDurationSeconds(wav);
        QVERIFY(wavSeconds);

        CompressAudioTask task(QUrl::fromLocalFile(wav), QUrl::fromLocalFile(vault.path()), CompressAudioTask::Target::Voice);
        QSignalSpy done(&task, &CompressAudioTask::audioCompressed);
        NullContextFixture fx;
        task.run(fx.ctx);

        const QString out = vault.filePath(QStringLiteral("a take.flac"));
        QCOMPARE(done.size(), 1);
        QCOMPARE(done.first().at(0).toUrl(), QUrl::fromLocalFile(wav));
        QCOMPARE(done.first().at(1).toUrl(), QUrl::fromLocalFile(out));
        QCOMPARE(visibleAndHidden(vault.path()), QStringList{QStringLiteral("a take.flac")});
        QVERIFY(QFileInfo::exists(trashed(QStringLiteral("a take.wav"))));
        QCOMPARE(probeStream(out, QStringLiteral("codec_name")), QStringLiteral("flac"));
        QCOMPARE(probeStream(out, QStringLiteral("sample_rate")), QStringLiteral("16000"));
        QCOMPARE(probeStream(out, QStringLiteral("channels")), QStringLiteral("1"));
        const std::optional<double> outSeconds = AudioDecoder::probeDurationSeconds(out);
        QVERIFY(outSeconds);
        QVERIFY(std::abs(*outSeconds - *wavSeconds) <= CompressAudioTask::kMaxDurationDriftSeconds);
        QCOMPARE(QFileInfo(out).lastModified(), mtime);
        QCOMPARE(task.savedBytes(), wavBytes - QFileInfo(out).size());
        QVERIFY(task.savedBytes() > 0);
        QVERIFY(task.isUndoable());
        QVERIFY(task.undoStillApplicable());
        QVERIFY2(task.undoLabel().startsWith(QStringLiteral("Compressed a take.flac — saved ")),
                 qPrintable(task.undoLabel()));
        QVERIFY(fx.ctx.warning().isEmpty());
    }

    void revertRestoresTheWavAndDropsTheCompressedCopy()
    {
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir vault;
        const QString wav = makeTake(vault.path(), QStringLiteral("undo me.wav"));
        QVERIFY(!wav.isEmpty());
        auto task = compress(vault.path(), wav);
        const QString flac = vault.filePath(QStringLiteral("undo me.flac"));

        std::unique_ptr<Task> undo = task->makeUndoTask();
        auto *revert = qobject_cast<RevertCompressionTask *>(undo.get());
        QVERIFY(revert);
        QCOMPARE(revert->lane(), Task::Lane::Instant);
        QVERIFY(!revert->isUndoable());
        QSignalSpy replaced(revert, &RevertCompressionTask::audioCompressed);
        NullContextFixture fx;
        revert->run(fx.ctx);
        QCOMPARE(replaced.size(), 1);
        QCOMPARE(replaced.first().at(0).toUrl(), QUrl::fromLocalFile(flac));
        QCOMPARE(replaced.first().at(1).toUrl(), QUrl::fromLocalFile(wav));
        QCOMPARE(visibleAndHidden(vault.path()), QStringList{QStringLiteral("undo me.wav")});
        QVERIFY(!QFileInfo::exists(trashed(QStringLiteral("undo me.wav"))));
        QVERIFY(!QFileInfo::exists(m_xdgDataHome.filePath(QStringLiteral("Trash/info/undo me.wav.trashinfo"))));
        QVERIFY(!task->undoStillApplicable());
    }

    // Full-quality FLAC → FLAC voice keeps its path; undo swaps the original
    // back into that same path.
    void fullQualityFlacIsReplacedInPlaceAndRevertable()
    {
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir vault;
        const QString flac = makeTake(vault.path(), QStringLiteral("full.flac"));
        QVERIFY(!flac.isEmpty());
        const qint64 fullBytes = QFileInfo(flac).size();

        auto task = compress(vault.path(), flac);
        QVERIFY(task->isUndoable());
        QVERIFY(task->undoStillApplicable());
        QCOMPARE(visibleAndHidden(vault.path()), QStringList{QStringLiteral("full.flac")});
        QCOMPARE(probeStream(flac, QStringLiteral("sample_rate")), QStringLiteral("16000"));
        QCOMPARE(probeStream(flac, QStringLiteral("channels")), QStringLiteral("1"));
        QVERIFY(QFileInfo::exists(trashed(QStringLiteral("full.flac"))));
        QVERIFY(!CompressAudioTask::isCompressible(flac, CompressAudioTask::Target::Voice));

        std::unique_ptr<Task> undo = task->makeUndoTask();
        QVERIFY(undo);
        QSignalSpy replaced(undo.get(), SIGNAL(audioCompressed(QUrl, QUrl)));
        NullContextFixture fx;
        undo->run(fx.ctx);
        QCOMPARE(replaced.size(), 1);
        QCOMPARE(replaced.first().at(0).toUrl(), replaced.first().at(1).toUrl());
        QCOMPARE(visibleAndHidden(vault.path()), QStringList{QStringLiteral("full.flac")});
        QCOMPARE(QFileInfo(flac).size(), fullBytes);
        QCOMPARE(probeStream(flac, QStringLiteral("sample_rate")), QStringLiteral("48000"));
    }

    // Lossless: a WAV becomes FLAC at its own rate and channels — nothing a
    // listener could hear is lost — and undoes like voice does.
    void losslessKeepsTheWavsRateAndChannels()
    {
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir vault;
        const QString wav = makeTake(vault.path(), QStringLiteral("hifi.wav"));
        QVERIFY(!wav.isEmpty());
        auto task = compress(vault.path(), wav, CompressAudioTask::Target::Lossless);
        const QString flac = vault.filePath(QStringLiteral("hifi.flac"));
        QCOMPARE(visibleAndHidden(vault.path()), QStringList{QStringLiteral("hifi.flac")});
        QCOMPARE(probeStream(flac, QStringLiteral("sample_rate")), QStringLiteral("48000"));
        QCOMPARE(probeStream(flac, QStringLiteral("channels")), QStringLiteral("2"));
        QVERIFY(task->savedBytes() > 0);
        QVERIFY(task->isUndoable());
        QVERIFY(task->displayName().contains(QStringLiteral("lossless FLAC")));

        std::unique_ptr<Task> undo = task->makeUndoTask();
        QVERIFY(undo);
        NullContextFixture fx;
        undo->run(fx.ctx);
        QCOMPARE(visibleAndHidden(vault.path()), QStringList{QStringLiteral("hifi.wav")});
    }

    // A FLAC is already lossless: never eligible, and a task that gets one
    // anyway leaves it untouched and says why.
    void losslessNeverTakesAFlac()
    {
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir vault;
        const QString flac = makeTake(vault.path(), QStringLiteral("already.flac"));
        QVERIFY(!flac.isEmpty());
        QVERIFY(!CompressAudioTask::isCompressible(flac, CompressAudioTask::Target::Lossless));
        QVERIFY(CompressAudioTask::isCompressible(vault.filePath(QStringLiteral("x.wav")),
                                                  CompressAudioTask::Target::Lossless));
        const qint64 bytes = QFileInfo(flac).size();
        CompressAudioTask task(QUrl::fromLocalFile(flac), QUrl::fromLocalFile(vault.path()),
                               CompressAudioTask::Target::Lossless);
        NullContextFixture fx;
        task.run(fx.ctx);
        QVERIFY(!fx.ctx.warning().isEmpty());
        QVERIFY(!task.isUndoable());
        QCOMPARE(QFileInfo(flac).size(), bytes);
    }

    // Already FLAC voice: Done, untouched, not undoable, and says why.
    void flacVoiceIsLeftAlone()
    {
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir vault;
        const QString flac = makeTake(vault.path(), QStringLiteral("voice.flac"), 1, 16000, 1);
        QVERIFY(!flac.isEmpty());
        const qint64 bytes = QFileInfo(flac).size();
        CompressAudioTask task(QUrl::fromLocalFile(flac), QUrl::fromLocalFile(vault.path()), CompressAudioTask::Target::Voice);
        QSignalSpy done(&task, &CompressAudioTask::audioCompressed);
        NullContextFixture fx;
        task.run(fx.ctx);
        QCOMPARE(done.size(), 0);
        QVERIFY(!fx.ctx.warning().isEmpty());
        QVERIFY(!task.isUndoable());
        QCOMPARE(QFileInfo(flac).size(), bytes);
    }

    // Trash emptied in the meantime: the compressed file is all that's left,
    // so the revert must fail without deleting it — in place or not.
    void revertKeepsTheCompressedFileWhenTheOriginalIsGone_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("compressedName");
        QTest::newRow("wav") << QStringLiteral("emptied.wav") << QStringLiteral("emptied.flac");
        QTest::newRow("in-place flac") << QStringLiteral("emptied2.flac") << QStringLiteral("emptied2.flac");
    }
    void revertKeepsTheCompressedFileWhenTheOriginalIsGone()
    {
        QFETCH(QString, name);
        QFETCH(QString, compressedName);
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir vault;
        const QString src = makeTake(vault.path(), name);
        QVERIFY(!src.isEmpty());
        auto task = compress(vault.path(), src);
        QVERIFY(QFile::remove(trashed(name)));
        QVERIFY(!task->undoStillApplicable());
        std::unique_ptr<Task> undo = task->makeUndoTask();
        QVERIFY(undo);
        NullContextFixture fx;
        QVERIFY_THROWS_EXCEPTION(TaskError, undo->run(fx.ctx));
        QCOMPARE(visibleAndHidden(vault.path()), QStringList{compressedName});
    }

    // One snackbar for a multi-file compress, carrying the summed saving.
    void batchUndoLabelSumsTheSaving()
    {
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir vault;
        auto a = compress(vault.path(), makeTake(vault.path(), QStringLiteral("batch a.wav")));
        auto b = compress(vault.path(), makeTake(vault.path(), QStringLiteral("batch b.flac")));
        QCOMPARE(a->batchUndoLabel({a.get()}), a->undoLabel());
        const QString total =
            QLocale().formattedDataSize(a->savedBytes() + b->savedBytes(), 1, QLocale::DataSizeSIFormat);
        QCOMPARE(b->batchUndoLabel({a.get(), b.get()}), QStringLiteral("Compressed 2 files — saved ") + total);
    }

    void undecodableWavFailsAndKeepsIt()
    {
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir dir;
        const QString wav = dir.filePath(QStringLiteral("junk.wav"));
        writeFile(wav, QByteArray(4096, 'x'));
        CompressAudioTask task(QUrl::fromLocalFile(wav), QUrl::fromLocalFile(dir.path()), CompressAudioTask::Target::Voice);
        QSignalSpy done(&task, &CompressAudioTask::audioCompressed);
        NullContextFixture fx;
        QVERIFY_THROWS_EXCEPTION(TaskError, task.run(fx.ctx));
        QCOMPARE(done.size(), 0);
        QCOMPARE(visibleAndHidden(dir.path()), QStringList{QStringLiteral("junk.wav")});
    }

    void cancelRemovesThePartialAndKeepsTheOriginal()
    {
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir dir;
        const QString wav = makeTake(dir.path(), QStringLiteral("take.wav"));
        QVERIFY(!wav.isEmpty());
        CompressAudioTask task(QUrl::fromLocalFile(wav), QUrl::fromLocalFile(dir.path()), CompressAudioTask::Target::Voice);
        NullContextFixture fx;
        fx.control.requestCancel();
        QString error;
        try {
            task.run(fx.ctx);
        } catch (const TaskError &e) {
            error = e.message();
        }
        QCOMPARE(error, QStringLiteral("Cancelled"));
        QCOMPARE(visibleAndHidden(dir.path()), QStringList{QStringLiteral("take.wav")});
    }

    void refusesToOverwriteAnExistingDestination()
    {
        if (!ffmpegAvailable())
            QSKIP("ffmpeg/ffprobe not available");
        QTemporaryDir dir;
        const QString wav = makeTake(dir.path(), QStringLiteral("take.wav"));
        QVERIFY(!wav.isEmpty());
        const QString existing = dir.filePath(QStringLiteral("take.flac"));
        writeFile(existing, "someone else's recording");
        CompressAudioTask task(QUrl::fromLocalFile(wav), QUrl::fromLocalFile(dir.path()), CompressAudioTask::Target::Voice);
        NullContextFixture fx;
        QVERIFY_THROWS_EXCEPTION(TaskError, task.run(fx.ctx));
        QFile f(existing);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("someone else's recording"));
        QCOMPARE(visibleAndHidden(dir.path()), (QStringList{QStringLiteral("take.flac"), QStringLiteral("take.wav")}));
    }

    // A contended task (another task names the file) finishes Done without
    // touching anything and says why.
    void contendedTaskKeepsTheOriginalAndWarns()
    {
        QTemporaryDir dir;
        const QString wav = dir.filePath(QStringLiteral("take.wav"));
        writeFile(wav, "RIFF");
        CompressAudioTask task(QUrl::fromLocalFile(wav), QUrl(), CompressAudioTask::Target::Voice);
        task.setContended(true);
        QSignalSpy done(&task, &CompressAudioTask::audioCompressed);
        NullContextFixture fx;
        task.run(fx.ctx);
        QCOMPARE(done.size(), 0);
        QVERIFY(!fx.ctx.warning().isEmpty());
        QCOMPARE(visibleAndHidden(dir.path()), QStringList{QStringLiteral("take.wav")});
    }

    // What the controller asks at dispatch to decide contention.
    void otherLiveTaskTouches_seesOnlyLiveOtherTasksOnTheSameRecording()
    {
        const QUrl wav = QUrl::fromLocalFile(QStringLiteral("/v/take.wav"));
        TaskQuery::TaskList tasks;
        tasks.push_back(std::make_unique<CompressAudioTask>(wav, QUrl(), CompressAudioTask::Target::Voice));
        const Task *self = tasks.front().get();
        QVERIFY(!TaskQuery::otherLiveTaskTouches(tasks, self));

        tasks.push_back(std::make_unique<DetectSilenceTask>(QUrl::fromLocalFile(QStringLiteral("/v/other.wav")), QUrl()));
        QVERIFY(!TaskQuery::otherLiveTaskTouches(tasks, self));

        // The sibling .md names the same recording.
        tasks.push_back(std::make_unique<DetectSilenceTask>(QUrl::fromLocalFile(QStringLiteral("/v/take.md")), QUrl()));
        QVERIFY(TaskQuery::otherLiveTaskTouches(tasks, self));
        tasks.back()->status = Task::Status::Done;
        QVERIFY(!TaskQuery::otherLiveTaskTouches(tasks, self));
        tasks.back()->status = Task::Status::Paused;
        QVERIFY(TaskQuery::otherLiveTaskTouches(tasks, self));
    }
};

QTEST_GUILESS_MAIN(TstTaskCompress)
#include "tst_task_compress.moc"
