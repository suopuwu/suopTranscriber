#include "VadChunker.h"

#include <whisper.h>

#include <algorithm>

namespace VadChunker {

std::vector<Window> groupSegmentsIntoWindows(const std::vector<std::pair<double, double>> &segments,
                                              double totalDurationSeconds, double targetWindowSeconds,
                                              double maxWindowSeconds)
{
    if (segments.empty())
        return {{0.0, totalDurationSeconds}};

    std::vector<Window> windows;
    double currentStart = 0.0;
    std::size_t i = 0;
    const std::size_t n = segments.size();
    while (i < n) {
        double windowSpanEnd = segments[i].second;
        ++i;
        // Greedily absorb consecutive segments until growing further would
        // either exceed the hard cap or the window has already reached its
        // target length — whichever comes first. The next segment is only
        // ever excluded (never split) once this window meets its target.
        while (i < n) {
            const double candidateEnd = segments[i].second;
            if (candidateEnd - currentStart > maxWindowSeconds)
                break;
            if (windowSpanEnd - currentStart >= targetWindowSeconds)
                break;
            windowSpanEnd = candidateEnd;
            ++i;
        }
        // Boundary sits at the midpoint of the gap before the next window's
        // first segment (or the file's end for the last window) — every
        // second still belongs to exactly one window, and the split never
        // lands inside a segment.
        const double windowEnd = (i < n) ? (windowSpanEnd + segments[i].first) / 2.0 : totalDurationSeconds;
        windows.push_back({currentStart, windowEnd});
        currentStart = windowEnd;
    }
    return windows;
}

std::vector<Window> computeWindows(const std::vector<float> &pcm16kMono, const QString &vadModelPath,
                                    QString *vadErrorOut, std::vector<std::pair<double, double>> *speechSpansOut,
                                    double targetWindowSeconds, double maxWindowSeconds)
{
    const double totalDurationSeconds = static_cast<double>(pcm16kMono.size()) / 16000.0;
    if (vadErrorOut)
        vadErrorOut->clear();

    if (vadModelPath.isEmpty()) {
        if (vadErrorOut)
            *vadErrorOut = QStringLiteral("no VAD model available");
        return {{0.0, totalDurationSeconds}};
    }

    whisper_vad_context_params ctxParams = whisper_vad_default_context_params();
    whisper_vad_context *vctx = whisper_vad_init_from_file_with_params(vadModelPath.toUtf8().constData(), ctxParams);
    if (!vctx) {
        if (vadErrorOut)
            *vadErrorOut = QStringLiteral("failed to load VAD model at %1").arg(vadModelPath);
        return {{0.0, totalDurationSeconds}};
    }

    whisper_vad_params vadParams = whisper_vad_default_params();
    // Bound how long a single continuous speech run can grow — otherwise a
    // long uninterrupted talker could hand back one VAD segment far past
    // maxWindowSeconds, which groupSegmentsIntoWindows() would then have to
    // honor as its own oversized, unsplittable window (never splits mid-
    // segment).
    vadParams.max_speech_duration_s = static_cast<float>(maxWindowSeconds);

    whisper_vad_segments *vadSegments =
        whisper_vad_segments_from_samples(vctx, vadParams, pcm16kMono.data(), static_cast<int>(pcm16kMono.size()));
    whisper_vad_free(vctx);

    if (!vadSegments) {
        if (vadErrorOut)
            *vadErrorOut = QStringLiteral("VAD detection failed");
        return {{0.0, totalDurationSeconds}};
    }

    std::vector<std::pair<double, double>> segments;
    const int n = whisper_vad_segments_n_segments(vadSegments);
    segments.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        segments.emplace_back(whisper_vad_segments_get_segment_t0(vadSegments, i),
                               whisper_vad_segments_get_segment_t1(vadSegments, i));
    }
    whisper_vad_free_segments(vadSegments);

    if (speechSpansOut)
        *speechSpansOut = segments;

    return groupSegmentsIntoWindows(segments, totalDurationSeconds, targetWindowSeconds, maxWindowSeconds);
}

std::vector<std::pair<double, double>> complementSpans(const std::vector<std::pair<double, double>> &speechSpans,
                                                         double totalSeconds)
{
    if (totalSeconds <= 0.0)
        return {};

    // Clamp each span into [0, totalSeconds] and drop anything that's
    // entirely outside or degenerate (t0 >= t1 after clamping) before
    // sorting/merging — a caller-supplied span list (e.g. raw transcript
    // segment times) isn't guaranteed clean the way VAD's own output is.
    std::vector<std::pair<double, double>> cleaned;
    cleaned.reserve(speechSpans.size());
    for (const auto &span : speechSpans) {
        const double t0 = std::clamp(span.first, 0.0, totalSeconds);
        const double t1 = std::clamp(span.second, 0.0, totalSeconds);
        if (t1 > t0)
            cleaned.emplace_back(t0, t1);
    }

    if (cleaned.empty())
        return {{0.0, totalSeconds}};

    std::sort(cleaned.begin(), cleaned.end());

    // Merge overlapping/touching speech spans so the gaps between what's
    // left are genuine silence stretches.
    std::vector<std::pair<double, double>> merged;
    merged.push_back(cleaned.front());
    for (std::size_t i = 1; i < cleaned.size(); ++i) {
        if (cleaned[i].first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, cleaned[i].second);
        else
            merged.push_back(cleaned[i]);
    }

    std::vector<std::pair<double, double>> silences;
    double cursor = 0.0;
    for (const auto &span : merged) {
        if (span.first > cursor)
            silences.emplace_back(cursor, span.first);
        cursor = std::max(cursor, span.second);
    }
    if (cursor < totalSeconds)
        silences.emplace_back(cursor, totalSeconds);
    return silences;
}

}
