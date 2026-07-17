#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

#include "EngineManifest.h"
#include "GpuProbe.h"

// One discovered out-of-process engine: its id (the folder name), parsed
// manifest, the directory it lives in (SubprocessEngine's working dir), and
// whether it can actually run on this machine + why not.
struct EngineDescriptor {
    QString id;                // engine folder name, also the "engine:" id prefix
    QString dir;               // absolute path to the engine folder
    EngineManifest manifest;   // parsed manifest (default-constructed if parse failed)
    bool manifestValid = false;
    bool available = false;    // runnable here (manifest ok + GPU ok + deps + model)
    QString unavailableReason; // empty iff available; human-readable otherwise
};

// Discovery + capability detection for DESIGN.md "Pluggable engines". Scans
// engine root dirs, parses each <root>/<id>/manifest.json, and marks each
// engine available or grays it out with a reason — never silently drops one.
//
// discover() is deterministic and side-effect-free given its inputs: the GPU
// status and the dependency/model presence checks are all INJECTED, so tests
// pass fixed values and the app wires the real probes (GpuProbe::probe(), a
// python-import check, ModelManager model-file presence). A null predicate
// means "treat that check as satisfied".
namespace EngineRegistry {

// Predicates keyed by engine id. depsPresent answers "are the engine's runtime
// dependencies installed" (e.g. `import nemo` resolvable); modelPresent answers
// "is the engine's model file downloaded for the current vault".
using EnginePredicate = std::function<bool(const EngineDescriptor &)>;

// rootDirs are scanned in order; a later root's engine OVERRIDES an earlier
// one with the same id (bundled first, then vault — same "same loader,
// different root" shape as IconTheme). Missing roots are skipped.
std::vector<EngineDescriptor> discover(const QStringList &rootDirs, const GpuStatus &gpuStatus,
                                        const EnginePredicate &depsPresent = {},
                                        const EnginePredicate &modelPresent = {});

} // namespace EngineRegistry
