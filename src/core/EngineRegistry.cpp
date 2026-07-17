#include "EngineRegistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <map>

namespace EngineRegistry {

namespace {

// Fills available/unavailableReason for one already-parsed descriptor, in the
// cheap-to-costly order the user most wants to see: a broken manifest first,
// then this machine's GPU, then installed deps, then the downloaded model.
void evaluate(EngineDescriptor &desc, const GpuStatus &gpuStatus, const EnginePredicate &depsPresent,
              const EnginePredicate &modelPresent)
{
    if (!desc.manifestValid) {
        desc.available = false; // reason already set at parse time
        return;
    }

    const QString gpuReason =
        GpuProbe::unmetRequirement(desc.manifest.capabilities.gpu, desc.manifest.capabilities.vramGbMin, gpuStatus);
    if (!gpuReason.isEmpty()) {
        desc.available = false;
        desc.unavailableReason = gpuReason;
        return;
    }

    if (depsPresent && !depsPresent(desc)) {
        desc.available = false;
        desc.unavailableReason = QStringLiteral("engine dependencies not installed");
        return;
    }

    if (modelPresent && !modelPresent(desc)) {
        desc.available = false;
        desc.unavailableReason = QStringLiteral("model not downloaded");
        return;
    }

    desc.available = true;
    desc.unavailableReason.clear();
}

} // namespace

std::vector<EngineDescriptor> discover(const QStringList &rootDirs, const GpuStatus &gpuStatus,
                                        const EnginePredicate &depsPresent, const EnginePredicate &modelPresent)
{
    // id -> descriptor, later roots overriding earlier; ordered map keeps the
    // engine list stable/alphabetical for the UI.
    std::map<QString, EngineDescriptor> byId;

    for (const QString &root : rootDirs) {
        QDir rootDir(root);
        if (!rootDir.exists())
            continue;
        const QStringList engineDirs = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &id : engineDirs) {
            const QString engineDir = rootDir.absoluteFilePath(id);
            const QString manifestPath = QDir(engineDir).filePath(QStringLiteral("manifest.json"));
            if (!QFile::exists(manifestPath))
                continue; // not an engine folder

            EngineDescriptor desc;
            desc.id = id;
            desc.dir = engineDir;

            QFile file(manifestPath);
            if (!file.open(QIODevice::ReadOnly)) {
                desc.manifestValid = false;
                desc.unavailableReason = QStringLiteral("could not read manifest.json");
                byId[id] = desc;
                continue;
            }
            QString parseError;
            const auto manifest = EngineManifest::parse(file.readAll(), &parseError);
            if (!manifest.has_value()) {
                desc.manifestValid = false;
                desc.unavailableReason = QStringLiteral("invalid manifest: %1").arg(parseError);
            } else {
                desc.manifest = *manifest;
                desc.manifestValid = true;
            }
            byId[id] = desc; // override any earlier root's same-id engine
        }
    }

    std::vector<EngineDescriptor> result;
    result.reserve(byId.size());
    for (auto &[id, desc] : byId) {
        evaluate(desc, gpuStatus, depsPresent, modelPresent);
        result.push_back(desc);
    }
    return result;
}

} // namespace EngineRegistry
