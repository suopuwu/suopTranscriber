#include "TranscriptionController.h"

#include "MdFrontmatter.h"
#include "TranscriptFormat.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QTemporaryFile>
#include <QTextStream>
#include <QTimer>
#include <QPair>
#include <QVector>

#include <csignal>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>

#include "AudioDecoder.h"
#include "CombineNaming.h"
#include "CombineScanner.h"
#include "JobListFormatter.h"
#include "DiarizationEngine.h"
#include "FileRename.h"
#include "LabelingEngine.h"
#include "ModelManager.h"
#include "Segment.h"
#include "EngineRegistry.h"
#include "GpuProbe.h"
#include "ParakeetEngine.h"
#include "SubprocessEngine.h"
#include "TaskQuery.h"
#include "UsbGrabJob.h"
#include "VadChunker.h"
#include "VaultTreeModel.h" // setVaultTreeModel + applyRemovalOutcome/applyRestoreOutcome calls
#include "WhisperCppEngine.h"
#include "tasks/BenchmarkTask.h"
#include "tasks/ChaptersTask.h"
#include "tasks/CombineTask.h"
#include "tasks/DeleteTask.h"
#include "tasks/DetectSilenceTask.h"
#include "tasks/DiarizationSweepTask.h"
#include "tasks/DiarizeTask.h"
#include "tasks/DownloadTask.h"
#include "tasks/FolderMoveTask.h"
#include "tasks/FolderRenameTask.h"
#include "tasks/ImportTask.h"
#include "tasks/MergeSpeakersTask.h"
#include "tasks/ModelIoTask.h"
#include "tasks/MoveTask.h"
#include "tasks/RelabelTask.h"
#include "tasks/RenameFileTask.h"
#include "tasks/RenameSpeakerTask.h"
#include "tasks/RestoreTask.h"
#include "tasks/Task.h"
#include "tasks/TrashTask.h"
#include "tasks/TaskFactory.h"
#include "tasks/TaskRunner.h"
#include "tasks/TranscribeTask.h"
#include "tasks/UsbGrabTask.h"


// Minimum spacing between TranscriptionWorker::segmentWritten() emissions
// (see run()'s onSegment) — a chatty stretch of audio can produce several
// whisper segments within milliseconds of each other, and each live-update
// tick costs QML a full file re-read/re-parse/re-render (see
// TranscriptionController::liveUpdateTick's doc comment), so bursts are
// coalesced down to this rate rather than firing one tick per segment.
constexpr qint64 kLiveUpdateThrottleMs = 200;


// DiarizationEngine renumbers cluster ids 0-based in first-appearance order
// (see its own doc comment) — the number of distinct speakers is simply one
// past the highest id seen, with no turns at all meaning zero speakers.
int distinctSpeakerCount(const std::vector<DiarizedTurn> &turns)
{
    int maxSpeaker = -1;
    for (const DiarizedTurn &turn : turns)
        maxSpeaker = std::max(maxSpeaker, turn.speaker);
    return maxSpeaker + 1;
}


QString taskStatusToString(Task::Status status)
{
    switch (status) {
    case Task::Status::Queued:
        return QStringLiteral("queued");
    case Task::Status::Active:
        return QStringLiteral("active");
    case Task::Status::Paused:
        return QStringLiteral("paused");
    case Task::Status::Done:
        return QStringLiteral("done");
    case Task::Status::Failed:
        return QStringLiteral("failed");
    }
    return QString();
}

// Item P6: priority order rebuildJobStatusIndex() uses to pick ONE status
// when multiple m_tasks entries share a path (e.g. a Done history entry plus
// a freshly re-queued run) — "in progress" beats "finished", and among
// finished entries Failed beats Done since a failure is the more actionable
// one for the file tree to surface.
int taskStatusPriority(Task::Status status)
{
    switch (status) {
    case Task::Status::Active:
        return 5;
    case Task::Status::Paused:
        return 4;
    case Task::Status::Queued:
        return 3;
    case Task::Status::Failed:
        return 2;
    case Task::Status::Done:
        return 1;
    }
    return 0;
}


// Item 14 "auto-categorize" mode: the existing tag vocabulary for a vault.
// Reads every .md's `tags:` frontmatter line directly rather than going
// through readMetadata()'s full parse, since that's all that's needed here.
// Returns a de-duplicated, sorted list of individual tags.
//
// `cache` (optional, per-file, keyed by absolute path) skips the open+parse
// for any file whose mtime+size is unchanged since the last call; the
// directory walk and its one stat per file still happen every time, which is
// what makes the mtime check possible in the first place. Pass nullptr for a
// guaranteed-fresh scan.
//
// The labeling job passes nullptr: it runs once per job, on the worker
// thread, and must not touch the GUI thread's cache. existingVaultTags()
// passes its cache, because it is called from a QML binding on every
// keystroke — see its doc comment in the header.
QStringList collectVaultFrontmatterList(const QString &vaultLocalPath, const QString &key,
                                        QHash<QString, TranscriptionController::CachedFileValues> *cache)
{
    QSet<QString> values;
    QDirIterator it(vaultLocalPath, {QStringLiteral("*.md")}, QDir::Files,
                     QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString mdPath = it.next();

        if (cache) {
            const QFileInfo info(mdPath);
            const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
            const qint64 size = info.size();
            const auto cached = cache->constFind(mdPath);
            if (cached != cache->constEnd() && cached->mtimeMs == mtimeMs && cached->size == size) {
                for (const QString &value : cached->values)
                    values.insert(value);
                continue;
            }
        }

        QFile mdFile(mdPath);
        if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QTextStream in(&mdFile);
        if (in.readLine() != QStringLiteral("---"))
            continue;
        // This file's own values for `key`, collected separately from the
        // vault-wide set so they can be cached per file.
        QStringList fileValues;
        QString line;
        while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
            const int colon = line.indexOf(':');
            if (colon < 0 || line.left(colon).trimmed() != key)
                continue;
            const QString rhs = line.mid(colon + 1).trimmed();
            for (const QString &part : rhs.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QString trimmed = part.trimmed();
                if (!trimmed.isEmpty())
                    fileValues.append(trimmed);
            }
        }
        for (const QString &value : fileValues)
            values.insert(value);
        if (cache) {
            // Re-stat rather than reuse the values read above: the file could
            // have been rewritten between that stat and the read just done,
            // which would otherwise cache the OLD mtime against NEW content
            // and never re-read it.
            const QFileInfo info(mdPath);
            cache->insert(mdPath, TranscriptionController::CachedFileValues{
                                      info.lastModified().toMSecsSinceEpoch(), info.size(), fileValues});
        }
    }
    QStringList sorted = values.values();
    sorted.sort(Qt::CaseInsensitive);
    return sorted;
}


// sanitizeForFilename(), matchRuleCapturedKey() and combineOutputPathFor() now
// live in CombineNaming.h — the ONE source of truth shared with the USB
// grabber's already-combined skip (see that header's top comment).

// Cost of one segment's "INDEX: text\n" formatting, matching
// LabelingEngine::generateChapters()'s own formatting exactly (digit count of
// the window-relative index, ": ", the text, '\n') so a window boundary lands
// where that single call's truncation would have.
static qsizetype chapterSegmentFormattedChars(int windowRelativeIndex, const QString &text)
{
    return QString::number(windowRelativeIndex).size() + 2 + text.size() + 1;
}

// See this function's doc comment in TranscriptionController.h.
QList<int> computeChapterWindows(const QStringList &segmentTexts)
{
    QList<int> windowStarts; // segment index each window begins at
    {
        int windowStart = 0;
        qsizetype windowChars = 0;
        for (int i = 0; i < segmentTexts.size(); ++i) {
            const qsizetype segChars = chapterSegmentFormattedChars(i - windowStart, segmentTexts.at(i));
            if (i > windowStart && windowChars + segChars > LabelingEngine::kMaxChapterInputChars) {
                windowStarts.append(windowStart);
                windowStart = i;
                windowChars = 0;
            }
            windowChars += segChars;
        }
        windowStarts.append(windowStart);
    }
    // Don't waste a whole extra LLM call on a pathologically small trailing
    // window (e.g. one segment left over) — fold it into the previous window
    // IF the combined window still fits kMaxChapterInputChars. Item 28 bug:
    // the old code folded unconditionally, producing a merged window that
    // exceeded the budget, which LabelingEngine::generateChapters() then
    // silently truncated back down — dropping exactly the folded-in tail, so
    // the final stretch of a long transcript got zero chapters. Now the fold
    // only happens when it's safe; otherwise the small trailing window stays
    // its own window and its segments still get proposed.
    if (windowStarts.size() > 1) {
        const qsizetype lastWindowSegCount = segmentTexts.size() - windowStarts.last();
        const qsizetype avgWindowSegCount = segmentTexts.size() / windowStarts.size();
        if (lastWindowSegCount < avgWindowSegCount / 4) {
            const int mergedStart = windowStarts.at(windowStarts.size() - 2);
            qsizetype mergedChars = 0;
            for (int i = mergedStart; i < segmentTexts.size(); ++i)
                mergedChars += chapterSegmentFormattedChars(i - mergedStart, segmentTexts.at(i));
            if (mergedChars <= LabelingEngine::kMaxChapterInputChars)
                windowStarts.removeLast();
        }
    }
    return windowStarts;
}

// --- Out-of-process engine discovery (DESIGN.md "Pluggable engines") -------
//
// A transcription model id is either a whisper ggml id ("base.en",
// "large-v3", resolved by ModelManager) or an out-of-process engine, written
// with the literal "engine:" prefix ("engine:parakeet"). These helpers turn
// the latter into a runnable SubprocessEngine, and back the availableEngines()
// capability list. All are free functions (internal linkage) so both the
// worker thread and the controller's availableEngines() can call them — they
// only do filesystem/process probing, no shared mutable state.

constexpr auto kEngineIdPrefix = "engine:";

bool isEngineModelId(const QString &modelId)
{
    return modelId.startsWith(QLatin1String(kEngineIdPrefix));
}

QString engineIdFromModelId(const QString &modelId)
{
    return isEngineModelId(modelId) ? modelId.mid(int(qstrlen(kEngineIdPrefix))) : QString();
}

// The read-only bundled engines dir shipped beside the app. Checked in a few
// install-layout-tolerant spots, with an env override for dev/tests. Empty if
// none exist (then only vault-local engines are discovered).
QString bundledEnginesDir()
{
    const QString env = qEnvironmentVariable("SUOP_ENGINES_DIR");
    if (!env.isEmpty() && QDir(env).exists())
        return QDir(env).absolutePath();
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{appDir + QStringLiteral("/engines"), appDir + QStringLiteral("/../engines"),
                                 appDir + QStringLiteral("/../share/suopTranscriber/engines")};
    for (const QString &candidate : candidates) {
        if (QDir(candidate).exists())
            return QDir(candidate).absolutePath();
    }
    return QString();
}

// Engine root search path: bundled first, then the vault's own
// .transcriber/engines (which overrides bundled by id, per EngineRegistry).
QStringList engineRootDirs(const QString &vaultPath)
{
    QStringList roots;
    const QString bundled = bundledEnginesDir();
    if (!bundled.isEmpty())
        roots << bundled;
    if (!vaultPath.isEmpty())
        roots << QDir(vaultPath).filePath(QStringLiteral(".transcriber/engines"));
    return roots;
}

// "Are this engine's runtime deps installed?" — runs its command with an extra
// --selftest arg (the run.py exits 0 iff its imports resolve, WITHOUT loading a
// model). Best-effort: a missing interpreter / an engine that doesn't support
// --selftest reads as "deps absent", i.e. grayed out with a reason.
bool engineDepsPresent(const EngineDescriptor &desc)
{
    if (desc.manifest.command.isEmpty())
        return false;
    QProcess probe;
    probe.setWorkingDirectory(desc.dir);
    QStringList args = desc.manifest.command.mid(1);
    args << QStringLiteral("--selftest");
    probe.start(desc.manifest.command.first(), args);
    if (!probe.waitForStarted(3000))
        return false;
    if (!probe.waitForFinished(20000)) {
        probe.kill();
        probe.waitForFinished(500);
        return false;
    }
    return probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
}

// Discovers every engine visible to `vaultPath`, capability-checked against
// this machine. GPU is probed once per process (nvidia-smi/vulkaninfo are
// slow-ish and static). Model presence is delegated to ModelManager.
std::vector<EngineDescriptor> discoverEngines(const QString &vaultPath)
{
    static const GpuStatus gpu = GpuProbe::probe();
    return EngineRegistry::discover(
        engineRootDirs(vaultPath), gpu, [](const EngineDescriptor &d) { return engineDepsPresent(d); },
        [](const EngineDescriptor &d) { return ModelManager::isEngineModelPresent(d.id); });
}

// Finds one engine by id for the transcribe path. Returns nullopt (with *error
// set) if it doesn't exist; a found-but-unavailable engine is returned so the
// caller can surface .unavailableReason.
std::optional<EngineDescriptor> resolveEngine(const QString &engineId, const QString &vaultPath, QString *error)
{
    for (const EngineDescriptor &desc : discoverEngines(vaultPath)) {
        if (desc.id == engineId) {
            if (!desc.available && error)
                *error = desc.unavailableReason;
            return desc;
        }
    }
    if (error)
        *error = QStringLiteral("engine \"%1\" not found").arg(engineId);
    return std::nullopt;
}

// Wave N (task system): TranscriptionWorker is GONE (1601 lines). It was one
// QObject with 12 kind-specific slots and ~22 unioned signals, owning the warm
// engines and every kind's doing-code in one body. All of it now lives in
// src/core/tasks/: one Task subclass per kind (its own run() + its own typed
// result signals), EngineCache (the warm ASR/label/diarization engines +
// GPU-hygiene eviction), TaskControl (the pause/cancel atomics),
// TaskContext::checkpoint() (the one cooperative interruption point every
// pausable kind funnels through), and TaskRunner (the thread host that turns
// run()-returned-or-threw into exactly one taskFinished). See .task/plan.md.


TranscriptionController::TranscriptionController(QObject *parent)
    : QObject(parent)
{
    // First, before anything else: the inline progress()/overallProgress()
    // getters dereference this, and its constructor is what loads the
    // persisted rolling averages (formerly loadPhaseTimings() right here).
    m_phaseTimer = new PhaseTimer(this);
    // THE progress signal path. PhaseTimer emits whenever it moves the blended
    // bar (recomputeOverallProgress()) or has it assigned outright
    // (setOverallProgress()); re-emitting it as this class's own signal is what
    // keeps the overallProgress Q_PROPERTY's NOTIFY firing for QML exactly as
    // it did when the value lived here.
    connect(m_phaseTimer, &PhaseTimer::overallProgressChanged, this,
            &TranscriptionController::overallProgressChanged);
    // See recomputeOverallProgress()'s doc comment — the only way
    // "loading"/"decoding" (no real progress callback) ever advance
    // overallProgress mid-phase. Only ever running while m_phase is one of
    // those two — started/stopped from the phase() handler in connectTask().
    m_indeterminatePhaseTimer = new QTimer(this);
    m_indeterminatePhaseTimer->setInterval(200);
    // A no-callback phase's fraction is synthesized purely from elapsed time, so
    // both the displayed per-phase number (progress, via
    // phaseRelativeSyntheticProgress()) and its blended slice of the overall bar
    // advance on this tick alone — re-notify both so QML repaints.
    connect(m_indeterminatePhaseTimer, &QTimer::timeout, this, [this] {
        emit progressChanged();
        recomputeOverallProgress();
    });

    m_runner = new TaskRunner;
    m_runner->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_runner, &QObject::deleteLater);
    // The ONE terminal-report path from the runner. A task cancelled before
    // it ever ran never reaches the runner at all — the queue synthesizes its
    // report directly (see cancelTaskAt()), so BOTH routes converge here and
    // every enqueued task reports exactly once. onTaskFinished() routes by the
    // task's lane, so the instant runner shares this same handler.
    connect(m_runner, &TaskRunner::taskFinished, this, &TranscriptionController::onTaskFinished);

    // The instant lane (see Task::Lane / pumpQueues()): a second worker thread
    // for fast Lane::Instant tasks so a rename never waits behind a serial
    // transcription.
    m_instantRunner = new TaskRunner;
    m_instantRunner->moveToThread(&m_instantThread);
    connect(&m_instantThread, &QThread::finished, m_instantRunner, &QObject::deleteLater);
    connect(m_instantRunner, &TaskRunner::taskFinished, this, &TranscriptionController::onTaskFinished);

    connect(this, &TranscriptionController::queueChanged, this, &TranscriptionController::persistQueue);
    connect(this, &TranscriptionController::queueChanged, this, &TranscriptionController::rebuildJobStatusIndex);
    m_thread.start();
    m_instantThread.start();
}

// Wires ONE task's signals to this controller. The generic ones (progress/
// phase/paused) are identical for every kind and are connected here; the
// kind-specific ones are the small localized switch at the bottom — the
// deliberate trade for typed per-subclass result signals instead of the old
// worker's ~22 signals unioned onto one object (see .task/plan.md's
// "Results" decision).
//
namespace {
// The scope strings QML passes to trash()/deletePermanently() <-> VaultFileScope.
// One place so the two invokables (and trashResult's echo back in connectTask())
// can't drift. Defined here, above the first use (connectTask below).
VaultFileScope scopeFromString(const QString &scope)
{
    if (scope == QStringLiteral("transcript"))
        return VaultFileScope::Transcript;
    if (scope == QStringLiteral("audioOnly"))
        return VaultFileScope::AudioOnly;
    if (scope == QStringLiteral("folder"))
        return VaultFileScope::Folder;
    return VaultFileScope::Files;
}

QString scopeToString(VaultFileScope scope)
{
    switch (scope) {
    case VaultFileScope::Transcript:
        return QStringLiteral("transcript");
    case VaultFileScope::AudioOnly:
        return QStringLiteral("audioOnly");
    case VaultFileScope::Folder:
        return QStringLiteral("folder");
    case VaultFileScope::Files:
        break;
    }
    return QStringLiteral("files");
}

QList<QUrl> urlsFromVariant(const QVariantList &urls)
{
    QList<QUrl> out;
    out.reserve(urls.size());
    for (const QVariant &v : urls) {
        const QUrl url = v.toUrl();
        if (!url.isEmpty())
            out.append(url);
    }
    return out;
}
} // namespace

// Every connection is scoped to `task` as the sender AND `this` as context,
// so they die with either object; a task is connected exactly once, right
// before its first dispatch (see startTask()).
void TranscriptionController::connectTask(Task *task)
{
    // Serial-lane UI wiring only: progress/phase/paused all mutate the
    // serial-lane display state (m_progress/m_phase/m_activePaused, the bottom
    // bar + overlay). An Instant task runs in parallel and must NOT clobber
    // what the serial task owns, so it skips these entirely — its result
    // signals below still connect. (Instant kinds don't emit progress/phase
    // anyway, being single-shot file rewrites; this makes that structural.)
    if (task->lane() == Task::Lane::Serial) {
    connect(task, &Task::progress, this, [this, task](double fraction) {
        m_progress = fraction;
        emit progressChanged();
        // The task declares its own progress shape (Task::ProgressModel), so
        // this no longer keeps a hand-maintained phase-name list here (which
        // had drifted out of sync with recomputeOverallProgress()'s copy). A
        // SelfCumulative kind (download/combine/chapters/import/grab/benchmark)
        // emits an already-cumulative fraction — assign it straight to the bar;
        // a WeightedPhases kind (transcribe/diarize) blends its phases.
        if (task->progressModel() == Task::ProgressModel::SelfCumulative) {
            m_phaseTimer->setOverallProgress(fraction);
        } else {
            recomputeOverallProgress();
        }
    });
    connect(task, &Task::phase, this, [this](const QString &phase) {
        m_phase = phase;
        m_phaseStartMs = activeElapsedMs();
        emit currentPhaseChanged();
        // Reset the displayed per-phase number the instant the phase changes —
        // progress() is now phase-relative (phaseRelativeSyntheticProgress()),
        // so entering a fresh phase must re-notify or the label lingers on the
        // previous phase's last value until the next tick.
        emit progressChanged();
        // The no-callback phases (isSyntheticProgressPhase) have no progress
        // signal of their own, so this timer is the only thing that advances
        // BOTH progress() and overallProgress() mid-phase; every other phase
        // gets its ticks from the real Task::progress signal instead.
        if (PhaseTimer::isSyntheticProgressPhase(phase))
            m_indeterminatePhaseTimer->start();
        else
            m_indeterminatePhaseTimer->stop();
        recomputeOverallProgress();
    });
    connect(task, &Task::paused, this, [this](bool isPaused) {
        m_activePaused = isPaused;
        if (isPaused)
            m_activeAccumulatedMs += m_activeSegmentTimer.elapsed();
        else
            m_activeSegmentTimer.restart();
        emit activePausedChanged();
    });
    } // end serial-lane UI wiring

    // --- Kind-specific result signals (both lanes) ---
    if (auto *transcribe = qobject_cast<TranscribeTask *>(task)) {
        connect(transcribe, &TranscribeTask::phaseDurationMs, this, &TranscriptionController::recordPhaseDuration);
        connect(transcribe, &TranscribeTask::segmentWritten, this, [this] {
            m_liveUpdateTick++;
            emit liveUpdateTickChanged();
        });
        connect(transcribe, &TranscribeTask::backendNameChanged, this, [this](const QString &name) {
            if (m_activeBackendName != name) {
                m_activeBackendName = name;
                emit activeBackendNameChanged();
            }
        });
    } else if (auto *rename = qobject_cast<RenameSpeakerTask *>(task)) {
        connect(rename, &RenameSpeakerTask::speakerRenamed, this,
                [this](const QString &path, const QString &oldName, bool ok, const QString &error) {
                    emit speakerRenamed(QUrl::fromLocalFile(path), oldName, ok, error);
                });
    } else if (auto *merge = qobject_cast<MergeSpeakersTask *>(task)) {
        connect(merge, &MergeSpeakersTask::speakersMerged, this,
                [this](const QString &path, const QString &sourceName, const QString &targetName, bool ok,
                       const QString &error) {
                    emit speakersMerged(QUrl::fromLocalFile(path), sourceName, targetName, ok, error);
                });
    } else if (auto *renameFileTask = qobject_cast<RenameFileTask *>(task)) {
        connect(renameFileTask, &RenameFileTask::fileRenamed, this,
                [this](const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error) {
                    // Update the tree IN PLACE before the watcher fires, so the
                    // rescan finds sameStructure() true and never resets the
                    // model (which would flash the whole tree — see
                    // VaultTreeModel::applyRenameOutcome). GUI thread: this
                    // queued slot runs after run() returned.
                    if (ok && m_vaultModel)
                        m_vaultModel->applyRenameOutcome(oldUrl, newUrl);
                    // Release the optimistic override: on success the tree/panels
                    // now show the real new name (applyRenameOutcome above +
                    // fileRenamed below repoint them); on failure they revert to
                    // the unchanged on-disk name. Erase by the OLD path (the key).
                    if (m_pendingRenames.remove(oldUrl.toLocalFile()) > 0) {
                        ++m_pendingRenamesRevision;
                        emit pendingRenamesRevisionChanged();
                    }
                    emit fileRenamed(oldUrl, newUrl, ok, error);
                });
    } else if (auto *moveFileTask = qobject_cast<MoveTask *>(task)) {
        connect(moveFileTask, &MoveTask::fileMoved, this, &TranscriptionController::fileMoved);
    } else if (auto *folderRename = qobject_cast<FolderRenameTask *>(task)) {
        connect(folderRename, &FolderRenameTask::folderRenamed, this, &TranscriptionController::folderRenamed);
    } else if (auto *folderMove = qobject_cast<FolderMoveTask *>(task)) {
        connect(folderMove, &FolderMoveTask::folderMoved, this, &TranscriptionController::folderMoved);
    } else if (auto *trashTask = qobject_cast<TrashTask *>(task)) {
        // On completion (GUI thread — the queued slot runs after run() returned,
        // so the members are stable): apply the model outcome (rebuild + the
        // per-file delete signals MainPanel reacts to; the model owns the
        // orphaned-vs-audio classification), then surface any untrashable entries
        // for the confirm-then-permanent-delete flow. The undo snackbar for what
        // DID trash is the generic undoAvailable (TrashTask is undoable).
        connect(trashTask, &TrashTask::trashDone, this, [this, trashTask] {
            if (m_vaultModel)
                m_vaultModel->applyRemovalOutcome(trashTask->scope(), trashTask->result().trashed);
            emit trashResult(scopeToString(trashTask->scope()), trashTask->result().untrashable);
        });
    } else if (auto *deleteTask = qobject_cast<DeleteTask *>(task)) {
        connect(deleteTask, &DeleteTask::deleteDone, this, [this, deleteTask] {
            if (m_vaultModel)
                m_vaultModel->applyRemovalOutcome(deleteTask->scope(), deleteTask->removed());
        });
    } else if (auto *restoreTask = qobject_cast<RestoreTask *>(task)) {
        connect(restoreTask, &RestoreTask::restoreDone, this, [this, restoreTask] {
            if (m_vaultModel)
                m_vaultModel->applyRestoreOutcome(restoreTask->restored());
        });
    } else if (auto *modelIo = qobject_cast<ModelIoTask *>(task)) {
        connect(modelIo, &ModelIoTask::modelIoFinished, this,
                [this](const QString &op, const QString &modelId, bool ok, const QString &error) {
                    // Just the outcome report — the models-list refresh
                    // (modelsChanged) rides the central completionChangesModelSet()
                    // emit in onTaskFinished(), not this per-kind lambda.
                    emit modelOperationFinished(op, modelId, ok, error);
                });
    } else if (auto *grab = qobject_cast<UsbGrabTask *>(task)) {
        // The task id rides along from HERE rather than being carried by the
        // task itself, keeping the task ignorant of the queue — same split the
        // old worker kept. Captured by value: `grab` outlives this lambda
        // (the queue owns it), and the id is stable for its whole lifetime.
        const int id = grab->id;
        connect(grab, &UsbGrabTask::grabCompleted, this,
                [this, id](int filesCopied, int filesSkipped, const QStringList &errors, bool cancelled) {
                    emit usbGrabFinished(id, filesCopied, filesSkipped, errors, cancelled);
                });
        // The pre-count pass reports its file count mid-run; phase()/progress()
        // don't emit queueChanged, so the Tasks row would never re-read
        // toVariantMap's fileCount without this.
        connect(grab, &UsbGrabTask::fileCountKnown, this, [this] { emit queueChanged(); });
    } else if (auto *combine = qobject_cast<CombineTask *>(task)) {
        // Durable combine log: a successful merge writes one entry to the vault's
        // combineHistory.json, so an auto-combine that moved the user's originals
        // into .combined/ (or deleted them) leaves an account of itself that
        // outlives the in-session Tasks row and undo. Appended from HERE (GUI
        // thread) rather than in run(), which is on a worker and must not touch
        // the GUI-owned VaultTreeModel — same split as the USB grab history.
        connect(combine, &CombineTask::combineCompleted, this,
                [this](const QString &outputPath, const QStringList &sourcePaths, bool deleteOriginals) {
                    if (!m_vaultModel)
                        return;
                    QVariantMap entry;
                    entry[QStringLiteral("timestamp")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
                    entry[QStringLiteral("outputPath")] = outputPath;
                    entry[QStringLiteral("sourcePaths")] = sourcePaths;
                    entry[QStringLiteral("partCount")] = sourcePaths.size();
                    entry[QStringLiteral("deleteOriginals")] = deleteOriginals;
                    m_vaultModel->appendCombineHistoryEntry(entry, nullptr);
                });
    } else if (auto *bench = qobject_cast<BenchmarkTask *>(task)) {
        connect(bench, &BenchmarkTask::benchmarkModelStarted, this,
                &TranscriptionController::benchmarkModelStarted);
        connect(bench, &BenchmarkTask::benchmarkModelDone, this, &TranscriptionController::benchmarkModelDone);
        connect(bench, &BenchmarkTask::benchmarkModelProgressText, this,
                &TranscriptionController::benchmarkModelProgressText);
        connect(bench, &BenchmarkTask::benchmarkRunFinished, this, &TranscriptionController::benchmarkFinished);
        // No separate glow wiring: a BenchmarkTask is a real queued task now,
        // so m_busy + overallProgress drive the existing glow for free. That
        // parallel path (setBenchmarkGlow) is deleted — see the
        // activeIsForeground Q_PROPERTY doc comment.
    } else if (auto *sweep = qobject_cast<DiarizationSweepTask *>(task)) {
        connect(sweep, &DiarizationSweepTask::comboStarted, this,
                &TranscriptionController::diarizationBenchmarkComboStarted);
        connect(sweep, &DiarizationSweepTask::comboDone, this,
                &TranscriptionController::diarizationBenchmarkComboDone);
        connect(sweep, &DiarizationSweepTask::sweepFinished, this,
                &TranscriptionController::diarizationBenchmarkFinished);
    }
}

TranscriptionController::~TranscriptionController()
{
    // Bug report (2026-07-09): "transcription was still running after
    // closing the GUI... it should never run in the background". Root
    // cause: m_thread.wait() below blocks the calling (GUI) thread until
    // the worker thread's current call returns — but nothing was ever
    // telling that call to stop, so closing the app while a job was
    // Active made the WHOLE APPLICATION SHUTDOWN silently hang/block until
    // whatever whisper.cpp/sherpa-onnx call was in flight happened to
    // finish naturally (which, for a long recording, could be minutes),
    // during which the transcription kept running exactly as if nothing
    // had been closed at all. Requesting cancel first lets it unblock at
    // the very next checkpoint the active job's phase supports (the same
    // ones the pause/cancel UI feature already relies on — see
    // TaskContext::checkpoint()'s doc comment for which phases those
    // are), so shutdown is prompt instead of an indefinite silent hang.
    // requestResume() wakes it first in case it was sitting paused, which
    // would otherwise block forever waiting for a pause that will never be
    // lifted by anyone once this destructor is running.
    if (m_runner) {
        m_runner->requestResume();
        m_runner->requestCancel();
    }
    // The instant lane too — an instant task is fast, but cancel+resume it for
    // symmetry so shutdown never blocks on one mid-rewrite.
    if (m_instantRunner) {
        m_instantRunner->requestResume();
        m_instantRunner->requestCancel();
    }
    m_thread.quit();
    m_instantThread.quit();
    m_thread.wait();
    m_instantThread.wait();
}

// The phase-timing subsystem lives in PhaseTimer now (2026-07-20) — the
// rolling averages and their QSettings persistence, the phase vocabulary
// (phaseBucket/canonicalPhaseKey are file-scope statics over there), and the
// blended overallProgress bar. What stays here is the Active-job context that
// only this class can observe, plus these two public forwarders so the names
// tests already spell keep resolving.
const QStringList TranscriptionController::kTimedPhases = PhaseTimer::timedPhases();

double TranscriptionController::defaultPhaseDurationMs(const QString &phase)
{
    return PhaseTimer::defaultPhaseDurationMs(phase);
}

// modelId of whichever job is currently Active, or empty if none — the
// per-config timing key (canonicalPhaseKey()) needs this at each of the three
// points below; job.modelId (set in transcribe()) and m_activeSpeakerDetection
// (set in startJob()) are both already available here since m_activeIndex
// only clears in onWorkerFinished(), which runs strictly after every
// phase()/phaseDurationMs() signal for that job.
QString TranscriptionController::activeModelIdForTiming() const
{
    if (m_activeIndex >= 0 && m_activeIndex < static_cast<int>(m_tasks.size())) {
        if (auto *t = qobject_cast<const TranscribeTask *>(m_tasks[m_activeIndex].get()))
            return t->modelId();
    }
    return QString();
}

void TranscriptionController::recordPhaseDuration(const QString &phase, qint64 durationMs)
{
    // The timing key is per-CONFIGURATION, and the two config axes beyond the
    // phase name — which model, and whether this is the diarize job — are
    // Active-job state that lives here, not on PhaseTimer. Read them off the
    // Active job and hand them over (see this method's header doc comment for
    // why they aren't threaded through the phaseDurationMs signal instead).
    m_phaseTimer->recordPhaseDuration(phase, activeModelIdForTiming(), m_activeSpeakerDetection,
                                      durationMs);
    recomputeOverallProgress();
}

void TranscriptionController::recomputeOverallProgress()
{
    // The active serial task is the single source of BOTH facts PhaseTimer
    // needs about the job shape — its ProgressModel and its ordered phase set
    // — so this never hardcodes a phase list that could drift against the
    // progress lambda's copy.
    Task *task = (m_activeIndex >= 0 && m_activeIndex < static_cast<int>(m_tasks.size()))
                     ? m_tasks[m_activeIndex].get()
                     : nullptr;
    const bool weightedPhases =
        task && task->progressModel() == Task::ProgressModel::WeightedPhases;
    m_phaseTimer->recomputeOverallProgress(
        weightedPhases ? task->progressPhases() : QStringList(), weightedPhases, m_phase,
        activeModelIdForTiming(), m_activeSpeakerDetection, m_progress,
        activeElapsedMs() - m_phaseStartMs);
}

bool TranscriptionController::isQueued(const QUrl &fileUrl) const
{
    return TaskQuery::isQueued(m_tasks, fileUrl);
}

void TranscriptionController::rebuildJobStatusIndex()
{
    QHash<QString, Task::Status> rebuilt;
    for (const auto &task : m_tasks) {
        if (task->fileUrl.isEmpty())
            continue; // download task — names no single vault file
        if (!task->occupiesFile())
            continue; // benchmark/sweep — no status dot on the file it reads
        const QString path = task->fileUrl.toLocalFile();
        const auto it = rebuilt.constFind(path);
        if (it == rebuilt.constEnd() || taskStatusPriority(task->status) > taskStatusPriority(it.value()))
            rebuilt[path] = task->status;
    }
    if (rebuilt != m_jobStatusByPath) {
        m_jobStatusByPath = rebuilt;
        emit jobStatusesChanged();
    }
}

int TranscriptionController::jobStatusForPath(const QString &path) const
{
    const auto it = m_jobStatusByPath.constFind(path);
    // -1 == "no task for this path" (the sentinel Task::Status has no member
    // for — see the class doc comment where JobStatus used to live).
    return it == m_jobStatusByPath.constEnd() ? -1 : static_cast<int>(it.value());
}

bool TranscriptionController::matchesExistingTranscription(const QUrl &fileUrl, const QString &modelId) const
{
    const QVariantMap meta = readMetadata(fileUrl);
    if (meta.isEmpty())
        return false; // not transcribed yet — nothing to match

    // Speaker detection is a separate task (diarizeFile()), not part of what
    // Transcribe produces, so it's not part of this equality check — only the
    // whisper model that actually ran matters.
    // Same model-resolution logic as ModelManager::ensureModel(), minus the
    // disk/network side effects — look up modelId's file name, falling back
    // to the default model's for an unrecognized id (mirroring ensureModel's
    // own fallback so this comparison never falsely reports "differs" for
    // the same fallback substitution that actually ran). Diarization no
    // longer affects which whisper model file is expected (it's a separate
    // pass, independent of modelId — see DiarizationEngine).
    QString requestedFileName;
    QString defaultFileName;
    for (const ModelManager::ModelInfo &info : ModelManager::availableModels()) {
        if (info.id == modelId)
            requestedFileName = info.fileName;
        if (info.id == QString::fromUtf8(ModelManager::kDefaultModelId))
            defaultFileName = info.fileName;
    }
    const QString expectedFileName = requestedFileName.isEmpty() ? defaultFileName : requestedFileName;

    return meta.value(QStringLiteral("model")).toString() == expectedFileName;
}

// The ONE way a task enters the queue — see the header doc comment.
int TranscriptionController::enqueueTask(std::unique_ptr<Task> task, int insertIndex)
{
    int insertAt = (insertIndex < 0) ? frontOfQueueInsertIndex() : insertIndex;

    // The models this kind declared it needs, each queued as its own
    // DownloadTask ahead of it if absent. Every enqueue path gets this
    // uniformly, from the kind's own prerequisites() — no site has to
    // remember which downloads which kind wants.
    queuePrerequisites(*task, task->vaultUrl, &insertAt);

    task->id = m_nextJobId++;
    // Stamp the active batch scope (0 = standalone). Set only inside a
    // withBatch()/enqueueBatch() scope; the prerequisite downloads queued just
    // above went through maybeQueueDownloadTask(), not this path, so they
    // correctly stay standalone. See m_currentBatchId's doc comment.
    task->batchId = m_currentBatchId;
    task->status = Task::Status::Queued;
    const int id = task->id;
    m_tasks.insert(m_tasks.begin() + std::clamp(insertAt, 0, static_cast<int>(m_tasks.size())), std::move(task));
    if (m_activeIndex >= insertAt)
        ++m_activeIndex;

    emit queueChanged();

    // pumpQueues() dispatches the front-most runnable task in EACH lane, so a
    // serial enqueue-while-idle starts here just as before, and an instant
    // enqueue starts on the instant runner immediately (unless it conflicts
    // with the active serial task on the same file).
    pumpQueues();
    return id;
}

void TranscriptionController::queuePrerequisites(const Task &task, const QUrl &vaultUrl, int *insertAt)
{
    for (const ModelPrerequisite &prereq : task.prerequisites()) {
        if (maybeQueueDownloadTask(prereq.target, vaultUrl, prereq.modelId, *insertAt) >= 0)
            ++(*insertAt);
    }
}

void TranscriptionController::transcribe(const QUrl &fileUrl, const QUrl &vaultUrl, const QString &modelId,
                                          bool autoLabel, bool restrictTagsToExisting,
                                          double tagReuseBias, double wordiness)
{
    // Done/Failed entries for the same fileUrl deliberately do NOT block
    // re-queueing (see class doc) — only Queued/Active does.
    if (isQueued(fileUrl))
        return;

    // Item 23: a transcribe call ONLY transcribes now. It used to chain a
    // second relabel-only job whenever autoLabel was true, so clicking
    // "Transcribe" silently also labeled — the dedicated "Regenerate label"
    // action is the sole way to label. The autoLabel parameter is kept (QML
    // call sites in Sidebar.qml/MainPanel.qml still pass
    // AppSettings.autoLabelEnabled, and this invokable signature must stay
    // call-compatible with them) but is now a NO-OP.
    Q_UNUSED(autoLabel);
    Q_UNUSED(restrictTagsToExisting);
    Q_UNUSED(tagReuseBias);
    Q_UNUSED(wordiness);

    enqueueTask(std::make_unique<TranscribeTask>(fileUrl, vaultUrl, modelId));
}

void TranscriptionController::transcribeAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                             const QString &modelId, bool autoLabel,
                                             bool restrictTagsToExisting, double tagReuseBias, double wordiness)
{
    // One batch so a multi-select transcribe shows as a single collapsed Tasks
    // row (empty label ⇒ derived from the members' "Transcribe" kindLabel).
    // Each transcribe() still applies its own Queued/Active dedup, so a
    // deduped-away file simply isn't a batch member.
    withBatch(QString(), [&] {
        for (const QVariant &v : fileUrls)
            transcribe(v.toUrl(), vaultUrl, modelId, autoLabel, restrictTagsToExisting, tagReuseBias,
                       wordiness);
    });
}

void TranscriptionController::importAudio(const QUrl &sourceUrl, const QUrl &destUrl, const QUrl &vaultUrl,
                                           bool overwrite)
{
    // Same Queued/Active dedup rule as transcribe(), keyed on the destination
    // (two drops resolving to the same in-vault file don't double-enqueue).
    if (isQueued(destUrl))
        return;

    auto task = std::make_unique<ImportTask>(sourceUrl.toLocalFile(), destUrl, overwrite);
    task->vaultUrl = vaultUrl;
    enqueueTask(std::move(task));
}

int TranscriptionController::enqueueUsbGrab(const UsbGrabRule &rule, const QString &mountPath, const QUrl &destUrl,
                                             const QUrl &vaultUrl, bool autoTriggered,
                                             const QVariantList &combineRules)
{
    // No isQueued(destUrl) dedup here, deliberately — see the header's doc
    // comment: a grab's fileUrl is a destination FOLDER that a device's two
    // volumes legitimately share.
    auto task = std::make_unique<UsbGrabTask>(rule, mountPath, destUrl, autoTriggered, combineRules);
    task->vaultUrl = vaultUrl;
    return enqueueTask(std::move(task));
}

void TranscriptionController::diarizeFile(const QUrl &fileUrl, const QUrl &vaultUrl,
                                          const QString &segmentationModelId, const QString &embeddingModelId,
                                          double sensitivity)
{
    // Same dedup rule as transcribe()/relabel(): a Queued/Active entry blocks
    // a duplicate request.
    if (isQueued(fileUrl))
        return;

    // Empty id ⇒ the ModelManager default (keeps the task's recorded ids
    // concrete for persistence + the download dedup key).
    const QString segId = segmentationModelId.isEmpty()
                              ? QString::fromLatin1(ModelManager::kDefaultSegmentationModelId)
                              : segmentationModelId;
    const QString embedId = embeddingModelId.isEmpty()
                                ? QString::fromLatin1(ModelManager::kDefaultEmbeddingModelId)
                                : embeddingModelId;

    enqueueTask(std::make_unique<DiarizeTask>(fileUrl, vaultUrl, segId, embedId, sensitivity));
}

void TranscriptionController::diarizeFilesForPaths(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                                   const QString &segmentationModelId,
                                                   const QString &embeddingModelId, double sensitivity)
{
    // One batch (derived "Detect speakers" label) — see transcribeAll().
    withBatch(QString(), [&] {
        for (const QVariant &v : fileUrls)
            diarizeFile(v.toUrl(), vaultUrl, segmentationModelId, embeddingModelId, sensitivity);
    });
}

void TranscriptionController::detectSilences(const QUrl &fileUrl, const QUrl &vaultUrl)
{
    // Same dedup rule as diarizeFile()/relabel(): a Queued/Active entry blocks
    // a duplicate request.
    if (isQueued(fileUrl))
        return;

    enqueueTask(std::make_unique<DetectSilenceTask>(fileUrl, vaultUrl));
}

void TranscriptionController::relabel(const QUrl &fileUrl, const QUrl &vaultUrl, bool restrictTagsToExisting,
                                       double tagReuseBias, double wordiness)
{
    // Same dedup rule as transcribe(): a Queued/Active entry (whether a full
    // transcribe or a relabel-only task) blocks a duplicate request.
    if (isQueued(fileUrl))
        return;

    enqueueTask(std::make_unique<RelabelTask>(fileUrl, vaultUrl, restrictTagsToExisting, tagReuseBias, wordiness,
                                               /*skipIfAlreadyLabeled=*/false));
}

void TranscriptionController::relabelAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                          bool restrictTagsToExisting, double tagReuseBias, double wordiness)
{
    // One batch (derived "Label" label) — see transcribeAll().
    withBatch(QString(), [&] {
        for (const QVariant &v : fileUrls)
            relabel(v.toUrl(), vaultUrl, restrictTagsToExisting, tagReuseBias, wordiness);
    });
}

void TranscriptionController::generateChapters(const QUrl &fileUrl, const QUrl &vaultUrl, double density)
{
    // Same dedup rule as relabel() — see its doc comment.
    if (isQueued(fileUrl))
        return;

    enqueueTask(std::make_unique<ChaptersTask>(fileUrl, vaultUrl, density));
}

void TranscriptionController::generateChaptersAll(const QVariantList &fileUrls, const QUrl &vaultUrl, double density)
{
    // One batch (derived "Generate chapters" label) — see transcribeAll().
    withBatch(QString(), [&] {
        for (const QVariant &v : fileUrls)
            generateChapters(v.toUrl(), vaultUrl, density);
    });
}

void TranscriptionController::runAllJobs(const QUrl &fileUrl, const QUrl &vaultUrl, const QString &modelId,
                                          bool restrictTagsToExisting, double tagReuseBias,
                                          double wordiness, double chapterDensity, const QString &segmentationModelId,
                                          const QString &embeddingModelId, double sensitivity)
{
    // See this method's header doc comment for the full ordering /
    // already-transcribed-skip / dedup contract. Same Queued/Active dedup gate
    // as the individual actions — checked once, up front, so re-entrancy from
    // the inserts below (which make fileUrl "queued") can't matter.
    if (isQueued(fileUrl))
        return;

    // Empty ids ⇒ ModelManager defaults (see diarizeFile()).
    const QString segId = segmentationModelId.isEmpty()
                              ? QString::fromLatin1(ModelManager::kDefaultSegmentationModelId)
                              : segmentationModelId;
    const QString embedId = embeddingModelId.isEmpty()
                                ? QString::fromLatin1(ModelManager::kDefaultEmbeddingModelId)
                                : embeddingModelId;

    // Skip the transcribe step for a file that already has a sibling .md (the
    // friendly, least-surprising "run all" on an already-done file — just
    // re-detect speakers and refresh its label+chapters). An orphaned .md's
    // sidecarPath() is itself, so this is true for those too.
    const bool alreadyTranscribed = QFileInfo::exists(sidecarPath(fileUrl.toLocalFile()));

    // Item O15: jump ahead of whatever's already Queued, but keep this file's
    // tasks CONTIGUOUS and IN ORDER by walking insertAt forward as each is
    // inserted (rather than each new task re-jumping to the very front —
    // which is exactly why this doesn't just call transcribe()/diarizeFile()/
    // relabel()/generateChapters() in sequence). Each enqueueTask() also
    // inserts that step's own prerequisites() ahead of it; maybeQueueDownloadTask()
    // dedups, so the label model the relabel AND chapters steps both want is
    // queued exactly once.
    int insertAt = frontOfQueueInsertIndex();
    const int startIndex = insertAt;
    const auto step = [&](std::unique_ptr<Task> task) {
        // Count what enqueueTask() actually inserted (the task + however many
        // prerequisite downloads it queued ahead of it) so the next step lands
        // after all of it.
        const int before = static_cast<int>(m_tasks.size());
        enqueueTask(std::move(task), insertAt);
        insertAt += static_cast<int>(m_tasks.size()) - before;
    };

    // (1) transcribe (pure ASR) — skipped if already transcribed.
    if (!alreadyTranscribed)
        step(std::make_unique<TranscribeTask>(fileUrl, vaultUrl, modelId));
    // (2) detect speakers — reads the .md the transcribe step (if any) just
    // wrote and splices "[Speaker N]" tags in.
    step(std::make_unique<DiarizeTask>(fileUrl, vaultUrl, segId, embedId, sensitivity));
    // (3) label — reads the .md the diarize step just updated.
    step(std::make_unique<RelabelTask>(fileUrl, vaultUrl, restrictTagsToExisting, tagReuseBias, wordiness,
                                        /*skipIfAlreadyLabeled=*/false));
    // (4) chapters — reads the labeled .md.
    step(std::make_unique<ChaptersTask>(fileUrl, vaultUrl, chapterDensity));

    // No trailing startTask(): the FIRST step()'s enqueueTask() already
    // auto-started the queue if it was idle (and startIndex just marks where
    // that first item landed). Every later step inserts behind the now-Active
    // task and correctly stays Queued.
    Q_UNUSED(startIndex);
}

void TranscriptionController::runAllJobsForPaths(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                                  const QString &modelId,
                                                  bool restrictTagsToExisting, double tagReuseBias, double wordiness,
                                                  double chapterDensity, const QString &segmentationModelId,
                                                  const QString &embeddingModelId, double sensitivity)
{
    // One batch across ALL files' pipeline steps — so "run all" over N files is
    // a single collapsed row ("Run all · N files") instead of up to 4N rows.
    // Explicit label because the members are mixed kinds (transcribe/diarize/
    // label/chapters), so there's no single kindLabel to derive from; the
    // collapsed row's file count comes from the distinct fileUrls, not the task
    // count (see collapseBatches()).
    withBatch(QStringLiteral("Run all"), [&] {
        for (const QVariant &v : fileUrls)
            runAllJobs(v.toUrl(), vaultUrl, modelId, restrictTagsToExisting, tagReuseBias, wordiness, chapterDensity,
                       segmentationModelId, embeddingModelId, sensitivity);
    });
}

void TranscriptionController::startTask(int index)
{
    // Safety-net preflight, before any state mutation below — see
    // maybeRedirectForCombine()'s doc comment. A true return means it already
    // recursed onto whatever should run instead (a newly-inserted combine
    // task); this call contributes nothing further.
    if (maybeRedirectForCombine(index))
        return;

    m_activeIndex = index;
    Task *task = m_tasks[index].get();
    task->status = Task::Status::Active;
    task->error.clear();

    // A pause/cancel left over from whatever the PREVIOUS task did must never
    // leak into this one. Elapsed-time tracking (activeElapsedMs()) restarts
    // from zero for every new Active task.
    m_runner->resetControl();
    m_activePaused = false;
    emit activePausedChanged();
    if (m_activeCancelling) {
        m_activeCancelling = false;
        emit activeStoppingChanged();
    }
    m_activeAccumulatedMs = 0;
    m_activeSegmentTimer.start();
    m_phaseTimer->setOverallProgress(0.0);
    // See recomputeOverallProgress()'s doc comment — true ONLY for a
    // "diarize" task, whose progress bar is weighted around the
    // detecting-speakers phase. A "transcribe" task is pure ASR (its bar is
    // weighted around loading/decoding/detecting-speech/transcribing). Every
    // other kind never emits one of those phases at all, so this is simply
    // never read for them.
    m_activeSpeakerDetection = task->kind() == QStringLiteral("diarize");
    m_phaseStartMs = 0;
    m_indeterminatePhaseTimer->stop();

    m_busy = true;
    emit busyChanged();
    // Presentation, not m_busy, is what decides the main-panel overlay now —
    // so a Background task (benchmark/sweep/download) gets a Tasks row + the
    // glow without hijacking the panel the user is reading, which is exactly
    // what the old benchmark side-door existed to work around.
    const bool foreground = task->presentation() == Task::Presentation::Foreground;
    if (m_activeIsForeground != foreground) {
        m_activeIsForeground = foreground;
        emit activeIsForegroundChanged();
    }
    m_progress = 0.0;
    emit progressChanged();
    // Cleared here (not just at the end of the previous task) so a kind that
    // never emits phase() at all doesn't inherit a stale phase left over from
    // an earlier one.
    m_phase.clear();
    emit currentPhaseChanged();
    m_currentFile = task->fileUrl;
    emit currentFileChanged();
    emit currentJobKindChanged();
    emit queueChanged();

    // Connected exactly once, on first dispatch. A Paused task that resumes
    // re-enters startTask(), so guard against double-wiring (which would
    // double-count every progress tick).
    if (!m_connectedTasks.contains(task)) {
        m_connectedTasks.insert(task);
        connectTask(task);
    }

    // The ENTIRE dispatch, for every kind. This replaced a 10-branch if/else
    // that marshalled each kind's own argument list into a string-named worker
    // slot — the single biggest reason adding a kind meant editing five
    // disjoint sites. Now the kind IS the object.
    QMetaObject::invokeMethod(m_runner, "execute", Qt::QueuedConnection, Q_ARG(Task *, task));
}

bool TranscriptionController::serialActiveConflictsWith(const QUrl &fileUrl) const
{
    return m_activeIndex >= 0 && m_activeIndex < static_cast<int>(m_tasks.size()) && !fileUrl.isEmpty()
        && isSameRecording(m_tasks[m_activeIndex]->fileUrl, fileUrl);
}

void TranscriptionController::pumpQueues()
{
    const int n = static_cast<int>(m_tasks.size());

    // --- Serial lane: one at a time, Paused (resume) before Queued. ---
    if (m_activeIndex < 0) {
        int next = -1;
        for (int i = 0; i < n; ++i) {
            if (m_tasks[i]->lane() == Task::Lane::Serial && m_tasks[i]->status == Task::Status::Paused) {
                next = i;
                break;
            }
        }
        if (next < 0) {
            for (int i = 0; i < n; ++i) {
                if (m_tasks[i]->lane() == Task::Lane::Serial && m_tasks[i]->status == Task::Status::Queued) {
                    next = i;
                    break;
                }
            }
        }
        if (next >= 0)
            startTask(next); // may recurse (combine preflight) and set m_activeIndex
    }

    // --- Instant lane: one at a time, front-most Queued Instant task with no
    // file conflict against the ACTIVE serial task. ---
    if (!m_instantActive) {
        for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
            Task *t = m_tasks[i].get();
            if (t->lane() == Task::Lane::Instant && t->status == Task::Status::Queued
                && !serialActiveConflictsWith(t->fileUrl)) {
                startInstantTask(i);
                break;
            }
        }
    }
}

void TranscriptionController::startInstantTask(int index)
{
    Task *task = m_tasks[index].get();
    task->status = Task::Status::Active;
    task->error.clear();
    m_instantActive = task;

    // Connected once, like a serial task — for its typed result signals
    // (speakerRenamed etc.). connectTask() no-ops the serial-only progress/
    // phase/paused wiring for an Instant task (see there).
    if (!m_connectedTasks.contains(task)) {
        m_connectedTasks.insert(task);
        connectTask(task);
    }
    emit queueChanged(); // status -> active shows in the Tasks list

    m_instantRunner->resetControl();
    QMetaObject::invokeMethod(m_instantRunner, "execute", Qt::QueuedConnection, Q_ARG(Task *, task));
}

int TranscriptionController::indexOfTask(const Task *task) const
{
    return TaskQuery::indexOfTask(m_tasks, task);
}

void TranscriptionController::onTaskFinished(Task *task, bool ok, const QString &error)
{
    // Instant lane (see Task::Lane / startInstantTask()): a separate, parallel
    // path that never touched the serial-lane UI state (m_busy/currentFile/
    // phase/overlay), so its completion doesn't either — it just records the
    // terminal status, emits the one report, and re-pumps (a serial task that
    // finishes may now unblock a same-file instant task, and vice versa).
    if (task == m_instantActive) {
        m_instantActive = nullptr;
        const QUrl finishedFile = task->fileUrl;
        if (indexOfTask(task) >= 0) {
            task->status = ok ? Task::Status::Done : Task::Status::Failed;
            task->error = error;
            task->durationMs = 0; // instant by construction — no elapsed tracking
        }
        emit queueChanged();
        emit finished(finishedFile, ok, error, task->isStructuralFileOp());
        if (indexOfTask(task) >= 0)
            maybeEmitUndoAvailable(task, ok); // fires per terminal; batch-aware
        pumpQueues();
        return;
    }

    m_busy = false;
    emit busyChanged();
    if (m_activeIsForeground) {
        m_activeIsForeground = false;
        emit activeIsForegroundChanged();
    }
    // A task that fails/gets cancelled mid-"loading"/"decoding" never gets
    // another phase() signal to naturally stop this.
    m_indeterminatePhaseTimer->stop();
    const QUrl finishedFile = m_currentFile;

    // Same requestCancel()->TaskError("Cancelled") mechanism as a real cancel,
    // distinguished only by this flag, which togglePauseActive() sets right
    // before triggering it (a pause during the unhookable "loading" phase).
    const bool wasStopping = m_pausingActiveAsRestart;
    const bool pausedAsRestart = m_pausingActiveAsRestart && !ok && error == QStringLiteral("Cancelled");
    m_pausingActiveAsRestart = false;
    if (wasStopping)
        emit activeStoppingChanged();

    const int taskIndex = indexOfTask(task);
    if (taskIndex >= 0) {
        if (pausedAsRestart) {
            task->status = Task::Status::Paused;
            task->error.clear();
        } else {
            task->status = ok ? Task::Status::Done : Task::Status::Failed;
            task->error = error;
            // Snapshot active-work elapsed time (excludes any paused stretches
            // — see activeElapsedMs()) as this task's final duration, shown in
            // the Tasks popup's history rows. Left unset (-1) for a Paused
            // task — it isn't finished.
            task->durationMs = activeElapsedMs();
            // Any task whose completion changes the on-disk model set (a landed
            // download, a successful model import) refreshes the Models page —
            // ONE central emit keyed off Task::completionChangesModelSet(),
            // rather than a per-kind check here plus another in the ModelIoTask
            // wiring.
            if (ok && task->completionChangesModelSet())
                emit modelsChanged();
            // A benchmark / diarization-sweep that throws BEFORE emitting its
            // run-finished summary (a decode failure, or an engine that throws
            // mid-sweep) otherwise pins the Benchmark page on "Running…"
            // forever: the page clears that state off benchmarkFinished /
            // diarizationBenchmarkFinished, which never arrives on the throw
            // path, and benchmarkFailed had no emit site at all. Surface it so
            // the page resets and shows why. A user cancel already reported
            // itself via the run-finished summary (cancelled=true) before
            // throwing "Cancelled", so skip that to avoid a double report
            // clobbering the "Cancelled." status.
            const QString failedKind = task->kind();
            if (!ok && error != QStringLiteral("Cancelled")
                && (failedKind == QStringLiteral("benchmark")
                    || failedKind == QStringLiteral("diarizationSweep")))
                emit benchmarkFailed(error);
        }
    }
    m_activeIndex = -1;
    m_activePaused = false;
    emit activePausedChanged();
    if (m_activeCancelling) {
        m_activeCancelling = false;
        emit activeStoppingChanged();
    }

    if (pausedAsRestart) {
        // Deliberately do NOT advance to the next Queued task — the Paused
        // task sits at the front until the user explicitly resumes it
        // (startNext() prioritizes any Paused task over Queued ones).
        m_currentFile = QUrl();
        emit currentFileChanged();
        emit currentJobKindChanged();
        emit queueChanged();
        QMetaObject::invokeMethod(m_runner, "evictIdle", Qt::QueuedConnection);
        emit finished(finishedFile, ok, error);
        return;
    }

    // Advance both lanes (a just-finished serial task may unblock a same-file
    // instant task). pumpQueues() starts the next serial task if any.
    pumpQueues();
    if (m_activeIndex < 0) {
        // Serial queue drained — clear the "what's running" UI and release the
        // warm engine the finished task was using.
        m_currentFile = QUrl();
        emit currentFileChanged();
        emit currentJobKindChanged();
        emit queueChanged();
        QMetaObject::invokeMethod(m_runner, "evictIdle", Qt::QueuedConnection);
    }

    emit finished(finishedFile, ok, error, task->isStructuralFileOp());
    // Fires on EVERY terminal (Done or Failed) so a batch can detect its last
    // member; the standalone path inside still gates on ok. A Paused task
    // isn't terminal, so skip it.
    if (taskIndex >= 0 && !pausedAsRestart)
        maybeEmitUndoAvailable(task, ok);
}

void TranscriptionController::maybeEmitUndoAvailable(const Task *task, bool ok)
{
    // --- Batch member: one snackbar for the whole set, fired by the LAST
    // member to finish. ---
    if (task->batchId != 0) {
        const int batchId = task->batchId;
        int undoableDone = 0;
        int labelMemberIndex = -1;
        for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
            const Task *t = m_tasks[i].get();
            if (t->batchId != batchId)
                continue;
            if (t->status != Task::Status::Done && t->status != Task::Status::Failed)
                return; // a sibling is still pending — this isn't the last one
            if (t->status == Task::Status::Done && t->makeUndoTask()) {
                ++undoableDone;
                labelMemberIndex = i; // any member of the kind can author the label
            }
        }
        // All members are terminal (cancelled-while-queued ones were removed
        // from m_tasks entirely, so they simply don't appear here). Emit once,
        // iff at least one member actually landed something undoable.
        if (undoableDone > 0 && labelMemberIndex >= 0)
            emit undoAvailable(batchId, m_tasks[labelMemberIndex]->batchUndoLabel(undoableDone));
        return;
    }

    // --- Standalone task: undoable iff it succeeded AND advertises a label
    // AND can build its inverse (see Task::undoLabel/makeUndoTask). We build
    // the inverse here only to confirm it exists, then discard it — undoTask()
    // rebuilds it on click, so the snackbar never holds a live Task. ---
    if (!ok)
        return;
    const QString label = task->undoLabel();
    if (label.isEmpty() || !task->makeUndoTask())
        return;
    emit undoAvailable(task->id, label);
}

void TranscriptionController::undoTask(int token)
{
    // A batch token: undo every Done member of the batch as ONE fresh batch
    // (so the undo is itself a single-snackbar-undoable action). batchId and
    // task id share the m_nextJobId counter, so a non-zero token is
    // unambiguously one or the other — try the batch interpretation first.
    if (token != 0) {
        std::vector<std::unique_ptr<Task>> inverses;
        for (const auto &task : m_tasks) {
            if (task->batchId == token && task->status == Task::Status::Done) {
                if (auto inverse = task->makeUndoTask())
                    inverses.push_back(std::move(inverse));
            }
        }
        if (!inverses.empty()) {
            enqueueBatch(std::move(inverses));
            return;
        }
    }

    // Otherwise a standalone task id.
    for (const auto &task : m_tasks) {
        if (task->id != token || task->status != Task::Status::Done)
            continue;
        std::unique_ptr<Task> inverse = task->makeUndoTask();
        if (inverse)
            enqueueTask(std::move(inverse), static_cast<int>(m_tasks.size()));
        return;
    }
}

int TranscriptionController::enqueueBatch(std::vector<std::unique_ptr<Task>> tasks, const QString &displayLabel)
{
    if (tasks.empty())
        return 0;
    // One shared id for the whole batch, drawn from the SAME counter as task
    // ids (assigned inside enqueueTask) so it can never collide with one —
    // that's what lets undoTask()/maybeEmitUndoAvailable()/collapseBatches()
    // treat a single int token as either a task id or a batch id unambiguously.
    // The stamping goes through m_currentBatchId (enqueueTask reads it), the
    // same scope withBatch() uses, so both batch paths are identical.
    int batchId = 0;
    withBatch(displayLabel, [&] {
        batchId = m_currentBatchId;
        for (auto &task : tasks) {
            // Append in order (like the speaker edits / renameFile) so a
            // batch's members run in the order the user selected them.
            enqueueTask(std::move(task), static_cast<int>(m_tasks.size()));
        }
    });
    return batchId;
}

void TranscriptionController::withBatch(const QString &displayLabel, const std::function<void()> &fn)
{
    // Not re-entrant: batches are built synchronously on the GUI thread, one at
    // a time. A nested withBatch() would clobber the outer scope's batchId, so
    // guard against it rather than silently mis-stamp.
    Q_ASSERT(m_currentBatchId == 0);
    const int batchId = m_nextJobId++;
    m_currentBatchId = batchId;
    if (!displayLabel.isEmpty())
        m_batchLabels.insert(batchId, displayLabel);
    fn();
    m_currentBatchId = 0;
    // Nothing landed under this batchId (e.g. every file was deduped away) — no
    // batch exists, so drop the label we speculatively recorded rather than
    // leak it. (A deduped-away op enqueues nothing at all, prereqs included,
    // since the single-file helpers return before enqueueTask.)
    if (!batchHasMembers(batchId))
        m_batchLabels.remove(batchId);
}

bool TranscriptionController::batchHasMembers(int batchId) const
{
    return TaskQuery::batchHasMembers(m_tasks, batchId);
}

void TranscriptionController::removeTaskAt(int index)
{
    Task *task = m_tasks[index].get();
    const int batchId = task->batchId;
    m_connectedTasks.remove(task);
    // m_instantActive is a pointer, not an index — clear it if THIS is the
    // instant-lane task being removed (a cancel-while-queued never sets it,
    // but a defensive clear keeps the invariant simple).
    if (m_instantActive == task)
        m_instantActive = nullptr;
    m_tasks.erase(m_tasks.begin() + index);
    if (m_activeIndex > index)
        --m_activeIndex;
    // Drop a batch's display label once its last member is gone, so
    // m_batchLabels doesn't accumulate stale entries across a long session.
    if (batchId != 0 && !batchHasMembers(batchId))
        m_batchLabels.remove(batchId);
}

void TranscriptionController::cancelTaskAt(int index)
{
    // Snapshot before erasing — this is the pre-run-cancel half of the
    // one-terminal-report invariant (.task/plan.md invariant 1): a task
    // cancelled while still Queued/Paused never reached the runner, so the
    // queue must synthesize its terminal report itself. The old system just
    // dropped the entry, which is why a consumer waiting on finished() (e.g.
    // UsbGrabberController's in-flight tracking) never heard back for a
    // cancelled-while-queued task.
    const QUrl url = m_tasks[index]->fileUrl;
    removeTaskAt(index);
    emit queueChanged();
    emit finished(url, false, QStringLiteral("Cancelled"));
}

void TranscriptionController::dismiss(const QUrl &fileUrl)
{
    // A fileUrl may have multiple entries (re-queueing a Done/Failed task is
    // allowed) — remove only the first Done/Failed match, skipping over any
    // Queued/Active entry for the same url rather than no-op'ing on it.
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        const Task *task = m_tasks[i].get();
        if (task->fileUrl == fileUrl
            && (task->status == Task::Status::Done || task->status == Task::Status::Failed)) {
            removeTaskAt(i);
            emit queueChanged();
            return;
        }
    }
}

void TranscriptionController::clearFinished()
{
    bool changed = false;
    for (int i = static_cast<int>(m_tasks.size()) - 1; i >= 0; --i) {
        if (m_tasks[i]->status == Task::Status::Done || m_tasks[i]->status == Task::Status::Failed) {
            removeTaskAt(i);
            changed = true;
        }
    }
    if (changed)
        emit queueChanged();
}

qint64 TranscriptionController::activeElapsedMs() const
{
    if (m_activeIndex < 0)
        return 0;
    return m_activeAccumulatedMs + (m_activePaused ? 0 : m_activeSegmentTimer.elapsed());
}

qint64 TranscriptionController::estimatedRemainingMs() const
{
    const double overall = m_phaseTimer->overallProgress();
    if (m_activeIndex < 0 || overall <= 0.0)
        return -1;
    const double remainingFraction = (1.0 - overall) / overall;
    return static_cast<qint64>(activeElapsedMs() * remainingFraction);
}

void TranscriptionController::pauseActive()
{
    if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_tasks.size()))
        return;
    // The task itself declares whether it can pause — the string allowlist
    // (transcribe/download/combine/chapters) is gone; a kind with a real
    // interruption point returns capabilities().pausable, and everything else
    // is a safe no-op here. This is the same capability the UI reads to enable
    // the pause button, so the two can never disagree (the 2026-07-09 bug was
    // exactly such a disagreement).
    if (!m_tasks[m_activeIndex]->capabilities().pausable)
        return;
    // See this method's header doc comment — "loading" is whisper.cpp's
    // in-process, unhookable model-load call (no interruption point at all),
    // unlike "decoding" (a real, killable/SIGSTOP-able ffmpeg subprocess —
    // see AudioDecoder.h) or transcribing/diarization (their own cooperative
    // checkpoints) — queuing a pause during "loading" would only take effect
    // much later, confusingly. Irrelevant to "combine" (never emits a
    // "loading" phase at all).
    if (m_phase == QStringLiteral("loading"))
        return;
    // A soft-pause-as-restart from an earlier "loading" click on THIS SAME
    // job is still in flight (see activeStopping's doc comment) —
    // its cancel flag is already set on the worker and hasn't been observed
    // yet. Layering a fresh pause request on top would only get silently
    // suppressed by that pending cancel (waitWhilePausedOrCancelled()'s wait
    // loop never runs while cancelled), permanently breaking pause for this
    // job. Wait for it to resolve instead of stacking a second request.
    if (m_pausingActiveAsRestart)
        return;
    m_runner->requestPause();
}

void TranscriptionController::resumeActive()
{
    m_runner->requestResume();
}

void TranscriptionController::togglePauseActive()
{
    // See this method's header doc comment for the race it exists to close
    // — mirrors BottomBar.qml's old onTapped branching exactly, just reading
    // m_busy/m_activePaused/m_phase (this object's own state) instead of
    // QML's copies of them.
    if (!m_busy) {
        startNext();
        return;
    }
    if (m_activePaused) {
        resumeActive();
        return;
    }
    // A soft-pause-as-restart is already in flight and hasn't resolved yet
    // (see activeStopping's doc comment) — ignore further clicks rather than
    // stacking a second request on top of the still-pending cancel.
    if (m_pausingActiveAsRestart)
        return;
    // Item 9 (2026-07-14): "loading" (whisper.cpp's blocking, unhookable
    // model-load call) has no cooperative checkpoint to pause AT. It used to
    // silently reinterpret the tap as cancelActive() + present the job as
    // Paused-via-restart — the "pause broken again" report: a click that
    // looked like it did nothing, and could latch a stray cancel that broke
    // pause for the rest of the job. That reinterpretation is gone: the
    // bottom bar now DISABLES the pause button during "loading" (see
    // BottomBar.qml's pauseButton.enabled) so this is never reached from the
    // UI during that phase, and pauseActive() below is a safe no-op for
    // "loading" if it somehow is. A pause is never turned into a cancel here.
    // (NOTE for a later wave — items 30/50: a genuine mid-loading pause would
    // need a real interruption hook that whisper.cpp doesn't expose today;
    // the JobStatus::Paused/activeStopping machinery is left dormant for
    // whenever that lands, not removed.)
    pauseActive();
}

void TranscriptionController::cancelActive()
{
    if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_tasks.size()))
        return;
    cancelJob(m_tasks[m_activeIndex]->id);
}

void TranscriptionController::cancelJob(int id)
{
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        Task *task = m_tasks[i].get();
        if (task->id != id)
            continue;
        // A Queued or Paused task has nothing running to interrupt — remove it
        // and synthesize its terminal report (cancelTaskAt), so a consumer
        // waiting on finished() hears back even though run() never ran.
        if (task->status == Task::Status::Queued || task->status == Task::Status::Paused) {
            cancelTaskAt(i);
            return;
        }
        if (task->status == Task::Status::Active) {
            // The runner emits the terminal report for THIS case (run() returns
            // via TaskError or early-return), so no synthesis here. Route to
            // whichever lane's runner is actually running this task.
            TaskRunner *runner = (task == m_instantActive) ? m_instantRunner : m_runner;
            // Enter the honest "Cancelling" state for a task that can't abort
            // mid-pass — its pass runs to its boundary, and without this the UI
            // would misread the between-tick paused(false) as a resume and let
            // pause silently no-op. Derived from the task's own capability, so
            // future non-mid-cancellable kinds inherit it. Only the serial lane
            // has the pause UI, so only flag it there.
            if (task != m_instantActive && task->capabilities().pausable
                && !task->capabilities().cancellableMidRun) {
                m_activeCancelling = true;
                emit activeStoppingChanged();
            }
            // No requestResume(): every pause-wait re-checks cancel on its own
            // poll (checkpoint()'s while-condition; ProcessRunner's top-of-loop
            // cancelled() check), so the cancel lands within one ~100ms tick
            // regardless of pause state. Resuming here only fired a misleading
            // paused(false)/SIGCONT edge that looked like the task had resumed.
            runner->requestCancel();
        }
        return;
    }
}

void TranscriptionController::cancelBatch(int batchId)
{
    if (batchId == 0)
        return;
    // Snapshot the member ids first — cancelJob() mutates m_tasks (a
    // Queued/Paused member is removed outright), so walking it live while
    // cancelling would invalidate indices. Route each through cancelJob() so a
    // member gets the exact same Queued-vs-Active handling a single cancel
    // would (removal + synthesized terminal report, or a runner cancel request).
    QList<int> ids;
    for (const auto &task : m_tasks) {
        if (task->batchId == batchId
            && (task->status == Task::Status::Queued || task->status == Task::Status::Paused
                || task->status == Task::Status::Active))
            ids.append(task->id);
    }
    for (const int id : std::as_const(ids))
        cancelJob(id);
}

void TranscriptionController::dismissBatch(int batchId)
{
    if (batchId == 0)
        return;
    // Remove every terminal (Done/Failed) member of the batch — the batch-wide
    // "clear this finished row". Walk back-to-front so each erase leaves the
    // earlier indices valid (same shape as clearFinished()).
    bool changed = false;
    for (int i = static_cast<int>(m_tasks.size()) - 1; i >= 0; --i) {
        const Task *task = m_tasks[i].get();
        if (task->batchId == batchId
            && (task->status == Task::Status::Done || task->status == Task::Status::Failed)) {
            removeTaskAt(i);
            changed = true;
        }
    }
    if (changed)
        emit queueChanged();
}

void TranscriptionController::cancelAll()
{
    // Snapshot the ids to cancel first — cancelTaskAt() mutates m_tasks (and
    // emits finished() per task), so iterating it live while removing would
    // be unsafe. Active job handled separately below (it's not removed here;
    // the runner reports it).
    QList<int> queuedIds;
    for (const auto &task : m_tasks) {
        if (task->status == Task::Status::Queued || task->status == Task::Status::Paused)
            queuedIds.append(task->id);
    }
    for (const int id : std::as_const(queuedIds)) {
        const int idx = [this, id] {
            for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i)
                if (m_tasks[i]->id == id)
                    return i;
            return -1;
        }();
        if (idx >= 0)
            cancelTaskAt(idx);
    }
    if (m_activeIndex >= 0) {
        Task *active = m_tasks[m_activeIndex].get();
        if (active->capabilities().pausable && !active->capabilities().cancellableMidRun) {
            m_activeCancelling = true;
            emit activeStoppingChanged();
        }
        m_runner->requestCancel(); // no requestResume() — see cancelJob()
    }
}

void TranscriptionController::moveQueuedJob(int id, int newIndex)
{
    // Index math is pure over the task list — TaskQuery::planQueuedMove().
    // What stays here is the part that can't be: the actual reorder of the
    // owning vector, the m_activeIndex fixups the shift implies, and the
    // queueChanged() emission.
    const TaskQuery::QueuedMove move = TaskQuery::planQueuedMove(m_tasks, id, newIndex);
    if (!move.valid)
        return;

    std::unique_ptr<Task> moved = std::move(m_tasks[move.fromIndex]);
    m_tasks.erase(m_tasks.begin() + move.fromIndex);
    if (m_activeIndex > move.fromIndex)
        --m_activeIndex;

    m_tasks.insert(m_tasks.begin() + move.insertIndex, std::move(moved));
    if (m_activeIndex >= move.insertIndex)
        ++m_activeIndex;
    emit queueChanged();
}

bool TranscriptionController::isDownloadQueued(ModelDownloadTarget target, const QUrl &vaultUrl,
                                                const QString &modelId) const
{
    return TaskQuery::isDownloadQueued(m_tasks, target, vaultUrl, modelId);
}

int TranscriptionController::maybeQueueDownloadTask(ModelDownloadTarget target, const QUrl &vaultUrl,
                                                     const QString &modelId, int insertIndex)
{
    bool alreadyOnDisk = false;
    switch (target) {
    case ModelDownloadTarget::WhisperModel:
        // An unrecognized modelId has no real file to pre-download — there
        // is nothing sensible to queue (ensureModel()'s own fallback-to-
        // default logic, run inline when the transcribe task actually
        // starts, is what actually resolves this case; see ModelManager's
        // class doc comment). Queuing a phantom download here would silently
        // redirect to the default model under a label that still says the
        // unrecognized id.
        if (ModelManager::modelFileName(modelId).isEmpty())
            return -1;
        alreadyOnDisk = ModelManager::isModelDownloaded(modelId);
        break;
    case ModelDownloadTarget::LabelModel:
        alreadyOnDisk = ModelManager::isLabelModelDownloaded();
        break;
    case ModelDownloadTarget::VadModel:
        alreadyOnDisk = ModelManager::isVadModelDownloaded();
        break;
    case ModelDownloadTarget::SegmentationModel:
        alreadyOnDisk = ModelManager::isSegmentationModelDownloaded(modelId);
        break;
    case ModelDownloadTarget::EmbeddingModel:
        alreadyOnDisk = ModelManager::isEmbeddingModelDownloaded(modelId);
        break;
    }
    if (alreadyOnDisk || isDownloadQueued(target, vaultUrl, modelId))
        return -1;

    auto task = std::make_unique<DownloadTask>(target, modelId);
    task->id = m_nextJobId++;
    task->vaultUrl = vaultUrl;
    task->status = Task::Status::Queued;
    const int id = task->id;
    const int at =
        (insertIndex < 0) ? static_cast<int>(m_tasks.size()) : std::clamp(insertIndex, 0, static_cast<int>(m_tasks.size()));
    m_tasks.insert(m_tasks.begin() + at, std::move(task));
    return id;
}

// See this method's header doc comment.
int TranscriptionController::frontOfQueueInsertIndex() const
{
    return TaskQuery::frontOfQueueInsertIndex(m_tasks, m_activeIndex);
}

QVariantList TranscriptionController::detectCombineGroups(const QVariantList &rules,
                                                            const QStringList &audioPaths) const
{
    return CombineScanner::detectCombineGroups(rules, audioPaths);
}

QString TranscriptionController::regexValidationError(const QString &pattern) const
{
    return CombineScanner::regexValidationError(pattern);
}

QStringList TranscriptionController::differentiatorNonParticipatingFiles(const QString &pattern, int groupIndex,
                                                                           const QStringList &audioPaths) const
{
    return CombineScanner::differentiatorNonParticipatingFiles(pattern, groupIndex, audioPaths);
}

bool TranscriptionController::isCombineQueued(const QString &outputPath) const
{
    return TaskQuery::isCombineQueued(m_tasks, outputPath);
}

void TranscriptionController::enqueueCombineJob(const CombineGroup &group, const QUrl &vaultUrl,
                                                 bool deleteOriginals, int insertIndex)
{
    auto task = std::make_unique<CombineTask>(group.files, QUrl::fromLocalFile(group.outputPath), deleteOriginals);
    task->id = m_nextJobId++;
    task->vaultUrl = vaultUrl;
    task->status = Task::Status::Queued;
    const int at = (insertIndex < 0) ? frontOfQueueInsertIndex() : insertIndex;
    m_tasks.insert(m_tasks.begin() + std::clamp(at, 0, static_cast<int>(m_tasks.size())), std::move(task));
}

// See this method's header doc comment. This is the scheduler-level preflight
// .task/plan.md flagged as the hardest port: it can rewrite the task's target
// file, insert a combine task ahead of it, and recurse — all strictly BEFORE
// dispatch, on the GUI thread, which is what makes mutating Task::fileUrl safe
// (run() hasn't started; see that field's doc comment).
bool TranscriptionController::maybeRedirectForCombine(int index)
{
    Task *task = m_tasks[index].get();
    // Only a plain transcribe is ever redirected — it's the sole kind whose
    // input file might turn out to be a combine fragment. Every *Only kind in
    // the old flag-soup guard was just "not transcribe"; a single qobject_cast
    // expresses that now.
    auto *transcribe = qobject_cast<TranscribeTask *>(task);
    if (!transcribe || transcribe->fileUrl.isEmpty())
        return false;

    const QFileInfo info(transcribe->fileUrl.toLocalFile());

    // Outcome 1: an earlier manual runCombineScan() run may already have
    // merged this exact file's group (and moved the original fragment into
    // .combined/) since this task was queued. The merged output's path is
    // recomputable from this filename alone, so check for it directly.
    for (const QVariant &ruleVar : m_combineRules) {
        const QVariantMap rule = ruleVar.toMap();
        const auto key = matchRuleCapturedKey(rule, info.fileName());
        if (!key)
            continue;
        const QString outputPath = combineOutputPathFor(rule.value(QStringLiteral("name")).toString(), *key,
                                                          info.absolutePath(), info.suffix());
        if (QFileInfo::exists(outputPath))
            transcribe->fileUrl = QUrl::fromLocalFile(outputPath);
        break; // only the first matching rule is used, mirroring findCombineGroups()
    }

    // Outcome 2: this file is a member of a currently-"ready" group per the
    // last runCombineScan() scan that hasn't been merged yet — insert the
    // combine task ahead of this one and redirect.
    const QString filePath = transcribe->fileUrl.toLocalFile();
    for (const CombineGroup &group : CombineScanner::findCombineGroups(m_combineRules, m_combineAudioPaths)) {
        if (group.status != QStringLiteral("ready") || !group.files.contains(filePath))
            continue;
        transcribe->fileUrl = QUrl::fromLocalFile(group.outputPath);
        if (!isCombineQueued(group.outputPath))
            enqueueCombineJob(group, transcribe->vaultUrl, m_combineDeleteOriginals, index);
        emit queueChanged();
        // Recurses onto the freshly-inserted combine task at this same index
        // (safe — see this method's header doc comment). If a combine task
        // for this group was somehow already queued elsewhere, nothing was
        // inserted at `index` here, so this recurses onto the SAME transcribe
        // task; its redirected fileUrl still won't exist yet and it fails
        // cleanly via the normal decode error rather than looping.
        startTask(index);
        return true;
    }
    return false;
}

// The shared spine of both bulk (runCombineScan) and single-group
// (combineGroup) enqueue: cache the scan inputs for maybeRedirectForCombine()'s
// later per-job check, then enqueue a combine job for every "ready", not-
// already-queued group that passes the outputPath filter, and kick the queue
// once if anything went in. outputPathFilter empty = every ready group;
// non-empty = only the group whose outputPath matches (a single-group action).
// Returns how many were enqueued.
int TranscriptionController::enqueueReadyCombineGroups(const QVariantList &rules, const QStringList &audioPaths,
                                                        const QUrl &vaultUrl, bool deleteOriginals,
                                                        const QString &outputPathFilter)
{
    // Cached for maybeRedirectForCombine()'s per-job safety check — see its
    // doc comment and the m_combineRules/m_combineAudioPaths field comments.
    // Set even when nothing ends up enqueued, matching the old behavior.
    m_combineRules = rules;
    m_combineAudioPaths = audioPaths;
    m_combineDeleteOriginals = deleteOriginals;

    int enqueued = 0;
    for (const CombineGroup &group : CombineScanner::findCombineGroups(rules, audioPaths)) {
        if (group.status != QStringLiteral("ready"))
            continue;
        if (!outputPathFilter.isEmpty() && group.outputPath != outputPathFilter)
            continue;
        if (isCombineQueued(group.outputPath))
            continue;
        enqueueCombineJob(group, vaultUrl, deleteOriginals);
        ++enqueued;
    }
    if (enqueued == 0)
        return 0;
    emit queueChanged();
    // Live trigger (like transcribe()), not a persisted-queue replay — auto-
    // starting when idle matches transcribe()'s own behavior, unlike
    // loadPersistedQueue()'s deliberately-not-auto-started restored jobs
    // (see startNext()'s doc comment).
    if (m_activeIndex < 0)
        startNext();
    return enqueued;
}

void TranscriptionController::runCombineScan(const QVariantList &rules, const QStringList &audioPaths,
                                               const QUrl &vaultUrl, bool deleteOriginals)
{
    enqueueReadyCombineGroups(rules, audioPaths, vaultUrl, deleteOriginals, /*outputPathFilter=*/QString());
}

// Sidebar's per-row "combine this group" hover button (item 51): same shape
// as runCombineScan() above but filtered to the ONE group whose outputPath
// matches — a deliberate single-group action, not "scan and enqueue
// everything ready" the ribbon button performs. Silently no-ops if that
// group is no longer "ready" (e.g. a concurrent edit/scan already changed
// its status) or already queued — the sidebar only shows this button for a
// row it just computed as "ready" via detectCombineGroups(), but that
// snapshot can go stale between hover and click just as easily as
// runCombineScan()'s own dedup check already accounts for.
void TranscriptionController::combineGroup(const QVariantList &rules, const QStringList &audioPaths,
                                            const QString &outputPath, const QUrl &vaultUrl, bool deleteOriginals)
{
    enqueueReadyCombineGroups(rules, audioPaths, vaultUrl, deleteOriginals, outputPath);
}

QString TranscriptionController::tasksJsonPath(const QUrl &vaultUrl)
{
    return vaultUrl.toLocalFile() + QStringLiteral("/.transcriber/tasks.json");
}

void TranscriptionController::persistQueue() const
{
    if (m_vaultUrl.toString().isEmpty())
        return;

    // Which tasks survive a restart, and their exact on-disk shape, is pure
    // over the task list — TaskQuery::serializeQueue() (see its doc comment
    // for the Queued-only / opt-in-persistable rules). What stays here is the
    // vault-path resolution and the atomic write.
    const QJsonArray array = TaskQuery::serializeQueue(m_tasks);

    QSaveFile file(tasksJsonPath(m_vaultUrl));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    file.commit();
}

void TranscriptionController::loadPersistedQueue(const QUrl &vaultUrl)
{
    m_vaultUrl = vaultUrl;

    QFile file(tasksJsonPath(vaultUrl));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return;

    // Replayed in file order — the order persistQueue() wrote. A download a
    // dependent task needs was written just before it, so replaying in order
    // queues each download ahead of its dependent (and the dependent's own
    // prerequisites() sees it already queued via isDownloadQueued(), no dup).
    //
    // Deliberately does NOT auto-start anything: every restored entry just
    // sits Queued until the user resumes via startNext(). So this appends at
    // the tail (explicit insertIndex == size) rather than going through the
    // front-jumping enqueueTask(), and reproduces the exact saved order —
    // which is ALSO why a "transcribe" entry followed by its "relabel"
    // sibling for the same fileUrl both replay: neither goes through the
    // public transcribe()/relabel() dedup guard (that guard would reject the
    // second), only through createTaskFromJson() + a raw insert.
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject obj = v.toObject();
        const QString kind = obj.value("kind").toString();

        std::unique_ptr<Task> task = createTaskFromJson(kind, obj);
        if (!task)
            continue; // unrecognized kind, or one whose isPersistable() is false

        // A download entry is self-contained (no prerequisites of its own); a
        // dependent kind gets its downloads queued ahead of it, deduped
        // against whatever the persisted download entries already queued.
        int insertAt = static_cast<int>(m_tasks.size());
        queuePrerequisites(*task, task->vaultUrl, &insertAt);

        task->id = m_nextJobId++;
        task->status = Task::Status::Queued;
        m_tasks.insert(m_tasks.begin() + std::clamp(insertAt, 0, static_cast<int>(m_tasks.size())), std::move(task));
        emit queueChanged();
    }
}

void TranscriptionController::startNext()
{
    // The QML play button's resume entry point — just the scheduler. Serial
    // Paused-before-Queued priority and the instant lane both live in
    // pumpQueues() now.
    pumpQueues();
}

void TranscriptionController::terminateApplicationImmediately() const
{
    // See this method's header doc comment for why this is a hard process
    // exit rather than a graceful QCoreApplication::quit() — ::_exit()
    // (POSIX, unistd.h — NOT std::exit()/std::quick_exit(), which still run
    // atexit/global-destructor cleanup) skips every remaining C++
    // destructor (including this object's own, and the worker QThread's)
    // and every bit of Qt event-loop teardown, ending the process instantly
    // regardless of what the worker thread happens to be doing at the time.
    ::_exit(0);
}

QVariantMap TranscriptionController::readMetadata(const QUrl &fileUrl) const
{
    // Item P23: see m_transcriptCacheMutex's doc comment — this function is
    // now also called from loadTranscriptAsync()'s background thread.
    QMutexLocker locker(&m_transcriptCacheMutex);

    const QString mdPath = sidecarPath(fileUrl.toLocalFile());
    const QFileInfo fi(mdPath);
    if (!fi.exists())
        return QVariantMap();

    // Item O19: a cheap stat guard — frontmatter is written once up front
    // and only touched again at a job's very end (see
    // syncSpeakersFrontmatterToBody()/writeLabelingFields()), so on the
    // common "nothing changed since last call" tick this skips the file
    // open/read/parse entirely. See m_frontmatterCache's doc comment.
    const qint64 size = fi.size();
    const QDateTime mtime = fi.lastModified();
    if (m_frontmatterCache.valid && m_frontmatterCache.mdPath == mdPath && m_frontmatterCache.size == size
        && m_frontmatterCache.mtime == mtime)
        return m_frontmatterCache.data;

    QVariantMap result;
    QFile mdFile(mdPath);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return result;

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---"))
        return result;

    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        if (value == QStringLiteral("true"))
            result[key] = true;
        else if (value == QStringLiteral("false"))
            result[key] = false;
        else
            result[key] = value;
    }

    m_frontmatterCache.mdPath = mdPath;
    m_frontmatterCache.size = size;
    m_frontmatterCache.mtime = mtime;
    m_frontmatterCache.data = result;
    m_frontmatterCache.valid = true;
    return result;
}

bool TranscriptionController::setLabel(const QUrl &fileUrl, const QString &label, QString *errorOut) const
{
    const QFileInfo fi(fileUrl.toLocalFile());
    const QString path = sidecarPath(fileUrl.toLocalFile());

    if (!QFileInfo::exists(path)) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has not been transcribed yet").arg(fi.fileName());
        return false;
    }

    QStringList frontmatterLines;
    QString body;
    if (!MdFrontmatter::read(path, &frontmatterLines, &body, errorOut))
        return false;

    MdFrontmatter::upsertKey(&frontmatterLines, QStringLiteral("label"), label);

    if (!writeFrontmatterBlock(path, frontmatterLines, body, errorOut))
        return false;
    // Item P19: this manual (non-queued) rewrite otherwise leaves the
    // sidebar's cached per-row metadata stale until an unrelated job
    // finishes or a full VaultTreeModel rebuild happens — see finished()'s
    // doc comment. A listener is expected to call
    // VaultTreeModel::refreshFile(fileUrl) from this signal (see its doc
    // comment for why rebuild()/the QFileSystemWatcher alone never catches
    // an in-place rewrite of a file that already existed) rather than wait
    // for the next unrelated directory event.
    emit finished(fileUrl, true, QString());
    return true;
}

bool TranscriptionController::setTags(const QUrl &fileUrl, const QStringList &tags, QString *errorOut) const
{
    const QFileInfo fi(fileUrl.toLocalFile());
    const QString path = sidecarPath(fileUrl.toLocalFile());

    if (!QFileInfo::exists(path)) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has not been transcribed yet").arg(fi.fileName());
        return false;
    }

    QStringList frontmatterLines;
    QString body;
    if (!MdFrontmatter::read(path, &frontmatterLines, &body, errorOut))
        return false;

    MdFrontmatter::upsertKey(&frontmatterLines, QStringLiteral("tags"), tags.join(QStringLiteral(", ")));

    if (!writeFrontmatterBlock(path, frontmatterLines, body, errorOut))
        return false;
    // Item P19-style notification: sidebar's tag-derived display (if any)
    // should refresh the same way a label edit does — see setLabel()'s
    // matching emit for why (VaultTreeModel::refreshFile(fileUrl)).
    emit finished(fileUrl, true, QString());
    return true;
}

bool TranscriptionController::setCollapsedChapters(const QUrl &fileUrl, const QStringList &chapterKeys, QString *errorOut) const
{
    const QFileInfo fi(fileUrl.toLocalFile());
    const QString path = sidecarPath(fileUrl.toLocalFile());

    if (!QFileInfo::exists(path)) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has not been transcribed yet").arg(fi.fileName());
        return false;
    }

    QStringList frontmatterLines;
    QString body;
    if (!MdFrontmatter::read(path, &frontmatterLines, &body, errorOut))
        return false;

    // Empty list REMOVES the key (no blank `collapsedChapters:` left behind);
    // a non-empty list upserts it with the comma-joined keys.
    MdFrontmatter::upsertKey(&frontmatterLines, QStringLiteral("collapsedChapters"),
                             chapterKeys.join(QLatin1Char(',')), /*removeIfEmpty=*/true);

    if (!writeFrontmatterBlock(path, frontmatterLines, body, errorOut))
        return false;
    // No finished() emit — see the header doc comment (collapse is pure view
    // state, must not trigger a metadata/transcript reload on every toggle).
    return true;
}

// Manual-edit counterpart for segment text (MainPanel's always-editable
// transcriptArea) — reuses parseSpeakerTaggedFile()/writeFrontmatterBlock()
// (originally written for renameSpeakerInFile()/mergeSpeakersInFile() above)
// since a single-segment text replace needs exactly the same "locate every
// segment line via segmentLinePattern(knownSpeakers), splice by captured
// offset, rewrite the whole file" shape — just targeting the TEXT capture
// group (3) per requested t0 instead of the speaker group (2).
bool TranscriptionController::setSentenceTexts(const QUrl &fileUrl, const QVariantList &edits, QString *errorOut) const
{
    const QString path = sidecarPath(fileUrl.toLocalFile());
    QStringList frontmatterLines;
    QString body;
    int speakersLineIndex = -1;
    QStringList knownSpeakers;
    QStringList distinctTags;
    if (!parseSpeakerTaggedFile(path, &frontmatterLines, &body, &speakersLineIndex, &knownSpeakers, &distinctTags,
                                 errorOut))
        return false;

    QVector<QRegularExpressionMatch> matches;
    QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatch(body);
    while (it.hasNext())
        matches.append(it.next());

    struct PendingEdit {
        qsizetype start;
        qsizetype length;
        QString text;
    };
    QVector<PendingEdit> pending;
    pending.reserve(edits.size());

    for (const QVariant &editVar : edits) {
        const QVariantMap edit = editVar.toMap();
        const double t0 = edit.value(QStringLiteral("t0")).toDouble();
        QString newText = edit.value(QStringLiteral("text")).toString();
        // A segment is exactly one body line by construction (see
        // MainPanel.qml's displayData doc comment) — a stray newline here
        // would corrupt that invariant, so collapse defensively rather than
        // trusting the caller (QML already blocks Return/strips pasted
        // newlines, but this is the actual safety boundary).
        newText.replace(QLatin1Char('\n'), QLatin1Char(' '));

        bool found = false;
        for (const QRegularExpressionMatch &m : std::as_const(matches)) {
            if (std::abs(parseCompactTimestamp(m.captured(1)) - t0) < 0.001) {
                pending.append({m.capturedStart(3), m.capturedLength(3), newText});
                found = true;
                break;
            }
        }
        if (!found) {
            if (errorOut)
                *errorOut = QStringLiteral("no segment found at timestamp %1").arg(formatTimestamp(t0));
            return false;
        }
    }

    // Splice from the END of the body backward so earlier edits' offsets
    // stay valid as later ones are replaced (same technique as
    // renameSpeakerInFile()/mergeSpeakersInFile() above).
    std::sort(pending.begin(), pending.end(),
              [](const PendingEdit &a, const PendingEdit &b) { return a.start > b.start; });
    for (const PendingEdit &edit : std::as_const(pending))
        body.replace(edit.start, edit.length, edit.text);

    if (!writeFrontmatterBlock(path, frontmatterLines, body, errorOut))
        return false;
    emit finished(fileUrl, true, QString());
    return true;
}

QString TranscriptionController::readPrelude(const QUrl &fileUrl) const
{
    // Builds on readTranscriptBody() rather than re-skipping frontmatter by
    // hand — that call is already cached behind a size+mtime check (see its
    // own doc comment), so this stays cheap on repeated calls, and correctly
    // invalidates when only the body (not frontmatter) changes, which is
    // exactly the case a manual prelude edit or a relabel produces.
    const QString body = readTranscriptBody(fileUrl);
    const int markerIdx = body.indexOf(kPreludeEndMarker);
    if (markerIdx < 0)
        return QString();
    return body.left(markerIdx).trimmed();
}

bool TranscriptionController::setPrelude(const QUrl &fileUrl, const QString &prelude, QString *errorOut) const
{
    const QFileInfo fi(fileUrl.toLocalFile());
    const QString path = sidecarPath(fileUrl.toLocalFile());

    if (!QFileInfo::exists(path)) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has not been transcribed yet").arg(fi.fileName());
        return false;
    }

    // This method never touches a frontmatter key — the raw lines are carried
    // through untouched; only the body's prelude changes.
    QStringList frontmatterLines;
    QString body;
    if (!MdFrontmatter::read(path, &frontmatterLines, &body, errorOut))
        return false;

    const QString newBody = spliceBodyPrelude(body, prelude);

    if (!writeFrontmatterBlock(path, frontmatterLines, newBody, errorOut))
        return false;
    // Item P19: see setLabel()'s matching emit for why
    // (VaultTreeModel::refreshFile(fileUrl)).
    emit finished(fileUrl, true, QString());
    return true;
}

QVariantList TranscriptionController::readChapters(const QUrl &fileUrl) const
{
    // stripPreludeIfPresent isn't load-bearing here — "## " can never
    // collide with the prelude's own kPreludeEndMarker sentinel or with
    // segmentLinePattern()'s "^\[" anchor, unlike the prelude's own
    // numbered-list false-positive risk (see stripPreludeIfPresent()'s doc
    // comment). Kept anyway purely for consistency with every other body
    // scanner in this file (parseSentences(), plainTextFromTranscriptBody())
    // — belt-and-suspenders, not correctness-required.
    const QString body = stripPreludeIfPresent(readTranscriptBody(fileUrl));

    QVariantList chapters;
    QRegularExpressionMatchIterator it = chapterMarkerPattern().globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QVariantMap chapter;
        chapter["t0"] = parseCompactTimestamp(m.captured(1));
        // Item 17: the manual sentinel is stripped from the DISPLAYED title
        // but its presence is surfaced as a "manual" flag (so the TOC can
        // show manual chapters distinctly / know editing an auto one promotes
        // it). Plain (auto) titles pass through unchanged.
        chapter["title"] = chapterDisplayTitle(m.captured(2));
        chapter["manual"] = chapterTitleCaptureIsManual(m.captured(2));
        chapters.append(chapter);
    }
    return chapters;
}

bool TranscriptionController::editChapterMarker(const QUrl &fileUrl, ChapterEditOp op, double timestampSeconds,
                                                 const QString &title, QString *errorOut) const
{
    const QFileInfo fi(fileUrl.toLocalFile());
    const QString path = sidecarPath(fileUrl.toLocalFile());

    if (!QFileInfo::exists(path)) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has not been transcribed yet").arg(fi.fileName());
        return false;
    }
    QStringList frontmatterLines;
    QString body;
    if (!MdFrontmatter::read(path, &frontmatterLines, &body, errorOut))
        return false;

    const QString tsText = formatTimestamp(timestampSeconds);
    switch (op) {
    case ChapterEditOp::Add: {
        // Replace any existing marker at this exact timestamp, then insert the
        // manual one in sorted position — so a re-add is idempotent and always
        // ends up manual.
        QRegularExpressionMatch existing;
        if (findChapterMarkerAt(body, timestampSeconds, &existing))
            removeChapterMarkerLine(&body, existing);
        const int offset = chapterInsertOffset(body, timestampSeconds);
        body.insert(offset, formatChapterMarker(tsText, title, /*manual=*/true) + QLatin1Char('\n'));
        break;
    }
    case ChapterEditOp::SetTitle: {
        QRegularExpressionMatch target;
        if (!findChapterMarkerAt(body, timestampSeconds, &target)) {
            if (errorOut)
                *errorOut = QStringLiteral("no chapter at %1").arg(tsText);
            return false;
        }
        // Editing an auto chapter's title PROMOTES it to manual (always write
        // the sentinel) so the next regeneration won't clobber the edit —
        // least-surprising. Reuse the marker's OWN stored timestamp text.
        body.replace(target.capturedStart(0), target.capturedLength(0),
                     formatChapterMarker(target.captured(1), title, /*manual=*/true));
        break;
    }
    case ChapterEditOp::Delete: {
        QRegularExpressionMatch target;
        if (!findChapterMarkerAt(body, timestampSeconds, &target)) {
            if (errorOut)
                *errorOut = QStringLiteral("no chapter at %1").arg(tsText);
            return false;
        }
        removeChapterMarkerLine(&body, target);
        break;
    }
    }

    if (!writeFrontmatterBlock(path, frontmatterLines, body, errorOut))
        return false;
    return true;
}

void TranscriptionController::addChapter(const QUrl &fileUrl, double timestampSeconds, const QString &title)
{
    QString error;
    const bool ok = editChapterMarker(fileUrl, ChapterEditOp::Add, timestampSeconds, title, &error);
    // Mirror setPrelude()/setLabel(): notify listeners so the sidebar/main
    // panel refresh this file's chapters immediately (item P19).
    emit finished(fileUrl, ok, error);
}

void TranscriptionController::setChapterTitle(const QUrl &fileUrl, double timestampSeconds, const QString &newTitle)
{
    QString error;
    const bool ok = editChapterMarker(fileUrl, ChapterEditOp::SetTitle, timestampSeconds, newTitle, &error);
    emit finished(fileUrl, ok, error);
}

void TranscriptionController::deleteChapter(const QUrl &fileUrl, double timestampSeconds)
{
    QString error;
    const bool ok = editChapterMarker(fileUrl, ChapterEditOp::Delete, timestampSeconds, QString(), &error);
    emit finished(fileUrl, ok, error);
}

void TranscriptionController::renameSpeaker(const QUrl &fileUrl, const QString &oldName, const QString &newName)
{
    // Same dedup rule as transcribe()/relabel() — but unlike those, a caller
    // here is actively waiting on speakerRenamed() for a definitive result
    // (see MainPanel.qml's pendingSpeakerRenames), so silently dropping the
    // request would leave its optimistic UI stuck forever. Report it instead.
    if (isQueued(fileUrl)) {
        emit speakerRenamed(fileUrl, oldName, false,
                             QStringLiteral("another task is already running for this file"));
        return;
    }

    // Appended at the tail (explicit insertIndex == size), not jumped ahead
    // like transcribe() — matches the old renameSpeaker()'s push_back. A
    // rename is cheap and non-urgent; it can wait behind queued transcription.
    enqueueTask(std::make_unique<RenameSpeakerTask>(fileUrl, oldName, newName),
                static_cast<int>(m_tasks.size()));
}

void TranscriptionController::mergeSpeakers(const QUrl &fileUrl, const QString &sourceName, const QString &targetName)
{
    // Same dedup rule as renameSpeaker() immediately above, for the same
    // reason — see its doc comment.
    if (isQueued(fileUrl)) {
        emit speakersMerged(fileUrl, sourceName, targetName, false,
                             QStringLiteral("another task is already running for this file"));
        return;
    }

    // Appended at the tail, same as renameSpeaker() — see its doc comment.
    enqueueTask(std::make_unique<MergeSpeakersTask>(fileUrl, sourceName, targetName),
                static_cast<int>(m_tasks.size()));
}

void TranscriptionController::renameFile(const QUrl &fileUrl, const QString &newBaseName)
{
    // Appended at the tail like the speaker edits — an instant-lane task, so
    // it still runs right away (in parallel with any serial transcription of a
    // DIFFERENT file); the scheduler defers it only if THIS file is actively
    // transcribing. Reports via fileRenamed().
    //
    // Record the pending stem BEFORE enqueueing so every view (see
    // pendingRenameStem()) shows the new name now — fileRenamed is deferred
    // behind a same-file transcription and would leave panels stale for the whole
    // job. Erased when the task completes (below).
    m_pendingRenames.insert(fileUrl.toLocalFile(), newBaseName);
    ++m_pendingRenamesRevision;
    emit pendingRenamesRevisionChanged();
    enqueueTask(std::make_unique<RenameFileTask>(fileUrl, newBaseName), static_cast<int>(m_tasks.size()));
}

QString TranscriptionController::pendingRenameStem(const QUrl &fileUrl) const
{
    return m_pendingRenames.value(fileUrl.toLocalFile());
}

void TranscriptionController::moveFiles(const QVariantList &fileUrls, const QUrl &destDirUrl, const QUrl &vaultUrl)
{
    // One instant-lane MoveTask per file, all sharing an undo batch — so a
    // multi-select move is a single "Moved N files" snackbar (see
    // enqueueBatch()). A single-file move is just a batch of one.
    std::vector<std::unique_ptr<Task>> tasks;
    for (const QVariant &entry : fileUrls) {
        const QUrl url = entry.toUrl();
        if (!url.isEmpty())
            tasks.push_back(std::make_unique<MoveTask>(url, destDirUrl, vaultUrl));
    }
    enqueueBatch(std::move(tasks));
}

void TranscriptionController::setVaultTreeModel(VaultTreeModel *model)
{
    if (m_vaultModel == model)
        return;
    m_vaultModel = model;
    emit vaultTreeModelChanged();
}

void TranscriptionController::trash(const QString &scope, const QVariantList &urls, const QUrl &vaultUrl)
{
    const QList<QUrl> list = urlsFromVariant(urls);
    if (list.isEmpty())
        return;
    // ONE instant-lane TrashTask for the whole batch ⇒ ONE undo snackbar (see
    // TrashTask). Appended at the tail like the other instant edits.
    enqueueTask(std::make_unique<TrashTask>(scopeFromString(scope), list, vaultUrl),
                static_cast<int>(m_tasks.size()));
}

void TranscriptionController::deletePermanently(const QString &scope, const QVariantList &urls, const QUrl &vaultUrl)
{
    const QList<QUrl> list = urlsFromVariant(urls);
    if (list.isEmpty())
        return;
    enqueueTask(std::make_unique<DeleteTask>(scopeFromString(scope), list, vaultUrl),
                static_cast<int>(m_tasks.size()));
}

void TranscriptionController::renameFolder(const QUrl &folderUrl, const QString &newName, const QUrl &vaultUrl)
{
    enqueueTask(std::make_unique<FolderRenameTask>(folderUrl, newName, vaultUrl),
                static_cast<int>(m_tasks.size()));
}

void TranscriptionController::moveFolder(const QUrl &folderUrl, const QUrl &destParentUrl, const QUrl &vaultUrl)
{
    enqueueTask(std::make_unique<FolderMoveTask>(folderUrl, destParentUrl, vaultUrl),
                static_cast<int>(m_tasks.size()));
}

QString TranscriptionController::readTranscriptBody(const QUrl &fileUrl) const
{
    // Item P23: see m_transcriptCacheMutex's doc comment.
    QMutexLocker locker(&m_transcriptCacheMutex);

    const QString mdPath = sidecarPath(fileUrl.toLocalFile());
    const QFileInfo fi(mdPath);
    if (!fi.exists())
        return QString();

    // Item O19: see m_bodyCache's doc comment — during an ACTIVE
    // transcription this is invoked on every live-update tick while the
    // file is constantly growing, so this stat-based guard alone rarely
    // skips work THEN, but it does for every other caller polling a file
    // that isn't currently changing (e.g. browsing while nothing is
    // transcribing) — a real full-read-avoided win, not just a
    // theoretical one.
    const qint64 size = fi.size();
    const QDateTime mtime = fi.lastModified();
    if (m_bodyCache.valid && m_bodyCache.mdPath == mdPath && m_bodyCache.size == size && m_bodyCache.mtime == mtime)
        return m_bodyCache.body;

    QFile mdFile(mdPath);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---"))
        return QString();

    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        // skip frontmatter lines
    }
    const QString body = in.readAll();

    m_bodyCache.mdPath = mdPath;
    m_bodyCache.size = size;
    m_bodyCache.mtime = mtime;
    m_bodyCache.body = body;
    m_bodyCache.valid = true;
    return body;
}

QVariantList TranscriptionController::parseSentences(const QUrl &fileUrl) const
{
    // Item P23: see m_transcriptCacheMutex's doc comment — RECURSIVE lock,
    // since readTranscriptBody()/readMetadata() below each lock it again.
    QMutexLocker locker(&m_transcriptCacheMutex);

    const QString mdPath = sidecarPath(fileUrl.toLocalFile());
    // stripPreludeIfPresent first: the prelude is prose ABOUT the transcript,
    // not a segment, and (per stripPreludeIfPresent's doc comment) could
    // otherwise be misdetected as one if it happens to start a line with
    // bracket-shaped text.
    const QString body = stripPreludeIfPresent(readTranscriptBody(fileUrl)); // itself cached — see its doc comment
    // The file's own known-speaker allowlist (see writeFrontmatter()'s and
    // segmentLinePattern()'s doc comments) — read via readMetadata() rather
    // than re-scanning frontmatter lines by hand, since that already parses
    // arbitrary "key: value" frontmatter lines generically (vaults are
    // small, a second open+read costs nothing worth avoiding here) and is
    // itself cached (see readMetadata()'s doc comment).
    const QStringList knownSpeakers =
        parseSpeakersList(readMetadata(fileUrl).value(QStringLiteral("speakers")).toString());

    // Item O19: segments are only ever APPENDED to a live-running
    // transcript (SegmentLineWriter's repeat-cycle merge is the one
    // exception — it rewrites the last couple of lines IN PLACE rather than
    // appending, see its own doc comment) — so once `body`'s first
    // m_sentenceCache.body.size() characters are confirmed to still equal
    // the cached body byte-for-byte, only the NEW trailing slice needs a
    // regex pass; the already-parsed prefix's sentences are reused as-is.
    // Falls back to a full reparse whenever that prefix check fails (a
    // different file, first call, or a merge having rewritten the tail) —
    // this only ever trades a slightly-too-eager full reparse for
    // correctness, never the other way around.
    if (m_sentenceCache.valid && m_sentenceCache.mdPath == mdPath) {
        if (m_sentenceCache.body == body)
            return m_sentenceCache.sentences;
        if (body.startsWith(m_sentenceCache.body)) {
            const qsizetype prefixLen = m_sentenceCache.body.size();
            QVariantList sentences = m_sentenceCache.sentences;
            const QStringView suffix = QStringView(body).mid(prefixLen);
            QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatchView(suffix);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                QVariantMap sentence;
                sentence["t0"] = parseCompactTimestamp(m.captured(1));
                sentence["speaker"] = m.captured(2);
                sentence["text"] = m.captured(3);
                sentence["start"] = static_cast<int>(prefixLen + m.capturedStart(3));
                sentence["length"] = m.capturedLength(3);
                sentence["repeatCount"] = m.capturedLength(4) == 0 ? 1 : m.captured(4).toInt();
                sentences.append(sentence);
            }
            m_sentenceCache.body = body;
            m_sentenceCache.sentences = sentences;
            return sentences;
        }
    }

    // Full (re)parse: no cache yet, a different file, or the cached prefix
    // no longer matches. globalMatch over the whole (multi-line) body, not a
    // per-line split, so capturedStart(3)/capturedLength(3) are exact
    // offsets into this SAME body string — what MainPanel.qml needs to
    // span-highlight the active sentence's substring without re-deriving
    // offsets itself. No "t1": it's no longer stored (see
    // SegmentLineWriter's doc comment — it was always identical to the next
    // segment's t0) and nothing ever consumed it here.
    QVariantList sentences;
    QRegularExpressionMatchIterator it = segmentLinePattern(knownSpeakers).globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QVariantMap sentence;
        sentence["t0"] = parseCompactTimestamp(m.captured(1));
        // Empty when speaker detection wasn't on for this file (group 2
        // didn't participate in the match) — QRegularExpression's captured()
        // returns an empty string in that case, not a crash. The literal tag
        // text itself, never translated through a lookup (see
        // segmentLinePattern()'s doc comment).
        sentence["speaker"] = m.captured(2);
        sentence["text"] = m.captured(3);
        sentence["start"] = m.capturedStart(3);
        sentence["length"] = m.capturedLength(3);
        // Group 4 (repeat count) only participates when SegmentLineWriter
        // merged consecutive identical lines into this one — absent means a
        // normal, unmerged line, i.e. repeatCount 1.
        sentence["repeatCount"] = m.capturedLength(4) == 0 ? 1 : m.captured(4).toInt();
        sentences.append(sentence);
    }

    m_sentenceCache.mdPath = mdPath;
    m_sentenceCache.body = body;
    m_sentenceCache.sentences = sentences;
    m_sentenceCache.valid = true;
    return sentences;
}

void TranscriptionController::loadTranscriptAsync(const QUrl &fileUrl)
{
    // See this function's own doc comment in the header. QThread::create()
    // (Qt::Core, no extra module needed) rather than QtConcurrent — this
    // app has no other use for the Concurrent module, and a plain
    // fire-and-forget QThread per call is simpler than introducing it for
    // one call site. The thread deletes itself (deleteLater on its own
    // finished() signal) once the lambda below returns.
    QThread *thread = QThread::create([this, fileUrl]() {
        // Both calls are cached + mutex-guarded (see
        // m_transcriptCacheMutex's doc comment) — safe to run here
        // concurrently with the GUI thread's own synchronous calls to the
        // same two functions.
        const QString body = readTranscriptBody(fileUrl);
        const QVariantList sentences = parseSentences(fileUrl);
        // Marshal back to the GUI thread — `this` (TranscriptionController)
        // lives there, so Qt::QueuedConnection posts this lambda onto its
        // event loop rather than running it inline on this background
        // thread.
        QMetaObject::invokeMethod(
            this, [this, fileUrl, body, sentences]() { emit transcriptLoaded(fileUrl, body, sentences); },
            Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

int TranscriptionController::activeSentenceIndex(const QVariantList &sentences, double positionSeconds) const
{
    if (sentences.isEmpty())
        return -1;

    // Last index whose t0 <= positionSeconds (std::upper_bound - 1). Sticky
    // through silence gaps: a position between sentence i's t1 and i+1's t0
    // still resolves to i, so the highlight holds rather than blanking out.
    const auto it = std::upper_bound(
        sentences.begin(), sentences.end(), positionSeconds,
        [](double pos, const QVariant &sentence) { return pos < sentence.toMap().value("t0").toDouble(); });
    int index = int(std::distance(sentences.begin(), it)) - 1;
    // Chapter headings are zero-length pseudo-entries interleaved into this list
    // (isChapterHeading), not spoken content. When the search lands DIRECTLY on
    // one there are two cases:
    //   - The chapter is COLLAPSED: its body sentences are dropped from this
    //     list, so the heading is the ONLY line representing that chapter and
    //     playback is somewhere inside its time range — the highlight belongs
    //     ON the heading. Return it as-is (the caller paints the whole row).
    //   - The chapter is EXPANDED: playback is in the lead-in silence after the
    //     marker but before the chapter's first sentence; walk back to the
    //     previous REAL sentence so the highlight holds there, exactly as it
    //     does across an ordinary silence gap.
    // Headings merely PASSED OVER by the walk-back (an expanded chapter's
    // lead-in heading, or a collapsed heading we did not land in) are always
    // skipped — the walk only ever stops on real content or on a directly-hit
    // collapsed heading. If only headings precede pos (a chapter opens the
    // transcript and is not collapsed), this falls through to -1 and the
    // caller's front-edge clamp lights the first real sentence instead.
    if (index >= 0) {
        const QVariantMap landed = sentences.at(index).toMap();
        if (landed.value(QStringLiteral("isChapterHeading")).toBool()
            && landed.value(QStringLiteral("collapsed")).toBool())
            return index;
    }
    // Silence phase 3: the between-segment divider is ANOTHER zero-length
    // pseudo-entry interleaved into this same list (isSilenceMarker, see
    // MainPanel.qml's displayData/pushSilenceMarker) — pure decoration, never
    // a landing spot, so it walks back exactly like an EXPANDED chapter
    // heading (no "collapsed" exception exists for it).
    while (index >= 0
           && (sentences.at(index).toMap().value(QStringLiteral("isChapterHeading")).toBool()
               || sentences.at(index).toMap().value(QStringLiteral("isSilenceMarker")).toBool()))
        --index;
    // Front edge (index < 0): playback hasn't reached the first entry's t0 yet
    // (or every preceding entry was an expanded lead-in heading we walked over).
    // If the transcript OPENS with a COLLAPSED chapter, its heading is the only
    // visible line representing that opening content, so hold the highlight ON
    // it from 0:00 — identical to when playback is already inside its range. A
    // first chapter's real t0 is often a hair above 0 (lead-in silence) even
    // though its gutter timestamp floors to "0:00", so pos==0 lands here, not on
    // the directly-hit branch above. An EXPANDED leading heading (no `collapsed`
    // key) still yields -1, leaving the caller's front-edge clamp to light the
    // first real sentence, exactly as item 010 established.
    if (index < 0) {
        const QVariantMap first = sentences.at(0).toMap();
        if (first.value(QStringLiteral("isChapterHeading")).toBool()
            && first.value(QStringLiteral("collapsed")).toBool())
            return 0;
    }
    return index;
}

QVariantList TranscriptionController::parseSilences(const QString &raw) const
{
    // formatSilencesValue()'s inverse: "12.4-18.9, 45-52.3" (compact
    // timestamps either side of a literal '-', comma-separated spans) ->
    // {t0, t1} maps. Compact timestamps never themselves contain '-' (see
    // formatTimestamp), so splitting each span on the FIRST '-' is safe and
    // unambiguous.
    QVariantList result;
    if (raw.trimmed().isEmpty())
        return result;
    for (const QString &part : raw.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed = part.trimmed();
        const int dash = trimmed.indexOf(QLatin1Char('-'));
        if (dash <= 0 || dash + 1 >= trimmed.size())
            continue;
        QVariantMap span;
        span[QStringLiteral("t0")] = parseCompactTimestamp(trimmed.left(dash));
        span[QStringLiteral("t1")] = parseCompactTimestamp(trimmed.mid(dash + 1));
        result.append(span);
    }
    return result;
}

double TranscriptionController::nextSilenceSkipTarget(const QVariantList &silences, double positionSeconds,
                                                       double thresholdSeconds) const
{
    for (const QVariant &entry : silences) {
        const QVariantMap span = entry.toMap();
        const double t0 = span.value(QStringLiteral("t0")).toDouble();
        const double t1 = span.value(QStringLiteral("t1")).toDouble();
        if (positionSeconds >= t0 && positionSeconds < t1 && (t1 - t0) >= thresholdSeconds)
            return t1;
    }
    return -1.0;
}

QStringList TranscriptionController::existingVaultTags(const QUrl &vaultUrl) const
{
    // See the header doc comment: GUI thread only, cached per file because
    // QML calls this from a binding on every keystroke.
    return collectVaultFrontmatterList(vaultUrl.toLocalFile(), QStringLiteral("tags"),
                                       &m_vaultTagFileCache);
}

QStringList TranscriptionController::existingVaultSpeakers(const QUrl &vaultUrl) const
{
    // Same contract and same reasoning as existingVaultTags() above, over the
    // `speakers:` key and its own cache.
    return collectVaultFrontmatterList(vaultUrl.toLocalFile(), QStringLiteral("speakers"),
                                       &m_vaultSpeakerFileCache);
}

int TranscriptionController::tasksTotal() const
{
    return TaskQuery::tasksTotal(m_tasks);
}

int TranscriptionController::tasksCompleted() const
{
    return TaskQuery::tasksCompleted(m_tasks);
}

QVariantList TranscriptionController::queuedFileUrls() const
{
    return TaskQuery::queuedFileUrls(m_tasks);
}

QString TranscriptionController::currentJobKind() const
{
    if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_tasks.size()))
        return QString();
    return m_tasks[m_activeIndex]->kind();
}

QString TranscriptionController::currentJobKindLabel() const
{
    if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_tasks.size()))
        return QString();
    return m_tasks[m_activeIndex]->kindLabel();
}

QString TranscriptionController::phaseLabel(const QString &phase) const
{
    return JobListFormatter::phaseLabel(phase);
}

QString TranscriptionController::phaseOverlayLabel(const QString &phase) const
{
    return JobListFormatter::phaseOverlayLabel(phase);
}

bool TranscriptionController::activeCanPause() const
{
    if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_tasks.size()))
        return false;
    return m_tasks[m_activeIndex]->capabilities().pausable;
}

QVariantList TranscriptionController::jobs() const
{
    QVariantList list;
    for (const auto &task : m_tasks) {
        // The base row every kind shares; each kind's own extra fields come
        // from its toVariantMap() (the giant per-flag switch this used to be
        // is now distributed authorship — each Task authors its own row).
        QVariantMap entry;
        entry["id"] = task->id;
        entry["fileUrl"] = task->fileUrl;
        entry["status"] = taskStatusToString(task->status);
        entry["error"] = task->error;
        entry["kind"] = task->kind();
        entry["kindLabel"] = task->kindLabel(); // human category label the row shows (BottomBar)
        entry["displayName"] = task->displayName();
        // -1 (QML sees it as a negative number) until Done/Failed.
        entry["durationMs"] = task->durationMs;
        // Whether the finished row should offer a per-row Undo button (the
        // persistent twin of the auto undo snackbar) — a Done task that
        // advertises an undo and can build its inverse. Only Done tasks
        // qualify: makeUndoTask() captures run()'s result.
        entry["undoable"] = task->status == Task::Status::Done && task->isUndoable();
        // Undo is offered (undoable) but no longer applicable — the Tasks-list
        // Undo button greys out rather than vanishing. Re-evaluated on every
        // queueChanged (a later move of the same file fires one), so it flips the
        // moment the file leaves m_resultUrl. See Task::undoStillApplicable().
        entry["undoStale"] = entry["undoable"].toBool() && !task->undoStillApplicable();
        // Batch grouping (0 = standalone). batchLabel is the collapsed row's
        // category label; empty ⇒ collapseBatches() derives it from a member's
        // kindLabel. Both are read only by collapseBatches() (a pure transform
        // that can't reach m_batchLabels), never by the per-row delegate.
        entry["batchId"] = task->batchId;
        entry["batchLabel"] = task->batchId != 0 ? m_batchLabels.value(task->batchId) : QString();
        const QVariantMap extra = task->toVariantMap();
        for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
            entry[it.key()] = it.value();
        list.append(entry);
    }
    return list;
}

QString TranscriptionController::backendKind(const QString &backendName)
{
    if (backendName.isEmpty())
        return QString();
    if (backendName.compare(QStringLiteral("CPU"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("cpu");
    return QStringLiteral("gpu");
}

double TranscriptionController::effectiveWordinessForLength(double baseWordiness, int wordCount)
{
    // Coarse tiers, not a precise formula — this only needs to keep the prelude
    // PROPORTIONATE to the transcript, not fine-tune prose length. Downward
    // caps (item P29): a tiny transcript never gets a disproportionately long
    // prelude no matter how high the slider. Upward floors (item 58): a long
    // transcript is ALLOWED a fuller prelude even at a modest slider setting —
    // a 2-3 hour recording (roughly 20k+ words at ~150 wpm) reaches
    // labelPreludePhrasing()'s top >=0.9 "walk through each major topic" tier.
    if (wordCount < 50)
        return std::min(baseWordiness, 0.15);
    if (wordCount < 150)
        return std::min(baseWordiness, 0.35);
    if (wordCount > 20000)
        return std::max(baseWordiness, 0.9);
    if (wordCount > 6000)
        return std::max(baseWordiness, 0.75);
    if (wordCount > 2000)
        return std::max(baseWordiness, 0.6);
    return baseWordiness;
}

QVariantList TranscriptionController::collapseBatches(const QVariantList &jobEntries)
{
    return JobListFormatter::collapseBatches(jobEntries);
}

QVariantList TranscriptionController::sortJobsForDisplay(const QVariantList &jobEntries)
{
    return JobListFormatter::sortJobsForDisplay(jobEntries);
}

QVariantList TranscriptionController::sortedJobs() const
{
    // Collapse multi-select batches into one row each FIRST, then partition —
    // so a batch lands in a single bucket by its aggregate status (see
    // collapseBatches()), instead of scattering its members across
    // active/queued/finished.
    return sortJobsForDisplay(collapseBatches(jobs()));
}

QVariantList TranscriptionController::availableModels() const
{
    QVariantList list;
    for (const ModelManager::ModelInfo &info : ModelManager::availableModels()) {
        QVariantMap entry;
        entry["id"] = info.id;
        entry["label"] = info.label;
        // Item O31: whether this tier's .bin is already on disk for the
        // CURRENTLY open vault (m_vaultUrl — set once per vault open by
        // loadPersistedQueue(), empty/false for every entry until then) —
        // lets the model dropdown show which tiers would need a fresh
        // download vs. start immediately.
        entry["downloaded"] = ModelManager::isModelDownloaded(info.id);
        list.append(entry);
    }
    return list;
}

QVariantList TranscriptionController::availableSegmentationModels() const
{
    QVariantList list;
    for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableSegmentationModels()) {
        QVariantMap entry;
        entry["id"] = info.id;
        entry["label"] = info.label;
        entry["downloaded"] = ModelManager::isSegmentationModelDownloaded(info.id);
        entry["nonCommercial"] = info.nonCommercial;
        list.append(entry);
    }
    return list;
}

QVariantList TranscriptionController::availableEmbeddingModels() const
{
    QVariantList list;
    for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableEmbeddingModels()) {
        QVariantMap entry;
        entry["id"] = info.id;
        entry["label"] = info.label;
        entry["downloaded"] = ModelManager::isEmbeddingModelDownloaded(info.id);
        entry["nonCommercial"] = info.nonCommercial;
        list.append(entry);
    }
    return list;
}

namespace {

// Human-readable file size for the Models page (SI-ish, matches the "~148 MB"
// style used in the model labels).
QString humanSize(qint64 bytes)
{
    if (bytes <= 0)
        return QString();
    const double kb = bytes / 1024.0;
    if (kb < 1024.0)
        return QStringLiteral("%1 KB").arg(kb, 0, 'f', 0);
    const double mb = kb / 1024.0;
    if (mb < 1024.0)
        return QStringLiteral("%1 MB").arg(mb, 0, 'f', mb < 10 ? 1 : 0);
    return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
}

QVariantMap manageableEntry(const QString &id, const QString &label, const QString &kind, bool downloaded)
{
    QVariantMap entry;
    entry["id"] = id;
    entry["label"] = label;
    entry["kind"] = kind;
    entry["downloaded"] = downloaded;
    entry["bundledOnly"] = ModelManager::isModelBundledOnly(id);
    // Import shape: only the parakeet bundle comes from a FOLDER; every other
    // model (including each single-file diarization segmentation/embedding
    // model) from a single FILE. Lets the page pick the right native dialog.
    entry["multiFile"] = (id == QLatin1String(ModelManager::kParakeetModelId));
    const qint64 size = ModelManager::modelSizeOnDisk(id);
    entry["sizeBytes"] = static_cast<double>(size);
    entry["sizeText"] = humanSize(size);
    return entry;
}

} // namespace

QVariantList TranscriptionController::manageableModels() const
{
    QVariantList list;
    // Transcription models — the dropdown tiers + parakeet.
    for (const ModelManager::ModelInfo &info : ModelManager::availableModels()) {
        list.append(manageableEntry(info.id, info.label, QStringLiteral("transcription"),
                                     ModelManager::isModelDownloaded(info.id)));
    }
    // Support models — no dropdown entry, keyed by their pseudo-ids.
    list.append(manageableEntry(QString::fromLatin1(ModelManager::kLabelModelId),
                                tr("AI auto-labeling (Qwen3-4B) — ~2.5 GB"), QStringLiteral("support"),
                                ModelManager::isLabelModelDownloaded()));
    list.append(manageableEntry(QString::fromLatin1(ModelManager::kVadModelId),
                                tr("Voice-activity detection (Silero VAD) — ~2 MB"), QStringLiteral("support"),
                                ModelManager::isVadModelDownloaded()));
    // Diarization is now a per-model set (selectable segmentation + embedding),
    // each its own downloadable/deletable entry rather than one lumped pair.
    for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableSegmentationModels()) {
        list.append(manageableEntry(info.id, tr("Speaker segmentation — %1").arg(info.label),
                                    QStringLiteral("support"), ModelManager::isSegmentationModelDownloaded(info.id)));
    }
    for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableEmbeddingModels()) {
        list.append(manageableEntry(info.id, tr("Speaker embedding — %1").arg(info.label),
                                    QStringLiteral("support"), ModelManager::isEmbeddingModelDownloaded(info.id)));
    }
    return list;
}

void TranscriptionController::downloadModel(const QString &modelId)
{
    ModelDownloadTarget target = ModelDownloadTarget::WhisperModel; // whisper tiers + parakeet
    if (modelId == QLatin1String(ModelManager::kLabelModelId)) {
        target = ModelDownloadTarget::LabelModel;
    } else if (modelId == QLatin1String(ModelManager::kVadModelId)) {
        target = ModelDownloadTarget::VadModel;
    } else {
        for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableSegmentationModels()) {
            if (info.id == modelId) {
                target = ModelDownloadTarget::SegmentationModel;
                break;
            }
        }
        for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableEmbeddingModels()) {
            if (info.id == modelId) {
                target = ModelDownloadTarget::EmbeddingModel;
                break;
            }
        }
    }

    // Same queue path used lazily before a transcribe — inserted at the tail so
    // it doesn't jump ahead of in-flight work. No-op if already downloaded or
    // already queued (maybeQueueDownloadTask returns -1).
    if (maybeQueueDownloadTask(target, m_vaultUrl, modelId) >= 0) {
        emit queueChanged();
        startNext();
    }
}

void TranscriptionController::deleteModel(const QString &modelId)
{
    QString error;
    const bool ok = ModelManager::deleteModel(modelId, &error);
    emit modelOperationFinished(QStringLiteral("delete"), modelId, ok, error);
    if (ok)
        emit modelsChanged();
}

void TranscriptionController::importModel(const QString &modelId, const QUrl &sourceUrl)
{
    // Wave N (task system): a REAL queued task now, not a fire-and-forget
    // QThread with no Tasks row. Serial lane — a multi-GB copy is heavy on the
    // disk, so it queues behind (and never thrashes against) a transcription;
    // Background presentation keeps it off the main-panel overlay (see
    // ModelIoTask). Result re-emitted as modelOperationFinished()/modelsChanged()
    // by connectTask(), so the Models page's existing wiring is unchanged.
    enqueueTask(std::make_unique<ModelIoTask>(ModelIoTask::Direction::Import, modelId, sourceUrl));
}

void TranscriptionController::exportModel(const QString &modelId, const QUrl &destDirUrl)
{
    enqueueTask(std::make_unique<ModelIoTask>(ModelIoTask::Direction::Export, modelId, destDirUrl));
}

void TranscriptionController::runBenchmark(const QUrl &fileUrl, const QStringList &modelIds)
{
    // Wave N (task system): a REAL queued task now. This used to bypass the
    // queue entirely — invoking a worker slot directly, with "no queue entry,
    // no file writes" and its own parallel setBenchmarkGlow() path — purely
    // because a queue entry would have raised MainPanel's blocking overlay
    // (m_busy was overloaded to mean both "glow" and "overlay"). That escape
    // hatch IS the bug class this refactor exists to remove: a task that runs
    // for minutes with no Tasks row and no cancel/progress plumbing of its
    // own. Presentation::Background now expresses "glow + row, no overlay"
    // directly, so it can just be enqueued like everything else — and it
    // inherits the queue's serialization (no GPU contention with a real
    // transcription) for free, which is what the manual worker dispatch was
    // hand-rolling.
    enqueueTask(std::make_unique<BenchmarkTask>(fileUrl, m_vaultUrl, modelIds));
}

void TranscriptionController::runDiarizationBenchmark(const QUrl &fileUrl, const QVariantList &combos)
{
    // Same story as runBenchmark() above.
    enqueueTask(std::make_unique<DiarizationSweepTask>(fileUrl, combos));
}

void TranscriptionController::cancelBenchmark()
{
    // Cancels whichever benchmark/sweep task is Active or Queued. Now that
    // both are real queued tasks, this is just cancelJob() over the two kinds
    // rather than a bare requestCancel() at the worker (which would also have
    // cancelled an unrelated real job that happened to be running).
    for (int i = static_cast<int>(m_tasks.size()) - 1; i >= 0; --i) {
        const QString kind = m_tasks[i]->kind();
        if (kind == QStringLiteral("benchmark") || kind == QStringLiteral("diarizationSweep"))
            cancelJob(m_tasks[i]->id);
    }
}

QVariantList TranscriptionController::availableEngines() const
{
    QVariantList list;
    const QString vaultPath = m_vaultUrl.toLocalFile();
    for (const EngineDescriptor &desc : discoverEngines(vaultPath)) {
        QVariantMap entry;
        entry["id"] = desc.id;
        entry["modelId"] = QString::fromLatin1(kEngineIdPrefix) + desc.id;
        entry["label"] = desc.manifestValid ? desc.manifest.name : desc.id;
        entry["available"] = desc.available;
        entry["unavailableReason"] = desc.unavailableReason;
        // "downloaded" mirrors the model dropdown's own flag so the UI can
        // reuse the same treatment; for an engine it's specifically whether the
        // engine's model file is present for this vault.
        entry["downloaded"] = ModelManager::isEngineModelPresent(desc.id);
        list.append(entry);
    }
    return list;
}
