#include "GpuProbe.h"

#include <QProcess>
#include <QStringList>

namespace GpuProbe {

namespace {

// Runs a short-lived probe process and returns its trimmed stdout, or an empty
// string if the tool is missing / errors / times out. Never throws.
QString runProbe(const QString &program, const QStringList &args, int timeoutMs = 3000)
{
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(timeoutMs))
        return QString();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        return QString();
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return QString();
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

} // namespace

GpuStatus probe()
{
    GpuStatus status;

    // nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits prints
    // one line per GPU, VRAM in MiB. Presence of any line ⇒ an NVIDIA GPU.
    const QString smi = runProbe(QStringLiteral("nvidia-smi"),
                                 {QStringLiteral("--query-gpu=memory.total"),
                                  QStringLiteral("--format=csv,noheader,nounits")});
    if (!smi.isEmpty()) {
        status.nvidiaPresent = true;
        int maxMib = 0;
        const QStringList lines = smi.split(QChar('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            bool ok = false;
            const int mib = line.trimmed().toInt(&ok);
            if (ok)
                maxMib = std::max(maxMib, mib);
        }
        status.nvidiaVramGb = maxMib / 1024; // integer GB, floor
    }

    // vulkaninfo --summary succeeds (exit 0) only when a Vulkan ICD + device is
    // usable; its absence or failure leaves vulkanPresent false (fail closed).
    const QString vk = runProbe(QStringLiteral("vulkaninfo"), {QStringLiteral("--summary")});
    if (!vk.isEmpty() && vk.contains(QStringLiteral("deviceName"), Qt::CaseInsensitive))
        status.vulkanPresent = true;

    return status;
}

QString unmetRequirement(const QString &gpu, int vramGbMin, const GpuStatus &status)
{
    // "cpu"/"any" need no GPU at all — always runnable.
    if (gpu == QLatin1String("cpu") || gpu == QLatin1String("any"))
        return QString();

    if (gpu == QLatin1String("cuda")) {
        if (!status.nvidiaPresent)
            return QStringLiteral("requires an NVIDIA (CUDA) GPU");
        if (vramGbMin > 0) {
            if (status.nvidiaVramGb <= 0)
                return QStringLiteral("could not verify GPU VRAM (needs %1 GB)").arg(vramGbMin);
            if (status.nvidiaVramGb < vramGbMin)
                return QStringLiteral("needs %1 GB VRAM (found %2 GB)").arg(vramGbMin).arg(status.nvidiaVramGb);
        }
        return QString();
    }

    if (gpu == QLatin1String("vulkan")) {
        if (!status.vulkanPresent)
            return QStringLiteral("requires a Vulkan-capable GPU");
        return QString();
    }

    // Unknown requirement — EngineManifest::parse() already rejects these, so
    // reaching here means a manifest bypassed validation; fail closed.
    return QStringLiteral("unknown GPU requirement \"%1\"").arg(gpu);
}

} // namespace GpuProbe
