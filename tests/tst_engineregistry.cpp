// tst_engineregistry — spec for EngineRegistry::discover(), the capability
// detection behind DESIGN.md "Pluggable engines". Uses temp-dir fixture
// manifests and INJECTED gpu/deps/model checks so every case is deterministic
// on any machine (no real GPU or engine needed).

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include "EngineRegistry.h"

class TstEngineRegistry : public QObject
{
    Q_OBJECT

    // Writes <root>/<id>/manifest.json with the given bytes; returns false if
    // the write fails. An empty `manifest` still creates the folder (to test a
    // folder that has no manifest.json → skipped) when writeManifest is false.
    static bool makeEngine(const QString &root, const QString &id, const QByteArray &manifest,
                           bool writeManifest = true)
    {
        const QString dir = QDir(root).filePath(id);
        if (!QDir().mkpath(dir))
            return false;
        if (!writeManifest)
            return true;
        QFile file(QDir(dir).filePath(QStringLiteral("manifest.json")));
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(manifest);
        return true;
    }

    static QByteArray manifest(const QString &name, const QString &gpu = QStringLiteral("any"), int vram = 0)
    {
        return QStringLiteral(R"({"name":"%1","command":["python","run.py"],)"
                              R"("capabilities":{"gpu":"%2","vram_gb_min":%3}})")
            .arg(name, gpu)
            .arg(vram)
            .toUtf8();
    }

    static const EngineDescriptor *find(const std::vector<EngineDescriptor> &v, const QString &id)
    {
        for (const auto &d : v)
            if (d.id == id)
                return &d;
        return nullptr;
    }

private slots:
    // Spec: a valid manifest whose GPU/deps/model checks all pass is available.
    void availableWhenAllChecksPass()
    {
        QTemporaryDir root;
        QVERIFY(makeEngine(root.path(), QStringLiteral("good"), manifest(QStringLiteral("Good"))));

        GpuStatus gpu;
        const auto engines = EngineRegistry::discover(
            {root.path()}, gpu, [](const EngineDescriptor &) { return true; },
            [](const EngineDescriptor &) { return true; });

        const EngineDescriptor *good = find(engines, QStringLiteral("good"));
        QVERIFY(good);
        QVERIFY(good->manifestValid);
        QVERIFY2(good->available, qPrintable(good->unavailableReason));
        QCOMPARE(good->manifest.name, QStringLiteral("Good"));
    }

    // Spec: a CUDA engine on a machine with no NVIDIA GPU is grayed out, not
    // dropped, with a GPU reason.
    void grayedOutWhenGpuUnmet()
    {
        QTemporaryDir root;
        QVERIFY(makeEngine(root.path(), QStringLiteral("cudaeng"),
                           manifest(QStringLiteral("CUDA Engine"), QStringLiteral("cuda"), 4)));

        GpuStatus gpu; // nvidiaPresent = false
        const auto engines = EngineRegistry::discover({root.path()}, gpu);

        const EngineDescriptor *e = find(engines, QStringLiteral("cudaeng"));
        QVERIFY(e);
        QVERIFY(!e->available);
        QVERIFY(e->unavailableReason.contains(QStringLiteral("NVIDIA")));
    }

    // Spec: dependencies missing → grayed out with a deps reason (this is the
    // "NeMo not installed" path for Parakeet).
    void grayedOutWhenDepsMissing()
    {
        QTemporaryDir root;
        QVERIFY(makeEngine(root.path(), QStringLiteral("eng"), manifest(QStringLiteral("Eng"))));

        GpuStatus gpu;
        const auto engines = EngineRegistry::discover(
            {root.path()}, gpu, [](const EngineDescriptor &) { return false; }, {});

        const EngineDescriptor *e = find(engines, QStringLiteral("eng"));
        QVERIFY(e);
        QVERIFY(!e->available);
        QVERIFY(e->unavailableReason.contains(QStringLiteral("dependencies")));
    }

    // Spec: model not downloaded → grayed out with a model reason (checked
    // after GPU + deps pass).
    void grayedOutWhenModelMissing()
    {
        QTemporaryDir root;
        QVERIFY(makeEngine(root.path(), QStringLiteral("eng"), manifest(QStringLiteral("Eng"))));

        GpuStatus gpu;
        const auto engines = EngineRegistry::discover(
            {root.path()}, gpu, [](const EngineDescriptor &) { return true; },
            [](const EngineDescriptor &) { return false; });

        const EngineDescriptor *e = find(engines, QStringLiteral("eng"));
        QVERIFY(e);
        QVERIFY(!e->available);
        QVERIFY(e->unavailableReason.contains(QStringLiteral("model")));
    }

    // Spec: a broken manifest is listed (never silently dropped) with an
    // explanatory reason and manifestValid=false.
    void invalidManifestListedWithReason()
    {
        QTemporaryDir root;
        QVERIFY(makeEngine(root.path(), QStringLiteral("broken"), QByteArray("{ not valid json")));

        GpuStatus gpu;
        const auto engines = EngineRegistry::discover({root.path()}, gpu);

        const EngineDescriptor *e = find(engines, QStringLiteral("broken"));
        QVERIFY(e);
        QVERIFY(!e->manifestValid);
        QVERIFY(!e->available);
        QVERIFY(e->unavailableReason.contains(QStringLiteral("invalid manifest")));
    }

    // Spec: a folder with no manifest.json is not an engine — skipped entirely.
    void folderWithoutManifestSkipped()
    {
        QTemporaryDir root;
        QVERIFY(makeEngine(root.path(), QStringLiteral("notanengine"), QByteArray(), /*writeManifest=*/false));

        GpuStatus gpu;
        const auto engines = EngineRegistry::discover({root.path()}, gpu);
        QVERIFY(!find(engines, QStringLiteral("notanengine")));
    }

    // Spec: a later root (the vault) overrides an earlier root (bundled) by id.
    void vaultRootOverridesBundledById()
    {
        QTemporaryDir bundled;
        QTemporaryDir vault;
        QVERIFY(makeEngine(bundled.path(), QStringLiteral("parakeet"), manifest(QStringLiteral("Bundled"))));
        QVERIFY(makeEngine(vault.path(), QStringLiteral("parakeet"), manifest(QStringLiteral("Vault Override"))));

        GpuStatus gpu;
        const auto engines = EngineRegistry::discover(
            {bundled.path(), vault.path()}, gpu, [](const EngineDescriptor &) { return true; },
            [](const EngineDescriptor &) { return true; });

        const EngineDescriptor *e = find(engines, QStringLiteral("parakeet"));
        QVERIFY(e);
        QCOMPARE(e->manifest.name, QStringLiteral("Vault Override"));
    }
};

QTEST_GUILESS_MAIN(TstEngineRegistry)
#include "tst_engineregistry.moc"
