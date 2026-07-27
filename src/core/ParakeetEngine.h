#pragma once

#include <QString>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "TranscriptionEngine.h"

namespace sherpa_onnx::cxx {
class OfflineRecognizer;
}

// In-process ASR engine for NVIDIA Parakeet-TDT, run via sherpa-onnx's
// OfflineRecognizer (the same sherpa-onnx library the app already links for
// DiarizationEngine — NO Python/NeMo). Loads an ONNX transducer bundle
// (encoder/decoder/joiner + tokens) and decodes 16 kHz mono float PCM
// directly (sherpa-onnx does mel-feature extraction internally), returning
// sentence-level Segments assembled from the model's per-token timestamps.
//
// Same TranscriptionEngine contract as WhisperCppEngine — warm model resident
// across transcribe() calls, chunk-relative timestamps (the app owns the
// offset merge), graceful degradation on load/decode failure (returns no
// segments, never throws — same spirit as DiarizationEngine). Speaker
// diarization is a separate pass (DiarizationEngine); this engine has no
// speaker concept of its own.
//
// Unlike whisper.cpp, sherpa-onnx's offline recognizer has no mid-decode
// progress/abort hook (Decode() is one blocking call), so progress is reported
// at the window boundaries and a cancel takes effect between windows — the
// app's VAD windowing keeps each Decode() bounded, same situation as
// DiarizationEngine.
class ParakeetEngine : public TranscriptionEngine {
public:
    // Paths to the four bundle files. modelType/modelingUnit match sherpa-onnx's
    // config for a NeMo transducer; provider is "cuda" or "cpu" (the ctor tries
    // CUDA first then falls back, like DiarizationEngine).
    ParakeetEngine(const QString &encoderPath, const QString &decoderPath, const QString &joinerPath,
                   const QString &tokensPath);
    ~ParakeetEngine() override;

    bool isValid() const override;

    std::vector<Segment> transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress,
                                     const SegmentCallback &onSegment,
                                     const AbortCallback &onAbort = {}) override;

    QString activeBackendName() const override { return m_backendName; }

    int takeInferenceFailureCount() override
    {
        const int count = m_decodeFailures;
        m_decodeFailures = 0;
        return count;
    }

    // Turns sherpa-onnx's per-token output into sentence-level Segments:
    // reassembles words from BPE subword tokens (the "▁" word-boundary marker),
    // then splits into sentences on .!? (Parakeet v2 emits punctuation +
    // casing). Each Segment's t0 is its first word's onset; t1 is the next
    // segment's t0. Exposed static + sherpa-free so it's unit-testable
    // (tst_parakeetengine) without a loaded model. fullText is only used as a
    // fallback when no per-token timing is present.
    static std::vector<Segment> assembleSentenceSegments(const std::vector<std::string> &tokens,
                                                          const std::vector<float> &timestamps,
                                                          const std::string &fullText);

    // Splits [0, pcm.size()) into contiguous, gapless [start, end) sample
    // ranges, each no longer than maxSamples (plus at most searchSamples of
    // pause-seeking slack). Parakeet's exported attention overruns past ~400 s
    // and sherpa-onnx decodes a whole window in one pass, so transcribe() feeds
    // each range separately instead of the whole chunk. Windows are near-equal
    // in length, and interior boundaries are nudged onto the quietest nearby
    // 20 ms frame so a split lands in a pause rather than mid-word. A chunk
    // already within maxSamples yields a single whole-chunk range. Exposed
    // static + sherpa-free so it's unit-testable (tst_parakeetengine) on a
    // synthetic buffer without a loaded model.
    static std::vector<std::pair<std::size_t, std::size_t>>
    subChunkRanges(const std::vector<float> &pcm, std::size_t maxSamples, std::size_t searchSamples);

private:
    std::unique_ptr<sherpa_onnx::cxx::OfflineRecognizer> m_recognizer;
    QString m_backendName;
    // Sub-windows whose Decode() threw during the CURRENT transcribe() call
    // (reset at its top) — see takeInferenceFailureCount().
    int m_decodeFailures = 0;
};
