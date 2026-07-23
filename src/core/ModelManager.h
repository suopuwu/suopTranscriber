#pragma once

#include <QString>

#include <functional>
#include <vector>

// Ensures a whisper.cpp ggml model is present in the shared, app-global model
// store, downloading it if missing (see DESIGN.md "Model management").
//
// Storage (2026-07-16): models live in ONE app-global store shared across all
// vaults — NOT per-vault — so a multi-GB model is downloaded once and reused
// everywhere, and there is a single directory to back up / import / export.
// The store is the writable data dir (see modelsStoreDir()); a read-only
// "bundled" directory shipped next to the binary (see below) is also searched
// so a distributor can pre-place models. Resolution for every presence check
// and path lookup is store-first, then bundled; downloads/imports/deletes only
// ever touch the writable store.
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

// The app-global, writable model store — all downloads/imports land here and
// deletes remove from here. Default: the writable generic-data location
// (~/.local/share on Linux) + "/suopTranscriber/models". Overridden by the
// SUOP_MODELS_DIR env var (used by tests to redirect to a temp dir so they
// never touch the real store — see CLAUDE.md's "never touch the user's real
// vault or app config"). The directory is created lazily on first write.
QString modelsStoreDir();

// The read-only "bundled" model directory searched AFTER the writable store
// for every presence check / path lookup, so a distributor can ship model
// files beside the binary and the app finds them without a download. Default:
// applicationDirPath() + "/models"; overridden by SUOP_BUNDLED_MODELS_DIR (for
// tests exercising the bundled-fallback path). May not exist — a missing
// bundled dir simply contributes no models.
QString bundledModelsDir();

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

// Absolute paths to the Parakeet bundle's four files, resolved store-first
// then bundled: if a complete bundle exists in either location its paths are
// returned; otherwise the writable-store paths (the download target) are
// returned even though they don't exist yet. isComplete() is true iff all four
// of the returned paths are present on disk.
ParakeetModelPaths parakeetModelPaths();

// Task system: resolves modelId to its ggml file name
// without touching disk/network — lets a caller check "is this already
// downloaded" (see isModelDownloaded()) before deciding whether a separate
// download job is needed. Empty if modelId isn't recognized (mirrors
// ensureModel()'s fallback logic being the CALLER's job to apply, not this
// function's — this one never substitutes kDefaultModelId).
QString modelFileName(const QString &modelId);

// True if modelFileName(modelId)'s file is present in the store or bundled
// dir — never downloads. False (not a fallback check) for an unrecognized
// modelId, same as modelFileName()'s empty result.
bool isModelDownloaded(const QString &modelId);
// Mirrors isModelDownloaded() for the AI-auto-labeling model (kLabelModelFileName).
bool isLabelModelDownloaded();
// Mirrors isModelDownloaded() for BOTH speaker-diarization models — true
// only if both are present, matching ensureDiarizationModels()'s
// never-partially-succeeds contract.
bool areDiarizationModelsDownloaded();

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
QString ensureModel(const QString &modelId, QString *errorOut = nullptr,
                     bool *usedFallback = nullptr, const DownloadProgressCallback &onProgress = {},
                     const DownloadControl &control = {});

// The GGUF model used for AI auto-labeling (see DESIGN.md "Model management" —
// LabelingEngine's second pass after transcription). One hardcoded model for
// now, same app-global storage/download pattern as ensureModel() above, no
// per-size picker — auto-labeling is a single opt-in setting, not a dropdown.
constexpr auto kLabelModelFileName = "Qwen3-4B-Instruct-2507-Q4_K_M.gguf";

// Returns the local label-model path, or empty on failure (errorOut set if
// given). Downloads on first use (~2.5 GB); Q_INVOKABLE callers must run this
// off the GUI thread, same as ensureModel(). onProgress/control: see
// ensureModel()'s doc comment.
QString ensureLabelModel(QString *errorOut = nullptr,
                          const DownloadProgressCallback &onProgress = {}, const DownloadControl &control = {});

// Silero VAD (ggml-converted), used to (a) gate whisper.cpp's own per-window
// inference away from silence (hallucination/silence robustness) and (b)
// find VAD-aligned window boundaries for chunking a long file (see DESIGN.md
// "Durable / resumable transcription" — chunking half only, no
// checkpoint/resume yet). ~2 MB, same app-global storage/download pattern
// as the models above. Unlike ensureModel(), a failure here must NOT block
// transcription — caller degrades gracefully (ASR without VAD gating/
// windowing, single whole-file chunk, recorded honestly in frontmatter),
// same contract as ensureDiarizationModels().
constexpr auto kVadModelFileName = "ggml-silero-v6.2.0.bin";

// Whether an out-of-process engine's model is present (app-global) (see
// DESIGN.md "Pluggable engines" capability detection). True when the engine
// ships no app-managed model file — i.e. its runtime fetches its own weights
// (Parakeet via NeMo's from_pretrained/HF cache), so a missing file never
// gates it here; its availability is instead gated by its dependencies. Never
// downloads.
bool isEngineModelPresent(const QString &engineId);

// Returns true if kVadModelFileName already exists — never downloads.
bool isVadModelDownloaded();

// Returns the local VAD model path, or empty on failure (errorOut set if
// given). onProgress/control: see ensureModel()'s doc comment.
QString ensureVadModel(QString *errorOut = nullptr,
                        const DownloadProgressCallback &onProgress = {}, const DownloadControl &control = {});

// Speaker diarization (see DESIGN.md "Speaker diarization" / DiarizationEngine):
// a SEGMENTATION model (local speaker-activity segmentation) + an EMBEDDING
// model (speaker voice fingerprint), both ONNX, run by sherpa-onnx. Unlike
// ensureModel(), a failure here is NOT meant to block transcription: the
// caller degrades gracefully (run ASR without diarization, record why in
// frontmatter) rather than fail the whole job — see TranscriptionController.
//
// Both halves are USER-SELECTABLE from small catalogs (the two
// availableSegmentationModels()/availableEmbeddingModels() below) — same
// {id,fileName,label} dropdown shape as availableModels(), but each entry also
// carries its own download recipe (ModelInfo has no URL). The defaults
// (kDefaultSegmentationModelId / kDefaultEmbeddingModelId) reproduce the prior
// fixed pair: pyannote-segmentation-3.0 int8 (MIT) + WeSpeaker ResNet293-LM
// (~115 MB, the most discriminative English/VoxCeleb embedding sherpa-onnx
// ships, 0.447% EER on VoxCeleb1-O). Alternatives exist for A/B-ing hard
// recordings — notably far-field group audio where the default clusters
// everyone into one speaker: Rev.ai's reverb-diarization-v1 segmentation
// (trained on messy conversational audio — NON-COMMERCIAL licensed, surfaced
// as such in the Models page), plus 3D-Speaker CAM++ (~28 MB, fast) and NVIDIA
// NeMo TitaNet-large embeddings. The clustering SENSITIVITY (distance
// threshold) is a separate DiarizationEngine ctor argument, not a model choice.
struct DiarizationModelInfo {
    QString id;                 // stable id: AppSettings value AND manageable-model id
    QString fileName;           // canonical local name in the store
    QString label;              // human-readable, shown in the picker
    QString url;                // full download URL (a direct .onnx, or a .tar.bz2 archive)
    QString archiveInnerPath;   // non-empty ⇒ url is a .tar.bz2; extract this inner path. Empty ⇒ direct file.
    bool nonCommercial = false; // license note surfaced in the Models page download UI
};

// Ordered default-first — dropdown display order. Every entry is downloadable
// and appears as its own manageable model (see TranscriptionController::
// manageableModels()).
const std::vector<DiarizationModelInfo> &availableSegmentationModels();
const std::vector<DiarizationModelInfo> &availableEmbeddingModels();

constexpr auto kDefaultSegmentationModelId = "pyannote-segmentation-3.0";
constexpr auto kDefaultEmbeddingModelId = "wespeaker-resnet293-lm";

// Local store filenames of the DEFAULT pair — kept as named constants because
// modelLayout()/a few call sites still special-case them. Equal to the default
// catalog entries' fileName.
constexpr auto kDiarizationSegmentationFileName = "pyannote-segmentation-3.0-int8.onnx";
constexpr auto kDiarizationEmbeddingFileName = "wespeaker-resnet293-lm.onnx";

// Resolve a chosen sub-model's DiarizationModelInfo by id; *usedFallback
// (optional) is set true iff id was unrecognized and the default was
// substituted (never silently misrepresent the model — same contract as
// ensureModel()). The returned reference is to a static catalog entry.
const DiarizationModelInfo &segmentationModelInfo(const QString &id, bool *usedFallback = nullptr);
const DiarizationModelInfo &embeddingModelInfo(const QString &id, bool *usedFallback = nullptr);

// True iff the chosen sub-model's file is present (store or bundled). An
// unrecognized id checks the default (matching *ModelInfo()'s fallback).
bool isSegmentationModelDownloaded(const QString &id);
bool isEmbeddingModelDownloaded(const QString &id);

// Ensure a single chosen sub-model is on disk (download if missing), returning
// its local path or empty on failure (errorOut set). usedFallback: as
// segmentationModelInfo(). onProgress/control: see ensureModel()'s doc comment.
QString ensureSegmentationModel(const QString &id, QString *errorOut = nullptr, bool *usedFallback = nullptr,
                                const DownloadProgressCallback &onProgress = {}, const DownloadControl &control = {});
QString ensureEmbeddingModel(const QString &id, QString *errorOut = nullptr, bool *usedFallback = nullptr,
                             const DownloadProgressCallback &onProgress = {}, const DownloadControl &control = {});

// True iff BOTH chosen models are present; unrecognized ids fall back to
// defaults. The no-arg overload checks the default pair (used where no
// selection is in scope).
bool areDiarizationModelsDownloaded(const QString &segmentationModelId, const QString &embeddingModelId);
bool areDiarizationModelsDownloaded();

// Ensure both chosen models are present, filling *_PathOut. false (errorOut
// set) if either fails — never partially succeeds without reporting it.
// *usedFallback set true iff EITHER id was unrecognized (default substituted).
// onProgress/control: see ensureModel()'s doc comment — spans both files when
// both need downloading (progress resets between them).
bool ensureDiarizationModels(const QString &segmentationModelId, const QString &embeddingModelId,
                             QString *segmentationPathOut, QString *embeddingPathOut, QString *errorOut = nullptr,
                             bool *usedFallback = nullptr, const DownloadProgressCallback &onProgress = {},
                             const DownloadControl &control = {});

// --- Model management (Settings → Models) --------------------------------
//
// The "support" models above (label / VAD / diarization) have no dropdown
// entry and thus no ModelInfo id. These stable pseudo-ids let the Models
// management page list/delete/import/export/predownload them through the same
// id-keyed API as the whisper tiers + parakeet. The ops below accept any
// whisper tier id, kParakeetModelId, OR one of these three.
constexpr auto kLabelModelId = "label";
constexpr auto kVadModelId = "vad";
constexpr auto kDiarizationModelId = "diarization";

// Total on-disk size (bytes) of the file(s) that model id occupies, resolved
// store-first then bundled; 0 if absent or the id is unknown.
qint64 modelSizeOnDisk(const QString &id);

// True iff id's file(s) resolve ONLY in the read-only bundled dir (nothing in
// the writable store) — the page must disable Delete for these (a shipped
// model can't be removed). False when not downloaded at all, or present in the
// writable store.
bool isModelBundledOnly(const QString &id);

// Removes id's file(s) from the WRITABLE store only (never the bundled dir).
// Permanent (models are large + re-downloadable — no trash). Returns false
// (errorOut set) for an unknown id or when nothing was in the store to remove.
bool deleteModel(const QString &id, QString *errorOut = nullptr);

// Copies user-supplied model file(s) into the store under their canonical
// name(s), so id then resolves as downloaded with no network fetch. sourcePath
// is a FILE for single-file models (a whisper tier, label, or vad) or a
// DIRECTORY containing the expected canonical files for multi-file models (the
// parakeet bundle's four files; the diarization pair). Blocking (a multi-GB
// copy) — call off the GUI thread. Returns false (errorOut set) on a bad id,
// wrong source shape, or a missing expected file.
bool importModel(const QString &id, const QString &sourcePath, QString *errorOut = nullptr);

// Copies id's downloaded file(s) into destDir for backup / re-import: a single
// file for single-file models, a <bundle>/ subdir for parakeet, both onnx
// files for diarization — the exact shape importModel() expects back.
// Blocking — call off the GUI thread. Returns false (errorOut set) if id isn't
// downloaded, destDir is missing, or a copy fails.
bool exportModel(const QString &id, const QString &destDir, QString *errorOut = nullptr);

}
