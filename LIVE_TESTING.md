# Live test plan

The manual checklist for what automation **can't** reach. suopTranscriber has a
deep automated suite (60 test binaries, ~22 of them real offscreen `QQuickView`
harnesses that drive actual QML with synthetic mouse/key events — see
`tests/`). That suite already covers most *UI logic*. This document is the
complement: the flows that need a **real GPU, real audio device, real
compositor, real filesystem watcher, or a human eye** to sign off.

## How to use this

- Each item is tagged:
  - **[LIVE]** — genuinely needs a human / real hardware; no automation substitutes.
  - **[AUTO+]** — the *logic* is already automated (test named); live-check only
    confirms it wired up in the real app (a quick spot-check, not a deep pass).
  - **[VIS]** — visual/pixel correctness; a human eye or a screenshot-diff (see
    "Reducing this list" at the end).
- Run against a **scratch vault**, never your real one. Launch with an explicit
  path: `./suopTranscriber /path/to/scratch-vault`. Note that this still
  persists `lastVaultPath` — if you care about your real vault being the
  remembered one, set it back afterward.
- `[ ]` boxes are for a run. Copy the file or check them off in your editor.
- Order matters only within a section; sections are independent.

Priority markers: **P0** = ship-blocker if broken, **P1** = important, **P2** =
polish/edge.

---

## 1. Vault & tree navigation

- [ ] **P0 [LIVE]** Open a vault via the folder dialog; open a *different* vault
  — the tree, title, and BottomBar path segments all repoint; no stale rows.
- [ ] **P1 [LIVE]** Open a vault with nested folders + an empty folder — expand/
  collapse each; the empty folder's glyph toggles (this one has a history of
  breaking: `tst_sidebar_expand`).
- [ ] **P1 [AUTO+]** Single-click a note opens it and single-selects it;
  Ctrl-click toggles; Shift-click ranges; Ctrl+A selects all. *(Logic:
  `tst_sidebar_selectall`, `tst_sidebar_keynav`.)* Live-check: the row
  highlight + the "currently open" outline look right together.
- [ ] **P1 [LIVE]** Keyboard nav: arrows move + open; Shift+arrows extend;
  Ctrl+arrows jump groups; Home/End. Focus has to be in the tree.
- [ ] **P2 [VIS]** Depth guides / indentation lines render correctly at each
  nesting level. *(Geometry: `tst_sidebar_guides`.)*

## 2. Transcription — the core, and GPU-bound

- [ ] **P0 [LIVE]** Transcribe a short real audio file with the default whisper
  tier. Segments stream in live; the file's tree indicator goes
  transcribing→transcribed; the sibling `.md` appears with correct
  frontmatter + `[start] text` lines.
- [ ] **P0 [LIVE]** Transcribe with **each** downloaded whisper tier
  (tiny/base/small/medium/large) — at least smoke each once; larger tiers
  exercise the model-load phase timing + memory.
- [ ] **P0 [LIVE]** Transcribe with **Parakeet-TDT (in-process, sherpa-onnx)** —
  `parakeet-tdt-0.6b-v2`. Confirm it's a normal downloadable tier and produces
  segments. *(Token→segment assembly: `tst_parakeetengine`; the live run is the
  real-decode + GPU path.)*
- [ ] **P1 [LIVE]** Out-of-process engine via `engine:<id>` prefix (the
  `engines/parakeet/` NeMo skeleton, if NeMo is installed) — one process per
  file, chunk-offset merge owned by the app.
- [ ] **P1 [LIVE]** Cross-vendor GPU: if you have a non-NVIDIA GPU available,
  smoke a transcription there (the dev GPU is NVIDIA; cross-vendor is a claim
  that only a real device proves).
- [ ] **P1 [LIVE]** Transcribe a LONG file (multi-chunk) — the app owns the
  chunk timestamp-offset merge; segments across chunk boundaries must be
  continuous, no duplicated/dropped text at seams.
- [ ] **P0 [LIVE]** Never runs on the GUI thread: the UI stays responsive
  (scroll, click other files) while a transcription runs. *(Thread assertion:
  `tst_transcription_queue::transcription_neverRunsOnGuiThread…`; live-check is
  actual responsiveness.)*

## 3. Playback & sentence highlighting — real audio device

- [ ] **P0 [LIVE]** Play a transcribed file. The active sentence highlights and
  tracks audio position; clicking a sentence seeks the audio to it. This is
  sentence-level only (no word timestamps) — the highlight should snap to
  sentence boundaries on `positionChanged`, never interpolate.
- [ ] **P1 [LIVE]** Seek around (scrub the position); the highlight binary-
  searches to the right sentence with no lag/flicker.
- [ ] **P1 [LIVE]** Confirm `QT_MEDIA_BACKEND=ffmpeg` is in effect (playback
  works on formats Qt's default backend wouldn't decode).
- [ ] **P1 [LIVE]** Live-follow: while a file is actively transcribing and you're
  scrolled at the bottom, new segments scroll into view; if you scroll up to
  re-read, it does NOT yank you back down. *(Regression-prone: `tst_mainpanel_livescroll`
  covers the math; the real feel needs a human.)*

## 4. Speaker diarization & editing

- [ ] **P0 [LIVE]** Detect speakers on a multi-speaker file (standalone diarize,
  no re-transcribe). `[Speaker N]` tags appear; frontmatter gains
  `speakerDetection`/`speakers`.
- [ ] **P1 [AUTO+]** Rename a speaker (inline, per-keystroke) → all its segments
  update. Merge two speakers → source folds into target. *(Logic:
  `tst_task_speaker_edits`, `tst_transcript_format`.)* Live-check: the pills
  update live and the `.md` is correct on disk.
- [ ] **P1 [LIVE]** Diarization tuning / model choice (segmentation + embedding
  pair) actually changes results; the pair downloads in-app.
- [ ] **P2 [LIVE]** Transcribe-with-speaker-detection inline (vs standalone): a
  diarization failure degrades to ASR-only, not a hard task failure.

## 5. Labeling, chapters, prelude

- [ ] **P1 [LIVE]** Auto-label a transcript → a label appears on the tree row +
  frontmatter. Regenerate label. *(Windowing: `tst_labelingengine`.)*
- [ ] **P1 [LIVE]** Generate chapters → chapter headings appear inline; collapse/
  expand a chapter persists to frontmatter. *(Refresh + selection:
  `tst_mainpanel_chapterrefresh`, `tst_mainpanel_chapterselection`.)*
- [ ] **P1 [LIVE]** Chapter density / label wordiness sliders change output.
- [ ] **P2 [LIVE]** Prelude renders (if present) above the transcript.

## 6. File operations — all are Tasks, all undoable where applicable

Each of these is a `Task`; verify the **Tasks row appears**, the operation
happens, and (where applicable) **one undo snackbar** reverses it.

- [ ] **P0 [AUTO+]** Rename a file (inline) → tree + open file update; undo
  snackbar renames back. *(`tst_task_renamefile`.)*
- [ ] **P0 [AUTO+]** Move a file to another folder (context menu / drag) → moves;
  undo. Multi-select move N files → **one** "Moved N files" undo snackbar
  reverses all N. *(`tst_task_move`, `tst_transcription_queue::moveFiles_batchUndo`,
  `tst_sidebar_dragmove`.)* Live-check the drag gesture feel.
- [ ] **P0 [AUTO+]** Trash a file (whole file) while it's **open** → the panel
  closes it; undo restores + reopens **and** the restored tree row is a real
  selection you can Shift-click from; the reopened file's **delete button is
  active again and correctly colored**. *(Automated: `tst_sidebar_selectall::
  restoredNote_becomesShiftClickAnchor`, `tst_mainpanel_deleteundo::
  asyncControllerDelete…` + `deleteFileButton_visibilityAndColorReflectState`.)*
  Live-check only the real trash + the feel of it.
- [ ] **P0 [AUTO+]** Trash **transcript only** (keeps audio) → reverts to
  untranscribed; undo. Trash **audio only** (keeps transcript) → panel follows
  the orphaned `.md`; undo brings audio back. *(`tst_mainpanel_deleteundo`,
  `tst_task_trash`.)*
- [ ] **P1 [LIVE]** Permanent delete (when trash refuses / explicit) → confirm
  dialog matches its siblings (modal, dim, Escape cancels); NOT undoable.
- [ ] **P1 [AUTO+]** Trash/rename/move a **folder** → tree updates; undo where
  applicable. *(`tst_task_folder`.)*
- [ ] **P1 [LIVE]** Combine N files into one → merged output; optionally deletes
  originals; the combine preflight retargets correctly. *(`tst_filecombine`.)*
- [ ] **P1 [LIVE]** Import external audio into the vault → copied in, appears in
  tree. *(`tst_transcription_queue::importAudio…`.)*
- [ ] **P1 [LIVE]** USB grab: plug a device matching a rule → files copied in as
  a Task. *(Rules: `tst_usbrulestore`, `tst_usbgrabbercontroller`; the hotplug +
  real copy is live.)*
- [ ] **P0 [LIVE]** Parity check: every per-file action (transcribe, detect
  speakers, regenerate label/chapters) exists BOTH in the Sidebar ribbon (bulk)
  AND the MainPanel icon cluster (single open file). *(Source parity guard in
  `tst_styleguide`; live-check both fire.)*

## 7. Task queue mechanics

- [ ] **P0 [LIVE]** Queue several transcriptions → they run one at a time
  (Serial lane); a rename/move (Instant lane) does NOT wait behind a running
  transcription. *(Lane logic: `tst_transcription_queue`.)*
- [ ] **P0 [LIVE]** Pause an active transcription → it actually stops making
  progress; resume → continues. Cancel a queued one → it leaves the queue and
  still reports finished (nothing hangs).
- [ ] **P1 [LIVE]** Pause during the "loading" phase is a no-op (not
  reinterpreted as cancel) — a known-tricky case.
- [ ] **P0 [LIVE]** **Persistence across restart**: queue work, quit the app,
  relaunch → Queued tasks reappear (Active/Paused persisted as Queued); they do
  NOT auto-start until you resume. *(`tst_transcription_queue::persistedQueue…`;
  live-check the actual quit/relaunch.)*
- [ ] **P1 [VIS]** BottomBar during work: the kind label (e.g. "Move to trash"),
  the phase label (e.g. "transcribing…"), the % and ETA, and the glow all read
  correctly and update per phase. *(Registry + label logic: `tst_styleguide`
  phase/kind guards, `tst_bottombar_pathsegments`.)*
- [ ] **P1 [LIVE]** A Background task (benchmark/download) shows a Tasks row +
  glow but does NOT hijack the main-panel overlay; a Foreground task (transcribe)
  does own the overlay.
- [ ] **P2 [LIVE]** Shut the app down mid-transcription → it stops promptly
  (does not keep running / block on the in-flight call).

## 8. Models page

- [ ] **P0 [LIVE]** Download a model tier → progress via the Tasks queue; on
  completion the Models page refreshes to "downloaded". *(Central refresh hook:
  `Task::completionChangesModelSet`.)*
- [ ] **P1 [LIVE]** Import a model (single file, and the Parakeet/diarization
  folder shapes) → resolves as downloaded with no network; the page refreshes.
- [ ] **P1 [LIVE]** Export a model → files land in the folder in import-ready
  shape; the page does NOT falsely refresh (export doesn't change the set).
- [ ] **P1 [LIVE]** Delete a model → confirm dialog (models are large +
  re-downloadable, so confirmed, unlike a reversible vault-file delete); removes
  from the store.
- [ ] **P2 [LIVE]** Unrecognized model id → visible fallback, never silent.

## 9. Search & filters

- [ ] **P1 [LIVE]** Text search filters the tree live as you type; a match opens
  the file + jumps to/highlights the phrase.
- [ ] **P1 [LIVE]** Tag filter and speaker filter (clicking a pill) narrow the
  tree; the search-scoped ribbon actions act on the filtered set.
- [ ] **P2 [LIVE]** Search include-transcriptions toggle re-runs against the
  right corpus. *(Proxy logic: `tst_vaultsearchproxymodel`.)*

## 10. Window chrome & compositor — Wayland/KWin, hardest to automate

- [ ] **P0 [VIS/LIVE]** Blur-behind renders behind the window; rounded corners
  clip; the window is actually translucent where intended. *(No automation —
  needs a real compositor. See the `qt-window-effects` skill.)*
- [ ] **P1 [LIVE]** Frameless / titlebar toggle takes effect on Wayland.
  *(`tst_titlebar_buttons` covers button logic, not the frameless flag.)*
- [ ] **P1 [LIVE]** Middle-click autoscroll in the transcript (Chrome-style pan)
  works and doesn't break left-click editing or hover cursors.
  *(`tst_middledragscroll` covers wiring; the gesture feel is live.)*

## 11. Snackbars, notifications, settings

- [ ] **P1 [AUTO+]** Undo snackbars appear for undoable ops and stack/replace
  correctly; errors post as error notifications. *(`tst_snackbar`.)*
- [ ] **P2 [LIVE]** Settings popup: changing accent color / default model /
  toggles takes effect and persists.
- [ ] **P2 [LIVE]** Benchmark page runs a real benchmark and its file-sync
  behaves. *(`tst_benchmark_combos`, `tst_benchmarkpage_filesync`.)*

## 12. Theme & visual regression [VIS]

- [ ] **P1 [VIS]** Light and dark themes both render every screen correctly
  (colors, contrast, hover outlines, cursors per StyleGuide.md).
- [ ] **P2 [AUTO+]** Transcript text wraps within its width (no horizontal body
  scroll). *(Automated: `tst_mainpanel_deleteundo::
  transcriptText_wrapsWithinWidth_noHorizontalOverflow`.)* Live-check only the
  window-chrome / other screens at narrow+wide widths.
- [ ] **P2 [VIS]** Delete-file button affordance: hidden with nothing open,
  red/enabled when open, grey/disabled while its file has a running job.
  *(Automated: `tst_mainpanel_deleteundo::deleteFileButton_visibilityAndColorReflectState`;
  opt-in pixel check: `tst_visual_snapshot::deleteIcon_enabledAndDisabledColors`.)*

---

## Reducing this list (candidates to pull into automation)

The offscreen `QQuickView` + `QTest` harness already covers most UI logic, so we
are **not** at the fundamental limit of the current approach — most remaining
[LIVE] items are [LIVE] because they need *real hardware* (GPU, audio, USB,
compositor), which no in-process framework changes.

**The reliable tier — property/geometry/color assertions — is the default, and
it's being extended.** Rather than diff pixels, assert the *observable that a
screenshot would only be a proxy for*: a color property, a `visible`/`enabled`
binding, a layout invariant (`contentWidth <= width`, no-gap segment geometry).
These are deterministic and machine-independent. Recent conversions:
`deleteFileButton_visibilityAndColorReflectState` (affordance color/visibility),
`transcriptText_wrapsWithinWidth_noHorizontalOverflow` (no h-scroll), joining the
existing `tst_bottombar_pathsegments` geometry checks. **Prefer this tier** —
most `[VIS]` items reduce to a property or geometry assertion.

**The pixel tier — `tst_visual_snapshot` — now exists, opt-in and dormant.** It
renders real QML offscreen and diffs against a golden with a perceptual
tolerance (2% of pixels may differ by ≤24/channel, absorbing AA/raster noise).
It does nothing unless run with `SUOP_SNAPSHOT=update` (generate goldens) or
`=compare` (check). Goldens are environment-specific and `.gitignore`d — a
normal `ctest` run skips it entirely. Reserve it for *emergent rendering* a
property can't capture (icon compositing, clipping); it is the coarse, flakier
fallback, not the default. Extend it by adding scenes (whole screens, dialogs,
both themes) once goldens are generated on the target machine.

Still worth doing (all in reach of the existing pattern):

- **A scripted "smoke flow" over the real app** — open→transcribe(tiny)→play→
  trash→undo, asserting on-disk `.md` + queue state. Needs a small real model but
  runs unattended; covers integration seams unit tests miss.
- **Compositor checks via the KWin-script + D-Bus technique** (`kwin-wayland-*`
  skills): position-save-on-drag-end and window-placement are scriptable even
  though blur/translucency pixels are not.
- Per-feature harness gaps: search-filter tree behavior, ModelsPage download/
  import/export refresh wiring, pause/resume phase transitions end-to-end.

**Assessment:** adopting a heavy external UI framework (Squish, GammaRay-driven
suites, an Appium/Selenium-style external driver) is **not** warranted — it
would duplicate what the offscreen `QQuickView` harness already does well and
add a large dependency + maintenance surface, without addressing the true gaps
(real GPU/audio/compositor). The higher-leverage investment is the property-tier
extension above plus closing the per-feature harness gaps. If a case for an
external framework ever arises, it would be specifically for *cross-process,
real-window* driving on Wayland — worth a focused research pass only if the
KWin-script approach proves insufficient.
