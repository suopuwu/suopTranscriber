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

    void modelManagement_uiAndRegistry()
    {
        QSKIP("DESIGN.md 'Model management': a whisper-model download/management UI backed by a "
              "registry in vault config. STILL PARTIAL: whisper tiers auto-download to "
              "<vault>/.transcriber/models/ and a fixed dropdown exists (tst_modelmanager); "
              "out-of-process ENGINES now DO have a real registry + capability detection "
              "(EngineRegistry, availableEngines(), see subprocessEngines_* note above), but a "
              "general whisper-model download/management UI is still not built.");
    }

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

    void perAppAudioCapture_singleApplicationIsolation()
    {
        QSKIP("Recording feature: capturing ONE specific application's audio individually (e.g. "
              "just Firefox, isolated from the rest of the system mix) is designed-but-not-built. "
              "AudioRecorder (src/core/AudioRecorder.h/.cpp) only supports mic + the FULL "
              "default-sink monitor mix via `parec`. Per-app isolation would need per-app stream "
              "enumeration/routing (libpulse's sink-input list, or PipeWire's node graph via "
              "libpipewire) to select just one application's stream instead of the whole monitor "
              "— deliberately deferred out of scope for this change.");
    }
};

QTEST_GUILESS_MAIN(TstFutureSpec)
#include "tst_future_spec.moc"
