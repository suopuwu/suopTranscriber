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
(`src/core/DiarizationEngine.h/.cpp`), sherpa-onnx's pyannote segmentation +
WeSpeaker embedding + agglomerative clustering, independent of which whisper
model is selected (unlike the old tdrz heuristic, it never constrains model
choice). Speaker count is always auto-detected via a clustering distance
threshold — no "number of speakers" UI.

It runs one full pass over the same 16 kHz mono PCM whisper transcribes,
producing `DiarizedTurn`s (`{start, end, speaker}`), and each segment is tagged
with the diarized turn it overlaps most (`labelForSegment`). Two ways to invoke
it, both through `TranscriptionController`'s single worker thread and sharing one
diarization-core routine (`TranscriptionWorker::runDiarizationPass`):

- **Inline, at transcribe time** — the `speakerDetection` toggle. Diarization
  runs *before* whisper so the real speaker-label set is known when frontmatter
  is written; each segment is labeled as it streams in. A failure here never
  blocks transcription — ASR still runs, ASR-only, and frontmatter records
  `speakerDetection: false` + `speakerDetectionError` (never-misrepresent).
- **Standalone, after the fact** — the `diarizeOnly` job (`diarizeFile()`, the
  MainPanel "Detect speakers" action). Re-runs (or adds) speaker detection on an
  already-transcribed file *without* re-transcribing: reads the existing `.md`'s
  segments, decodes the audio, diarizes, and splices the speaker tags back in
  (`spliceBodySpeakers`). A diarization failure here *is* the job's result.

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
  substitution.
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
    models/                     # downloaded model files (large: 2–4 GB each)
  interviews/2026-06-30.m4a
  interviews/2026-06-30.md              # human-readable transcript = "done" (green)
  interviews/.2026-06-30.partial.jsonl  # exists only mid-transcription (resume)
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
  preference. USB device allowlist and a model registry (below) are NOT part
  of this file yet — still not implemented.

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

## USB import

- Enumerate removable drives with `QStorageInfo::mountedVolumes()`.
- User picks device(s); persist their identifiers in `config.json`.
- On "grab new," diff by filename + hash against the vault and copy only new
  audio in.

## Model management

- Models are large (whisper large-v3 ≈ 3 GB, Parakeet ≈ 2–4 GB). Provide a
  download/management UI and a `models/` dir; registry in config.
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

## Licensing

- whisper.cpp MIT; faster-whisper/CTranslate2 MIT; NeMo Apache-2.0.
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
