#include "EngineManifest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

const QStringList &EngineManifest::validGpuValues()
{
    static const QStringList values{QStringLiteral("cuda"), QStringLiteral("vulkan"), QStringLiteral("cpu"),
                                    QStringLiteral("any")};
    return values;
}

bool EngineManifest::isLanguageSupported(const QString &lang) const
{
    // Empty list or the "*" wildcard both mean "any language".
    if (capabilities.languages.isEmpty() || capabilities.languages.contains(QStringLiteral("*")))
        return true;
    return capabilities.languages.contains(lang, Qt::CaseInsensitive);
}

std::optional<EngineManifest> EngineManifest::parse(const QByteArray &json, QString *errorOut)
{
    const auto fail = [errorOut](const QString &why) -> std::optional<EngineManifest> {
        if (errorOut)
            *errorOut = why;
        return std::nullopt;
    };

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(QStringLiteral("invalid JSON: %1").arg(parseError.errorString()));
    if (!doc.isObject())
        return fail(QStringLiteral("manifest is not a JSON object"));
    const QJsonObject root = doc.object();

    EngineManifest manifest;

    manifest.name = root.value(QStringLiteral("name")).toString();
    if (manifest.name.isEmpty())
        return fail(QStringLiteral("missing or empty \"name\""));

    const QJsonValue commandValue = root.value(QStringLiteral("command"));
    if (!commandValue.isArray())
        return fail(QStringLiteral("missing or non-array \"command\""));
    for (const QJsonValue &arg : commandValue.toArray()) {
        if (!arg.isString())
            return fail(QStringLiteral("\"command\" entries must all be strings"));
        manifest.command.append(arg.toString());
    }
    if (manifest.command.isEmpty())
        return fail(QStringLiteral("\"command\" must have at least one entry"));

    // capabilities is optional as a whole; each field falls back to its
    // EngineCapabilities default when absent.
    const QJsonValue capsValue = root.value(QStringLiteral("capabilities"));
    if (!capsValue.isUndefined() && !capsValue.isNull()) {
        if (!capsValue.isObject())
            return fail(QStringLiteral("\"capabilities\" is not a JSON object"));
        const QJsonObject caps = capsValue.toObject();

        const QJsonValue languages = caps.value(QStringLiteral("languages"));
        if (languages.isArray()) {
            manifest.capabilities.languages.clear();
            for (const QJsonValue &lang : languages.toArray()) {
                if (lang.isString())
                    manifest.capabilities.languages.append(lang.toString());
            }
        }

        const QJsonValue gpu = caps.value(QStringLiteral("gpu"));
        if (gpu.isString()) {
            const QString value = gpu.toString();
            if (!validGpuValues().contains(value))
                return fail(QStringLiteral("\"capabilities.gpu\" must be one of %1")
                                .arg(validGpuValues().join(QStringLiteral(", "))));
            manifest.capabilities.gpu = value;
        }

        const QJsonValue vram = caps.value(QStringLiteral("vram_gb_min"));
        if (vram.isDouble())
            manifest.capabilities.vramGbMin = vram.toInt();
    }

    return manifest;
}
