#pragma once

#include <QString>
#include <QUrl>

#include <functional>
#include <vector>

// Ensures a whisper.cpp ggml model is present at <vault>/.transcriber/models/,
// downloading it if missing (see DESIGN.md "Model management" / vault layout).
//
// If modelId is not one of availableModels()'s ids (e.g. a stale
// AppSettings.lastModel from a removed model tier), ensureModel silently
// *would* have resolved to kDefaultModelId with no way for the caller to
// know a substitution happened — the caller would believe the requested
// model was used. To avoid ever misrepresenting which model actually ran,
// pass usedFallback: it is set to true exactly when this default-substitution
// occurs. Callers that persist the result (see TranscriptionController's
// modelFallback/modelRequested frontmatter fields) should surface this to the
// user rather than only logging it.
// Blocking — call off the GUI thread.
namespace ModelManager {

struct ModelInfo {
    QString id;       // stable identifier, also the AppSettings.lastModel value
    QString fileName; // ggml file name on huggingface.co/ggerganov/whisper.cpp
    QString label;    // human-readable, shown in the model dropdown
};

// Ordered fastest/smallest to slowest/most-accurate — dropdown display order.
const std::vector<ModelInfo> &availableModels();

constexpr auto kDefaultModelId = "base.en";

// In-process NVIDIA Parakeet-TDT, run via sherpa-onnx's OfflineRecognizer (see
// ParakeetEngine) — NOT whisper ggml and NOT an out-of-process subprocess
// engine. It appears in availableModels() like the whisper tiers, but its
// "model" is a multi-file ONNX bundle (encoder/decoder/joiner + tokens)
// downloaded/extracted from k2-fsa's release, so modelFileName()/
// isModelDownloaded()/ensureModel() special-case this id. ensureModel()
// returns the bundle DIRECTORY path for it; parakeetModelPaths() resolves the
// four files inside.
constexpr auto kParakeetModelId = "parakeet-tdt-0.6b-v2";

bool isParakeetModelId(const QString &modelId);

struct ParakeetModelPaths {
    QString encoder;
    QString decoder;
    QString joiner;
    QString tokens;
    bool isComplete() const;
};

// Absolute paths to the Parakeet bundle's files for this vault (whether or not
// they exist yet). isComplete() is true iff all four are present on disk.
ParakeetModelPaths parakeetModelPaths(const QUrl &vaultPath);

// Wave N (task system, item 5): resolves modelId to its ggml file name
// without touching disk/network — lets a caller check "is this already
// downloaded" (see isModelDownloaded()) before deciding whether a separate
// download job is needed. Empty if modelId isn't recognized (mirrors
// ensureModel()'s fallback logic being the CALLER's job to apply, not this
// function's — this one never substitutes kDefaultModelId).
QString modelFileName(const QString &modelId);

// True if modelFileName(modelId)'s file already exists at
// <vault>/.transcriber/models/ — never downloads. False (not a fallback
// check) for an unrecognized modelId, same as modelFileName()'s empty
// result.
bool isModelDownloaded(const QUrl &vaultPath, const QString &modelId);
// Mirrors isModelDownloaded() for the AI-auto-labeling model (kLabelModelFileName).
bool isLabelModelDownloaded(const QUrl &vaultPath);
// Mirrors isModelDownloaded() for BOTH speaker-diarization models — true
// only if both are present, matching ensureDiarizationModels()'s
// never-partially-succeeds contract.
bool areDiarizationModelsDownloaded(const QUrl &vaultPath);

// Progress callback for the download functions below: bytesDone/bytesTotal
// from polling the growing .part file's size against a Content-Length
// probed once up front — bytesTotal is 0 if the server didn't report a
// length (progress is then unknown, caller should show indeterminate).
using DownloadProgressCallback = std::function<void(qint64 bytesDone, qint64 bytesTotal)>;
// Cooperative pause/cancel for an in-progress download (Wave N task
// system) — paused()/cancelled() are polled periodically (see
// downloadToModelsDir()'s doc comment in the .cpp) rather than delivered as
// an interrupt. Default-constructed (both null) means "never pause or
// cancel", same optional-callback convention as TranscriptionEngine's
// AbortCallback.
struct DownloadControl {
    std::function<bool()> paused;
    std::function<bool()> cancelled;
};

// Returns the local model path, or empty on failure (errorOut set if given).
// usedFallback, if given, is set to true/false depending on whether an
// unrecognized modelId forced a default substitution (see class comment).
// onProgress/control (Wave N): optional real-time byte progress + cooperative
// pause/cancel for the download itself — a no-op to pass when the model is
// already downloaded (the common case once item 5's separate download job
// has already run), since this function returns immediately without
// touching either callback when the file already exists.
QString ensureModel(const QUrl &vaultPath, const QString &modelId, QString *errorOut = nullptr,
                     bool *usedFallback = nullptr, const DownloadProgressCallback &onProgress = {},
                     const DownloadControl &control = {});

// The GGUF model used for AI auto-labeling (see DESIGN.md "Model management" —
// LabelingEngine's second pass after transcription). One hardcoded model for
// now, same vault-scoped storage/download pattern as ensureModel() above, no
// per-size picker — auto-labeling is a single opt-in setting, not a dropdown.
constexpr auto kLabelModelFileName = "Qwen3-4B-Instruct-2507-Q4_K_M.gguf";

// Returns the local label-model path, or empty on failure (errorOut set if
// given). Downloads on first use (~2.5 GB); Q_INVOKABLE callers must run this
// off the GUI thread, same as ensureModel(). onProgress/control: see
// ensureModel()'s doc comment.
QString ensureLabelModel(const QUrl &vaultPath, QString *errorOut = nullptr,
                          const DownloadProgressCallback &onProgress = {}, const DownloadControl &control = {});

// Silero VAD (ggml-converted), used to (a) gate whisper.cpp's own per-window
// inference away from silence (hallucination/silence robustness) and (b)
// find VAD-aligned window boundaries for chunking a long file (see DESIGN.md
// "Durable / resumable transcription" — chunking half only, no
// checkpoint/resume yet). ~2 MB, same vault-scoped storage/download pattern
// as the models above. Unlike ensureModel(), a failure here must NOT block
// transcription — caller degrades gracefully (ASR without VAD gating/
// windowing, single whole-file chunk, recorded honestly in frontmatter),
// same contract as ensureDiarizationModels().
constexpr auto kVadModelFileName = "ggml-silero-v5.1.2.bin";

// Whether an out-of-process engine's model is present for this vault (see
// DESIGN.md "Pluggable engines" capability detection). True when the engine
// ships no app-managed model file — i.e. its runtime fetches its own weights
// (Parakeet via NeMo's from_pretrained/HF cache), so a missing file never
// gates it here; its availability is instead gated by its dependencies. Never
// downloads.
bool isEngineModelPresent(const QUrl &vaultPath, const QString &engineId);

// Returns true if kVadModelFileName already exists — never downloads.
bool isVadModelDownloaded(const QUrl &vaultPath);

// Returns the local VAD model path, or empty on failure (errorOut set if
// given). onProgress/control: see ensureModel()'s doc comment.
QString ensureVadModel(const QUrl &vaultPath, QString *errorOut = nullptr,
                        const DownloadProgressCallback &onProgress = {}, const DownloadControl &control = {});

// Speaker diarization (see DESIGN.md "Speaker diarization" / DiarizationEngine):
// pyannote-segmentation-3.0, int8-quantized (local speaker-activity
// segmentation) + WeSpeaker ResNet293-LM (speaker embedding), both ONNX,
// ~116 MB combined — same vault-scoped storage/download pattern as the
// models above. Unlike ensureModel(), a failure here is NOT meant to block
// transcription: the caller is expected to degrade gracefully (run ASR
// without diarization, record why in frontmatter) rather than fail the
// whole job — see TranscriptionController.
//
// Embedding model choice (2026-07-07): swapped from 3D-Speaker's CAM++
// (en_voxceleb, ~30 MB) to WeSpeaker's ResNet293-LM (en_voxceleb, ~115 MB) —
// the largest English/VoxCeleb speaker-embedding checkpoint sherpa-onnx's
// speaker-recongition-models release ships (full list: `gh`/GitHub API
// against that release tag). ResNet293-LM is WeSpeaker's own best-performing
// released checkpoint (0.447% EER on VoxCeleb1-O after large-margin
// finetuning) — the intent is a more discriminative embedding space to
// reduce the over-splitting seen on real recordings (one speaker's voice
// fragmenting into multiple clusters, see DiarizationEngine's
// kMinorSpeakerDurationFraction doc comment). Trade-off: ~4x the download
// size and a slower per-segment forward pass than CAM++ — acceptable here
// since the whole diarization pass already runs on CPU specifically because
// it's dominated by hundreds of small sequential calls rather than one long
// batched one (see DiarizationEngine's kTryCuda doc comment), so a somewhat
// heavier per-call model doesn't change that shape, just its constant
// factor. Filename changed (not just the URL) so a vault with the OLD cached
// file re-downloads the new one automatically rather than silently keeping
// stale weights.
//
// Segmentation model choice (2026-07-07, same day): switched to the int8-
// quantized export inside the SAME sherpa-onnx pyannote-segmentation-3.0
// release archive (still MIT-licensed, still the exact same trained
// weights, just quantized — not a different model) — ~4x smaller (1.5 MB vs
// 6 MB) and faster on CPU, which is what matters since segmentation (not
// embedding) is the per-call-count-dominated half of the pipeline (see
// kTryCuda's doc comment in DiarizationEngine.cpp). sherpa-onnx also offers
// genuinely different segmentation models (Rev.ai's "reverb-diarization"
// v1/v2) in that same release, but those are non-commercial-licensed and
// unevidenced to help the specific over-splitting problem here, so
// deliberately not used.
constexpr auto kDiarizationSegmentationFileName = "pyannote-segmentation-3.0-int8.onnx";
constexpr auto kDiarizationEmbeddingFileName = "wespeaker-resnet293-lm.onnx";

// Returns true and fills both *_PathOut params if both files are present or
// were downloaded successfully; false (errorOut set) if either fails. Never
// partially succeeds without reporting it — either both paths are valid or
// neither is (callers shouldn't have to handle a "half available" state).
// onProgress/control: see ensureModel()'s doc comment — spans BOTH files
// when both need downloading (bytesDone/bytesTotal cover whichever one is
// currently transferring, resetting between them; a caller wanting one
// smooth 0..1 across both must track that itself, same as any multi-file
// download).
bool ensureDiarizationModels(const QUrl &vaultPath, QString *segmentationPathOut, QString *embeddingPathOut,
                              QString *errorOut = nullptr, const DownloadProgressCallback &onProgress = {},
                              const DownloadControl &control = {});

}
