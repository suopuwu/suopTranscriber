// Living spec: ModelManager — whisper.cpp ggml model catalog + on-demand fetch,
// plus the speaker-diarization ONNX model pair.
//
// Sources of truth:
//   DESIGN.md  "Model management" / vault layout — models are stored inside the
//              vault at <vault>/.transcriber/models/<file name>.
//   DESIGN.md  "Speaker diarization" — pyannote-segmentation-3.0 +
//              WeSpeaker ResNet293-LM ONNX models, downloaded as a pair (see
//              ensureDiarizationModels).
//
// Network policy: these tests NEVER exercise the download path. Every
// ensure* call pre-creates the expected file so the manager returns early;
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
        const QString returned =
            ModelManager::ensureModel(QUrl::fromLocalFile(vault.path()), "base.en", &error);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(returned).absoluteFilePath(), QFileInfo(planted).absoluteFilePath());
        QCOMPARE(readAll(returned), fakeBytes);                    // bytes untouched
        QVERIFY(!QFileInfo::exists(planted + ".part"));            // no download started
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
            QUrl::fromLocalFile(vault.path()), "no-such-model", &error, &usedFallback);

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
                QUrl::fromLocalFile(vault.path()), info.id, &error, &usedFallback);

            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(QFileInfo(returned).absoluteFilePath(), QFileInfo(planted).absoluteFilePath());
            QVERIFY2(!usedFallback, qPrintable("recognized id " + info.id + " must not report a fallback"));
        }
    }

    // --- Download path (documented, untestable offline) ----------------------

    // DESIGN.md: a missing model is downloaded via curl from
    // huggingface.co/ggerganov/whisper.cpp to <name>.part, then renamed into
    // place; failures set errorOut and remove the .part file. This requires
    // the network, and the test suite must pass on a machine with none.
    void downloadingAMissingModel_requiresNetwork()
    {
        QSKIP("Download path deliberately untested: tests must pass with no network "
              "(would fetch ~150 MB from huggingface.co via curl).");
    }

    // --- Speaker diarization models (DESIGN.md "Speaker diarization") -------

    // Both the segmentation and embedding ONNX files must already be present
    // for ensureDiarizationModels to return early without downloading — same
    // no-redownload guarantee as ensureModel()/ensureLabelModel() above.
    void ensureDiarizationModels_returnsExistingFilesWithoutDownloading()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QByteArray segBytes = "not a real segmentation onnx model — planted by the test";
        const QByteArray embBytes = "not a real embedding onnx model — planted by the test";
        const QString plantedSeg =
            plantFakeModel(vault.path(), QString(ModelManager::kDiarizationSegmentationFileName), segBytes);
        const QString plantedEmb =
            plantFakeModel(vault.path(), QString(ModelManager::kDiarizationEmbeddingFileName), embBytes);
        QVERIFY(!plantedSeg.isEmpty());
        QVERIFY(!plantedEmb.isEmpty());

        QString error;
        QString segOut, embOut;
        const bool ok = ModelManager::ensureDiarizationModels(QUrl::fromLocalFile(vault.path()), &segOut, &embOut,
                                                               &error);

        QVERIFY2(ok, qPrintable(error));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(segOut).absoluteFilePath(), QFileInfo(plantedSeg).absoluteFilePath());
        QCOMPARE(QFileInfo(embOut).absoluteFilePath(), QFileInfo(plantedEmb).absoluteFilePath());
        QCOMPARE(readAll(segOut), segBytes); // bytes untouched
        QCOMPARE(readAll(embOut), embBytes); // bytes untouched
        QVERIFY(!QFileInfo::exists(plantedSeg + ".part"));
        QVERIFY(!QFileInfo::exists(plantedEmb + ".part"));
    }

    // Only the segmentation file present (embedding still missing) must NOT
    // report success without attempting the embedding download — callers
    // (TranscriptionController) rely on ensureDiarizationModels never
    // reporting a "half available" state as success.
    void ensureDiarizationModels_downloadRequiresNetwork()
    {
        QSKIP("Download path deliberately untested: tests must pass with no network "
              "(would fetch ~35 MB total from huggingface.co via curl).");
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
