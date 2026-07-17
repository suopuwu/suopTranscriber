#pragma once

#include <QString>
#include <QStringList>

#include <optional>

// Parsed form of an out-of-process engine's `manifest.json` (see DESIGN.md
// "Pluggable engines — out-of-process"). Adding an engine is dropping a folder
// with one of these plus its runner script — no rebuild. This struct is the
// in-memory shape; EngineRegistry does the on-disk discovery + capability
// probing around it.
//
// parse() is a PURE function of its input bytes (no filesystem/network), so it
// is unit-testable on a QByteArray alone (tst_enginemanifest) — same
// pure-parser discipline as the rest of core.
struct EngineCapabilities {
    // Languages the engine handles: ["*"] means "any" (the manifest wildcard),
    // otherwise a list of language codes. Empty in the parsed struct is treated
    // as ["*"] by isLanguageSupported().
    QStringList languages;
    // Compute requirement: "cuda" | "vulkan" | "cpu" | "any". Governs whether
    // GpuProbe considers this machine able to run the engine at all. Defaults
    // to "any" when the manifest omits it.
    QString gpu = QStringLiteral("any");
    // Minimum VRAM in whole GB the engine needs; 0 = unspecified/none.
    int vramGbMin = 0;
};

struct EngineManifest {
    QString name;        // human-readable, shown in the engine picker
    QStringList command; // argv relative to the manifest dir, e.g. ["python", "run.py"]
    EngineCapabilities capabilities;

    // The four accepted values for capabilities.gpu — a manifest naming
    // anything else is rejected by parse() (an unrecognized requirement can't
    // be capability-checked, so it must fail loudly rather than default-run).
    static const QStringList &validGpuValues();

    bool isLanguageSupported(const QString &lang) const;

    // Parses `json` (a manifest.json's bytes) into an EngineManifest. Returns
    // nullopt with *errorOut set (if given) on any structural problem: not a
    // JSON object, missing/empty `name`, missing/empty/non-string `command`,
    // or a `capabilities.gpu` outside validGpuValues(). Optional capability
    // fields fall back to their EngineCapabilities defaults when absent. Never
    // throws.
    static std::optional<EngineManifest> parse(const QByteArray &json, QString *errorOut = nullptr);
};
