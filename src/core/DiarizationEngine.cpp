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
                                                 float threshold, const char *provider)
{
    sc::OfflineSpeakerDiarizationConfig config;
    config.segmentation.pyannote.model = segmentationModelPath.toStdString();
    config.embedding.model = embeddingModelPath.toStdString();
    // Speaker count is always auto-detected (see class doc) — num_clusters
    // stays 0 (its default), so the threshold is what actually governs
    // clustering. It's now the user-facing "sensitivity" (DiarizationEngine's
    // ctor doc); 0.5 is sherpa-onnx's own documented default for this exact
    // clustering algorithm (cosine distance, complete linkage).
    config.clustering.num_clusters = 0;
    config.clustering.threshold = threshold;
    config.segmentation.provider = provider;
    config.embedding.provider = provider;
    // Relevant for the "cpu" attempt (and any CPU-side ops even under
    // "cuda") — see cpuThreadCount()'s own doc comment.
    config.segmentation.num_threads = cpuThreadCount();
    config.embedding.num_threads = cpuThreadCount();
    return config;
}

// SUPERSEDES an earlier (2026-07-07, same day) measurement against the old
// CAM++ embedding model, which found CUDA reliably SLOWER than CPU (2.35s
// vs 0.24s on an 11s clip) — root-caused to the segmentation model's shape
// (~1 forward pass per second of audio, hundreds of small sequential calls,
// GPU kernel-launch/transfer overhead dominating). That conclusion drove
// kTryCuda to false, but was never re-verified after the embedding model was
// swapped to WeSpeaker's ResNet293-LM (~115 MB, a much deeper conv net — see
// ModelManager::kDiarizationEmbeddingFileName's doc comment) later that same
// day — the embedding half's own per-call cost grew substantially with that
// swap, and it turns out that outweighs the segmentation half's GPU penalty
// well before we cared to look.
//
// Re-measured (2026-07-07, later) on real audio against the CURRENT
// (WeSpeaker) embedding model: CUDA is now reliably FASTER on real-length
// recordings — 330.1s (CPU, 263 raw turns) vs 148.4s (CUDA, 250 raw turns)
// on a 24.6-minute real recording, a >2x wall-clock win, on an RTX 5090.
// Turn counts differ slightly between providers (expected float-precision
// divergence in the embedding/clustering math, not a correctness issue —
// both land in the same ballpark and both get the same
// mergeMinorSpeakersIntoNearestMajor() cleanup below regardless). The old
// short-clip numbers likely still hold in isolation (CUDA's fixed per-call
// overhead doesn't disappear), but real recordings are the case that
// actually matters, and there CUDA wins decisively now.
constexpr bool kTryCuda = true;

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
sc::OfflineSpeakerDiarization createSafely(const QString &segmentationModelPath, const QString &embeddingModelPath,
                                            float threshold, bool *usedCuda = nullptr)
{
    if (usedCuda)
        *usedCuda = false;
    if constexpr (kTryCuda) {
        try {
            sc::OfflineSpeakerDiarization sd = sc::OfflineSpeakerDiarization::Create(
                buildConfig(segmentationModelPath, embeddingModelPath, threshold, "cuda"));
            if (sd.Get() != nullptr) {
                if (usedCuda)
                    *usedCuda = true;
                return sd;
            }
            qWarning() << "DiarizationEngine: CUDA init returned no handle — falling back to CPU";
        } catch (const std::exception &e) {
            qWarning() << "DiarizationEngine: CUDA init failed (" << e.what() << ") — falling back to CPU";
        }
    }

    try {
        return sc::OfflineSpeakerDiarization::Create(
            buildConfig(segmentationModelPath, embeddingModelPath, threshold, "cpu"));
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

}

// Fix (applied here, not inside sherpa-onnx's clustering, which is
// compiled code we don't control): the original version of this function
// judged a cluster "minor" (noise) by total TALK TIME — under 5% of the
// largest cluster's duration got folded into the nearest major cluster.
// That worked for the 4.8-hour recording it was tuned against, but proved
// wrong in general: a genuine, recurring second speaker whose turns
// happen to land across MANY small raw clusters (the same fragmentation
// this function exists to clean up) can have every one of those clusters
// individually fall under a duration cutoff even though their combined
// talk time is substantial — silently erasing a real speaker down to
// "everyone is Speaker 1" on long/imbalanced recordings (confirmed: a
// 68-minute real two-person conversation collapsed entirely to one
// speaker this way).
//
// Fixed criterion, from the SAME 4.8-hour analysis that motivated this
// function in the first place: what actually distinguished noise from
// real speakers there wasn't a duration ratio, it was TURN COUNT — 367 of
// 533 raw clusters (69%) had EXACTLY ONE turn (an isolated, non-recurring
// blip — cross-talk or a one-off noisy embedding), while every real
// speaker, including the least active one, recurred across multiple
// turns. So only true singleton (exactly one turn) clusters are treated
// as minor and reassigned to the nearest major cluster by temporal
// proximity; any cluster with more than one turn is kept as its own
// speaker regardless of how little it talked in total.
void mergeMinorSpeakersIntoNearestMajor(std::vector<sc::OfflineSpeakerDiarizationSegment> &segments)
{
    if (segments.empty())
        return;

    std::map<int, int> turnCount;
    for (const auto &seg : segments)
        ++turnCount[seg.speaker];

    std::set<int> majorSpeakers;
    for (const auto &kv : turnCount) {
        if (kv.second > 1)
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
        return; // degenerate (every cluster is a singleton, e.g. a very short clip) — nothing to merge into

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

DiarizationEngine::DiarizationEngine(const QString &segmentationModelPath, const QString &embeddingModelPath,
                                     float threshold)
    : m_segmentationModelPath(segmentationModelPath)
    , m_embeddingModelPath(embeddingModelPath)
    , m_threshold(threshold)
    , m_sd(createSafely(segmentationModelPath, embeddingModelPath, threshold, &m_usingCuda))
{
}

std::vector<DiarizedTurn> DiarizationEngine::diarize(const std::vector<float> &pcm16kMono,
                                                      const DiarizationProgressCallback &onProgress,
                                                      bool *inferenceFailed) const
{
    std::vector<DiarizedTurn> turns;
    if (inferenceFailed)
        *inferenceFailed = false;
    if (!isValid() || pcm16kMono.empty())
        return turns;

    std::vector<sc::OfflineSpeakerDiarizationSegment> segments;
    const auto runProcess = [&](const sc::OfflineSpeakerDiarization &sd) {
        if (onProgress) {
            segments = sd.Process(pcm16kMono.data(), static_cast<int32_t>(pcm16kMono.size()),
                                   [&onProgress](int32_t numProcessedChunks, int32_t numTotalChunks) {
                                       onProgress(numTotalChunks > 0
                                                      ? static_cast<double>(numProcessedChunks) / numTotalChunks
                                                      : 0.0);
                                   });
        } else {
            segments = sd.Process(pcm16kMono.data(), static_cast<int32_t>(pcm16kMono.size()));
        }
    };

    try {
        runProcess(m_sd);
    } catch (const std::exception &e) {
        qWarning() << "DiarizationEngine: diarize() failed:" << e.what();
        // A CUDA inference failure on a long recording is almost always the
        // onnxruntime CUDA arena exhausting VRAM (it caches and grows
        // monotonically — measured ~29 GB on a multi-hour file; see
        // patches/sherpa-onnx-batch-segmentation.patch). The load-time CUDA→CPU
        // fallback in createSafely() only covers a failed CUDA *init*, so retry
        // here on a freshly-built CPU session (its arena lives in system RAM),
        // and latch to CPU so the rest of a batch skips the doomed CUDA attempt.
        if (m_usingCuda && !m_segmentationModelPath.isEmpty()) {
            try {
                sc::OfflineSpeakerDiarization cpuSd = sc::OfflineSpeakerDiarization::Create(
                    buildConfig(m_segmentationModelPath, m_embeddingModelPath, m_threshold, "cpu"));
                if (cpuSd.Get() != nullptr) {
                    qWarning() << "DiarizationEngine: retrying diarization on CPU (GPU out of memory)";
                    runProcess(cpuSd);
                    m_usingCuda = false;
                } else {
                    if (inferenceFailed)
                        *inferenceFailed = true;
                    return turns;
                }
            } catch (const std::exception &e2) {
                qWarning() << "DiarizationEngine: CPU fallback also failed:" << e2.what();
                if (inferenceFailed)
                    *inferenceFailed = true;
                return turns;
            }
        } else {
            // Already on CPU (or no model paths) — no lower-memory tier left;
            // this is a real failure (the signal makes DiarizeTask throw).
            if (inferenceFailed)
                *inferenceFailed = true;
            return turns;
        }
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
