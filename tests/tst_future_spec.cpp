#include <QtTest>

// Living-spec placeholders for behaviors the documentation commits to but the
// code does not implement yet. Each slot QSKIPs with the documented behavior
// as its reason, so every ctest run lists exactly what is still owed. When a
// feature lands, delete its placeholder here and write real tests for it.
class TstFutureSpec : public QObject
{
    Q_OBJECT
private slots:
    // sentenceHighlighting_syncedToPlayback: BUILT. TranscriptionController::
    // parseSentences()/activeSentenceIndex() (tested in
    // tst_transcript_format.cpp) + MainPanel.qml's RichText-span highlight
    // driven by AudioPlayer.positionChanged now implement DESIGN.md's
    // "Sentence highlighting" section. Visual sync correctness is on the
    // UI-validation list for manual testing (no click/playback automation in
    // this environment).

    // vadChunking_silenceAlignedWindows: BUILT. VadChunker::computeWindows()
    // (src/core/VadChunker.h/.cpp) chunks on silence via whisper.cpp's Silero
    // VAD in ~2-5 min windows, splitting only at gaps between VAD segments
    // (never mid-segment) — see tests/tst_vadchunking.cpp. The resume/
    // checkpoint half of this same DESIGN.md milestone (below) is still not
    // built.

    void partialJsonl_checkpointAppend()
    {
        QSKIP("DESIGN.md / CLAUDE.md: after each window, append committed segments to "
              "'.name.partial.jsonl' with the last global offset. NOT BUILT (CLAUDE.md marks it "
              "'future, not yet implemented'). Current durability is coarser: frontmatter at job "
              "start + per-segment flush of the .md itself.");
    }

    void partialJsonl_resumeFromOffset()
    {
        QSKIP("DESIGN.md: on restart, if the .md is missing but a partial exists, resume "
              "transcription from the recorded offset. NOT BUILT: a killed job leaves a truncated "
              ".md and a re-run starts over from scratch.");
    }

    void partialJsonl_completionDeletesPartial()
    {
        QSKIP("DESIGN.md: on full completion, write the .md and delete the partial. NOT BUILT "
              "(no partial file exists to delete).");
    }

    // offsetMerge_multiChunkStitching: BUILT. TranscriptionController::run()'s
    // worker loop now transcribes each VadChunker window as its own
    // AudioChunk and adds offsetSeconds to every segment in onSegment before
    // writing/labeling — see tst_engine_contract.cpp's
    // multiChunkStitch_producesGloballyOrderedOffsetSegments().

    // subprocessEngines_manifestAndCapabilities: BUILT. DESIGN.md "Pluggable
    // engines" is now real — EngineManifest parses manifest.json
    // {name, command, capabilities{languages, gpu, vram_gb_min}}
    // (tst_enginemanifest); EngineRegistry discovers engine folders (bundled +
    // <vault>/.transcriber/engines) and capability-detects them against
    // GpuProbe + a dependency self-test + model presence, graying out
    // unrunnable ones with a reason rather than dropping them
    // (tst_engineregistry); SubprocessEngine drives an engine's command via
    // QProcess and parses the JSONL wire protocol (tst_subprocessengine,
    // against a fake engine script). TranscriptionController selects an engine
    // via the "engine:" model-id prefix and exposes availableEngines() to QML.
    // The bundled Parakeet engine (engines/parakeet/) ships as a
    // verified-by-contract skeleton — it needs NeMo installed to actually run.

    // modelManagement_uiAndRegistry: BUILT. The Settings → Models page
    // (ModelsPage.qml) lists/deletes/predownloads/imports/exports every
    // app-managed model, backed by an app-global store + read-only bundled
    // search path (DESIGN.md "Model management"). Real coverage: tst_modelmanager
    // (deleteModel/importModel/exportModel/bundled resolution) +
    // tst_styleguide::settingsPopup_hasModelsManagementPage. (Converted from a
    // passing-QSKIP marker to a comment, matching the BUILT specs above.)

    void vaultConfigJson_appScopedSettingsDivergence()
    {
        QSKIP("DESIGN.md 'Vault / file layout': RESOLVED for settings (Wave E item 9) — "
              "appearance/behavior settings now round-trip through <vault>/.transcriber/settings.json "
              "(JSON), see VaultTreeModel::readVaultSettings/writeVaultSettings and "
              "AppSettings.qml's loadFromVault/saveToVault (tested: "
              "tst_vaulttreemodel.cpp's vaultSettings_* cases). RESOLVED for the USB device "
              "allowlist too (USB Grabber feature) — app-scoped via UsbRuleStore/QSettings, "
              "tested in tst_usbrulestore.cpp/tst_usbgrabbercontroller.cpp, deliberately NOT "
              "vault-scoped JSON (same physical device is used across multiple vaults). STILL "
              "DIVERGENT: a model registry is not implemented anywhere yet (no code to test "
              "against) — that part of this placeholder remains accurate.");
    }

    void noRetranscription_mdPresenceMeansDone()
    {
        QSKIP("DESIGN.md 'Vault / file layout': presence of the sibling .md means done — no "
              "retranscription. DIVERGENCE (deliberate, Milestone 3 decision): the Transcribe "
              "button re-runs and overwrites; only the status dot reflects .md presence. The "
              "queue does dedup concurrent requests (covered by tst_transcription_queue).");
    }

    void segmentMergeSplit_onSentencePunctuation()
    {
        QSKIP("DESIGN.md 'Sentence highlighting': merge/split native whisper segments on .?! "
              "keeping the first segment's t0 and the last's t1 — explicitly 'not built day one'. "
              "NOT BUILT: native segments are used as-is.");
    }

    void instantLane_runsInParallelWithSerialQueue_deferringOnlyOnSameFile()
    {
        QSKIP("Task system (DESIGN.md 'Task system'): a Lane::Instant task (rename/merge — a "
              "single .md rewrite) runs on a SEPARATE worker thread in parallel with the serial "
              "GPU queue, so it never waits behind a transcription; the scheduler defers it ONLY "
              "when a serial task on the SAME file is active (both would rewrite that .md). BUILT "
              "and unit-tested at the edges: Task::lane()==Instant (tst_task_speaker_edits), the "
              "instant tasks execute+complete end-to-end through the controller "
              "(tst_transcription_queue's renameSpeaker/mergeSpeakers cases), and "
              "serialActiveConflictsWith() is the pure conflict predicate. What has NO clean "
              "deterministic unit-test path is the cross-THREAD timing itself — proving 'the "
              "rename finished while the transcription was still mid-run' needs a serial task "
              "held reliably active, which requires a real model/long download; verified by "
              "inspection + live use instead of a flaky wall-clock race.");
    }

    // everyFileOpIsATask: BUILT. CLAUDE.md 'Every file/vault operation is a
    // Task' is now fully realized — file rename/move (RenameFileTask/MoveTask),
    // trash/permanent-delete/restore (TrashTask/DeleteTask/RestoreTask over the
    // VaultFileScope variants), model import/export (ModelIoTask), and FOLDER
    // rename/move (FolderRenameTask/FolderMoveTask) are all instant/serial-lane
    // tasks, undoable via the generic snackbar where reversible (undo = the
    // inverse task; trash's is a RestoreTask over the captured restorable set).
    // The pure filesystem work lives in FileRename.h/VaultFileOps.h so it runs
    // off the GUI thread; trash/delete/restore keep the per-file delete/restore
    // signal emission in the model (applyRemovalOutcome/applyRestoreOutcome,
    // called by the controller on completion) so MainPanel's open-file
    // view-state handlers are unchanged. Coverage: tst_task_renamefile,
    // tst_task_move, tst_task_trash, tst_task_folder, tst_transcription_queue
    // (batch undo), plus tst_styleguide's controller-routing guards. The
    // synchronous VaultTreeModel methods survive only as no-controller fallbacks.

    void perAppAudioCapture_singleApplicationIsolation()
    {
        QSKIP("Queue item 036: app-stream SELECTION into the one mixed WAV is now built — "
              "AudioRecorderIO::parseSystemSources() (see tst_audiorecorder) enumerates "
              "Stream/Output/Audio nodes via `pw-dump`, AudioRecorder::availableSystemSources() "
              "surfaces them to the RecorderPage.qml dropdown, and AudioRecorderWorker::"
              "startSystemAudio() taps the chosen one non-invasively via `pw-record --target` "
              "instead of the whole default-sink monitor. What remains owed (and is NOT wanted per "
              "the user's 2026-07-20 decision) is SIMULTANEOUS multi-track capture — recording "
              "several sources at once into separate output tracks rather than one mixed WAV. "
              "Capture-follows-selection itself has no offline unit path (needs a real PipeWire "
              "server/pw-record) — see item 036's manual verification steps.");
    }

    void emptyCapture_isDiscardedNotWrittenAsZeroLengthWav()
    {
        QSKIP("Queue item 033: AudioRecorderWorker::finalizeAndStop() discards a capture that "
              "wrote zero audio bytes (m_bytesWritten == 0) — removing the 44-byte header-only "
              "file and emitting recordingFinished(\"\") instead of dropping a zero-length "
              "suopAudio_*.wav into the vault. That branch lives inside the private worker class "
              "and needs a REAL capture device to reach (open a source, stop before the first mix "
              "tick), so it has no offline unit path — verified by the manual check in the item. "
              "The GUI-side synchronous state machine IS covered by "
              "tst_audiorecorder::startStopRecording_stateFlipsSynchronouslyOnTheCall.");
    }
};

QTEST_GUILESS_MAIN(TstFutureSpec)
#include "tst_future_spec.moc"
