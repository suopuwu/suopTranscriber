#pragma once

#include <QString>

// What compute this machine actually offers, for gating out-of-process engines
// by their manifest `capabilities.gpu`/`vram_gb_min` (DESIGN.md "GPU:
// cross-vendor is not uniform across engines" + "Capability detection"). The
// probe is best-effort and fails CLOSED: anything it can't verify stays
// false/0, so a wrongly-optimistic "available" never leads to a CUDA-OOM crash
// in a child process — a false negative is just a visible, explained gray-out.
struct GpuStatus {
    bool nvidiaPresent = false; // an NVIDIA GPU usable for CUDA engines
    int nvidiaVramGb = 0;       // total VRAM (GB) of the largest NVIDIA GPU, 0 = unknown
    bool vulkanPresent = false; // a Vulkan-capable GPU (cross-vendor baseline)
};

namespace GpuProbe {

// Probes the real machine: shells `nvidia-smi` for NVIDIA presence/VRAM and
// `vulkaninfo` for Vulkan. Missing tools / parse failures leave fields at their
// fail-closed defaults. Blocking (spawns short-lived processes) — call off the
// GUI thread.
GpuStatus probe();

// Pure: given a manifest's gpu requirement ("cuda"|"vulkan"|"cpu"|"any") and
// VRAM floor, returns an empty string if `status` satisfies it, else a
// human-readable reason it doesn't. Unverifiable VRAM on an otherwise-present
// GPU fails closed (returns a reason), consistent with GpuStatus's contract.
QString unmetRequirement(const QString &gpu, int vramGbMin, const GpuStatus &status);

} // namespace GpuProbe
