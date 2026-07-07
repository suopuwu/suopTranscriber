// Living spec: ModelManager — whisper.cpp ggml model catalog + on-demand fetch.
//
// Sources of truth:
//   DESIGN.md  "Model management" / vault layout — models are stored inside the
//              vault at <vault>/.transcriber/models/<ggml file name>.
//   CLAUDE.md  speaker detection constraint — tinydiarize only works with the
//              "-tdrz" fine-tune, a different model file from a different HF
//              repo, so speakerDetection=true overrides the requested modelId.
//
// Network policy: these tests NEVER exercise the download path. Every
// ensureModel call pre-creates the expected file so the manager returns early;
// the one behavior that genuinely needs the network is QSKIPped.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>

#include "ModelManager.h"

namespace {

// Pre-creates a fake ggml file at the documented vault-relative location and
// returns its full path — this is how every test avoids the download path.
QString plantFakeModel(const QString &vaultPath, const QString &fileName, const QByteArray &content)
{
    const QDir modelsDir(vaultPath + "/.transcriber/models");
    if (!QDir().mkpath(modelsDir.path()))
        return {};
    const QString path = modelsDir.filePath(fileName);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(content);
    return path;
}

QByteArray readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

} // namespace

class TstModelManager : public QObject
{
    Q_OBJECT

private slots:
    // --- Model catalog -----------------------------------------------------

    // The dropdown offers exactly the four documented English models, ordered
    // fastest/smallest to slowest/most-accurate, with the documented ggml file
    // names (which double as the on-disk names under .transcriber/models/).
    void availableModels_areExactlyTheFourDocumentedEnglishModels_inSpeedOrder()
    {
        const auto &models = ModelManager::availableModels();
        QCOMPARE(models.size(), size_t(4));

        QCOMPARE(models[0].id, QStringLiteral("tiny.en"));
        QCOMPARE(models[0].fileName, QStringLiteral("ggml-tiny.en.bin"));
        QCOMPARE(models[1].id, QStringLiteral("base.en"));
        QCOMPARE(models[1].fileName, QStringLiteral("ggml-base.en.bin"));
        QCOMPARE(models[2].id, QStringLiteral("small.en"));
        QCOMPARE(models[2].fileName, QStringLiteral("ggml-small.en.bin"));
        QCOMPARE(models[3].id, QStringLiteral("medium.en"));
        QCOMPARE(models[3].fileName, QStringLiteral("ggml-medium.en.bin"));

        // Every entry has a human-readable label for the model dropdown.
        for (const ModelManager::ModelInfo &info : models)
            QVERIFY2(!info.label.isEmpty(), qPrintable(info.id + " has no label"));
    }

    // The documented default model is base.en ("balanced"), and it is one of
    // the catalog entries — kDefaultModelId is also the fallback used when an
    // unknown modelId is requested (see fallback test below).
    void defaultModelId_isBaseEn_andPresentInCatalog()
    {
        QCOMPARE(QString(ModelManager::kDefaultModelId), QStringLiteral("base.en"));

        bool found = false;
        for (const ModelManager::ModelInfo &info : ModelManager::availableModels())
            found = found || info.id == ModelManager::kDefaultModelId;
        QVERIFY(found);
    }

    // --- Storage layout + no-redownload guarantee ---------------------------

    // DESIGN.md vault layout: models live at <vault>/.transcriber/models/.
    // If the ggml file is already there, ensureModel returns that exact path
    // WITHOUT downloading — the pre-created file's bytes are untouched and no
    // .part temp file appears (this test runs with no network).
    void ensureModel_returnsExistingFileWithoutDownloading()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QByteArray fakeBytes = "not a real ggml model — planted by the test";
        const QString planted = plantFakeModel(vault.path(), "ggml-base.en.bin", fakeBytes);
        QVERIFY(!planted.isEmpty());

        QString error;
        const QString returned = ModelManager::ensureModel(
            QUrl::fromLocalFile(vault.path()), "base.en", /*speakerDetection=*/false, &error);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(returned).absoluteFilePath(), QFileInfo(planted).absoluteFilePath());
        QCOMPARE(readAll(returned), fakeBytes);                    // bytes untouched
        QVERIFY(!QFileInfo::exists(planted + ".part"));            // no download started
    }

    // --- Speaker detection constraint (CLAUDE.md / DESIGN.md) ---------------

    // tinydiarize speaker detection only works with the -tdrz fine-tune, so
    // speakerDetection=true resolves to ggml-small.en-tdrz.bin regardless of
    // which modelId the caller asked for.
    void speakerDetection_forcesTdrzModel_regardlessOfRequestedModelId()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QByteArray fakeBytes = "fake tdrz model";
        const QString planted = plantFakeModel(vault.path(), ModelManager::kTdrzModelName, fakeBytes);
        QVERIFY(!planted.isEmpty());
        QCOMPARE(QString(ModelManager::kTdrzModelName), QStringLiteral("ggml-small.en-tdrz.bin"));

        // Every catalog id — and even the default — is overridden by tdrz.
        for (const ModelManager::ModelInfo &info : ModelManager::availableModels()) {
            QString error;
            const QString returned = ModelManager::ensureModel(
                QUrl::fromLocalFile(vault.path()), info.id, /*speakerDetection=*/true, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(QFileInfo(returned).absoluteFilePath(), QFileInfo(planted).absoluteFilePath());
        }
        QCOMPARE(readAll(planted), fakeBytes); // still no download, bytes untouched
    }

    // --- Unknown modelId fallback --------------------------------------------

    // DESIGN.md "Model management": an unknown modelId (e.g. a stale
    // AppSettings.lastModel from a removed model tier) falls back to the
    // default model (base.en) rather than failing outright — but the
    // fallback must never be silent. ensureModel signals it via usedFallback
    // so the caller (TranscriptionController) can record it in the
    // transcript's own frontmatter (modelRequested/modelFallback) instead of
    // letting the file read as if the requested model had been honored.
    void unknownModelId_fallsBackToDefaultModel_andSignalsIt()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString planted = plantFakeModel(vault.path(), "ggml-base.en.bin", "fake default model");
        QVERIFY(!planted.isEmpty());

        QString error;
        bool usedFallback = false;
        const QString returned = ModelManager::ensureModel(
            QUrl::fromLocalFile(vault.path()), "no-such-model", /*speakerDetection=*/false, &error, &usedFallback);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(returned).absoluteFilePath(), QFileInfo(planted).absoluteFilePath());
        QVERIFY2(usedFallback, "an unrecognized modelId must set usedFallback so the substitution is never silent");
    }

    // The flag must stay false whenever the requested modelId genuinely was
    // honored — a false positive here would make every ordinary
    // transcription look like a fallback happened.
    void recognizedModelId_neverSignalsFallback()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        for (const ModelManager::ModelInfo &info : ModelManager::availableModels()) {
            const QString planted = plantFakeModel(vault.path(), info.fileName, "fake model");
            QVERIFY(!planted.isEmpty());

            QString error;
            bool usedFallback = true; // deliberately wrong initial value
            const QString returned = ModelManager::ensureModel(
                QUrl::fromLocalFile(vault.path()), info.id, /*speakerDetection=*/false, &error, &usedFallback);

            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(QFileInfo(returned).absoluteFilePath(), QFileInfo(planted).absoluteFilePath());
            QVERIFY2(!usedFallback, qPrintable("recognized id " + info.id + " must not report a fallback"));
        }
    }

    // The tdrz override (an intentional, documented substitution) is
    // distinct from the unknown-id fallback and must not be conflated with
    // it — usedFallback stays false even though the requested modelId (a
    // real catalog id) was overridden by speakerDetection.
    void speakerDetectionOverride_isNotReportedAsFallback()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString planted = plantFakeModel(vault.path(), ModelManager::kTdrzModelName, "fake tdrz model");
        QVERIFY(!planted.isEmpty());

        QString error;
        bool usedFallback = true; // deliberately wrong initial value
        const QString returned = ModelManager::ensureModel(
            QUrl::fromLocalFile(vault.path()), "small.en", /*speakerDetection=*/true, &error, &usedFallback);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(returned).absoluteFilePath(), QFileInfo(planted).absoluteFilePath());
        QVERIFY2(!usedFallback, "the tdrz override is intentional, not the unknown-id fallback");
    }

    // --- Download path (documented, untestable offline) ----------------------

    // DESIGN.md: a missing model is downloaded via curl from
    // huggingface.co/ggerganov/whisper.cpp (or akashmjn/tinydiarize-whisper.cpp
    // for the tdrz model) to <name>.part, then renamed into place; failures
    // set errorOut and remove the .part file. This requires the network, and
    // the test suite must pass on a machine with none.
    void downloadingAMissingModel_requiresNetwork()
    {
        QSKIP("Download path deliberately untested: tests must pass with no network "
              "(would fetch ~150 MB from huggingface.co via curl).");
    }

    // --- AI auto-labeling model (DESIGN.md "Model management") ---------------

    // Same vault-scoped storage/no-redownload guarantee as ensureModel(), for
    // the one hardcoded label-model file — no per-size picker for this one.
    void ensureLabelModel_returnsExistingFileWithoutDownloading()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QByteArray fakeBytes = "not a real gguf model — planted by the test";
        const QString planted =
            plantFakeModel(vault.path(), QString(ModelManager::kLabelModelFileName), fakeBytes);
        QVERIFY(!planted.isEmpty());

        QString error;
        const QString returned = ModelManager::ensureLabelModel(QUrl::fromLocalFile(vault.path()), &error);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(returned).absoluteFilePath(), QFileInfo(planted).absoluteFilePath());
        QCOMPARE(readAll(returned), fakeBytes);         // bytes untouched
        QVERIFY(!QFileInfo::exists(planted + ".part")); // no download started
    }

    void ensureLabelModel_downloadRequiresNetwork()
    {
        QSKIP("Download path deliberately untested: tests must pass with no network "
              "(would fetch ~2.5 GB from huggingface.co via curl).");
    }
};

QTEST_GUILESS_MAIN(TstModelManager)
#include "tst_modelmanager.moc"
