# suopTranscriber — Design

A cross-platform, GPU-accelerated wrapper for whisper.cpp (and other ASR engines)
with durable transcriptions, an Obsidian-style vault, and Spotify-lyrics–style
sentence highlighting synced to audio playback.

Primary target: Linux (CachyOS). Dev GPU: NVIDIA. Cross-vendor (NVIDIA + AMD)
supported from the start via the engine model below.

## Tech stack

- **UI:** Qt 6 with QML. Chosen for direct C++ linkage to whisper.cpp, native
  audio with sample-position signals (Qt Multimedia), a near-free tree sidebar
  (`QFileSystemModel`/`TreeView`), and first-class Linux support.
- **Default engine:** whisper.cpp, linked **in-process** (see below).
- **Other engines:** run **out-of-process** as subprocesses behind a uniform
  protocol.
- **Audio decode/resample:** ffmpeg (LGPL build). **Playback:** Qt Multimedia
  with the FFmpeg backend.

## Engine architecture

The abstraction lives at the **`Segment` level, not the transport level**. Every
engine produces the same normalized `std::vector<Segment>`; only the adapter
differs. Everything downstream (chunking, offset merge, transcript writing,
resume, highlighting) is shared.

```
TranscriptionEngine (interface → std::vector<Segment>)
├── WhisperCppEngine   → in-process, calls libwhisper C API directly   (DEFAULT)
└── SubprocessEngine   → spawns an out-of-process engine, parses JSONL
```

```cpp
struct Segment { double t0, t1; std::string text; };   // sentence-level only
class TranscriptionEngine {
  virtual std::vector<Segment> transcribe(AudioChunk, ProgressCb) = 0;
};
```

### whisper.cpp — first-class, in-process

whisper.cpp is the trusted default and is integrated more deeply than the
pluggable engines. Being in-process buys:

- **Warm model:** libwhisper stays resident across files (no per-file cold
  start). This is the main UX win vs. subprocess engines.
- **Native callbacks:** `whisper_full_params.progress_callback` and
  `new_segment_callback` drive a smooth progress bar and let segments stream into
  the UI *as produced*.
- **No temp WAV:** feed the ffmpeg-decoded PCM buffer straight into
  `whisper_full`.

Tradeoffs accepted:

- **No crash isolation:** a CUDA OOM here takes down the app. Guard VRAM/model
  selection. Acceptable for the trusted default.
- **Threading is ours:** run on a `QThread` worker, never the GUI thread.

### Pluggable engines — out-of-process

Genuinely different engines (faster-whisper, NeMo Parakeet, etc.) are Python/CUDA
and run as subprocesses. Benefits: async via `QProcess`, crash isolation, and
**license isolation** (GPL-ish Python deps never link into the Qt binary).

**Engine manifest** — adding an engine = drop a folder, no rebuild:

```jsonc
// engines/faster-whisper/manifest.json
{
  "name": "faster-whisper (large-v3)",
  "command": ["python", "run.py"],   // relative to manifest dir
  "capabilities": {
    "languages": ["*"],
    "gpu": "cuda",                    // "cuda" | "vulkan" | "cpu" | "any"
    "vram_gb_min": 6
  }
}
```

**Wire protocol** — JSON Lines over stdout, one object per line:

```jsonc
// App → engine (audio already normalized to 16k mono):
//   run.py --audio chunk.wav --offset 120.0 --lang auto

// Engine → app, streamed:
{"type":"progress","seconds_done":12.4,"seconds_total":60.0}
{"type":"segment","t0":121.2,"t1":124.8,"text":"So what I mean is this."}
{"type":"done"}
{"type":"error","message":"CUDA out of memory","recoverable":false}
```

Every timestamp an engine emits is **relative to its chunk**; the **app** adds the
chunk's global `--offset`. This offset merge is written once, in the app, for all
engines — it is the most common source of bugs in this kind of app.

**Capability detection** at startup: enumerate manifests, check GPU + installed
model files, gray out engines that can't run on this machine.

**BUILT.** `EngineManifest` (pure `manifest.json` parser), `EngineRegistry`
(discovers `engines/<id>/` in the bundled dir + `<vault>/.transcriber/engines/`,
capability-checks each against `GpuProbe` + a dependency self-test + model
presence, graying out unrunnable ones with a reason), and `SubprocessEngine`
(drives the manifest `command` via `QProcess`, parses the streamed JSONL into
`Segment`s — chunk-relative, the app owns the offset merge — with cooperative
pause/cancel and graceful degradation on a child crash). Selected through
`TranscriptionController` via an `engine:<id>` model-id prefix (the whisper
path is unchanged for un-prefixed ids); `availableEngines()` surfaces the
capability list to QML. `engines/parakeet/` (NeMo Parakeet-TDT) ships as a
verified-by-contract skeleton — its `run.py` speaks the protocol but needs
`pip install nemo_toolkit[asr]` to run; until then capability detection reports
it unavailable with a reason. Tests: `tst_enginemanifest`,
`tst_subprocessengine` (fake engine, no NeMo), `tst_engineregistry`.

### Parakeet — in-process via sherpa-onnx (the preferred path)

Parakeet-TDT also runs **in-process**, via sherpa-onnx's `OfflineRecognizer` —
the SAME sherpa-onnx library already linked for diarization, so **no Python /
NeMo**. `ParakeetEngine` (`src/core/ParakeetEngine.h/.cpp`) implements
`TranscriptionEngine` over an ONNX transducer bundle (encoder/decoder/joiner +
tokens), feeds 16 kHz mono PCM straight in (sherpa does mel internally with
`model_type="nemo_transducer"`), and assembles the model's per-token timestamps
into sentence-level `Segment`s (`assembleSentenceSegments`, split on `.!?`). It's
a normal entry in `ModelManager::availableModels()` (id `parakeet-tdt-0.6b-v2`),
so it appears in the model dropdown and auto-downloads (k2-fsa's int8 `.tar.bz2`,
multi-file extract via `downloadAndExtractBundle`) exactly like the whisper
tiers and diarization models — the answer to "download in-app + in-process like
the others". Tries CUDA then falls back to CPU (like `DiarizationEngine`).
Tests: `tst_parakeetengine` (pure token→segment assembly + a real-decode case
gated on `SUOP_PARAKEET_DIR`, verified end-to-end on the JFK sample), plus the
bundle logic in `tst_modelmanager`.

This makes the out-of-process NeMo `engines/parakeet/` above the *fallback*
route for Parakeet; the subprocess subsystem's real value is genuinely
Python-only engines (faster-whisper, etc.).

## Task system (the one queue)

Every unit of background work — transcribe, detect-speakers (diarize),
relabel, chapters, rename/merge speaker, combine split recordings, import,
USB grab, model download, model benchmark, diarization sweep — is a
**polymorphic `Task`** (`src/core/tasks/Task.h`), one subclass per kind. They
all run through **one FIFO queue**, on one worker thread (`TaskRunner`), at most
one at a time — so nothing contends for the GPU, and a warm model
(`EngineCache`) is reused across consecutive tasks.

**The queue engine is a display-blind `TaskScheduler`** (`src/core/TaskScheduler.h`),
split out of `TranscriptionController`. It owns ONLY the mechanics — the task
list, the two worker threads/runners, enqueue/pump/start/finish, and the
cancel/dismiss/undo/combine machinery — and never reads or writes a display
field, emits a QML-facing signal, or wires a task's result signals. The
dependency is **inverted**: the scheduler knows only an abstract
`TaskSchedulerObserver`, which `TranscriptionController` implements. The
controller stays a thin adapter that owns all display/progress state (`m_busy`/
`m_phase`/`m_currentFile`/…), does every typed-signal emit, and forwards each
queue `Q_INVOKABLE` to the scheduler — so the QML-facing surface is unchanged.
The scheduler calls back through the observer at the exact synchronous points
where display should update (a task starting, an active task ending, the queue
draining), which is what makes the emission *order* the QML bindings depend on
survive the split byte-for-byte. This is a genuine one-directional seam
(scheduler → observer; the sole read-back is `activeElapsedMs()`, a pure value
query) — not the earlier "extract the whole queue" idea, which was rejected
because the scheduler and the serial-lane display state were a co-owned mesh.
The behavior is pinned by the `characterize_*` slots in
`tst_transcription_queue.cpp`, which lock the golden signal-emission sequence.

**Two lanes** (`Task::lane()`). Most kinds are `Serial`: they run one-at-a-time
because they're INTENSIVE on a scarce system resource. That's not only "needs
the GPU / a warm model / exclusive access to a vault file" — it also covers a
heavy CPU/IO job with no GPU at all, like a multi-GB model import/export
(`ModelIoTask`), which queues behind a transcription rather than thrashing the
disk against it (there is deliberately no third "parallel I/O" lane — the rule
is simply "intensive ⇒ Serial"). `Instant` is for a fast, side-effect-light
rewrite — a speaker rename/merge, a file rename, or a file move — that must NOT
wait behind a long transcription: it runs on a SECOND worker thread in parallel
with the serial queue. The one exception the scheduler enforces
(`pumpQueues`/`serialActiveConflictsWith`): an `Instant` task on file X waits
while a `Serial` task on X is active, since both would rewrite that file's
`.md`. Both lanes feed the ONE task list (all the bookkeeping — the Tasks row,
persistence, cancel, history — stays uniform); the "second lane" is only a
second *runner*, not a second queue. A file rename is `RenameFileTask` and a
file move is `MoveTask` (`renameRecordingFile()`/`moveRecordingFile()` do the
pure filesystem work off the GUI thread; `VaultTreeModel` refreshes itself via
its `QFileSystemWatcher`), reporting `fileRenamed`/`fileMoved(oldUrl, newUrl,
ok, error)` that QML repoints the player on — the synchronous
`VaultTreeModel::renameFile()`/`moveFile()` survive only as no-controller
fallbacks (plain filename mode / test harnesses).

**Undo is the inverse task** (`Task::undoLabel()`/`makeUndoTask()`). An undoable
kind returns a label and a factory for its inverse (a rename-back, a move-back);
when it finishes, the controller emits `undoAvailable(token, label)` and the UI
shows one snackbar whose "Undo" calls `undoTask(token)`, which enqueues that
inverse (itself reported + re-undoable like any task). A multi-select op is
enqueued as a **batch** (`enqueueBatch()` — N tasks sharing a `batchId` drawn
from the same counter as task ids, so a single `int` token is unambiguously a
task id OR a batch id). The batch stays silent until its LAST member finishes,
then emits ONE snackbar (`batchUndoLabel(k)` → "Moved 5 files") whose undo
enqueues every member's inverse as a fresh one-snackbar-undoable batch — so N
moves are one reversible action, not N.

**A batch is also ONE row, not N** (`sortedJobs()` runs `collapseBatches()`
before partitioning). Every multi-select op is now enqueued as a batch — not
just move but the bulk ASR ops too (`transcribeAll`/`relabelAll`/
`generateChaptersAll`/`diarizeFilesForPaths`/`runAllJobsForPaths`), each via
`withBatch()`, which allocates the shared batchId and sets `m_currentBatchId`
so `enqueueTask()` stamps it onto the task it inserts (NOT onto the prerequisite
downloads it queues ahead — a shared prereq belongs to no one batch). The tasks
still run/report/persist **individually** — one-at-a-time warm-model reuse, the
per-file tree status dots (`m_jobStatusIndex`), per-file cancel — none of which
a monolithic "N files in one task" could express; the collapse is purely at the
DISPLAY layer. `collapseBatches()` (a pure transform, like `sortJobsForDisplay`)
folds a batch's rows into one aggregate row: `isBatch`, a `members` list (the
per-task rows, for the expandable BottomBar delegate), `batchCount` = distinct
files (so a run-all's several-tasks-per-file still reads "5 files"),
`batchDoneCount`, an any-active-wins aggregate `status`, and a summed duration
once terminal. A batch of ONE member renders flat (no "1 file" row). The
collapsed row's label is the members' shared `kindLabel` ("Transcribe"/"Move"/…)
or an explicit one for a mixed-kind batch ("Run all", passed to `withBatch()`
and carried in `m_batchLabels`). Its single close button drives `cancelBatch()`/
`dismissBatch()` (whole-batch); expanding it exposes the per-file rows with
their own cancel. Trash/delete are already ONE multi-file task (atomic, no
per-file lifecycle), so they render as one row for free — same visual language.

**Trash / delete / restore** are instant-lane tasks too — `TrashTask`,
`DeleteTask`, `RestoreTask` over the four `VaultFileScope`s (files / transcript /
audio-only / folder). The pure filesystem work lives in `VaultFileOps.h`
(`trashVaultEntries`/`deleteVaultEntriesPermanently`/`restoreVaultEntries`), so a
whole multi-select delete is ONE `TrashTask` ⇒ ONE generic undo snackbar (undo =
a `RestoreTask` over the captured `restorable` set). But the tree rebuild and the
delicate per-file signals `MainPanel` reacts to
(`fileDeleted`/`transcriptDeleted`/`audioDeletedTranscriptKept` and their restore
counterparts) stay in the model — `VaultTreeModel::applyRemovalOutcome()`/
`applyRestoreOutcome()`, which the controller calls on the GUI thread once the
task completes (it holds a `vaultTreeModel` binding, like `UsbGrabberController`).
The model owns that emission because it owns the orphaned-.md-vs-audio
classification; keeping it there means the async migration left every `MainPanel`
delete/restore handler *unchanged*. Entries the filesystem refuses to trash
(non-destructive — the original is untouched) come back via `trashResult(scope,
untrashable)`, which the Sidebar turns into the confirm-then-`deletePermanently`
prompt. The synchronous `VaultTreeModel::trash*`/`delete*`/`restoreFromTrash`
survive as the no-controller fallback (test harnesses). **Folder rename/move**
are instant-lane tasks too (`FolderRenameTask`/`FolderMoveTask` over
`renameVaultFolder()`/`moveVaultFolder()`) — the folder analogues of the file
rename/move, undoable via the generic snackbar, with no per-file model signals
to re-emit (the tree just refreshes via the watcher). Every file/vault operation
is now a `Task`.

This replaced an earlier design where "a job" was a single 30-field `Job`
struct with nine mutually-exclusive bool flags, a `jobKind()` flag-waterfall,
a ten-branch `startJob()` that marshalled each kind's args into a string-named
slot on a monolithic `TranscriptionWorker`, and opt-*out* persistence. A
kind's knowledge lived in five disjoint places and one could always be
forgotten — which is exactly how work went missing (see below). Now the kind
*is* the object: its `run()` (the doing-code), its typed result signals, its
`toVariantMap()` Tasks-row fields, its `displayName()`/`kindLabel()` (the row's
full description + short category label — the latter replaced a QML
`kindLabel(kind)` switch a new kind could forget, silently mislabeling itself
"Transcribe"), `capabilities()`, `presentation()`, `isPersistable()`, and
`prerequisites()` all live in one file.

A `toVariantMap()` key IS the Tasks-list display contract: `modelId`
(`TranscribeTask`) and `segmentationModelId`/`embeddingModelId` (`DiarizeTask`)
render as the row's model sub-line via `JobListFormatter::modelShortLabel()`.

The same "author it on the kind, not in a disjoint controller site" rule
governs three more per-kind facts the controller used to hardcode (each in
*two* places that had already drifted apart):

- **`progressModel()` + `progressPhases()`** — how a kind's `run()` drives the
  overall (0..1) bar. `SelfCumulative` (the default: download/combine/chapters/
  import/grab/benchmark) emits an already-cumulative fraction the controller
  assigns straight to the bar; `WeightedPhases` (transcribe/diarize) declares
  its ordered phase set, each phase weighted by its rolling-average duration.
  This is now the *single* source of the phase set — it replaced a hand-kept
  phase-name list in the `progress()` lambda **and** a diverging copy in
  `recomputeOverallProgress()` (importing/grabbing/benchmarking were in one but
  not the other, so those bars briefly mis-computed on each phase change).
- **`completionChangesModelSet()`** — true iff a *successful* finish changes the
  on-disk model set (a landed download, a model import), so the controller
  fires `modelsChanged()` **once, centrally**, in `onTaskFinished()` keyed off
  this hook — instead of a `kind=="download"` check there plus a second
  `op=="import"` check in the `ModelIoTask` wiring.
- **The phase→label vocabulary** — one registry in `TranscriptionController`
  (`phaseLabel()` for the bottom bar, `phaseOverlayLabel()` for MainPanel's
  blocking-overlay title) replaced *two* QML `switch`es (BottomBar's
  `phaseLabel` + MainPanel's `transcribingOverlayLabel`) that each mapped the
  same phase slugs with their own wording; a new emitted phase that forgot a
  case rendered blank. `tst_styleguide` scans the task sources and fails if any
  emitted phase lacks a registry row.

Persistence is opt-in per kind (`isPersistable()`), but the *rebuild* side is
one switch (`createTaskFromJson`); `tst_transcription_queue` round-trips a
representative of every persistable kind through it, with a completeness
tripwire that fails if a new persistable kind is added without a factory branch
(else it would persist fine and be silently dropped on restart).

**Load-bearing invariants** (the actual bug-killers, see `.task/plan.md`):

- **Exactly one terminal report per enqueued task.** Whether `run()` finishes,
  throws `TaskError`, or the task is cancelled *before it ever ran*, the queue
  emits `finished` exactly once. The old system silently dropped a
  cancelled-while-Queued job, so a consumer waiting on the result never heard
  back (`UsbGrabberController` had to reconcile against the queue to notice).
- **Everything queued gets a Tasks row. No exceptions.** A task is never
  invisible — though the members of one multi-select batch share a single
  *collapsed* row (expandable to the per-task rows; see `collapseBatches()`
  above), so "a row" can mean "one expandable batch row" without any task going
  unrepresented. `Task::Presentation` (`Foreground`/`Background`) controls only
  whether it *also* raises MainPanel's blocking overlay — NOT whether it
  appears in the list or drives the bottom-bar glow. A benchmark is
  `Background`: real queued task, real row + glow, no overlay over the
  transcript you're reading. This axis is why the benchmark no longer needs a
  side door (it used to bypass the queue entirely, precisely because "being a
  job" indivisibly granted the overlay it had to avoid).
- **Persistence is opt-*in*** (`isPersistable()`): a kind whose inputs name an
  external/removable path (import, usbGrab) simply never overrides it, instead
  of every new kind having to remember to add itself to an opt-out list.
- **Capabilities are declared, not guessed.** The UI asks `capabilities()`
  (`pausable`, `cancellableMidRun`) instead of re-deriving a kind allowlist in
  QML — the source of the 2026-07-09 pause-vs-cancel latch bug.
- **Model prerequisites** ride on the kind (`prerequisites()`): the queue
  turns each into a `DownloadTask` inserted ahead of the requester (deduped),
  so a kind declares its model needs once instead of at every enqueue site and
  the persisted-queue replay separately.

The scheduler keeps two hooks worth naming: a **combine preflight**
(`maybeRedirectForCombine`) that can retarget a queued transcribe at a merged
output *before* dispatch (the only place `Task::fileUrl` is mutated, and only
on the GUI thread, pre-run), and **`TaskFactory`**, the one place a persisted
kind slug maps back to a concrete class.

Threading: a `Task` lives on the GUI thread its whole life (including as a
Done/Failed history row); its `run()` executes on the worker thread and
communicates back only through queued signals — it never touches GUI-thread
state. `TaskContext::checkpoint()` is the single cooperative interruption point
every pausable kind funnels through.

## Benchmark (top-level page)

A display-only comparison harness, its own **top-level page** (a TitleBar icon
button next to the USB grabber, mounted by `MainPanel` in a `ScrollView` — no
longer a Settings sub-page). It defaults to the file open in the left panel
(`currentFilePath`), falling back to a vault-wide picker. Two sweeps, each a
real **`Background` task** on the one queue (`BenchmarkTask` /
`DiarizationSweepTask` — see "Task system" above), so they serialize with real
work and never contend for the GPU, show a Tasks row + progress glow without
raising the transcript overlay, and write **no** `.md`:

- **Model benchmark** — `TranscriptionController::runBenchmark()` runs every
  *already-downloaded* model plus every *available* engine over the file,
  reporting per-model run time, realtime factor, and segment/word/char counts
  alongside each transcript. Its only footprint is
  `<vault>/.transcriber/benchmarkLastRun.json` (overwritten each run, reloaded on
  show); persistence via `VaultTreeModel::read/writeBenchmarkLastRun`.
- **Diarization sweep** — `runDiarizationBenchmark(fileUrl, combos)` runs a
  cartesian product of chosen segmentation × embedding × sensitivity values over
  ONE decode, reporting **how many speakers** each combo finds (plus turns /
  runtime / realtime factor) — the money metric for "which combo actually splits
  a hard far-field recording". Guarded by a confirm dialog above ~10 tests. Each
  combo builds a fresh `DiarizationEngine` (threshold is baked in at
  construction). Not persisted (session-only).

UI in `BenchmarkPage.qml`.

## GPU: cross-vendor is not uniform across engines

| Engine            | NVIDIA        | AMD             | Notes |
|-------------------|---------------|-----------------|-------|
| **whisper.cpp**   | CUDA or Vulkan| **Vulkan**      | Build Vulkan → one binary runs on both. Universal baseline. |
| faster-whisper    | CUDA          | ✗               | Best speed/accuracy tradeoff. |
| NeMo Parakeet     | CUDA          | ✗               | Very accurate English, heavy NVIDIA stack. |

- **whisper.cpp + Vulkan is the cross-vendor floor** that works everywhere; CUDA
  engines are NVIDIA-only upgrades gated by capability detection.
- Build whisper.cpp with `-DGGML_VULKAN=1` for the universal binary; optionally a
  separate `-DGGML_CUDA=1` build for max NVIDIA speed.

## Audio: decode vs. playback are separate concerns

- **Transcription input:** every engine wants **16 kHz mono PCM**. Normalize with
  ffmpeg — do **not** use Qt to resample:
  `ffmpeg -i input.ext -ar 16000 -ac 1 -c:a pcm_s16le -f wav chunk.wav`
- **Playback:** Qt Multimedia on the *original* file. Force the FFmpeg backend on
  Linux: `QT_MEDIA_BACKEND=ffmpeg` (default in Qt 6.5+, but be explicit) — the
  gstreamer backend is flaky with arbitrary formats and has worse position
  accuracy.

## Sentence highlighting (not word-level)

Highlighting is **sentence-level only**, which removes real complexity:

- No DTW / alignment heads in whisper.cpp — segment timestamps come free from
  `whisper_full_get_segment_t0/t1`.
- WhisperX (its value was word-level forced alignment) is unnecessary.
- Protocol carries only `t0`/`t1`/`text` — no `words[]`.
- **Timing is easy:** sentences are seconds apart, so `positionChanged`
  (~100–250 ms) is accurate enough. **No interpolated clock needed.** On each
  position event, **binary-search** the active sentence by time (don't linear
  scan).

**Note: a whisper "segment" ≠ a "sentence"** — it's a phrase/utterance chunk,
usually sentence-ish. Start by highlighting native segments as-is. Only if needed
later, merge/split on `.?!`, keeping the first segment's `t0` and the last's `t1`.
Do not build that on day one.

## Speaker diarization

Speaker detection is a **separate engine** from transcription — `DiarizationEngine`
(`src/core/DiarizationEngine.h/.cpp`), sherpa-onnx's segmentation + embedding +
agglomerative clustering, independent of which whisper model is selected (unlike
the old tdrz heuristic, it never constrains model choice). Speaker count is
always auto-detected via a clustering distance threshold — **no "number of
speakers" UI**. That decision stands: a fixed speaker *count* (`num_clusters`)
is still never exposed. What *is* exposed (added when the default collapsed
far-field group recordings into one speaker) is the **clustering threshold
itself**, as a continuous "sensitivity" knob (`AppSettings.diarizationSensitivity`
→ `DiarizationEngine`'s ctor) — lower = more speakers, higher = fewer. It tunes
the always-on auto-detection rather than replacing it with a count.

Both models are **user-selectable** (`AppSettings.diarizationSegmentationModel` /
`diarizationEmbeddingModel`, pickers in Settings → Transcription & AI): the
default pair is pyannote-segmentation-3.0 + WeSpeaker ResNet293-LM, with
Rev.ai reverb-diarization-v1 (segmentation), 3D-Speaker CAM++ and NeMo
TitaNet-large (embedding) as alternatives, each its own downloadable/manageable
model. The selection + sensitivity are carried on the `DiarizeTask` and can
be swept in the benchmark (see below) to compare combinations on a hard file.
See `ModelManager::availableSegmentationModels()`/`availableEmbeddingModels()`.

It runs one full pass over the same 16 kHz mono PCM whisper transcribes,
producing `DiarizedTurn`s (`{start, end, speaker}`), and each segment is tagged
with the diarized turn it overlaps most (`labelForSegment`). Diarization is a
**standalone task, never bundled into transcription** — **Transcribe is pure
ASR** (plus its intrinsic VAD windowing; see below). It's invoked two ways, both
through the one task queue and sharing one diarization core
(`EngineCache::ensureDiar` + `DiarizeTask`'s progress-driving pass):

- **Standalone** — the `DiarizeTask` (`diarizeFile()`, the MainPanel /
  Sidebar-ribbon "Detect speakers" action). Runs (or re-runs) speaker detection
  on an already-transcribed file *without* re-transcribing: reads the existing
  `.md`'s segments, decodes the audio, diarizes, and splices the speaker tags +
  `speakerDetection: true`/`speakers:` back in (`spliceBodySpeakers`). A
  diarization failure here *is* the job's result.
- **As a `runAllJobs()` pipeline step** — the one-click "run all available
  jobs" path queues transcribe → **detect speakers** → label → chapters as
  separate FIFO jobs (the diarize step reads the `.md` the transcribe job just
  wrote). This replaced the old inline `speakerDetection` toggle on transcribe.
  Only the steps that haven't run yet are queued — `pendingJobKinds()` derives
  that from the `.md` alone (sibling file / `speakerDetection: true` /
  `transcriptIsLabeled()` / a chapter marker), and a file with nothing owing
  queues nothing.

A fresh transcript is therefore always written speaker-less
(`speakerDetection: false`, no `[Speaker N]` tags); the diarize job adds them
later.

Speakers are written **into the `.md`**: a leading `[Speaker N]` tag on each
segment line (the tag text is the display name — renaming rewrites the tags in
the body), plus a `speakers:` frontmatter allowlist of the labels actually used.
Diarization has **no mid-pass abort hook** (sherpa-onnx's progress callback is
`void`), so a cancel takes effect at the post-diarize checkpoint, not instantly.

## Durable / resumable transcription

Independent of highlighting granularity — this exists for the resume requirement.

- **BUILT:** chunk on **silence via VAD** (Silero VAD, whisper.cpp's built-in
  API), ~2–5 min VAD-aligned windows, splitting only at gaps between speech
  segments — never a fixed 30s slice that could cut a word mid-segment. See
  `VadChunker::computeWindows()`/`groupSegmentsIntoWindows()`
  (`src/core/VadChunker.h/.cpp`, tested in `tests/tst_vadchunking.cpp`).
  Degrades gracefully to a single whole-file window (today's pre-chunking
  behavior) if the VAD model can't be downloaded/loaded — recorded honestly
  via the `.md`'s `vad`/`vadError` frontmatter fields, never a silent
  substitution. This VAD windowing is intrinsic to every Transcribe job (it
  does nothing standalone, so it's *not* a separate task) and is surfaced as
  its own visible **`detecting-speech`** progress phase — the transcribe
  job's phases are `loading → decoding → detecting-speech → transcribing`
  (each weighted into the progress bar by its rolling-average duration; VAD,
  like loading/decoding, has no native progress callback so its within-phase
  fraction is synthesized). Distinct from the standalone diarize job's
  `detecting-speakers` phase.
- **BUILT:** the app-side offset merge is now real (not a no-op) — each
  window is its own `AudioChunk` with `offsetSeconds` set to the window's
  start, and `TranscriptionController`'s `onSegment` adds that offset before
  writing/labeling. See `tst_engine_contract.cpp`'s
  `multiChunkStitch_producesGloballyOrderedOffsetSegments()`.
  whisper.cpp's own per-window Silero VAD gating (`WhisperCppEngine::
  setVadModelPath()`) is also wired in, independent of the windowing above —
  it further skips any residual silence gap that survived grouping *inside*
  a window, reducing whisper.cpp's silence-driven hallucination.
- **NOT YET BUILT:** checkpoint after each window to `.name.partial.jsonl`
  with the last global offset; on restart, resume from that offset if `.md`
  is missing but a partial exists; on full completion, delete the partial.
  Current durability is coarser without this: frontmatter written at job
  start + per-segment flush of the `.md` itself (see "Vault / file layout"
  below) — a killed job leaves a truncated `.md` (which now carries
  `partial: true`, written up front and cleared only on completion, so it is
  distinguishable from a done transcript — see "Transcribed indicator") and a
  re-run starts over from scratch, it just no longer re-decodes/re-runs VAD as
  one giant single-shot pass now that chunking exists underneath where resume
  will eventually hook in.

## Vault / file layout (Obsidian-style)

One user-chosen root holds config, audio, and transcriptions:

```
vault/
  .transcriber/
    settings.json                # vault-scoped settings (JSON) — see below
    engines/                    # engine manifests + scripts
                                # (model files are NOT here — see "Model
                                #  management": one app-global store, shared
                                #  across vaults)
  interviews/2026-06-30.m4a
  interviews/2026-06-30.md              # human-readable transcript = "done" (green)
  interviews/.2026-06-30.partial.jsonl  # exists only mid-transcription (resume)
  interviews/.combined/                 # original parts, after a combine merged them
```

- **Vault-scoped settings (`.transcriber/settings.json`):** appearance/behavior
  settings (`AppSettings.qml`'s `defaults` map — colors, opacity, radii,
  label-display toggles, auto-labeling options, etc.) round-trip through this
  JSON file inside the vault's hidden `.transcriber/` config dir, via
  `VaultTreeModel::readVaultSettings()`/`writeVaultSettings()` (the latter
  creates `.transcriber/` if missing). `.transcriber/` is itself a hidden
  dotdir, so it's naturally excluded from the sidebar tree (`QDir`'s default
  entry filters skip hidden entries). Loaded on `vaultPath` change
  (`AppSettings.loadFromVault`), saved (debounced) on any settings change
  (`AppSettings.saveToVault`). `lastVaultPath`/`lastModel` stay app-scoped
  (`QtCore.Settings`, `~/.config`) — they're last-used bootstrap state needed
  BEFORE a vault's own settings file can be read, not a vault-specific
  preference. The USB device allowlist is NOT part of this file yet — still
  not implemented. (Models are no longer vault-scoped at all — see "Model
  management": one app-global store.)

- **Transcribed indicator:** sibling `.md` exists → green; missing → yellow;
  orphaned `.md` (no sibling audio file) → blue.
  sibling `.md` with `partial: true` frontmatter (a cancelled or killed
  transcribe) → orange — a truncated transcript, not a finished one.
  Watch with `QFileSystemWatcher` for live updates.
- **No retranscription:** presence of `.md` means done — with one exception: a
  `partial: true` `.md` is NOT done; it is shown distinctly (orange) and is
  currently inert for bulk relabel/chapters/detect-speakers (making it
  directly re-transcribe-eligible is a documented follow-up).
- **All transcription data lives in the single `.md` — no separate JSON
  sidecar** (this supersedes an earlier draft of this doc that split timing
  into a hidden `.sentences.json`; CLAUDE.md's load-bearing constraint is the
  current decision). Format:
  - YAML-ish frontmatter: `model` (the ggml file that actually ran),
    `speakerDetection` (bool), `transcribedAt` (ISO 8601), and `partial` (bool,
    present only while a transcribe is unfinished — written up front, cleared on
    completion; a leftover `partial: true` marks a cancelled/killed run).
    Frontmatter is written the moment a job starts, before transcription runs,
    so a killed/stopped job still leaves a file recording what it was run with.
  - One `[start] text` line per segment, appended and flushed as each segment
    is produced — not buffered to the end. The timestamp is a compact start
    only (leading/trailing zeros trimmed, e.g. `[1:07]`); there is no end
    timestamp — it is always the next segment's start. When speaker detection
    ran, a `[Speaker N]` tag follows the timestamp (written by
    DiarizationEngine); consecutive identical lines collapse to a trailing
    `(×N)` repeat marker. This is also where sentence-highlight sync re-derives
    timing from (see "Sentence highlighting" above). The old trailing
    `[SPEAKER_TURN]` marker (tinydiarize, since replaced by DiarizationEngine)
    is now only PARSED for backward-compatible reads of older files — never
    written.
  - If the requested model id isn't recognized, `ModelManager` falls back to
    the default model rather than failing outright (see "Model management"
    below) — the frontmatter also carries `modelRequested` and
    `modelFallback: true` in that case, so the file never reads as if the
    requested model had been honored when it wasn't.

- **`.combined/`:** a group's original parts are moved here after a
  split-recording combine merges them (see below) — a hidden dotfolder, so
  the sidebar tree naturally excludes it for the same reason `.transcriber/`
  is excluded.

## Split-recording combine

Recorders routinely split one recording into numbered parts. Combine merges
those parts back into a single audio file *before* transcription, so the user
transcribes one file instead of N fragments with N broken sentences at the
seams. Implementation lives in `TranscriptionController`; the authoritative
spec is `findCombineGroups()`'s doc comment in the header — this section is
the overview, not a duplicate of it.

- **Rule-driven only — never selection-driven.** A merge only ever happens for
  a group that a user-authored regex rule (`AppSettings.combineRules`) actually
  matched. There are exactly two entry points, both of which take the rules as
  an argument: the sidebar ribbon's scan button (`runCombineScan()`, enqueues
  every "ready" group) and the per-row "combine this group" button
  (`combineGroup()`, the one matched group). A selection-driven "combine
  whatever N files I selected" action was built and then deliberately removed:
  with no rule consulted it would merge unrelated recordings no rule would ever
  group, and it ignored the configured `deleteOriginalsAfterCombine`. The
  source-scan guard `sidebar_combineIsRuleDrivenOnly` in `tst_styleguide.cpp`
  keeps it from coming back.
- **Never automatic on a file-add.** Silently merging every time a new part
  lands is a surprise a recorder shouldn't spring on the user — combine used to
  fire on every vault-tree rebuild and was removed for exactly that (it merged
  files out from under people watching a vault fill up mid-recording). Both
  entry points above stay user-initiated. Two narrow exceptions:
  - `maybeRedirectForCombine()`, a safety net at the top of `startJob()`:
    transcribing a file that is a member of a currently-"ready" group inserts
    the combine ahead of it and retargets the job at the merged output, so a
    part that appeared after the user's last scan can't get transcribed alone.
  - **The auto-combiner** (`AppSettings.autoCombineAfterGrab`, default off):
    when a batch of USB **auto-grabs** finishes, `Main.qml` runs the ordinary
    `runCombineScan()` over the vault. This is not the old behavior returning:
    it's opt-in, and it fires on ONE discrete event — a device the user chose to
    auto-grab has just finished offloading, which is the moment a split
    recording is complete and combining it is what they'd do by hand anyway —
    rather than on every filesystem change. It waits for the LAST auto-grab to
    leave the queue (`UsbGrabberController::autoGrabsSettled()`), so a recording
    whose parts arrive across several grab jobs is combined once, as one group,
    not half-combined twice. Still rule-driven and still "ready"-gated like
    every other combine, and it posts a snackbar — a merge that moves the
    user's originals into `.combined/` must never happen with no account of
    itself. A manual "Grab now" does NOT trigger it (the user is right there and
    can click combine); neither does a drag-and-drop import.
- **Grouping identity = captured text + containing folder.** Rules match with
  QRegularExpression *search* semantics against the bare filename; the
  concatenated capture groups form the group key. Uncaptured wildcard spans
  vary freely between members without affecting grouping ("same real string =
  combine"). The containing folder is part of the identity, so identically
  named parts in two folders are two groups, never one cross-folder group —
  which also keeps `outputPath` unambiguous and lets the vault tree draw one
  outline around what are always sibling rows.
- **Order within a group** is mtime ascending by default. A rule may instead
  designate *one* capture group as a sort differentiator (numeric-aware),
  which then stops being a grouping group — it can't be both. mtime, not
  creation/birth time: birth time doesn't survive a plain copy on Linux, while
  every standard copy tool preserves mtime, and the app can't know whether a
  vault's audio was recorded in place or copied off an SD card.
- **`outputPath`** is derived from (sanitized rule name, sanitized key) — not
  from any one member's filename — so it stays re-derivable from a single
  surviving filename after the other parts have moved into `.combined/`. The
  readable form is `<rule> - <key>.combined.<ext>`, e.g. `Acme Mic 3 - 20260710
  edit.combined.wav`: spaces are allowed in the name, the rule and key are
  joined with ` - `, and the key's capture groups are space-joined with a
  consecutive-duplicate collapsed (a rule that captures the same token twice
  yields `… edit`, not `… edit edit`). The naming is the single source of
  truth in `CombineNaming.h` (shared with the USB grabber — see below).
- **Group status** is one of `ready`, `singleton` (only one match),
  `alreadyTranscribed` (a member already has a sibling `.md` — never silently
  discard existing work), `alreadyCombined` (output already on disk), or
  `pending` (the newest member was modified within a few seconds, so the
  recorder may still be writing it). Only `ready` groups are enqueued.
- **The merge itself** is an ffmpeg concat-demuxer stream copy (`-c copy`, no
  re-encode) run as a normal `combine`-kind job in the same queue as
  transcription, so it gets the same progress/pause/cancel handling. On
  success the parts move into `.combined/` (or are deleted, if
  `deleteOriginalsAfterCombine`) so the next scan doesn't re-detect the group.
  **The merge is undoable** (`UncombineTask` — move the parts back out of
  `.combined/`, remove the merged file), except when
  `deleteOriginalsAfterCombine` deleted the parts, and except once the merged
  file has been transcribed (its `.md` is real work the undo would discard —
  the Tasks-row Undo greys out via `undoStillApplicable()`).
  A combine never also queues a transcribe — the merged file just becomes a
  normal untranscribed vault entry.
- **The merged file inherits the first part's mtime**, rather than taking the
  time of the merge. It stands in for the original recording, and mtime is what
  this app treats as when a recording happened (it's the default group ordering
  signal above, for the same "birth time doesn't survive a copy" reason), so
  combining a week-old recording today must not make it look like today's.
  "First" means first in *combine order*, not oldest on disk — the two differ
  whenever a rule designates a sort differentiator. The restamp happens after
  ffmpeg exits, since writing the file stamps mtime as now.
- **`detectCombineGroups()` is pure** over its inputs (disk access limited to
  `QFileInfo` stats on the paths passed in), so the same entry point backs both
  the Settings "test this rule" preview and the real enqueue decisions.

## USB import

- **Detection:** `UsbDeviceMonitor` (`src/core/UsbDeviceMonitor.h/.cpp`) watches
  UDisks2 over D-Bus (`InterfacesAdded`/`InterfacesRemoved`) for mounted removable
  volumes — not `QStorageInfo::mountedVolumes()` polling.
- **The app does its own mounting.** Nothing on a stock Linux desktop is obliged to
  auto-mount: KDE ships `device_automounter` off, and Plasma just shows a Device
  Notifier for the user to click, so a recorder plugged in with the app running can
  sit unmounted forever — no mount point, no `deviceAdded`, no grab. So an unmounted
  external volume is reported as `unmountedVolumeDetected` (identity comes off the
  `Drive` object and needs no mount), and `UsbDeviceMonitor::mountVolume()` asks
  UDisks2's `Filesystem.Mount()` to mount it. A mounted volume is then reported as an
  ordinary `deviceAdded` — the app's mounts and the desktop's are indistinguishable
  downstream, and the two racing on one hotplug still report the volume exactly once
  (`m_knownObjectPaths`; `AlreadyMounted` is treated as success, not failure). The
  call never prompts (`auth.no_user_interaction`): a hotplug isn't a user action, so
  a polkit dialog springing from one would be unattributable. It doesn't normally
  arise — udisks2's `filesystem-mount` is `allow_active=yes` for non-system devices,
  which is all the gate below ever offers it.
- **Mount gate = an enabled rule, nothing else.** `UsbGrabberController` mounts a
  volume only if `resolveEffectiveRule().enabled` — the device's own rule or the `"*"`
  catch-all — so the app mounts precisely what it is willing to grab from.
  Deliberately NOT a setting of its own (off by default, it
  would leave the out-of-box experience exactly as broken) and deliberately not
  unconditional (that makes a transcription app into a general-purpose automounter for
  every photo SD card the user owns). Note the mount gate is `enabled`, NOT
  `autoGrab` (below): mounting is what makes a device addressable at all —
  previewable, configurable, grabbable by hand — so a device the user has
  vouched for should mount whether or not they also asked for hands-off
  grabbing. `autoGrab` decides what happens *after* the mount, not whether it
  happens.
- **Two gates, not one: `enabled` then `autoGrab`.** `enabled` says a device is
  grabbable at all; `autoGrab` (per-rule, default **off**) says a hotplug may
  fire the grab *by itself*, with no click. An enabled-but-not-autoGrab device
  still mounts, still lists, still previews, and still grabs from "Grab now" —
  it just waits to be asked. Per-device rather than one global toggle because
  the two devices a user owns want opposite answers: the recorder they plug in
  to offload should just do it; the photo SD card that happens to match the
  catch-all shouldn't start copying on its own. Default off because auto-grab is
  the surprising behavior — which does mean an already-`enabled` rule stops
  auto-grabbing until its owner opts in, a deliberate one-time narrowing. Known cost: an unconfigured device with no
  catch-all never mounts, so it never reaches the settings list to be configured —
  mount it once by hand to set it up. If that friction bites, widen what the device
  list shows (unmounted identity is already available), not what the app mounts.
  Deciding whether to mount is the controller's job (it holds the rules); deciding
  what *may* be mounted is the monitor's (the external-media gate below). The monitor
  never mounts off its own signal — `usbGrabber_mountIsRuleGated` in
  `tst_styleguide.cpp` guards both halves.
- **External media only (safety-critical):** a mounted filesystem becomes a visible
  device *only* if it passes `usbDeviceIsExternalMedia()` — UDisks2's
  `Block.HintSystem` must be false **and** the backing `Drive` must be either
  `Removable` or on a `usb`/`sdio` `ConnectionBus`. Both are required: `HintSystem`
  is only a hint (udev's `UDISKS_SYSTEM` can set it on anything) so it can't be the
  sole positive evidence, while `Removable`/`ConnectionBus` say nothing about
  whether the OS considers the device its own. Unreadable properties fail closed.
  This gate is load-bearing rather than cosmetic: "is it mounted?" alone makes `/`
  and `/boot` ordinary grab targets that the `"*"` catch-all rule would recurse
  into and copy wholesale into the vault. Internal drives are invisible to the
  monitor, not merely deprioritised by its callers.
- **Defence in depth:** independently of the above, `usbGrabRefusalReason()`
  (`UsbGrabJob.h`) re-checks the concrete paths of each run and refuses a scan root
  that canonicalises to `/`, one that doesn't exist, or a destination vault nested
  inside the scan root (which would make the walk feed itself). Deliberately
  redundant with the detection gate — a rule pointed somewhere catastrophic is
  refused even if enumeration ever hands up something it shouldn't have.
- **Devices vs. volumes (two identities, don't conflate):** a physical device can
  mount several filesystems at once — a multi-LUN recorder exposing internal memory
  and an SD card is one UDisks2 `Drive` (one Vendor/Model/Serial) behind two `Block`
  objects. So `UsbDeviceInfo::serial` identifies the **device**, is stable across
  replug, is the rule key, is shared by that device's volumes, and may be empty;
  while `UsbDeviceInfo::id` identifies the **volume**, is the UDisks2 block-device
  object path, is unique, is runtime-only (`/dev/sda` can return as `/dev/sdc`), and
  must never be persisted or used as a rule key. `deviceAdded`/`deviceRemoved` and
  the controller's in-flight job map are all per-volume, keyed by `id`. Keying
  runtime state by `serial` is a shipped-and-fixed bug: it made unmounting one
  volume report *both* as gone, let two concurrent jobs for one device evict each
  other, and left a two-volume recorder's second volume permanently un-grabbed.
- **Rules:** one `UsbGrabRule` (`src/core/UsbRuleStore.h`) per device, keyed by a
  stable serial, plus a `"*"` catch-all rule that applies to any device without
  its own enabled rule. One rule covers a device's every volume — the UI shows one
  row per device (with a volume count), and a grab runs one job per mounted volume
  under that single rule, so nothing is silently skipped. Per-volume rules are not
  offered: volumes have no stable identity to key them by (the dev machine's
  recorder reports an identical serial *and* an identical filesystem UUID for both
  of its volumes). Rules are **app-scoped** (`QSettings`, not the vault's
  `.transcriber/settings.json`) since the same physical device is reused across
  vaults. Each rule carries: `enabled`, `extensionFilter` (empty = all files),
  `filenameRegex` (empty = no regex constraint; see below),
  `preserveStructure` (mirror the device's subfolder layout vs. flatten),
  `startFolder` (relative to the device root), and `destinationFolder` (relative
  to the *vault* root; empty = vault root, created automatically on first grab if
  it doesn't exist yet).
- **Filters (`extensionFilter` + `filenameRegex`) are ANDed**, and each is
  independently "no constraint" when blank (never "match nothing") — a file is a
  grab candidate only if it clears both. The regex deliberately has the *same
  semantics as the split-recording combine rules* (`matchRuleCapturedKey()`):
  `QRegularExpression`, matched against the **file name only** (never the path),
  and **unanchored** unless the pattern says so — so one pattern reads the same
  in both features. Capture groups carry no meaning here (there they decide
  group identity; here the pattern is a pure include-test), which is why the
  differentiator/sort-group apparatus isn't carried over.
  - **An invalid pattern fails closed** — it matches nothing, so the rule grabs
    zero files. Reading unparseable as "no constraint" would silently *widen* a
    rule the user was narrowing, and on the `"*"` catch-all that means
    bulk-copying a whole device on a typo. Zero files copied is recoverable and
    visible; the field's own live ✓/✗ indicator says why.
  - **QML validates through C++** (`UsbGrabberController::regexValidationError()`,
    sharing one definition with the combine rules' check via `RegexValidation.h`)
    rather than JS `RegExp` — the syntaxes aren't guaranteed identical, so a
    JS-valid pattern could still match nothing at grab time.
- **Grab (`runUsbGrabScan()`, `src/core/UsbGrabJob.h/.cpp`):** recursively walks
  the device, diffs against files already in the destination folder by **rsync's
  quick-check: filename + size + mtime**, and copies only files that are both
  filter-matching and not already present (never silently overwrites a
  same-named-but-different file — appends a numeric suffix instead). A plain
  synchronous function over its inputs that knows nothing about threads, the
  queue, or history entries — it walks, it copies, it returns what happened.
- **Already-combined parts are skipped too**, not just already-present ones.
  After a combine the original parts leave the vault — they move into the hidden
  `.combined/` (which the quick-check index doesn't walk) or are deleted
  outright with `deleteOriginalsAfterCombine` — so the quick-check alone would
  see them as absent and re-grab them, re-triggering the merge and doubling the
  disk use. So for each candidate the walk also recomputes the merged-output
  name the combiner *would* produce from that filename (via `CombineNaming.h`,
  the same `matchRuleCapturedKey`/`combineOutputPathFor` the combiner and
  `maybeRedirectForCombine()` use — first matching rule wins) and skips the file
  as a duplicate if that output already exists beside where it would land. This
  covers both the move and the delete disposition with **no persisted grab
  ledger** (a ledger would grow without bound). The combine rules come from
  `AppSettings.combineRules`, threaded from QML through
  `UsbGrabberController.combineRules` into every grab and preview — deliberately
  NOT `TranscriptionController::m_combineRules`, which is a cache that's empty
  until the first combine scan and reset each launch, so it would miss a device
  replugged in a fresh session. When no combine rules are configured the check
  is inert and the plain quick-check dedup stands alone.
  Triggered either manually (`UsbGrabberController::triggerManualGrab()`) or
  automatically on hotplug when the two gates above both pass and a vault is
  open — auto-grabs always target whichever vault is open at that moment, never
  a pinned path.
- **A grab is a queued task, not its own thread.** `UsbGrabberController::
  startGrabJob()` hands the resolved rule to `TranscriptionController::
  enqueueUsbGrab()`, so a grab is an ordinary `UsbGrabTask` in the ONE task
  queue alongside transcribe/import/combine, and inherits that queue's Tasks
  row, progress glow, and pause/cancel with no bespoke plumbing (`UsbGrabTask::
  run()` just wires `runUsbGrabScan`'s hooks to the phase/progress/pause/cancel
  contract; pause/cancel land at a file boundary, since `QFile::copy()` is
  uninterruptible). This replaced a
  dedicated QThread per grab. The cost, accepted: a hotplug grab that lands
  mid-transcription waits its turn instead of starting immediately — which also
  means it can't fight the transcription for the same disk. Grab jobs are
  **never persisted** across a restart (`persistQueue()` skips them), for a
  stronger version of the reason import jobs aren't: the mount path names
  removable media that is likely gone next session, and a reused mount point
  would make a restored job walk a *different* device. Replugging re-fires the
  auto-grab; that's the resume.
  - The **preview** stays on its own thread (`UsbGrabPreviewJob`) rather than
    joining the queue: it's an interactive answer to "what would this rule do?"
    wanted immediately while editing a rule, and it writes nothing — queueing it
    behind a running transcription would make the rule editor unusable.
  - The **history entry** is written by `UsbGrabberController` on the GUI thread
    when the job reports back (`TranscriptionController::usbGrabFinished`),
    never by the worker (`VaultTreeModel` is GUI-thread-owned). A **cancelled**
    grab writes no history entry and posts no snackbar: its counts are a partial
    tally, and reporting "grabbed 3 files" for a run the user stopped would
    misstate what happened — the Tasks row already tells that story.
  - **Why not a content hash** (it was one, until it wasn't): hashing is O(bytes on
    the device), and it's slowest in exactly the case it's asked about most — a
    device whose files are *already* grabbed, where every candidate is byte-identical
    so no size check can short-circuit and every file is read in full. Measured on a
    13.8 GB recorder over USB at ~9 MB/s, a preview took **~25 minutes** and looked
    hung. The quick-check reads no file content and answers instantly. The tradeoff
    accepted: a file that keeps its name, size AND mtime while its bytes change is
    wrongly skipped — recorders don't do that, and the cost is a visible missed
    import, not data loss (nothing is ever overwritten).
  - **The copy must stamp the source's mtime onto the copy** (`QFileDevice::
    setFileTime`). `QFile::copy()` gives the copy a *fresh* mtime, so without the
    stamp no grabbed file would ever match its source again and every grab would
    re-copy the whole device under `_1`/`_2`/… suffixes. The quick-check is only
    sound because of this.
  - **The copy must verify its own size** — `QFile::copy()`'s return value is not
    trustworthy on removable media. Verified against Qt 6.11: when the source stops
    delivering bytes partway (a recorder unplugged mid-grab) it returns **true**,
    having written a *truncated* file. Unchecked, that file lands in the vault
    wearing a recording's name — the tree lists it, it plays, it transcribes into
    garbage — and the run reports a clean success with it counted in `filesCopied`.
    So `copyGrabbedFile()` compares what landed against the stat the walk already
    took and, on a mismatch, deletes it and reports an ordinary per-file error
    (non-fatal: the rest of the device still grabs). It rejects any *mismatch*
    rather than only a short file, so a source still being written on the device is
    caught too — half a recording is worth less than a re-grab that gets it whole,
    and the next grab does exactly that.
    - **Why no temp-file staging of our own:** Qt copies through an unnamed
      `O_TMPFILE` inode and `linkat()`s it into place at the end, so a copy that
      never finishes at all (crash, power loss) leaves the destination untouched —
      verified by `SIGKILL`ing a 3 GB copy mid-flight and finding an empty
      destination folder. Hand-rolled `.partial` staging would only trade that for
      litter of our own to clean up. The size check is the part Qt doesn't do.
- **Preview:** `UsbGrabberController::requestGrabPreview()` runs the identical
  scan/filter/quick-check logic in a dry-run mode (`runUsbGrabScan(dryRun=true)`,
  driven off the GUI thread by `UsbGrabPreviewJob`) — literally the same function
  a real grab runs, so the preview cannot drift from what the grab would do —
  no copying, no history entry — and returns a line-by-line classification of
  every file the scan sees: `"grab"` (would be copied), `"duplicate"` (matches the
  filters but a file with the same name, size and mtime is already present at the
  destination), or `"filtered"` (failed the extension filter and/or the filename
  regex — one status covers both, since the entry carries the path and the
  pattern field's own indicator is what explains a regex that won't compile).
  Async, like the real grab: the walk still stats every file on an external
  device, which is too slow for the GUI thread.
  - Re-run automatically on any rule-field commit while that device's preview is
    on screen (`MainPanel.qml`'s `commitRuleField()`), so the preview follows an
    edited regex the way the combine preview follows its pattern field — but on
    *commit*, never per keystroke: this walks a physical device, unlike the
    combine preview's pure in-memory pass over the vault's file list.
- **History:** each completed grab appends one entry (timestamp, device, files
  copied/skipped, errors) to the vault-scoped `.transcriber/usbGrabHistory.json`
  via `VaultTreeModel::appendUsbGrabHistoryEntry()`.

## Model management

- Models are large (whisper large-v3 ≈ 3 GB, Parakeet ≈ 2–4 GB).
- **One app-global store, shared across every vault** (`ModelManager::
  modelsStoreDir()` — the writable generic-data dir, `~/.local/share/
  suopTranscriber/models`; `SUOP_MODELS_DIR` overrides it, which is how tests
  redirect to a temp dir). A multi-GB model is downloaded once and reused
  everywhere; there's a single directory to back up. This replaced the earlier
  per-vault `<vault>/.transcriber/models/` layout (pre-stable, no migration).
- **Bundled search path (durability escape hatch):** every presence check and
  path lookup resolves store-first, then a read-only "bundled" dir shipped
  beside the binary (`bundledModelsDir()` = `applicationDirPath()/models`;
  `SUOP_BUNDLED_MODELS_DIR` overrides). No models ship by default, but a
  distributor (or the user) can drop model files there so the app works even
  if a download URL breaks. Downloads/imports/deletes only ever touch the
  writable store; a bundled model is read-only (Delete is disabled for it).
- **Settings → Models page** (`ModelsPage.qml`, a nav-rail page): lists every
  app-managed model — the transcription tiers + Parakeet, plus the support
  models (auto-label GGUF / Silero VAD / diarization pair) — with a
  downloaded/size indicator and per-model actions:
  - **Predownload** a model ahead of use (`downloadModel()` queues the same
    `"download"` job kind used lazily before a transcribe — progress/pause via
    the Tasks queue).
  - **Delete** (`deleteModel()`) — removes from the writable store only;
    permanent (models are large + re-downloadable, so NOT trashed), and IS
    confirmed (unlike a reversible vault-file delete — a re-download can fail,
    which is the risk this whole feature guards against).
  - **Import** a user-supplied copy (`importModel()`) — a file for single-file
    models, a folder for the Parakeet bundle / diarization pair — copied into
    the store so it resolves as downloaded with no network.
  - **Export** (`exportModel()`) — copies the file(s) to a chosen folder for
    backup, in the exact shape `importModel()` expects back.
  - Import/export are `ModelIoTask`s — real queued tasks (Serial lane, since a
    multi-GB copy is heavy IO; Background presentation, since they're launched
    from this settings page, not the main panel). They get a Tasks row + the
    glow like any task and answer via `modelOperationFinished`; `modelsChanged`
    fires on a successful import (and when a (pre)download job completes) so the
    page refreshes — via the one central `completionChangesModelSet()` hook,
    not a per-kind emit site (see the task-system section). (This replaced a
    fire-and-forget `QThread` that had no Tasks row at all — exactly the
    invisible-work failure the task system removes.)
- Capability detection checks which model files are present, not just which engine
  binaries exist.
- **Unrecognized model id → visible fallback, never a silent one.** If a
  requested model id doesn't match any entry in the catalog (e.g. a stale
  last-used-model setting after a model tier is removed), `ModelManager`
  substitutes its default model rather than failing the job outright. That
  substitution is never silent: it's logged (`qWarning`) *and*, because this
  app's transcripts are their own source of truth (see "Vault / file
  layout"), recorded directly in the resulting `.md`'s frontmatter via
  `modelRequested` + `modelFallback: true` — the file never reads as though
  the requested model had actually been used. This is orthogonal to the
  speaker-detection tdrz override above, which is an intentional, always-on
  substitution rather than a fallback for bad input.

## Iconography (swappable icon packs)

Icons are a **pack system**, not hardcoded glyphs. The whole UI names icons
semantically (`AppIcon { name: "transcribe" }`); which pack draws them is a
setting.

- **A pack is a directory** under `assets/icons/<id>/`: a `pack.json` manifest
  plus, for svg packs, the SVG files it names. Packs are **discovered** by
  scanning that tree in the Qt resource system (`IconTheme::loadPacksFrom`) —
  there is no hardcoded pack list, so loading packs from a user directory
  later is the same loader over a different root (deliberately the only thing
  that changes). The CMake glob that bundles the assets is `RELATIVE` so each
  resource alias matches `IconTheme`'s `kIconRoot`.
- **Two built-in packs.** `material` (default) — Google Material Symbols
  Rounded, one SVG per icon, fills stripped so the app owns tint. `classic` —
  the app's original hand-rolled set (Unicode glyphs + two drawn shapes),
  kept as an opt-in pack. `material` is also the **fallback**: it must define
  every name any pack defines, so resolution can't bottom out at nothing
  (asserted by `tst_icontheme`).
- **Three icon kinds**, so one pack can mix them: `glyph` (a Unicode char in
  a `Text`), `svg` (rastered + tinted by `IconImageProvider`), and `builtin`
  (a small drawn `Canvas` component — `PlayGlyph`/`PauseGlyph`, which exist
  because a font `▶` can't be reliably centered on its triangle). `AppIcon`
  routes to the right renderer; its `builtins` map is closed, never an
  arbitrary URL a pack could name.
- **Resolution order:** per-icon override → active pack → fallback pack, each
  step skipped (not terminal) if that pack lacks the name, so a partial pack
  degrades per-icon. `IconTheme::resolve(name, packId, overrides)` is a
  **pure function** of its args — `AppIcon` binds it to both
  `AppSettings.iconPack` and `AppSettings.iconOverrides`, so switching packs
  or overriding one icon re-resolves every icon in the app with no
  change-signal plumbing.
- **Two vault-scoped settings** (`AppSettings`, persisted like all
  appearance prefs): `iconPack` (the default pack) and `iconOverrides` (a
  `name → packId` map letting the user pull individual icons from another
  pack). Both are editable from Settings → Appearance (a pack-picker chip row
  plus a per-icon override grid).
- **Rendering cost:** `IconImageProvider` tints each svg once into a QImage
  cached by `(pack, file, color, size)` — chosen over per-call-site
  `MultiEffect { colorization }` because the app instantiates 100+ icons
  (the sidebar's per-row clusters exist for every row, opacity-0 until
  hover); that many live offscreen render passes would be wasteful for what
  are static monochrome images.
- Call-site conventions live in StyleGuide.md "Iconography".

## Licensing

- whisper.cpp MIT; faster-whisper/CTranslate2 MIT; NeMo Apache-2.0.
- **Google Material Symbols** (the default icon pack, vendored under
  `assets/icons/material/`) is Apache-2.0 — `LICENSE` ships beside the SVGs.
- **Qt: dynamically link** (LGPL) if distributing closed-source.
- ffmpeg: **LGPL build** (audio-only needs nothing GPL).
- Out-of-process engines keep any GPL-ish Python deps behind a process boundary,
  which is also a license boundary.

## Packaging: Flatpak is the production target

Production distribution is Flatpak. This is compatible with the architecture
above with a few constraints to keep in mind as features are added:

- **Vault folder access goes through the desktop portal**, not raw filesystem
  access — this is already how `QtQuick.Dialogs`' `FolderDialog` behaves on
  Linux, so no rework: the sandbox gets scoped access to just the folder the
  user picks, no `--filesystem=host` needed.
- **Flatpak builds are Vulkan-only.** CUDA has no standard Flatpak GPU
  extension (unlike Vulkan/GL, which get `--device=dri` + a matching driver
  extension). Build Flatpak releases with `GGML_VULKAN=1` / `GGML_CUDA=0` —
  consistent with Vulkan already being the cross-vendor floor, not a
  Flatpak-only carve-out.
- **ffmpeg** is pulled via the shared `org.freedesktop.Platform.ffmpeg-full`
  extension rather than bundled, per usual Flathub convention.
- **whisper.cpp is vendored as a pinned `git` source** in the manifest
  (mirroring the submodule commit) since `flatpak-builder` builds offline by
  default — no live network fetch during the sandboxed build. The Vulkan
  shader toolchain (glslc, spirv-headers) is a *build-time-only* sandbox
  dependency; end users only need the Vulkan loader + driver at runtime.
- **Known friction to solve when their milestones arrive, not now:**
  - USB auto-detection (`QStorageInfo::mountedVolumes()`) likely won't see
    host mounts from inside the sandbox without extra permissions or a
    udisks2 D-Bus based rewrite.
  - Downloaded models should resolve via `QStandardPaths::AppDataLocation`
    rather than a hardcoded path, so they land in the per-app Flatpak data
    dir automatically.

The actual `flatpak-builder` manifest (runtime, permissions, pinned sources)
is deferred until there's more app to package — tracked as follow-up work,
not blocking current feature development.

## Suggested first milestone

Vault open + sidebar tree with status indicators + play a file with
`QMediaPlayer`. This exercises the file layout and audio backend before touching
the engine protocol.
