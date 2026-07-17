// tst_enginemanifest — spec for EngineManifest::parse(), the pure manifest.json
// reader for DESIGN.md "Pluggable engines — out-of-process". Parsing is a pure
// function of the input bytes (no disk/network), so every case here runs on a
// QByteArray literal.

#include <QtTest>

#include "EngineManifest.h"

class TstEngineManifest : public QObject
{
    Q_OBJECT

private slots:
    // Spec: a well-formed manifest yields every field, with capability
    // sub-fields read through.
    void parsesWellFormedManifest()
    {
        const QByteArray json = R"({
            "name": "Parakeet TDT 0.6B v2",
            "command": ["python", "run.py"],
            "capabilities": { "languages": ["en"], "gpu": "cuda", "vram_gb_min": 4 }
        })";
        QString error;
        const auto manifest = EngineManifest::parse(json, &error);
        QVERIFY2(manifest.has_value(), qPrintable(error));
        QCOMPARE(manifest->name, QStringLiteral("Parakeet TDT 0.6B v2"));
        QCOMPARE(manifest->command, (QStringList{QStringLiteral("python"), QStringLiteral("run.py")}));
        QCOMPARE(manifest->capabilities.languages, QStringList{QStringLiteral("en")});
        QCOMPARE(manifest->capabilities.gpu, QStringLiteral("cuda"));
        QCOMPARE(manifest->capabilities.vramGbMin, 4);
    }

    // Spec: capabilities as a whole is optional — an omitted block leaves the
    // EngineCapabilities defaults (gpu "any", no vram floor, "any" language).
    void capabilitiesOptionalWithDefaults()
    {
        const QByteArray json = R"({ "name": "Bare", "command": ["./run"] })";
        const auto manifest = EngineManifest::parse(json);
        QVERIFY(manifest.has_value());
        QCOMPARE(manifest->capabilities.gpu, QStringLiteral("any"));
        QCOMPARE(manifest->capabilities.vramGbMin, 0);
        QVERIFY(manifest->isLanguageSupported(QStringLiteral("en")));
        QVERIFY(manifest->isLanguageSupported(QStringLiteral("zz")));
    }

    // Spec: the "*" languages wildcard means every language is supported;
    // an explicit list restricts to it (case-insensitively).
    void languageSupportRespectsWildcardAndList()
    {
        const auto wildcard = EngineManifest::parse(
            R"({ "name": "W", "command": ["x"], "capabilities": { "languages": ["*"] } })");
        QVERIFY(wildcard.has_value());
        QVERIFY(wildcard->isLanguageSupported(QStringLiteral("fr")));

        const auto english = EngineManifest::parse(
            R"({ "name": "E", "command": ["x"], "capabilities": { "languages": ["en"] } })");
        QVERIFY(english.has_value());
        QVERIFY(english->isLanguageSupported(QStringLiteral("EN")));
        QVERIFY(!english->isLanguageSupported(QStringLiteral("de")));
    }

    // Spec: structural problems fail with a message, never a crash and never a
    // silently-degraded manifest. Table-driven over each rejection case.
    void rejectsMalformedManifests_data()
    {
        QTest::addColumn<QByteArray>("json");
        QTest::newRow("not json") << QByteArray("{ this is not json ");
        QTest::newRow("not an object") << QByteArray("[1, 2, 3]");
        QTest::newRow("missing name") << QByteArray(R"({ "command": ["x"] })");
        QTest::newRow("empty name") << QByteArray(R"({ "name": "", "command": ["x"] })");
        QTest::newRow("missing command") << QByteArray(R"({ "name": "N" })");
        QTest::newRow("empty command") << QByteArray(R"({ "name": "N", "command": [] })");
        QTest::newRow("non-string command entry") << QByteArray(R"({ "name": "N", "command": ["x", 7] })");
        QTest::newRow("bad gpu enum")
            << QByteArray(R"({ "name": "N", "command": ["x"], "capabilities": { "gpu": "metal" } })");
    }
    void rejectsMalformedManifests()
    {
        QFETCH(QByteArray, json);
        QString error;
        const auto manifest = EngineManifest::parse(json, &error);
        QVERIFY(!manifest.has_value());
        QVERIFY2(!error.isEmpty(), "a rejected manifest must explain why");
    }
};

QTEST_GUILESS_MAIN(TstEngineManifest)
#include "tst_enginemanifest.moc"
