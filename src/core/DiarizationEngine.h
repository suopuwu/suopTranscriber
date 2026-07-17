#pragma once

#include <QString>

#include <functional>
#include <vector>

#include "sherpa-onnx/c-api/cxx-api.h"

// fraction is 0.0-1.0, computed from sherpa-onnx's own
// (num_processed_chunks, num_total_chunks) callback — real progress through
// the diarization pass, not a guess (see DiarizationEngine::diarize).
using DiarizationProgressCallback = std::function<void(double fraction)>;

// One diarized speaker turn: [start, end) in seconds. speaker is a stable,
// 0-based cluster id renumbered into first-appearance order (sherpa-onnx's
// own raw cluster ids are arbitrary/unordered) — the caller never needs to
// know anything about sherpa-onnx's internal id scheme.
struct DiarizedTurn {
    double start = 0.0;
    double end = 0.0;
    int speaker = 0;
};

// Real speaker diarization (pyannote segmentation + WeSpeaker ResNet293-LM
// embedding, see ModelManager::kDiarizationEmbeddingFileName's doc comment +
// agglomerative clustering), via sherpa-onnx's OfflineSpeakerDiarization C++
// API (see DESIGN.md "Speaker diarization") — replaces tinydiarize's
// turn-only heuristic entirely. Runs as a separate pass over the SAME PCM
// buffer whisper.cpp transcribes (see TranscriptionController), independent
// of which whisper model is selected — unlike tdrz, it never constrains
// model choice. One instance keeps both ONNX models resident (warm), same
// pattern as WhisperCppEngine/LabelingEngine. Speaker count is always
// auto-detected via a clustering distance threshold, never a fixed count
// (see DESIGN.md decision — no "number of speakers" UI).
class DiarizationEngine {
public:
    // segmentationModelPath/embeddingModelPath: see
    // ModelManager::ensureDiarizationModels(). Tries CUDA first (ONNX
    // Runtime has no Vulkan EP, unlike whisper.cpp's cross-vendor
    // baseline — GPU here means NVIDIA-only, and needs the CUDA toolkit/
    // cuDNN installed, not just the driver), falling back to CPU
    // automatically if CUDA isn't usable on this machine (see the .cpp's
    // createSafely()) — never a hard requirement.
    //
    // threshold is the clustering distance cutoff (the user-facing
    // "sensitivity", see AppSettings.diarizationSensitivity) — LOWER = stricter
    // = MORE speakers, HIGHER = more merging = FEWER speakers. Speaker COUNT is
    // still auto-detected (num_clusters stays 0); this only tunes that
    // auto-detection, it is NOT a fixed speaker count. 0.5 is sherpa-onnx's own
    // documented default (cosine distance, complete linkage).
    DiarizationEngine(const QString &segmentationModelPath, const QString &embeddingModelPath, float threshold = 0.5f);

    bool isValid() const { return m_sd.Get() != nullptr; }

    // pcm16kMono: 16kHz mono float PCM, same format as AudioChunk (see
    // TranscriptionEngine.h). Returns turns sorted by start time. Empty on
    // failure or if no speech was detected — never crashes on unusual input
    // (silence, single speaker, many speakers). onProgress (optional) is
    // called periodically as sherpa-onnx works through the file's sliding
    // windows — this pass can take real time on longer files, and unlike
    // whisper.cpp's transcription this had no progress reporting at all
    // until now (see TranscriptionController's "loading/detecting/
    // transcribing" phase reporting).
    std::vector<DiarizedTurn> diarize(const std::vector<float> &pcm16kMono,
                                       const DiarizationProgressCallback &onProgress = {}) const;

private:
    sherpa_onnx::cxx::OfflineSpeakerDiarization m_sd;
};

// Exposed (out of DiarizationEngine.cpp's anonymous namespace) purely so
// tst_diarizationengine.cpp can exercise the singleton-cluster merge policy
// directly with hand-built segments — real sherpa-onnx clustering output
// isn't controllable enough from a test to hit specific merge scenarios
// (see mergeMinorSpeakersIntoNearestMajor's doc comment in the .cpp for why
// singleton turn count, not duration share, is the merge criterion).
void mergeMinorSpeakersIntoNearestMajor(std::vector<sherpa_onnx::cxx::OfflineSpeakerDiarizationSegment> &segments);
