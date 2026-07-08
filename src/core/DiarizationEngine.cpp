#include "DiarizationEngine.h"

#include <QDebug>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <thread>

namespace sc = sherpa_onnx::cxx;

namespace {

// sherpa-onnx's own default is num_threads=1 for BOTH the segmentation and
// embedding sessions — single-threaded ONNX Runtime CPU inference. Fine for
// a short clip, but the sliding-window segmentation pass alone runs the
// model once per ~1s hop (90% overlap on a 10s window, baked into the
// pyannote model itself — not something this app configures), so a longer
// recording means hundreds of sequential single-threaded forward passes.
// Capped at 4 rather than hardware_concurrency() outright: ONNX Runtime's
// intra-op parallelism has diminishing (sometimes negative, from
// thread-pool overhead) returns past a handful of threads for models this
// small, and diarization already runs strictly before whisper.cpp starts
// (see TranscriptionController), so there's no contention to share cores
// with at this point either way.
int cpuThreadCount()
{
    return std::min(4u, std::max(1u, std::thread::hardware_concurrency()));
}

// OfflineSpeakerDiarization has no default constructor (only a move ctor and
// a private ctor-from-C-handle), so m_sd must be built via the member
// initializer list rather than default-constructed-then-assigned — this
// helper builds the config that Create() needs, ahead of that call.
// provider: "cuda" or "cpu" (see createSafely()'s try-CUDA-then-CPU logic).
sc::OfflineSpeakerDiarizationConfig buildConfig(const QString &segmentationModelPath, const QString &embeddingModelPath,
                                                 const char *provider)
{
    sc::OfflineSpeakerDiarizationConfig config;
    config.segmentation.pyannote.model = segmentationModelPath.toStdString();
    config.embedding.model = embeddingModelPath.toStdString();
    // Speaker count is always auto-detected (see class doc) — num_clusters
    // stays 0 (its default), so the threshold below is what actually governs
    // clustering. 0.5 is sherpa-onnx's own documented default for this exact
    // clustering algorithm (cosine distance, complete linkage).
    config.clustering.num_clusters = 0;
    config.clustering.threshold = 0.5f;
    config.segmentation.provider = provider;
    config.embedding.provider = provider;
    // Relevant for the "cpu" attempt (and any CPU-side ops even under
    // "cuda") — see cpuThreadCount()'s own doc comment.
    config.segmentation.num_threads = cpuThreadCount();
    config.embedding.num_threads = cpuThreadCount();
    return config;
}

// Measured on real audio (2026-07-07, against the CAM++ embedding model —
// see NOTE below): CUDA is reliably SLOWER than CPU for this specific
// pipeline, not just occasionally unavailable — 2.35s vs 0.24s on an 11s
// clip, 45.6s vs 5.8s on a 3:40 clip (scales with length, so it's per-call
// overhead, not one-time warmup). Root cause: the segmentation model runs as
// ~1 forward pass per second of audio (90% overlap on a 10s window, baked
// into the model itself — hundreds of small, sequential calls for a real
// recording), and GPU kernel-launch/transfer overhead dominates for that
// shape of work when each call is this small. This is the OPPOSITE of
// whisper.cpp's GPU win, which is one long streaming pass with large batched
// matmuls — a genuinely different workload shape, not a contradiction.
// kTryCuda stays available (tested, working — see the "cuda" branch below)
// in case a future model/architecture change here benefits from it, but
// CPU is the correct default given what's actually measured today.
//
// NOTE (2026-07-07, same day): the embedding model was swapped right after
// this measurement from CAM++ (~30 MB) to WeSpeaker's ResNet293-LM (~115 MB,
// a much deeper conv net — see ModelManager::kDiarizationEmbeddingFileName's
// doc comment) to reduce over-splitting. The SEGMENTATION half of the
// pipeline (still pyannote, unchanged, and the dominant cost per the root
// cause above) is why CPU should still win, but the embedding half's own
// per-call cost just grew substantially and hasn't been re-measured against
// CUDA with the new model — this conclusion is inherited, not re-verified.
constexpr bool kTryCuda = false;

// sherpa-onnx gracefully returns a null handle (Create()'s result has
// .Get()==nullptr) when a model path doesn't EXIST — confirmed empirically.
// But a file that exists and is simply not valid ONNX (corrupt, truncated,
// wrong format) makes the underlying ONNX Runtime throw Ort::Exception,
// which sherpa-onnx does NOT catch — left uncaught, this crashes the whole
// process (std::terminate), directly defeating the graceful-degradation
// design (see DESIGN.md "Speaker diarization") this class exists for.
//
// When kTryCuda is true: a failed CUDA init (whether it throws, the more
// likely failure mode for a missing/mismatched CUDA runtime, or merely
// returns an invalid handle) falls back to CPU rather than leaving speaker
// detection unavailable entirely. A genuinely bad/missing/corrupt model
// file fails the SAME way under both providers, so this fallback costs
// nothing in that case — it just repeats the same (fast, local) failure
// once more before giving up for real.
sc::OfflineSpeakerDiarization createSafely(const QString &segmentationModelPath, const QString &embeddingModelPath)
{
    if constexpr (kTryCuda) {
        try {
            sc::OfflineSpeakerDiarization sd = sc::OfflineSpeakerDiarization::Create(
                buildConfig(segmentationModelPath, embeddingModelPath, "cuda"));
            if (sd.Get() != nullptr)
                return sd;
            qWarning() << "DiarizationEngine: CUDA init returned no handle — falling back to CPU";
        } catch (const std::exception &e) {
            qWarning() << "DiarizationEngine: CUDA init failed (" << e.what() << ") — falling back to CPU";
        }
    }

    try {
        return sc::OfflineSpeakerDiarization::Create(buildConfig(segmentationModelPath, embeddingModelPath, "cpu"));
    } catch (const std::exception &e) {
        qWarning() << "DiarizationEngine: failed to load models (corrupt or invalid ONNX?):" << e.what();
        sc::OfflineSpeakerDiarizationConfig invalid;
        invalid.segmentation.pyannote.model = "/nonexistent-after-load-failure";
        invalid.embedding.model = "/nonexistent-after-load-failure";
        return sc::OfflineSpeakerDiarization::Create(invalid);
    }
}

// sherpa-onnx's complete-linkage clustering (see buildConfig()'s doc
// comment) fragments badly on long, acoustically noisy real-world
// recordings — confirmed on an actual 4.8-hour user recording: the default
// threshold (0.5) produced 533 distinct raw clusters, of which 367 (69%)
// had exactly ONE turn each. Only a handful of clusters (286, 237, 219,
// 146, 80 turns) plausibly correspond to real recurring speakers; the long
// tail of near-singleton clusters is almost certainly noise/cross-talk
// producing unstable, one-off embeddings that complete linkage — which
// merges two clusters only if EVERY pairwise distance between their
// members is below threshold, so a single noisy outlier blocks an entire
// merge — refuses to fold into a real speaker. Raising the raw threshold
// was tried empirically and does NOT fix this in a scale-invariant way: at
// 30 minutes of the same recording, threshold 0.9 (already well above
// sherpa-onnx's default) still gave 16 clusters, and the count keeps
// growing with recording length regardless of threshold — a single
// distance cutoff can't compensate for an algorithm that gets more
// fragile as more (noisy) data accumulates.
//
// Fix (applied here, not inside sherpa-onnx's clustering, which is
// compiled code we don't control): a real recurring speaker accumulates
// substantial total TALK TIME over a long recording; a noise-driven
// micro-cluster does not. So any cluster whose total duration is under
// kMinorSpeakerDurationFraction of the LARGEST cluster's total duration is
// treated as "minor" and every one of its turns is reassigned to whichever
// "major" cluster has the temporally NEAREST turn — i.e. "this was
// probably actually the person talking right around then, misattributed."
// 5% was chosen empirically against the real recording above: it reduced
// 29 raw clusters (10-minute excerpt) to 6, and 59 raw clusters
// (30-minute excerpt) to 10 — both plausible speaker counts for that
// recording's actual content, and the reduction held up consistently
// across both excerpt lengths (not just a fluke at one length).
constexpr double kMinorSpeakerDurationFraction = 0.05;

void mergeMinorSpeakersIntoNearestMajor(std::vector<sc::OfflineSpeakerDiarizationSegment> &segments)
{
    if (segments.empty())
        return;

    std::map<int, double> totalDuration;
    for (const auto &seg : segments)
        totalDuration[seg.speaker] += (seg.end - seg.start);

    double maxDuration = 0.0;
    for (const auto &kv : totalDuration)
        maxDuration = std::max(maxDuration, kv.second);
    const double cutoff = maxDuration * kMinorSpeakerDurationFraction;

    std::set<int> majorSpeakers;
    for (const auto &kv : totalDuration) {
        if (kv.second >= cutoff)
            majorSpeakers.insert(kv.first);
    }

    // Collect indices of major-speaker segments once — these are never
    // mutated below (only minor segments' .speaker fields are reassigned),
    // so looking them up by these fixed indices stays valid throughout.
    std::vector<size_t> majorIndices;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (majorSpeakers.count(segments[i].speaker))
            majorIndices.push_back(i);
    }
    if (majorIndices.empty())
        return; // degenerate (e.g. every cluster exactly ties) — nothing to merge into

    for (auto &seg : segments) {
        if (majorSpeakers.count(seg.speaker))
            continue;
        const double midpoint = (seg.start + seg.end) / 2.0;
        size_t best = majorIndices.front();
        double bestDist = std::numeric_limits<double>::infinity();
        for (size_t idx : majorIndices) {
            const double otherMid = (segments[idx].start + segments[idx].end) / 2.0;
            const double dist = std::abs(otherMid - midpoint);
            if (dist < bestDist) {
                bestDist = dist;
                best = idx;
            }
        }
        seg.speaker = segments[best].speaker;
    }
}

}

DiarizationEngine::DiarizationEngine(const QString &segmentationModelPath, const QString &embeddingModelPath)
    : m_sd(createSafely(segmentationModelPath, embeddingModelPath))
{
}

std::vector<DiarizedTurn> DiarizationEngine::diarize(const std::vector<float> &pcm16kMono,
                                                      const DiarizationProgressCallback &onProgress) const
{
    std::vector<DiarizedTurn> turns;
    if (!isValid() || pcm16kMono.empty())
        return turns;

    std::vector<sc::OfflineSpeakerDiarizationSegment> segments;
    try {
        if (onProgress) {
            segments = m_sd.Process(pcm16kMono.data(), static_cast<int32_t>(pcm16kMono.size()),
                                     [&onProgress](int32_t numProcessedChunks, int32_t numTotalChunks) {
                                         onProgress(numTotalChunks > 0
                                                        ? static_cast<double>(numProcessedChunks) / numTotalChunks
                                                        : 0.0);
                                     });
        } else {
            segments = m_sd.Process(pcm16kMono.data(), static_cast<int32_t>(pcm16kMono.size()));
        }
    } catch (const std::exception &e) {
        // Same "never crash the app" principle as createSafely() above —
        // an inference-time failure degrades to "no turns found", not a
        // process crash.
        qWarning() << "DiarizationEngine: diarize() failed:" << e.what();
        return turns;
    }

    // See mergeMinorSpeakersIntoNearestMajor()'s doc comment — collapses
    // spurious near-singleton clusters (confirmed to dominate on long, noisy
    // real recordings) into whichever real recurring speaker was talking
    // around the same time, BEFORE the stable renumbering below.
    mergeMinorSpeakersIntoNearestMajor(segments);

    // sherpa-onnx's raw speaker ids are arbitrary cluster indices — renumber
    // into 0-based first-appearance order (segments are already sorted by
    // start time, see OfflineSpeakerDiarization::Process) so the caller gets
    // a stable, predictable "Speaker 1"/"Speaker 2"/... numbering instead of
    // whatever internal id the clustering happened to assign.
    std::map<int, int> renumbered;
    turns.reserve(segments.size());
    for (const auto &seg : segments) {
        auto [it, inserted] = renumbered.try_emplace(seg.speaker, static_cast<int>(renumbered.size()));
        DiarizedTurn turn;
        turn.start = seg.start;
        turn.end = seg.end;
        turn.speaker = it->second;
        turns.push_back(turn);
    }
    return turns;
}
