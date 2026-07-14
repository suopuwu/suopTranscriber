#pragma once

#include <QString>

#include <utility>
#include <vector>

// Groups a file's audio into VAD-aligned windows for chunked transcription
// (DESIGN.md "Durable / resumable transcription" — chunking half only;
// checkpoint/resume to .partial.jsonl is a separate, not-yet-built
// milestone). Splitting only at gaps between distinct speech segments —
// never mid-segment, never a fixed-time slice that could cut a word — keeps
// each whisper_full() call's decode context bounded to a few minutes, which
// reduces the long-context hallucination drift documented on multi-hour
// single-pass whisper.cpp runs, and lays the groundwork for future resume
// checkpoints landing on the same window boundaries.
namespace VadChunker {

struct Window {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
};

// Pure grouping logic, no audio/model involved — the unit-testable core.
// segments: sorted, non-overlapping (t0, t1) speech spans in seconds.
// Returned windows always cover [0, totalDurationSeconds] with no gaps
// (silence belongs to whichever window it falls into) and never split a
// segment across two windows. An empty segments list (silent/instrumental
// file) yields a single window spanning the whole file — same as no
// chunking at all.
std::vector<Window> groupSegmentsIntoWindows(const std::vector<std::pair<double, double>> &segments,
                                              double totalDurationSeconds, double targetWindowSeconds = 180.0,
                                              double maxWindowSeconds = 300.0);

// Runs whisper.cpp's public VAD API once over the whole buffer and groups the
// result via groupSegmentsIntoWindows(). Degrades gracefully: an empty
// vadModelPath, or a model that fails to load/detect, returns a single
// whole-file window (today's behavior) rather than blocking transcription —
// vadErrorOut (if given) is set to a human-readable reason exactly when that
// fallback happens, so the caller can record it honestly in frontmatter
// (never a silent substitution, per CLAUDE.md's "Unrecognized model id"
// precedent for ModelManager).
std::vector<Window> computeWindows(const std::vector<float> &pcm16kMono, const QString &vadModelPath,
                                    QString *vadErrorOut = nullptr, double targetWindowSeconds = 180.0,
                                    double maxWindowSeconds = 300.0);

}
