#include "UsbRuleStore.h"

#include <QSettings>
#include <QVariantMap>

namespace {

const auto kGroup = QStringLiteral("usbGrabRules");

// QSettings treats '/' as a group separator even inside what's meant to be a
// single key, so a deviceSerial containing '/' would otherwise be silently
// split into nested subgroups. Percent-encode '/' (and '%' itself, so the
// encoding round-trips unambiguously) before using a serial as a key name.
QString encodeSerialKey(const QString &serial)
{
    QString encoded = serial;
    encoded.replace(QLatin1Char('%'), QStringLiteral("%25"));
    encoded.replace(QLatin1Char('/'), QStringLiteral("%2F"));
    return encoded;
}

QString decodeSerialKey(const QString &key)
{
    QString decoded = key;
    decoded.replace(QStringLiteral("%2F"), QStringLiteral("/"));
    decoded.replace(QStringLiteral("%25"), QStringLiteral("%"));
    return decoded;
}

QVariantMap ruleToVariantMap(const UsbGrabRule &rule)
{
    QVariantMap map;
    map.insert(QStringLiteral("enabled"), rule.enabled);
    map.insert(QStringLiteral("autoGrab"), rule.autoGrab);
    map.insert(QStringLiteral("extensionFilter"), rule.extensionFilter);
    map.insert(QStringLiteral("filenameRegex"), rule.filenameRegex);
    map.insert(QStringLiteral("preserveStructure"), rule.preserveStructure);
    map.insert(QStringLiteral("startFolder"), rule.startFolder);
    map.insert(QStringLiteral("deviceLabel"), rule.deviceLabel);
    map.insert(QStringLiteral("destinationFolder"), rule.destinationFolder);
    return map;
}

UsbGrabRule ruleFromVariantMap(const QString &deviceSerial, const QVariantMap &map)
{
    UsbGrabRule rule;
    rule.deviceSerial = deviceSerial;
    rule.enabled = map.value(QStringLiteral("enabled"), false).toBool();
    rule.autoGrab = map.value(QStringLiteral("autoGrab"), false).toBool();
    rule.extensionFilter = map.value(QStringLiteral("extensionFilter")).toStringList();
    rule.filenameRegex = map.value(QStringLiteral("filenameRegex")).toString();
    rule.preserveStructure = map.value(QStringLiteral("preserveStructure"), true).toBool();
    rule.startFolder = map.value(QStringLiteral("startFolder")).toString();
    rule.deviceLabel = map.value(QStringLiteral("deviceLabel")).toString();
    rule.destinationFolder = map.value(QStringLiteral("destinationFolder")).toString();
    return rule;
}

} // namespace

QList<UsbGrabRule> UsbRuleStore::loadAllRules() const
{
    QList<UsbGrabRule> rules;
    QSettings settings;
    settings.beginGroup(kGroup);
    const QStringList keys = settings.childKeys();
    rules.reserve(keys.size());
    for (const QString &key : keys)
        rules.append(ruleFromVariantMap(decodeSerialKey(key), settings.value(key).toMap()));
    settings.endGroup();
    return rules;
}

void UsbRuleStore::saveRule(const UsbGrabRule &rule)
{
    QSettings settings;
    settings.beginGroup(kGroup);
    settings.setValue(encodeSerialKey(rule.deviceSerial), ruleToVariantMap(rule));
    settings.endGroup();
}

void UsbRuleStore::removeRule(const QString &deviceSerial)
{
    QSettings settings;
    settings.beginGroup(kGroup);
    settings.remove(encodeSerialKey(deviceSerial));
    settings.endGroup();
}

UsbGrabRule UsbRuleStore::ruleForSerial(const QString &deviceSerial) const
{
    QSettings settings;
    settings.beginGroup(kGroup);
    const QString key = encodeSerialKey(deviceSerial);
    const bool exists = settings.contains(key);
    const QVariantMap map = exists ? settings.value(key).toMap() : QVariantMap();
    settings.endGroup();

    if (exists)
        return ruleFromVariantMap(deviceSerial, map);

    UsbGrabRule rule;
    rule.deviceSerial = deviceSerial;
    return rule;
}
