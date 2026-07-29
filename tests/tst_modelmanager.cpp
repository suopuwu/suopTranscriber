// Living spec: ModelManager — whisper.cpp ggml model catalog + on-demand fetch,
// plus the speaker-diarization ONNX model pair.
//
// Sources of truth:
//   DESIGN.md  "Model management" — models live in ONE app-global store shared
//              across vaults (modelsStoreDir()), searched store-first then a
//              read-only bundled dir (bundledModelsDir()); downloads/imports/
//              deletes touch only the writable store.
//   DESIGN.md  "Speaker diarization" — pyannote-segmentation-3.0 +
//              WeSpeaker ResNet293-LM ONNX models, downloaded as a pair (see
//              ensureDiarizationModels).
//
// Isolation: SUOP_MODELS_DIR / SUOP_BUNDLED_MODELS_DIR are redirected to fresh
// temp dirs in init() so the tests never touch the real store (or the
// user's real vault or app config).
//
// Network policy: these tests NEVER exercise the download path. Every
// ensure* call pre-creates the expected file so the manager returns early;
// the one behavior that genuinely needs the network is QSKIPped.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUrl>

#include "ModelManager.h"

namespace {

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

    // Fresh writable-store + bundled dirs per test, redirected via env so the
    // real store is never touched.
    QScopedPointer<QTemporaryDir> m_store;
    QScopedPointer<QTemporaryDir> m_bundled;

    // Plants a fake model file into the writable store (the location every
    // presence check / ensure* resolves first) — how every test avoids the
    // download path. Returns its full path.
    QString plantFakeModel(const QString &fileName, const QByteArray &content)
    {
        const QDir modelsDir(ModelManager::modelsStoreDir());
        if (!QDir().mkpath(modelsDir.path()))
            return {};
        const QString path = modelsDir.filePath(fileName);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(content);
        return path;
    }

    // Plants a fake model file into the read-only BUNDLED dir (searched after
    // the store) — used to exercise the bundled-fallback resolution.
    QString plantBundledModel(const QString &fileName, const QByteArray &content)
    {
        const QDir modelsDir(ModelManager::bundledModelsDir());
        if (!QDir().mkpath(modelsDir.path()))
            return {};
        const QString path = modelsDir.filePath(fileName);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write(content);
        return path;
    }

private slots:
    void init()
    {
        m_store.reset(new QTemporaryDir);
        m_bundled.reset(new QTemporaryDir);
        QVERIFY(m_store->isValid());
        QVERIFY(m_bundled->isValid());
        qputenv("SUOP_MODELS_DIR", m_store->path().toLocal8Bit());
        qputenv("SUOP_BUNDLED_MODELS_DIR", m_bundled->path().toLocal8Bit());
    }

    void cleanup()
    {
        qunsetenv("SUOP_MODELS_DIR");
        qunsetenv("SUOP_BUNDLED_MODELS_DIR");
        m_store.reset();
        m_bundled.reset();
    }

    // --- Model catalog -----------------------------------------------------

    // The dropdown offers the four documented English whisper tiers plus the
    // multilingual large-v3 tier (item O31), ordered fastest/smallest to
    // slowest/most-accurate with the documented ggml file names (which double
    // as the on-disk names in the model store), then the in-process
    // Parakeet model (sherpa-onnx, see ParakeetEngine) as a final entry.
    void availableModels_areTheWhisperTiersPlusParakeet_inSpeedOrder()
    {
        const auto &models = ModelManager::availableModels();
        QCOMPARE(models.size(), size_t(6));

        QCOMPARE(models[0].id, QStringLiteral("tiny.en"));
        QCOMPARE(models[0].fileName, QStringLiteral("ggml-tiny.en.bin"));
        QCOMPARE(models[1].id, QStringLiteral("base.en"));
        QCOMPARE(models[1].fileName, QStringLiteral("ggml-base.en.bin"));
        QCOMPARE(models[2].id, QStringLiteral("small.en"));
        QCOMPARE(models[2].fileName, QStringLiteral("ggml-small.en.bin"));
        QCOMPARE(models[3].id, QStringLiteral("medium.en"));
        QCOMPARE(models[3].fileName, QStringLiteral("ggml-medium.en.bin"));
        QCOMPARE(models[4].id, QStringLiteral("large-v3"));
        QCOMPARE(models[4].fileName, QStringLiteral("ggml-large-v3.bin"));
        // Parakeet: a bundle DIRECTORY name, not a single ggml file.
        QCOMPARE(models[5].id, QString(ModelManager::kParakeetModelId));
        QVERIFY(ModelManager::isParakeetModelId(models[5].id));

        // Every entry has a human-readable label for the model dropdown.
        for (const ModelManager::ModelInfo &info : models)
            QVERIFY2(!info.label.isEmpty(), qPrintable(info.id + " has no label"));

        // No English-only "large" ggml model exists upstream (see
        // ModelManager.cpp) — large-v3 must not claim "(English)" the way
        // the other four tiers do.
        QVERIFY(!models[4].label.contains(QStringLiteral("(English)")));
    }

    // Parakeet is a multi-file ONNX bundle, not a single ggml file: its four
    // files resolve under the store's <bundle>/ subdir, isModelDownloaded is
    // true only when ALL are present, and isParakeetModelId distinguishes it
    // from the whisper tiers. No network touched (nothing planted ⇒ absent).
    void parakeetModel_isABundleGatedOnAllFourFiles()
    {
        QVERIFY(ModelManager::isParakeetModelId(QString(ModelManager::kParakeetModelId)));
        QVERIFY(!ModelManager::isParakeetModelId(QStringLiteral("base.en")));

        const ModelManager::ParakeetModelPaths paths = ModelManager::parakeetModelPaths();
        for (const QString &p : {paths.encoder, paths.decoder, paths.joiner, paths.tokens})
            QVERIFY2(p.contains(QStringLiteral("parakeet-tdt-0.6b-v2")), qPrintable(p));

        // Nothing on disk ⇒ not complete, not downloaded.
        QVERIFY(!paths.isComplete());
        QVERIFY(!ModelManager::isModelDownloaded(QString(ModelManager::kParakeetModelId)));

        // Plant three of the four files ⇒ still not complete (never a
        // half-extracted "downloaded").
        const QDir bundle(ModelManager::modelsStoreDir() + "/parakeet-tdt-0.6b-v2");
        QVERIFY(QDir().mkpath(bundle.path()));
        for (const QString &f : {paths.encoder, paths.decoder, paths.joiner}) {
            QFile file(f);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        QVERIFY(!ModelManager::parakeetModelPaths().isComplete());
        QVERIFY(!ModelManager::isModelDownloaded(QString(ModelManager::kParakeetModelId)));

        // The fourth (tokens) present ⇒ complete + downloaded.
        QFile tokens(paths.tokens);
        QVERIFY(tokens.open(QIODevice::WriteOnly));
        tokens.write("x");
        QVERIFY(ModelManager::parakeetModelPaths().isComplete());
        QVERIFY(ModelManager::isModelDownloaded(QString(ModelManager::kParakeetModelId)));
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

    // DESIGN.md "Model management": models live in the app-global store.
    // If the ggml file is already there, ensureModel returns that exact path
    // WITHOUT downloading — the pre-created file's bytes are untouched and no
    // .part temp file appears (this test runs with no network).
    void ensureModel_returnsExistingFileWithoutDownloading()
    {
        const QByteArray fakeBytes = "not a real ggml model — planted by the test";
        const QString planted = plantFakeModel("ggml-base.en.bin", fakeBytes);
        QVERIFY(!planted.isEmpty());

        QString error;
        const QString returned = ModelManager::ensureModel("base.en", &error);

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
        const QString planted = plantFakeModel("ggml-base.en.bin", "fake default model");
        QVERIFY(!planted.isEmpty());

        QString error;
        bool usedFallback = false;
        const QString returned = ModelManager::ensureModel("no-such-model", &error, &usedFallback);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(returned).absoluteFilePath(), QFileInfo(planted).absoluteFilePath());
        QVERIFY2(usedFallback, "an unrecognized modelId must set usedFallback so the substitution is never silent");
    }

    // The flag must stay false whenever the requested modelId genuinely was
    // honored — a false positive here would make every ordinary
    // transcription look like a fallback happened.
    void recognizedModelId_neverSignalsFallback()
    {
        for (const ModelManager::ModelInfo &info : ModelManager::availableModels()) {
            // Parakeet's "fileName" is a bundle DIRECTORY, not a single ggml
            // file — its download path is separate (multi-file extract) and it
            // never signals a fallback, so the plant-a-fake-file trick here
            // doesn't apply. Covered by parakeetModel_isABundleGatedOnAllFourFiles.
            if (ModelManager::isParakeetModelId(info.id))
                continue;
            const QString planted = plantFakeModel(info.fileName, "fake model");
            QVERIFY(!planted.isEmpty());

            QString error;
            bool usedFallback = true; // deliberately wrong initial value
            const QString returned = ModelManager::ensureModel(info.id, &error, &usedFallback);

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
        const QByteArray segBytes = "not a real segmentation onnx model — planted by the test";
        const QByteArray embBytes = "not a real embedding onnx model — planted by the test";
        const QString plantedSeg =
            plantFakeModel(QString(ModelManager::kDiarizationSegmentationFileName), segBytes);
        const QString plantedEmb =
            plantFakeModel(QString(ModelManager::kDiarizationEmbeddingFileName), embBytes);
        QVERIFY(!plantedSeg.isEmpty());
        QVERIFY(!plantedEmb.isEmpty());

        QString error;
        QString segOut, embOut;
        // Default segmentation + embedding ids resolve to the planted default
        // pair (kDiarizationSegmentationFileName / kDiarizationEmbeddingFileName).
        const bool ok = ModelManager::ensureDiarizationModels(
            QString(ModelManager::kDefaultSegmentationModelId), QString(ModelManager::kDefaultEmbeddingModelId),
            &segOut, &embOut, &error);

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

    // Same app-global storage/no-redownload guarantee as ensureModel(), for
    // the one hardcoded label-model file — no per-size picker for this one.
    void ensureLabelModel_returnsExistingFileWithoutDownloading()
    {
        const QByteArray fakeBytes = "not a real gguf model — planted by the test";
        const QString planted = plantFakeModel(QString(ModelManager::kLabelModelFileName), fakeBytes);
        QVERIFY(!planted.isEmpty());

        QString error;
        const QString returned = ModelManager::ensureLabelModel(&error);

        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(returned).absoluteFilePath(), QFileInfo(planted).absoluteFilePath());
        QCOMPARE(readAll(returned), fakeBytes);         // bytes untouched
        QVERIFY(!QFileInfo::exists(planted + ".part")); // no download started
    }

    void ensureLabelModel_downloadRequiresNetwork()
    {
        QSKIP("Download path deliberately untested: tests must pass with no network "
              "(would fetch ~2.5 GB from huggingface.co via curl). The download-integrity "
              "gate (expectedSha256()/verifyFileSha256(), applied to the installed file "
              "before/after each download route) is covered offline by the "
              "'Download integrity (pinned SHA-256)' slots below instead.");
    }

    // --- Download integrity (pinned SHA-256) ---------------------------------

    // Digest of the exact bytes plantFakeModel() writes verbatim (no trailing
    // newline) — computed with `sha256sum` while scouting. Recompute if the
    // payload string below changes by so much as a byte.
    void fileSha256_matchesAKnownDigest()
    {
        const QString planted =
            plantFakeModel("whatever.bin", QByteArrayLiteral("not a real model — planted by the test"));
        QVERIFY(!planted.isEmpty());
        QCOMPARE(ModelManager::fileSha256(planted),
                 QStringLiteral("919d91020d15a3a490b5f1a2e74071ec285b49ff26a1fa30365132df1dabe634"));

        QVERIFY(ModelManager::fileSha256(QStringLiteral("/no/such/path")).isEmpty());
    }

    void verifyFileSha256_acceptsAMatch_rejectsAMismatch()
    {
        const QString planted = plantFakeModel("whatever.bin", "some model bytes");
        QVERIFY(!planted.isEmpty());
        const QString digest = ModelManager::fileSha256(planted);
        QVERIFY(!digest.isEmpty());

        QString error;
        QVERIFY2(ModelManager::verifyFileSha256(planted, digest, &error), qPrintable(error));
        QVERIFY(error.isEmpty());

        const QString zeros = QString(64, QLatin1Char('0'));
        error.clear();
        QVERIFY(!ModelManager::verifyFileSha256(planted, zeros, &error));
        // Actionable message: names both the expected and the actual digest.
        QVERIFY2(error.contains(zeros), qPrintable(error));
        QVERIFY2(error.contains(digest), qPrintable(error));

        QVERIFY(!ModelManager::verifyFileSha256(QStringLiteral("/no/such/path"), digest));
    }

    // An empty pin is the documented escape hatch for an artifact nobody has
    // captured a digest for yet — without this, an unpinned artifact becomes
    // undownloadable rather than merely unverified.
    void verifyFileSha256_treatsAnEmptyPinAsUnpinned()
    {
        const QString planted = plantFakeModel("whatever.bin", "some model bytes");
        QVERIFY(!planted.isEmpty());
        QVERIFY(ModelManager::verifyFileSha256(planted, QString()));
    }

    // Living-documentation slot: every artifact the catalogs can actually
    // download must either carry a pinned SHA-256 or be named on the declared
    // not-yet-pinned exemption list — never silently fall through unpinned AND
    // unlisted. Also asserts the reverse: a stale exemption (a tier that was
    // removed from the catalog) can't linger unnoticed.
    void everyCatalogArtifact_isPinnedOrDeclaredUnpinned()
    {
        // Every catalog artifact is pinned as of this writing (see
        // DEPENDENCIES.md); kept as a real (empty) exemption list rather than
        // deleting the mechanism, since a future tier can land without a local
        // copy or a huggingface LFS entry to hash.
        const QStringList kNotYetPinned = {};

        QStringList names;
        for (const ModelManager::ModelInfo &info : ModelManager::availableModels()) {
            if (ModelManager::isParakeetModelId(info.id)) {
                // fileName is a bundle DIRECTORY, not a store-relative file — the
                // pin table keys the four extracted members individually instead.
                names << QStringLiteral("parakeet-tdt-0.6b-v2/encoder.int8.onnx")
                      << QStringLiteral("parakeet-tdt-0.6b-v2/decoder.int8.onnx")
                      << QStringLiteral("parakeet-tdt-0.6b-v2/joiner.int8.onnx")
                      << QStringLiteral("parakeet-tdt-0.6b-v2/tokens.txt");
                continue;
            }
            names << info.fileName;
        }
        for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableSegmentationModels())
            names << info.fileName;
        for (const ModelManager::DiarizationModelInfo &info : ModelManager::availableEmbeddingModels())
            names << info.fileName;
        names << QString(ModelManager::kLabelModelFileName);
        names << QString(ModelManager::kVadModelFileName);

        static const QRegularExpression kSha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
        for (const QString &name : std::as_const(names)) {
            if (kNotYetPinned.contains(name))
                continue;
            const QString pin = ModelManager::expectedSha256(name);
            QVERIFY2(!pin.isEmpty(), qPrintable(name + " has no pinned SHA-256 and is not on kNotYetPinned"));
            QVERIFY2(kSha256Pattern.match(pin).hasMatch(),
                     qPrintable(name + " pin is not 64 lowercase hex chars: " + pin));
        }

        // A stale exemption (the artifact it names was removed/pinned since)
        // must not linger unnoticed either.
        for (const QString &exempt : kNotYetPinned)
            QVERIFY2(names.contains(exempt), qPrintable(exempt + " is on kNotYetPinned but not in any catalog"));
    }

    // --- Bundled-dir fallback (Settings → Models: search-path bundling) ------

    // A model absent from the writable store but present in the read-only
    // bundled dir resolves as downloaded, and ensureModel() returns the bundled
    // path directly (no download). This is the durability escape hatch: a
    // distributor can ship models beside the binary.
    void bundledModel_resolvesAndIsUsedWithoutDownload()
    {
        const QByteArray bytes = "bundled fake model";
        const QString bundled = plantBundledModel("ggml-base.en.bin", bytes);
        QVERIFY(!bundled.isEmpty());

        QVERIFY(ModelManager::isModelDownloaded("base.en"));
        QVERIFY(ModelManager::isModelBundledOnly("base.en"));

        QString error;
        const QString returned = ModelManager::ensureModel("base.en", &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(returned).absoluteFilePath(), QFileInfo(bundled).absoluteFilePath());
        QVERIFY(!QFileInfo::exists(bundled + ".part")); // never tried to download
    }

    // The store shadows the bundled dir: a file in BOTH resolves to the store
    // copy, and it is no longer "bundled only".
    void storeShadowsBundled()
    {
        plantBundledModel("ggml-base.en.bin", "bundled");
        const QString store = plantFakeModel("ggml-base.en.bin", "store");
        QCOMPARE(QFileInfo(ModelManager::ensureModel("base.en")).absoluteFilePath(),
                 QFileInfo(store).absoluteFilePath());
        QVERIFY(!ModelManager::isModelBundledOnly("base.en"));
    }

    // --- Delete (Settings → Models) ------------------------------------------

    // deleteModel removes a store file and flips isModelDownloaded to false.
    void deleteModel_removesFromStore()
    {
        plantFakeModel("ggml-base.en.bin", "fake");
        QVERIFY(ModelManager::isModelDownloaded("base.en"));

        QString error;
        QVERIFY2(ModelManager::deleteModel("base.en", &error), qPrintable(error));
        QVERIFY(!ModelManager::isModelDownloaded("base.en"));
    }

    // A bundled-only model can't be deleted (the bundled dir is read-only /
    // shipped) — deleteModel reports failure and leaves the file in place.
    void deleteModel_bundledOnly_refuses()
    {
        const QString bundled = plantBundledModel("ggml-base.en.bin", "bundled");
        QVERIFY(ModelManager::isModelBundledOnly("base.en"));

        QString error;
        QVERIFY(!ModelManager::deleteModel("base.en", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(QFileInfo::exists(bundled)); // untouched
    }

    // Deleting the multi-file parakeet bundle removes the whole store subdir.
    void deleteModel_parakeetBundle_removesDir()
    {
        const QDir bundle(ModelManager::modelsStoreDir() + "/parakeet-tdt-0.6b-v2");
        QVERIFY(QDir().mkpath(bundle.path()));
        for (const QString &f : {QStringLiteral("encoder.int8.onnx"), QStringLiteral("decoder.int8.onnx"),
                                 QStringLiteral("joiner.int8.onnx"), QStringLiteral("tokens.txt")}) {
            QFile file(bundle.filePath(f));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        QVERIFY(ModelManager::isModelDownloaded(QString(ModelManager::kParakeetModelId)));
        QString error;
        QVERIFY2(ModelManager::deleteModel(QString(ModelManager::kParakeetModelId), &error), qPrintable(error));
        QVERIFY(!QDir(bundle.path()).exists());
        QVERIFY(!ModelManager::isModelDownloaded(QString(ModelManager::kParakeetModelId)));
    }

    // --- Import (Settings → Models) ------------------------------------------

    // A single-file model imports from a FILE, copied into the store under its
    // canonical name so it resolves as downloaded with no network.
    void importModel_singleFile_copiesIntoStore()
    {
        // Source file the user "brings" — outside the store, any name.
        QTemporaryDir src;
        QVERIFY(src.isValid());
        const QString srcPath = src.filePath("whatever-the-user-named-it.bin");
        {
            QFile f(srcPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("imported bytes");
        }
        QVERIFY(!ModelManager::isModelDownloaded("small.en"));

        QString error;
        QVERIFY2(ModelManager::importModel("small.en", srcPath, &error), qPrintable(error));
        QVERIFY(ModelManager::isModelDownloaded("small.en"));
        QCOMPARE(readAll(ModelManager::ensureModel("small.en")), QByteArray("imported bytes"));
    }

    // A single-file import rejects a directory source (wrong shape).
    void importModel_singleFile_rejectsFolder()
    {
        QTemporaryDir src;
        QString error;
        QVERIFY(!ModelManager::importModel("small.en", src.path(), &error));
        QVERIFY(!error.isEmpty());
    }

    // QFile::copy() returns TRUE on a short read (see ModelManager.cpp's
    // copyVerified() comment) — a truncated/interrupted source must not land in
    // the store wearing a clean "Done" result. A symlink to /proc/self/status is
    // the deterministic way to force that: it stats at size 0 but reads back
    // ~1.5 KB, so the copy "succeeds" with a landed size that disagrees with the
    // source's stat, exactly the mismatch copyVerified() rejects. Same recipe as
    // tst_usbgrabjob.cpp's copyThatDoesNotLandWhole_* tests.
    void importModel_truncatedCopy_failsAndLeavesNothingInTheStore()
    {
#ifndef Q_OS_LINUX
        QSKIP("truncated-copy recipe needs /proc/self/status (Linux-only)");
#endif
        QTemporaryDir src;
        QVERIFY(src.isValid());
        const QString srcPath = src.filePath("whatever-the-user-named-it.bin");
        QVERIFY(QFile::link(QStringLiteral("/proc/self/status"), srcPath));
        // The premise of the trick, asserted rather than assumed.
        QCOMPARE(QFileInfo(srcPath).size(), 0);

        QString error;
        QVERIFY(!ModelManager::importModel("small.en", srcPath, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(ModelManager::modelsStoreDir() + "/ggml-small.en.bin"));
        QVERIFY(!ModelManager::isModelDownloaded("small.en"));
    }

    // The parakeet bundle imports from a FOLDER holding its four files.
    void importModel_parakeetBundle_fromFolder()
    {
        QTemporaryDir src;
        QVERIFY(src.isValid());
        for (const QString &f : {QStringLiteral("encoder.int8.onnx"), QStringLiteral("decoder.int8.onnx"),
                                 QStringLiteral("joiner.int8.onnx"), QStringLiteral("tokens.txt")}) {
            QFile file(src.filePath(f));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        QString error;
        QVERIFY2(ModelManager::importModel(QString(ModelManager::kParakeetModelId), src.path(), &error),
                 qPrintable(error));
        QVERIFY(ModelManager::isModelDownloaded(QString(ModelManager::kParakeetModelId)));
    }

    // A multi-file import fails if the folder is missing an expected file.
    void importModel_parakeetBundle_missingFileFails()
    {
        QTemporaryDir src;
        QVERIFY(src.isValid());
        // only 3 of 4
        for (const QString &f : {QStringLiteral("encoder.int8.onnx"), QStringLiteral("decoder.int8.onnx"),
                                 QStringLiteral("joiner.int8.onnx")}) {
            QFile file(src.filePath(f));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        QString error;
        QVERIFY(!ModelManager::importModel(QString(ModelManager::kParakeetModelId), src.path(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!ModelManager::isModelDownloaded(QString(ModelManager::kParakeetModelId)));
    }

    // --- Export (Settings → Models) ------------------------------------------

    // Exporting a single-file model copies it into the chosen dir under its
    // canonical name, re-importable as-is.
    void exportModel_singleFile_copiesOut()
    {
        plantFakeModel("ggml-base.en.bin", "export me");
        QTemporaryDir dest;
        QVERIFY(dest.isValid());

        QString error;
        QVERIFY2(ModelManager::exportModel("base.en", dest.path(), &error), qPrintable(error));
        const QString exported = dest.filePath("ggml-base.en.bin");
        QVERIFY(QFileInfo::exists(exported));
        QCOMPARE(readAll(exported), QByteArray("export me"));
    }

    // Mirror of importModel_truncatedCopy_failsAndLeavesNothingInTheStore on the
    // export side: the STORE file is the lying symlink this time, so exporting a
    // model whose store copy is quietly truncated/corrupt must not hand the user
    // a broken file while reporting success.
    void exportModel_truncatedCopy_failsAndLeavesNothingAtTheDestination()
    {
#ifndef Q_OS_LINUX
        QSKIP("truncated-copy recipe needs /proc/self/status (Linux-only)");
#endif
        const QDir modelsDir(ModelManager::modelsStoreDir());
        QVERIFY(QDir().mkpath(modelsDir.path()));
        const QString storePath = modelsDir.filePath("ggml-base.en.bin");
        QVERIFY(QFile::link(QStringLiteral("/proc/self/status"), storePath));
        QCOMPARE(QFileInfo(storePath).size(), 0);

        QTemporaryDir dest;
        QVERIFY(dest.isValid());
        QString error;
        QVERIFY(!ModelManager::exportModel("base.en", dest.path(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFileInfo::exists(dest.filePath("ggml-base.en.bin")));
    }

    // Export round-trips: a parakeet bundle exported to a folder re-imports.
    void exportModel_parakeetBundle_roundTrips()
    {
        const QDir bundle(ModelManager::modelsStoreDir() + "/parakeet-tdt-0.6b-v2");
        QVERIFY(QDir().mkpath(bundle.path()));
        for (const QString &f : {QStringLiteral("encoder.int8.onnx"), QStringLiteral("decoder.int8.onnx"),
                                 QStringLiteral("joiner.int8.onnx"), QStringLiteral("tokens.txt")}) {
            QFile file(bundle.filePath(f));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
        }
        QTemporaryDir dest;
        QVERIFY(dest.isValid());
        QString error;
        QVERIFY2(ModelManager::exportModel(QString(ModelManager::kParakeetModelId), dest.path(), &error),
                 qPrintable(error));
        // Exported as a <bundle>/ subdir with all four files — the exact shape
        // importModel expects back.
        QVERIFY2(ModelManager::importModel(QString(ModelManager::kParakeetModelId),
                                           dest.filePath("parakeet-tdt-0.6b-v2"), &error),
                 qPrintable(error));
    }

    // Export refuses a model that isn't downloaded.
    void exportModel_notDownloaded_fails()
    {
        QTemporaryDir dest;
        QString error;
        QVERIFY(!ModelManager::exportModel("base.en", dest.path(), &error));
        QVERIFY(!error.isEmpty());
    }

    // --- Size ----------------------------------------------------------------

    void modelSizeOnDisk_sumsPresentFiles()
    {
        QCOMPARE(ModelManager::modelSizeOnDisk("base.en"), qint64(0)); // absent
        const QByteArray bytes(1234, 'x');
        plantFakeModel("ggml-base.en.bin", bytes);
        QCOMPARE(ModelManager::modelSizeOnDisk("base.en"), qint64(1234));
    }

    // Each selectable diarization sub-model (segmentation + embedding) is now
    // its own single-file manageable id, routing through the same id-keyed ops.
    void diarizationSubModels_areManaged()
    {
        const QString segId = QString(ModelManager::kDefaultSegmentationModelId);
        const QString embedId = QString(ModelManager::kDefaultEmbeddingModelId);
        plantFakeModel(QString(ModelManager::kDiarizationSegmentationFileName), QByteArray(10, 'a'));
        plantFakeModel(QString(ModelManager::kDiarizationEmbeddingFileName), QByteArray(20, 'b'));
        QCOMPARE(ModelManager::modelSizeOnDisk(segId), qint64(10));
        QCOMPARE(ModelManager::modelSizeOnDisk(embedId), qint64(20));
        QVERIFY(ModelManager::isSegmentationModelDownloaded(segId));
        QVERIFY(ModelManager::isEmbeddingModelDownloaded(embedId));
        QVERIFY(ModelManager::areDiarizationModelsDownloaded(segId, embedId));

        QString error;
        QVERIFY2(ModelManager::deleteModel(segId, &error), qPrintable(error));
        QVERIFY(!ModelManager::isSegmentationModelDownloaded(segId));
        QVERIFY(!ModelManager::areDiarizationModelsDownloaded(segId, embedId));
        QVERIFY2(ModelManager::deleteModel(embedId, &error), qPrintable(error));
        QVERIFY(!ModelManager::isEmbeddingModelDownloaded(embedId));
    }

    // An unrecognized segmentation/embedding id falls back to the default entry
    // (never silently misrepresents which model ran — see ensureModel()).
    void diarizationSubModels_unknownIdFallsBackToDefault()
    {
        bool usedFallback = false;
        const ModelManager::DiarizationModelInfo &seg =
            ModelManager::segmentationModelInfo(QStringLiteral("nope-not-a-model"), &usedFallback);
        QVERIFY(usedFallback);
        QCOMPARE(seg.id, QString(ModelManager::kDefaultSegmentationModelId));

        usedFallback = false;
        const ModelManager::DiarizationModelInfo &embed =
            ModelManager::embeddingModelInfo(QString(ModelManager::kDefaultEmbeddingModelId), &usedFallback);
        QVERIFY(!usedFallback);
        QCOMPARE(embed.id, QString(ModelManager::kDefaultEmbeddingModelId));
    }
};

QTEST_GUILESS_MAIN(TstModelManager)
#include "tst_modelmanager.moc"
