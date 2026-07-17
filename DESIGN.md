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

## Benchmark (top-level page)

A display-only comparison harness, its own **top-level page** (a TitleBar icon
button next to the USB grabber, mounted by `MainPanel` in a `ScrollView` — no
longer a Settings sub-page). It defaults to the file open in the left panel
(`currentFilePath`), falling back to a vault-wide picker. Two sweeps, both on the
transcription worker thread (so they serialize with real jobs and never contend
for the GPU), writing **no** `.md`:

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
model. The selection + sensitivity are carried on the `diarizeOnly` job and can
be swept in the benchmark (see below) to compare combinations on a hard file.
See `ModelManager::availableSegmentationModels()`/`availableEmbeddingModels()`.

It runs one full pass over the same 16 kHz mono PCM whisper transcribes,
producing `DiarizedTurn`s (`{start, end, speaker}`), and each segment is tagged
with the diarized turn it overlaps most (`labelForSegment`). Diarization is a
**standalone task, never bundled into transcription** — **Transcribe is pure
ASR** (plus its intrinsic VAD windowing; see below). It's invoked two ways, both
through `TranscriptionController`'s single worker thread and sharing one
diarization-core routine (`TranscriptionWorker::runDiarizationPass`):

- **Standalone** — the `diarizeOnly` job (`diarizeFile()`, the MainPanel /
  Sidebar-ribbon "Detect speakers" action). Runs (or re-runs) speaker detection
  on an already-transcribed file *without* re-transcribing: reads the existing
  `.md`'s segments, decodes the audio, diarizes, and splices the speaker tags +
  `speakerDetection: true`/`speakers:` back in (`spliceBodySpeakers`). A
  diarization failure here *is* the job's result.
- **As a `runAllJobs()` pipeline step** — the one-click "do everything" path
  queues transcribe → **detect speakers** → label → chapters as separate FIFO
  jobs (the diarize step reads the `.md` the transcribe job just wrote). This
  replaced the old inline `speakerDetection` toggle on transcribe.

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
  below) — a killed job leaves a truncated `.md` and a re-run starts over
  from scratch, it just no longer re-decodes/re-runs VAD as one giant
  single-shot pass now that chunking exists underneath where resume will
  eventually hook in.

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
  Watch with `QFileSystemWatcher` for live updates.
- **No retranscription:** presence of `.md` means done.
- **All transcription data lives in the single `.md` — no separate JSON
  sidecar** (this supersedes an earlier draft of this doc that split timing
  into a hidden `.sentences.json`; CLAUDE.md's load-bearing constraint is the
  current decision). Format:
  - YAML-ish frontmatter: `model` (the ggml file that actually ran),
    `speakerDetection` (bool), `transcribedAt` (ISO 8601). Frontmatter is
    written the moment a job starts, before transcription runs, so a
    killed/stopped job still leaves a file recording what it was run with.
  - One `[HH:MM:SS.mmm - HH:MM:SS.mmm] text` line per segment, appended and
    flushed as each segment is produced — not buffered to the end. An
    optional trailing `[SPEAKER_TURN]` marker marks a tinydiarize speaker change.
    This is also where sentence-highlight sync will later re-derive timing
    from, once that milestone lands (see "Sentence highlighting" above).
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
- **Never automatic.** Both entry points are user-initiated. Silently merging
  on every file-add is a surprise a recorder shouldn't spring on the user.
  The one exception is `maybeRedirectForCombine()`, a safety net at the top of
  `startJob()`: transcribing a file that is a member of a currently-"ready"
  group inserts the combine ahead of it and retargets the job at the merged
  output, so a part that appeared after the user's last scan can't get
  transcribed alone.
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
  surviving filename after the other parts have moved into `.combined/`.
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
  A combine never also queues a transcribe — the merged file just becomes a
  normal untranscribed vault entry.
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
  catch-all — which is the same predicate the grab uses, so the app mounts precisely
  what it would grab from. Deliberately NOT a setting of its own (off by default, it
  would leave the out-of-box experience exactly as broken) and deliberately not
  unconditional (that makes a transcription app into a general-purpose automounter for
  every photo SD card the user owns). Known cost: an unconfigured device with no
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
- **Grab (`UsbGrabJob`/`UsbGrabJobWorker`, `src/core/UsbGrabJob.h/.cpp`):** runs on
  its own worker thread (same QThread+moveToThread shape as `AudioRecorder`), never
  the GUI thread — recursively walks the device, diffs against files already in the
  destination folder by **rsync's quick-check: filename + size + mtime**, and copies
  only files that are both filter-matching and not already present (never silently
  overwrites a same-named-but-different file — appends a numeric suffix instead).
  Triggered either manually (`UsbGrabberController::triggerManualGrab()`) or
  automatically on hotplug when a device's own rule (or the catch-all) is enabled
  and a vault is open — auto-grabs always target whichever vault is open at that
  moment, never a pinned path.
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
- **Preview:** `UsbGrabberController::requestGrabPreview()` runs the identical
  scan/filter/quick-check logic in a dry-run mode (`UsbGrabJob::startPreview()`) —
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
  - Import/export run the (possibly multi-GB) copy on a background thread and
    answer via `modelOperationFinished`; `modelsChanged` also fires when a
    (pre)download job completes so the page refreshes.
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
