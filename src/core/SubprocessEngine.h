#pragma once

#include <QString>

#include "EngineManifest.h"
#include "TranscriptionEngine.h"

// Out-of-process TranscriptionEngine (see DESIGN.md "Pluggable engines —
// out-of-process"). Spawns an engine's manifest `command` as a QProcess, hands
// it one AudioChunk as a temp WAV, and parses the JSONL it streams back on
// stdout into Segments — the wire protocol pinned in tst_engine_contract.cpp:
//   {"type":"progress","seconds_done":..,"seconds_total":..}
//   {"type":"segment","t0":..,"t1":..,"text":".."}
//   {"type":"done"}
//   {"type":"error","message":"..","recoverable":true|false}
//
// Same TranscriptionEngine contract as WhisperCppEngine, so the two are
// swappable behind the interface: timestamps are CHUNK-RELATIVE (the app owns
// the offset merge — this engine must NOT pre-apply chunk.offsetSeconds; the
// --offset arg it passes to the child is informational only), progress/segments
// stream through the callbacks as produced, and onAbort() drives cooperative
// pause/cancel.
//
// Crash isolation is the whole point of running out-of-process: a child crash,
// non-zero exit, spawn failure, or a fatal `error` line degrades to "return the
// segments produced so far" with a qWarning — never a throw, never a process
// abort — the same graceful-degradation contract DiarizationEngine follows.
class SubprocessEngine : public TranscriptionEngine {
public:
    // manifestDir is the engine folder (the QProcess working directory, so a
    // relative `command` like ["python","run.py"] resolves). scratchBaseDir is
    // where the per-call temp WAV is written — pass a caller-owned dir (never
    // the vault/.transcriber, per CLAUDE.md); empty means QStandardPaths'
    // TempLocation. language is forwarded as --lang ("auto" by default).
    SubprocessEngine(EngineManifest manifest, QString manifestDir, QString scratchBaseDir = QString(),
                     QString language = QStringLiteral("auto"));

    std::vector<Segment> transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress,
                                     const SegmentCallback &onSegment,
                                     const AbortCallback &onAbort = {}) override;

private:
    EngineManifest m_manifest;
    QString m_manifestDir;
    QString m_scratchBaseDir;
    QString m_language;
};
