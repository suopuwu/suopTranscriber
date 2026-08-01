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

// Priority order rebuildJobStatusIndex() uses to pick ONE status
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


// "auto-categorize" mode: the existing tag vocabulary for a vault.
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
    // IF the combined window still fits kMaxChapterInputChars. Bug:
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

// Task system: TranscriptionWorker is GONE (1601 lines). It was one
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
    // estimatedRemainingMs()'s stall check: record the LAST time the bar
    // actually moved forward, not just the last time this signal fired (a
    // redundant re-notify at the same value — e.g. the reset to 0.0 in
    // onSerialTaskStarting() — must not look like fresh progress). A
    // genuinely wedged transcription (2026-07-24 batch-wedge report) leaves
    // overallProgress frozen for the rest of the job, so this timestamp stops
    // advancing right along with it.
    connect(m_phaseTimer, &PhaseTimer::overallProgressChanged, this, [this] {
        const double current = m_phaseTimer->overallProgress();
        if (current > m_lastProgressTrackedValue) {
            m_lastProgressTrackedValue = current;
            m_lastProgressAdvanceMs = activeElapsedMs();
        }
    });
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
        recomputeOverallProgress();       // WeightedPhases no-callback phases
        advanceSelfCumulativeSynthetic(); // SelfCumulative frozen-bar fallback
    });

    // PHASE Q: the display-blind queue engine. It owns the task list, the two
    // worker threads/runners, and the pause/cancel intent; this class is its
    // observer (all display + typed emits). Not parented to `this` so it's
    // deleted (and its threads shut down) explicitly at the very top of the
    // destructor, while this observer is still fully valid.
    m_scheduler = new TaskScheduler(this);
    // Re-emit the scheduler's every queue mutation as this class's own
    // queueChanged — a DIRECT connection, so the ordering (and the synchronous
    // persistQueue + rebuildJobStatusIndex below, which drive off THIS signal)
    // is preserved exactly as when the queue lived here.
    connect(m_scheduler, &TaskScheduler::queueChanged, this, &TranscriptionController::queueChanged);
    connect(this, &TranscriptionController::queueChanged, this, &TranscriptionController::persistQueue);
    connect(this, &TranscriptionController::queueChanged, this, &TranscriptionController::rebuildJobStatusIndex);
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
            // A real fraction has arrived — the synthetic frozen-bar fallback
            // (advanceSelfCumulativeSynthetic()) stands down for the rest of the
            // job so it can never run ahead of the true value.
            m_activeHadRealProgress = true;
            m_indeterminatePhaseTimer->stop();
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
        // gets its ticks from the real Task::progress signal instead. It ALSO
        // drives the SelfCumulative frozen-bar fallback: a SelfCumulative task
        // that hasn't reported real progress needs the ramp for ANY phase, not
        // just the three WeightedPhases names — a real progress() tick stops the
        // timer and takes over (see the progress handler above).
        Task *active = m_scheduler->activeTask();
        const bool selfCumulativeNeedsSynthetic = active
            && active->progressModel() == Task::ProgressModel::SelfCumulative
            && !m_activeHadRealProgress;
        if (PhaseTimer::isSyntheticProgressPhase(phase) || selfCumulativeNeedsSynthetic)
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
    // The prompt-shutdown thread teardown (bug report 2026-07-09: never leave a
    // transcription running after the GUI closes) moved into ~TaskScheduler with
    // the threads. Delete it FIRST — explicitly, while this observer is still
    // fully valid — so the requestCancel()+wait() runs before any of this
    // class's own members are torn down.
    delete m_scheduler;
    m_scheduler = nullptr;
}

// The phase-timing subsystem lives in PhaseTimer now — the
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
    if (auto *t = qobject_cast<const TranscribeTask *>(m_scheduler->activeTask()))
        return t->modelId();
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
    Task *task = m_scheduler->activeTask();
    const bool weightedPhases =
        task && task->progressModel() == Task::ProgressModel::WeightedPhases;
    m_phaseTimer->recomputeOverallProgress(
        weightedPhases ? task->progressPhases() : QStringList(), weightedPhases, m_phase,
        activeModelIdForTiming(), m_activeSpeakerDetection, m_progress,
        activeElapsedMs() - m_phaseStartMs);
}

void TranscriptionController::advanceSelfCumulativeSynthetic()
{
    Task *task = m_scheduler->activeTask();
    // Only a SelfCumulative task that has emitted a phase but no real progress
    // yet needs synthesizing — a WeightedPhases task is handled by
    // recomputeOverallProgress(), and a real progress() tick has already stopped
    // this timer for a SelfCumulative one.
    if (!task || task->progressModel() != Task::ProgressModel::SelfCumulative
        || m_activeHadRealProgress || m_phase.isEmpty())
        return;
    const double synthetic = m_phaseTimer->selfCumulativeSyntheticProgress(
        m_phase, task->kind(), activeModelIdForTiming(), activeElapsedMs());
    // Only ever raise the bar — the ramp is monotonic and clamped (<0.95), so it
    // moves the frozen 0% forward without ever reversing or claiming completion.
    if (synthetic > m_phaseTimer->overallProgress())
        m_phaseTimer->setOverallProgress(synthetic);
}

bool TranscriptionController::isQueued(const QUrl &fileUrl) const
{
    return TaskQuery::isQueued(m_scheduler->tasks(), fileUrl);
}

void TranscriptionController::rebuildJobStatusIndex()
{
    QHash<QString, Task::Status> rebuilt;
    for (const auto &task : m_scheduler->tasks()) {
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

// The ONE way a task enters the queue — a thin forwarder to the scheduler (the
// mechanics moved there; queuePrerequisites, run inside, calls back into
// resolveDownloadPrerequisite() for the model knowledge).
int TranscriptionController::enqueueTask(std::unique_ptr<Task> task, int insertIndex)
{
    return m_scheduler->enqueueTask(std::move(task), insertIndex);
}

void TranscriptionController::setActiveRecordingPath(const QString &path)
{
    if (m_activeRecordingPath == path)
        return;
    m_activeRecordingPath = path;
    m_scheduler->setActiveRecordingUrl(path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path));
    emit activeRecordingPathChanged();
}

void TranscriptionController::transcribe(const QUrl &fileUrl, const QUrl &vaultUrl, const QString &modelId,
                                          bool autoLabel, bool restrictTagsToExisting,
                                          double tagReuseBias, double wordiness)
{
    // Done/Failed entries for the same fileUrl deliberately do NOT block
    // re-queueing (see class doc) — only Queued/Active does.
    if (isQueued(fileUrl))
        return;

    // A transcribe call ONLY transcribes now. It used to chain a
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
    m_scheduler->withBatch(QString(), [&] {
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
    // Per-KIND dedup, not per-file: a transcribe still in flight for this file
    // must not block queueing detect-speakers behind it (it lands after the
    // transcribe via the ordered insert below) — only a diarize already in
    // flight blocks a duplicate.
    if (TaskQuery::isKindQueued(m_scheduler->tasks(), fileUrl, QStringLiteral("diarize")))
        return;

    // Empty id ⇒ the ModelManager default (keeps the task's recorded ids
    // concrete for persistence + the download dedup key).
    const QString segId = segmentationModelId.isEmpty()
                              ? QString::fromLatin1(ModelManager::kDefaultSegmentationModelId)
                              : segmentationModelId;
    const QString embedId = embeddingModelId.isEmpty()
                                ? QString::fromLatin1(ModelManager::kDefaultEmbeddingModelId)
                                : embeddingModelId;

    enqueueTask(std::make_unique<DiarizeTask>(fileUrl, vaultUrl, segId, embedId, sensitivity),
                TaskQuery::insertIndexAfterFileTasks(m_scheduler->tasks(), fileUrl));
}

void TranscriptionController::diarizeFilesForPaths(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                                   const QString &segmentationModelId,
                                                   const QString &embeddingModelId, double sensitivity)
{
    // One batch (derived "Detect speakers" label) — see transcribeAll().
    m_scheduler->withBatch(QString(), [&] {
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
    // Per-KIND dedup, not per-file: a transcribe still in flight for this file
    // must not block queueing a relabel behind it (it lands after the
    // transcribe via the ordered insert below) — only a relabel already in
    // flight blocks a duplicate.
    if (TaskQuery::isKindQueued(m_scheduler->tasks(), fileUrl, QStringLiteral("relabel")))
        return;

    enqueueTask(std::make_unique<RelabelTask>(fileUrl, vaultUrl, restrictTagsToExisting, tagReuseBias, wordiness,
                                               /*skipIfAlreadyLabeled=*/false),
                TaskQuery::insertIndexAfterFileTasks(m_scheduler->tasks(), fileUrl));
}

void TranscriptionController::relabelAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                          bool restrictTagsToExisting, double tagReuseBias, double wordiness)
{
    // One batch (derived "Label" label) — see transcribeAll().
    m_scheduler->withBatch(QString(), [&] {
        for (const QVariant &v : fileUrls)
            relabel(v.toUrl(), vaultUrl, restrictTagsToExisting, tagReuseBias, wordiness);
    });
}

void TranscriptionController::generateChapters(const QUrl &fileUrl, const QUrl &vaultUrl, double density)
{
    // Per-KIND dedup, not per-file — see relabel()'s doc comment.
    if (TaskQuery::isKindQueued(m_scheduler->tasks(), fileUrl, QStringLiteral("chapters")))
        return;

    enqueueTask(std::make_unique<ChaptersTask>(fileUrl, vaultUrl, density),
                TaskQuery::insertIndexAfterFileTasks(m_scheduler->tasks(), fileUrl));
}

void TranscriptionController::generateChaptersAll(const QVariantList &fileUrls, const QUrl &vaultUrl, double density)
{
    // One batch (derived "Generate chapters" label) — see transcribeAll().
    m_scheduler->withBatch(QString(), [&] {
        for (const QVariant &v : fileUrls)
            generateChapters(v.toUrl(), vaultUrl, density);
    });
}

QStringList TranscriptionController::pendingJobKinds(const QUrl &fileUrl) const
{
    // No .md at all ⇒ nothing has run.
    const QString mdPath = sidecarPath(fileUrl.toLocalFile());
    if (!QFileInfo::exists(mdPath))
        return {QStringLiteral("transcribe"), QStringLiteral("diarize"), QStringLiteral("relabel"),
                QStringLiteral("chapters")};

    QStringList frontmatterLines;
    QString body;
    // A malformed/unreadable .md still counts as transcribed (the file exists —
    // re-transcribing would clobber it); everything derived from it is unknown,
    // so offer the derived passes.
    if (!MdFrontmatter::read(mdPath, &frontmatterLines, &body, nullptr))
        return {QStringLiteral("diarize"), QStringLiteral("relabel"), QStringLiteral("chapters")};

    QString speakerDetection, label, tags;
    for (const QString &line : std::as_const(frontmatterLines)) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        if (key == QStringLiteral("speakerDetection"))
            speakerDetection = value;
        else if (key == QStringLiteral("label"))
            label = value;
        else if (key == QStringLiteral("tags"))
            tags = value;
    }

    QStringList pending;
    if (!MdFrontmatter::isTrue(speakerDetection))
        pending << QStringLiteral("diarize");
    if (!transcriptIsLabeled(label, tags, body))
        pending << QStringLiteral("relabel");
    // Same scan readChapters() uses, so "has chapters" means exactly what the
    // TOC shows (manual markers included — a hand-added chapter is still a
    // reason not to re-run the generator).
    if (!chapterMarkerPattern().match(stripPreludeIfPresent(body)).hasMatch())
        pending << QStringLiteral("chapters");
    return pending;
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

    // Only the steps that haven't run yet (see pendingJobKinds()). Nothing left
    // ⇒ enqueue nothing at all, so the button is a visible no-op on a fully
    // processed file rather than a surprise re-run.
    const QStringList pending = pendingJobKinds(fileUrl);
    if (pending.isEmpty())
        return;

    // Jump ahead of whatever's already Queued, but keep this file's
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
        const int before = static_cast<int>(m_scheduler->tasks().size());
        enqueueTask(std::move(task), insertAt);
        insertAt += static_cast<int>(m_scheduler->tasks().size()) - before;
    };

    // (1) transcribe (pure ASR).
    if (pending.contains(QStringLiteral("transcribe")))
        step(std::make_unique<TranscribeTask>(fileUrl, vaultUrl, modelId));
    // (2) detect speakers — reads the .md the transcribe step (if any) just
    // wrote and splices "[Speaker N]" tags in.
    if (pending.contains(QStringLiteral("diarize")))
        step(std::make_unique<DiarizeTask>(fileUrl, vaultUrl, segId, embedId, sensitivity));
    // (3) label — reads the .md the diarize step just updated.
    if (pending.contains(QStringLiteral("relabel")))
        step(std::make_unique<RelabelTask>(fileUrl, vaultUrl, restrictTagsToExisting, tagReuseBias, wordiness,
                                            /*skipIfAlreadyLabeled=*/false));
    // (4) chapters — reads the labeled .md.
    if (pending.contains(QStringLiteral("chapters")))
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
    m_scheduler->withBatch(QStringLiteral("Run all"), [&] {
        for (const QVariant &v : fileUrls)
            runAllJobs(v.toUrl(), vaultUrl, modelId, restrictTagsToExisting, tagReuseBias, wordiness, chapterDensity,
                       segmentationModelId, embeddingModelId, sensitivity);
    });
}

// --- TaskSchedulerObserver: serial start (verbatim the old startTask display
// block, minus the scheduler mechanics). Golden emission order; connectTask
// (which emits nothing) runs at the end, before the scheduler's queueChanged.
void TranscriptionController::onSerialTaskStarting(Task *task, bool wasCancelling)
{
    m_activePaused = false;
    emit activePausedChanged();
    if (wasCancelling)
        emit activeStoppingChanged();
    // Elapsed-time tracking (activeElapsedMs()) restarts from zero for every new
    // Active task.
    m_activeAccumulatedMs = 0;
    m_activeSegmentTimer.start();
    m_phaseTimer->setOverallProgress(0.0);
    // A fresh job must never inherit the previous job's stall clock — see
    // estimatedRemainingMs()'s doc comment. The setOverallProgress(0.0) just
    // above won't naturally reset these (0.0 isn't > a stale tracked value
    // left over from a job that finished near 1.0).
    m_lastProgressAdvanceMs = 0;
    m_lastProgressTrackedValue = 0.0;
    // See recomputeOverallProgress()'s doc comment — true ONLY for a "diarize"
    // task, whose progress bar is weighted around the detecting-speakers phase.
    m_activeSpeakerDetection = task->kind() == QStringLiteral("diarize");
    m_phaseStartMs = 0;
    m_indeterminatePhaseTimer->stop();

    m_busy = true;
    emit busyChanged();
    // Presentation, not m_busy, decides the main-panel overlay: a Background
    // task (benchmark/sweep/download) gets a Tasks row + glow without hijacking
    // the panel the user is reading.
    const bool foreground = task->presentation() == Task::Presentation::Foreground;
    if (m_activeIsForeground != foreground) {
        m_activeIsForeground = foreground;
        emit activeIsForegroundChanged();
    }
    m_progress = 0.0;
    m_activeHadRealProgress = false;
    emit progressChanged();
    // Cleared so a kind that never emits phase() doesn't inherit a stale phase.
    m_phase.clear();
    emit currentPhaseChanged();
    m_currentFile = task->fileUrl;
    emit currentFileChanged();
    emit currentJobKindChanged();

    // Connected exactly once, on first dispatch (a resumed Paused task re-enters
    // this, so guard against double-wiring). Emits nothing.
    if (!m_connectedTasks.contains(task)) {
        m_connectedTasks.insert(task);
        connectTask(task);
    }
}

// --- TaskSchedulerObserver: instant start. Just the connectTask guard — an
// instant task never touches the serial-lane display state.
void TranscriptionController::onInstantTaskStarting(Task *task)
{
    if (!m_connectedTasks.contains(task)) {
        m_connectedTasks.insert(task);
        connectTask(task);
    }
}

// --- TaskSchedulerObserver: serial finish phase 1 ("active work ended").
void TranscriptionController::onSerialActiveEnded(bool wasStopping)
{
    m_busy = false;
    emit busyChanged();
    if (m_activeIsForeground) {
        m_activeIsForeground = false;
        emit activeIsForegroundChanged();
    }
    // A task that fails/gets cancelled mid-"loading"/"decoding" never gets
    // another phase() signal to naturally stop this.
    m_indeterminatePhaseTimer->stop();
    // Snapshot the finishing file so the terminal finished() report names it
    // even after pumpQueues() has moved m_currentFile on to the next task.
    m_finishingFileUrl = m_currentFile;
    if (wasStopping)
        emit activeStoppingChanged();
}

// --- TaskSchedulerObserver: serial finish phase 2 ("terminal status reached").
// Fires the central modelsChanged / benchmarkFailed emits, returns the task's
// final active-work duration for the scheduler to stamp.
qint64 TranscriptionController::onSerialTaskTerminal(Task *task, bool ok, const QString &error)
{
    // Any task whose completion changes the on-disk model set (a landed
    // download, a successful model import) refreshes the Models page — ONE
    // central emit keyed off Task::completionChangesModelSet().
    if (ok && task->completionChangesModelSet())
        emit modelsChanged();
    // A benchmark / diarization-sweep that throws BEFORE emitting its
    // run-finished summary otherwise pins the Benchmark page on "Running…"
    // forever. A user cancel already reported via the summary, so skip it.
    const QString failedKind = task->kind();
    if (!ok && error != QStringLiteral("Cancelled")
        && (failedKind == QStringLiteral("benchmark") || failedKind == QStringLiteral("diarizationSweep")))
        emit benchmarkFailed(error);
    // Active-work elapsed (excludes paused stretches — see activeElapsedMs()).
    // Read while the scheduler's active slot is still this task (m_activeIndex
    // not yet cleared), matching the old timing.
    const qint64 elapsed = activeElapsedMs();
    // Teach the SelfCumulative frozen-bar fallback: a task that completed OK
    // having emitted phase() but never a real progress() tick (the very case the
    // ramp exists for) just measured its final phase's real duration — record it
    // so next time the ramp is elapsed-vs-real-average, not vs a fixed guess.
    // m_phase/elapsed still hold this task's final values here.
    if (ok && !m_activeHadRealProgress && !m_phase.isEmpty()
        && task->progressModel() == Task::ProgressModel::SelfCumulative)
        m_phaseTimer->recordSelfCumulativePhaseDuration(m_phase, task->kind(),
                                                        activeModelIdForTiming(), elapsed);
    return elapsed;
}

// --- TaskSchedulerObserver: serial finish phase 3 ("active slot cleared").
void TranscriptionController::onSerialActiveCleared(bool wasCancelling)
{
    m_activePaused = false;
    emit activePausedChanged();
    if (wasCancelling)
        emit activeStoppingChanged();
}

// --- TaskSchedulerObserver: queue idle drain. Clears the "what's running" UI.
void TranscriptionController::onQueueDrained()
{
    m_currentFile = QUrl();
    emit currentFileChanged();
    emit currentJobKindChanged();
}

// --- TaskSchedulerObserver: the terminal finished() report. threeArg
// reproduces the pausedAsRestart branch's 3-arg emit (omits structural).
void TranscriptionController::reportSerialFinished(bool ok, const QString &error, bool structural, bool threeArg)
{
    if (threeArg)
        emit finished(m_finishingFileUrl, ok, error);
    else
        emit finished(m_finishingFileUrl, ok, error, structural);
}

// --- TaskSchedulerObserver: the instant lane's terminal finished() report.
void TranscriptionController::reportInstantFinished(Task *task, const QUrl &finishedFile, bool ok,
                                                    const QString &error)
{
    emit finished(finishedFile, ok, error, task->isStructuralFileOp());
}

// --- TaskSchedulerObserver: the undo-snackbar emit. The scheduler computes the
// token+label from queue state (maybeEmitUndoAvailable); this only emits.
void TranscriptionController::emitUndoAvailable(int token, const QString &label)
{
    emit undoAvailable(token, label);
}

void TranscriptionController::undoTask(int token)
{
    m_scheduler->undoTask(token);
}

// --- TaskSchedulerObserver: forget a task the scheduler is erasing, from the
// connectTask wired-once guard (heap reuse would otherwise falsely read as
// already-connected).
void TranscriptionController::onTaskRemoved(Task *task)
{
    m_connectedTasks.remove(task);
}

void TranscriptionController::dismiss(const QUrl &fileUrl)
{
    m_scheduler->dismiss(fileUrl);
}

void TranscriptionController::clearFinished()
{
    m_scheduler->clearFinished();
}

qint64 TranscriptionController::activeElapsedMs() const
{
    if (m_scheduler->activeIndex() < 0)
        return 0;
    return m_activeAccumulatedMs + (m_activePaused ? 0 : m_activeSegmentTimer.elapsed());
}

// See estimatedRemainingMs()'s doc comment and the kStalledEtaMs/
// etaStallThresholdMs doc comments on the header. Constants deliberately
// generous in both directions: the floor keeps an ordinary brief gap between
// progress ticks (even a slow one) from misreading as "stalled", and the
// multiplier scales that tolerance UP for a phase whose own historical pace
// is already slow (a big model's "transcribing" phase legitimately goes
// longer between ticks than a tiny model's does) — this is a DISPLAY-only
// judgment call (unlike TranscribeTask's stall watchdog, which actually
// aborts real work and so must be far more conservative), so erring toward
// "still shows a number a bit too long" over "flips to stalled too eagerly"
// is the safer direction here too.
qint64 TranscriptionController::etaStallThresholdMs(double phaseAverageDurationMs)
{
    constexpr qint64 kFloorMs = 60'000; // 1 minute, regardless of phase pace
    constexpr double kMultiplier = 10.0;
    return qMax<qint64>(kFloorMs, static_cast<qint64>(kMultiplier * phaseAverageDurationMs));
}

qint64 TranscriptionController::estimatedRemainingMs() const
{
    const double overall = m_phaseTimer->overallProgress();
    if (m_scheduler->activeIndex() < 0 || overall <= 0.0)
        return -1;
    const qint64 sinceLastAdvance = activeElapsedMs() - m_lastProgressAdvanceMs;
    const qint64 stallThreshold = etaStallThresholdMs(
        m_phaseTimer->phaseAverageDurationMs(m_phase, activeModelIdForTiming(), m_activeSpeakerDetection));
    if (sinceLastAdvance > stallThreshold)
        return kStalledEtaMs;
    const double remainingFraction = (1.0 - overall) / overall;
    const qint64 remaining = static_cast<qint64>(activeElapsedMs() * remainingFraction);
    // Belt-and-suspenders ceiling: the stall check above is the real fix, but
    // this catches any extrapolation the stall check doesn't (e.g. a job that
    // IS still ticking, just at a near-zero rate) before it reaches the
    // "~114 hours left" territory that prompted this whole change.
    constexpr qint64 kEtaCeilingMs = 48LL * 60 * 60 * 1000; // 48h
    return qMin(remaining, kEtaCeilingMs);
}

void TranscriptionController::pauseActive()
{
    Task *task = m_scheduler->activeTask();
    if (!task)
        return;
    // The task itself declares whether it can pause — the string allowlist
    // (transcribe/download/combine/chapters) is gone; a kind with a real
    // interruption point returns capabilities().pausable, and everything else
    // is a safe no-op here. This is the same capability the UI reads to enable
    // the pause button, so the two can never disagree (the 2026-07-09 bug was
    // exactly such a disagreement).
    if (!task->capabilities().pausable)
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
    if (m_scheduler->isPausingActiveAsRestart())
        return;
    m_scheduler->requestPause();
}

void TranscriptionController::resumeActive()
{
    m_scheduler->requestResume();
}

void TranscriptionController::togglePauseActive()
{
    // See this method's header doc comment for the race it exists to close
    // — mirrors BottomBar.qml's old onTapped branching exactly, just reading
    // m_busy/m_activePaused/m_phase (this object's own state) instead of
    // QML's copies of them.
    if (!m_busy) {
        m_scheduler->startNext();
        return;
    }
    if (m_activePaused) {
        resumeActive();
        return;
    }
    // A soft-pause-as-restart is already in flight and hasn't resolved yet
    // (see activeStopping's doc comment) — ignore further clicks rather than
    // stacking a second request on top of the still-pending cancel.
    if (m_scheduler->isPausingActiveAsRestart())
        return;
    // "loading" (whisper.cpp's blocking, unhookable
    // model-load call) has no cooperative checkpoint to pause AT. It used to
    // silently reinterpret the tap as cancelActive() + present the job as
    // Paused-via-restart — the "pause broken again" report: a click that
    // looked like it did nothing, and could latch a stray cancel that broke
    // pause for the rest of the job. That reinterpretation is gone: the
    // bottom bar now DISABLES the pause button during "loading" (see
    // BottomBar.qml's pauseButton.enabled) so this is never reached from the
    // UI during that phase, and pauseActive() below is a safe no-op for
    // "loading" if it somehow is. A pause is never turned into a cancel here.
    // (NOTE: a genuine mid-loading pause would
    // need a real interruption hook that whisper.cpp doesn't expose today;
    // the JobStatus::Paused/activeStopping machinery is left dormant for
    // whenever that lands, not removed.)
    pauseActive();
}

// The cancel/dismiss/move family is pure queue mechanics — thin forwarders.
void TranscriptionController::cancelActive() { m_scheduler->cancelActive(); }
void TranscriptionController::cancelJob(int id) { m_scheduler->cancelJob(id); }
void TranscriptionController::cancelBatch(int batchId) { m_scheduler->cancelBatch(batchId); }
void TranscriptionController::dismissBatch(int batchId) { m_scheduler->dismissBatch(batchId); }
void TranscriptionController::cancelAll() { m_scheduler->cancelAll(); }
void TranscriptionController::moveQueuedJob(int id, int newIndex) { m_scheduler->moveQueuedJob(id, newIndex); }

bool TranscriptionController::isDownloadQueued(ModelDownloadTarget target, const QUrl &vaultUrl,
                                                const QString &modelId) const
{
    return TaskQuery::isDownloadQueued(m_scheduler->tasks(), target, vaultUrl, modelId);
}

// Wrinkle 1: the scheduler's queuePrerequisites() calls this back through the
// observer so ModelManager (and DownloadTask construction) stays out of the
// scheduler. Returns the id, or -1 if no download was needed.
int TranscriptionController::resolveDownloadPrerequisite(ModelDownloadTarget target, const QUrl &vaultUrl,
                                                         const QString &modelId, int insertIndex)
{
    return maybeQueueDownloadTask(target, vaultUrl, modelId, insertIndex);
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

    // The queue MECHANICS (id allocation + insertion) live on the scheduler; the
    // controller only owns the model-knowledge decision above + building the
    // DownloadTask here.
    auto task = std::make_unique<DownloadTask>(target, modelId);
    task->vaultUrl = vaultUrl;
    return m_scheduler->insertResolvedDownload(std::move(task), insertIndex);
}

// See this method's header doc comment.
int TranscriptionController::frontOfQueueInsertIndex() const
{
    return m_scheduler->frontOfQueueInsertIndex();
}

int TranscriptionController::combineStabilityWindowSecs() const
{
    return int(CombineScanner::kCombineStabilityWindowSecs);
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

// The combine QUEUE glue (isCombineQueued/enqueueCombineJob/
// maybeRedirectForCombine/enqueueReadyCombineGroups) moved to TaskScheduler —
// it mutates the task list + m_combineRules/m_combineAudioPaths cache the
// scheduler owns. These two Q_INVOKABLEs stay thin forwarders so QML is unchanged.
void TranscriptionController::runCombineScan(const QVariantList &rules, const QStringList &audioPaths,
                                               const QUrl &vaultUrl, bool deleteOriginals)
{
    m_scheduler->runCombineScan(rules, audioPaths, vaultUrl, deleteOriginals);
}

void TranscriptionController::combineGroup(const QVariantList &rules, const QStringList &audioPaths,
                                            const QString &outputPath, const QUrl &vaultUrl, bool deleteOriginals)
{
    m_scheduler->combineGroup(rules, audioPaths, outputPath, vaultUrl, deleteOriginals);
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
    const QJsonArray array = TaskQuery::serializeQueue(m_scheduler->tasks());

    QSaveFile file(tasksJsonPath(m_vaultUrl));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    file.commit();
}

void TranscriptionController::loadPersistedQueue(const QUrl &vaultUrl)
{
    // Remember the vault for subsequent persistQueue() writes (controller-side
    // persistence state), then let the scheduler replay the saved queue (the
    // file read + createTaskFromJson + raw insert are queue mechanics).
    m_vaultUrl = vaultUrl;
    m_scheduler->loadPersistedQueue(vaultUrl);
}

void TranscriptionController::startNext()
{
    m_scheduler->startNext();
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
    // See m_transcriptCacheMutex's doc comment — this function is
    // now also called from loadTranscriptAsync()'s background thread.
    QMutexLocker locker(&m_transcriptCacheMutex);

    const QString mdPath = sidecarPath(fileUrl.toLocalFile());
    const QFileInfo fi(mdPath);
    if (!fi.exists())
        return QVariantMap();

    // A cheap stat guard — frontmatter is written once up front
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
        // The boolean-token rule is single-sourced in MdFrontmatter so this
        // typed reader can't diverge from VaultTreeModel::frontmatterField's
        // string reader on what "true"/"false" mean.
        if (MdFrontmatter::isTrue(value))
            result[key] = true;
        else if (MdFrontmatter::isFalse(value))
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
    // This manual (non-queued) rewrite otherwise leaves the
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
    // The sidebar's tag-derived display (if any)
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
    // See setLabel()'s matching emit for why
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
        // The manual sentinel is stripped from the DISPLAYED title
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
    // panel refresh this file's chapters immediately.
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
                static_cast<int>(m_scheduler->tasks().size()));
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
                static_cast<int>(m_scheduler->tasks().size()));
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
    enqueueTask(std::make_unique<RenameFileTask>(fileUrl, newBaseName), static_cast<int>(m_scheduler->tasks().size()));
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
    m_scheduler->enqueueBatch(std::move(tasks));
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
                static_cast<int>(m_scheduler->tasks().size()));
}

void TranscriptionController::deletePermanently(const QString &scope, const QVariantList &urls, const QUrl &vaultUrl)
{
    const QList<QUrl> list = urlsFromVariant(urls);
    if (list.isEmpty())
        return;
    enqueueTask(std::make_unique<DeleteTask>(scopeFromString(scope), list, vaultUrl),
                static_cast<int>(m_scheduler->tasks().size()));
}

void TranscriptionController::renameFolder(const QUrl &folderUrl, const QString &newName, const QUrl &vaultUrl)
{
    enqueueTask(std::make_unique<FolderRenameTask>(folderUrl, newName, vaultUrl),
                static_cast<int>(m_scheduler->tasks().size()));
}

void TranscriptionController::moveFolder(const QUrl &folderUrl, const QUrl &destParentUrl, const QUrl &vaultUrl)
{
    enqueueTask(std::make_unique<FolderMoveTask>(folderUrl, destParentUrl, vaultUrl),
                static_cast<int>(m_scheduler->tasks().size()));
}

QString TranscriptionController::readTranscriptBody(const QUrl &fileUrl) const
{
    // See m_transcriptCacheMutex's doc comment.
    QMutexLocker locker(&m_transcriptCacheMutex);

    const QString mdPath = sidecarPath(fileUrl.toLocalFile());
    const QFileInfo fi(mdPath);
    if (!fi.exists())
        return QString();

    // See m_bodyCache's doc comment — during an ACTIVE
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
    // See m_transcriptCacheMutex's doc comment — RECURSIVE lock,
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

    // Segments are only ever APPENDED to a live-running
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

bool TranscriptionController::isSameFile(const QUrl &a, const QUrl &b) const
{
    // Decoded local paths, not toString(): the two URLs may have been built
    // differently (fromLocalFile vs a QUrl(string)) and so encode a space as a
    // literal ' ' on one side and "%20" on the other — see the header doc.
    // Both empty (neither is a local file) is not "the same file".
    const QString pa = a.toLocalFile();
    const QString pb = b.toLocalFile();
    return !pa.isEmpty() && pa == pb;
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
    // first real sentence.
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
    return TaskQuery::tasksTotal(m_scheduler->tasks());
}

int TranscriptionController::tasksCompleted() const
{
    return TaskQuery::tasksCompleted(m_scheduler->tasks());
}

QVariantList TranscriptionController::queuedFileUrls() const
{
    return TaskQuery::queuedFileUrls(m_scheduler->tasks());
}

QString TranscriptionController::currentJobKind() const
{
    Task *task = m_scheduler->activeTask();
    return task ? task->kind() : QString();
}

QString TranscriptionController::currentJobKindLabel() const
{
    Task *task = m_scheduler->activeTask();
    return task ? task->kindLabel() : QString();
}

QString TranscriptionController::phaseLabel(const QString &phase) const
{
    return JobListFormatter::phaseLabel(phase);
}

QString TranscriptionController::phaseOverlayLabel(const QString &phase) const
{
    return JobListFormatter::phaseOverlayLabel(phase);
}

QString TranscriptionController::modelShortLabel(const QString &modelId) const
{
    return JobListFormatter::modelShortLabel(modelId);
}

bool TranscriptionController::activeCanPause() const
{
    Task *task = m_scheduler->activeTask();
    return task && task->capabilities().pausable;
}

QVariantList TranscriptionController::jobs() const
{
    QVariantList list;
    for (const auto &task : m_scheduler->tasks()) {
        // The base row every kind shares; each kind's own extra fields come
        // from its toVariantMap() (the giant per-flag switch this used to be
        // is now distributed authorship — each Task authors its own row).
        QVariantMap entry;
        entry["id"] = task->id;
        entry["fileUrl"] = task->fileUrl;
        entry["status"] = taskStatusToString(task->status);
        entry["error"] = task->error;
        // A non-fatal diagnostic on an otherwise-successful (Done) task — see
        // Task::warning's doc comment. Distinct from `error`: it never implies
        // Status::Failed, and the Tasks-list row renders it as such (a small
        // amber glyph + tooltip, not the failure styling) — see BottomBar.qml.
        entry["warning"] = task->warning;
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
        entry["batchLabel"] = task->batchId != 0 ? m_scheduler->batchLabels().value(task->batchId) : QString();
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
    // caps: a tiny transcript never gets a disproportionately long
    // prelude no matter how high the slider. Upward floors: a long
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
        // Whether this tier's .bin is already on disk for the
        // CURRENTLY open vault (m_vaultUrl — set once per vault open by
        // loadPersistedQueue(), empty/false for every entry until then) —
        // lets the model dropdown show which tiers would need a fresh
        // download vs. start immediately.
        entry["downloaded"] = ModelManager::isModelDownloaded(info.id);
        list.append(entry);
    }
    return list;
}

bool TranscriptionController::modelUsesVad(const QString &modelId) const
{
    // The one predictor of VAD windowing: only self-segmenting subprocess
    // engines bypass it (TranscribePipeline::planWindows' selfSegmenting arg).
    // Whisper and in-process parakeet both take the VAD-windowed path.
    return !isEngineModelId(modelId);
}

bool TranscriptionController::modelUsesVadGating(const QString &modelId) const
{
    // Per-window silence gating is a WhisperCppEngine-only feature (the
    // dynamic_cast in TranscribePipeline::planWindows). Parakeet is in-process
    // but a different engine class, so it takes windowing without gating.
    return !isEngineModelId(modelId) && !ModelManager::isParakeetModelId(modelId);
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
    // Task system: a REAL queued task now, not a fire-and-forget
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
    // Task system: a REAL queued task now. This used to bypass the
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
    // cancelled an unrelated real job that happened to be running). Snapshot
    // the ids first — cancelJob() mutates the scheduler's task list.
    QList<int> ids;
    for (const auto &task : m_scheduler->tasks()) {
        if (task->kind() == QStringLiteral("benchmark") || task->kind() == QStringLiteral("diarizationSweep"))
            ids.append(task->id);
    }
    for (const int id : std::as_const(ids))
        cancelJob(id);
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
