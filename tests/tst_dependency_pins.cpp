#include <QtTest>

#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

// Source-scan guard for DEPENDENCIES.md: every pin identifier/value it names
// must still exist at its declared site, so a rename/move/re-derive can leave
// the runbook silently pointing at nothing. See DEPENDENCIES.md itself for
// the bump procedure this file only keeps honest, not for what to do.
class TstDependencyPins : public QObject
{
    Q_OBJECT

    static QString readAll(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        return QString::fromUtf8(f.readAll());
    }

    // `src` with its // line comments removed, so a check for "does the code
    // still have X" can't be satisfied by a comment merely mentioning X (or
    // defeated by one). Copied from tst_styleguide.cpp's helper of the same
    // name/contract (this repo's convention is a per-file copy, not a shared
    // header) — see that file's doc comment for the "bites both ways" case
    // history. Skips '//' inside double-quoted strings so URL literals like
    // "https://" survive intact.
    static QString stripComments(const QString &src)
    {
        QString out;
        out.reserve(src.size());
        bool inString = false;
        for (int i = 0; i < src.size(); ++i) {
            const QChar c = src.at(i);
            if (inString) {
                if (c == QLatin1Char('\\') && i + 1 < src.size()) {
                    out += c;
                    out += src.at(++i); // escaped char, never ends the string
                    continue;
                }
                if (c == QLatin1Char('"'))
                    inString = false;
                out += c;
                continue;
            }
            if (c == QLatin1Char('"')) {
                inString = true;
                out += c;
                continue;
            }
            if (c == QLatin1Char('/') && i + 1 < src.size() && src.at(i + 1) == QLatin1Char('/')) {
                while (i < src.size() && src.at(i) != QLatin1Char('\n'))
                    ++i;
                if (i < src.size())
                    out += QLatin1Char('\n'); // keep line structure intact
                continue;
            }
            out += c;
        }
        return out;
    }

private slots:
    void dependenciesDocExists()
    {
        const QString doc = readAll(QStringLiteral(SUOP_REPO_DIR "/DEPENDENCIES.md"));
        QVERIFY2(!doc.isEmpty(), "DEPENDENCIES.md must exist at the repo root");
    }

    // The two model-file constants DEPENDENCIES.md names, plus the exact
    // string-literal file names it quotes for them, must still be exactly
    // that in ModelManager.h.
    void modelManagerConstants_matchHeader()
    {
        const QString doc = readAll(QStringLiteral(SUOP_REPO_DIR "/DEPENDENCIES.md"));
        QVERIFY2(!doc.isEmpty(), "DEPENDENCIES.md must exist");
        const QString header =
            stripComments(readAll(QStringLiteral(SUOP_SRC_DIR "/core/ModelManager.h")));
        QVERIFY2(!header.isEmpty(), "src/core/ModelManager.h must exist");

        QVERIFY2(doc.contains(QStringLiteral("kVadModelFileName")),
                 "DEPENDENCIES.md must name kVadModelFileName");
        QVERIFY2(header.contains(QStringLiteral("kVadModelFileName")),
                 "kVadModelFileName must still exist in ModelManager.h");
        QVERIFY2(doc.contains(QStringLiteral("\"ggml-silero-v6.2.0.bin\"")),
                 "DEPENDENCIES.md must quote the exact VAD file name");
        QVERIFY2(header.contains(QStringLiteral("\"ggml-silero-v6.2.0.bin\"")),
                 "ModelManager.h's kVadModelFileName literal must match DEPENDENCIES.md");

        QVERIFY2(doc.contains(QStringLiteral("kLabelModelFileName")),
                 "DEPENDENCIES.md must name kLabelModelFileName");
        QVERIFY2(header.contains(QStringLiteral("kLabelModelFileName")),
                 "kLabelModelFileName must still exist in ModelManager.h");
        QVERIFY2(doc.contains(QStringLiteral("\"Qwen3-4B-Instruct-2507-Q4_K_M.gguf\"")),
                 "DEPENDENCIES.md must quote the exact label-model file name");
        QVERIFY2(header.contains(QStringLiteral("\"Qwen3-4B-Instruct-2507-Q4_K_M.gguf\"")),
                 "ModelManager.h's kLabelModelFileName literal must match DEPENDENCIES.md");
    }

    // The Parakeet archive URL DEPENDENCIES.md describes (by its identifying
    // tail) must still appear in ModelManager.cpp's kParakeetArchiveUrl.
    void parakeetArchiveUrl_matchesModelManagerCpp()
    {
        const QString doc = readAll(QStringLiteral(SUOP_REPO_DIR "/DEPENDENCIES.md"));
        QVERIFY2(!doc.isEmpty(), "DEPENDENCIES.md must exist");
        const QString cpp =
            stripComments(readAll(QStringLiteral(SUOP_SRC_DIR "/core/ModelManager.cpp")));
        QVERIFY2(!cpp.isEmpty(), "src/core/ModelManager.cpp must exist");

        QVERIFY2(doc.contains(QStringLiteral("kParakeetArchiveUrl")),
                 "DEPENDENCIES.md must name kParakeetArchiveUrl");
        QVERIFY2(cpp.contains(QStringLiteral("kParakeetArchiveUrl")),
                 "kParakeetArchiveUrl must still exist in ModelManager.cpp");

        const QString tail = QStringLiteral("sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8.tar.bz2");
        QVERIFY2(doc.contains(tail), "DEPENDENCIES.md must quote the Parakeet archive file name");
        QVERIFY2(cpp.contains(tail),
                 "ModelManager.cpp's kParakeetArchiveUrl must still end in the file name DEPENDENCIES.md quotes");
    }

    // Every CUDA-12 wheel name + SHA256 DEPENDENCIES.md lists must appear in
    // CMakeLists.txt's suop_fetch_cuda12_wheel() calls.
    void cudaWheelPins_matchCMakeLists()
    {
        const QString doc = readAll(QStringLiteral(SUOP_REPO_DIR "/DEPENDENCIES.md"));
        QVERIFY2(!doc.isEmpty(), "DEPENDENCIES.md must exist");
        const QString cmake = readAll(QStringLiteral(SUOP_REPO_DIR "/CMakeLists.txt"));
        QVERIFY2(!cmake.isEmpty(), "CMakeLists.txt must exist");

        const QStringList wheels = {
            QStringLiteral("nvidia_cublas_cu12"),
            QStringLiteral("nvidia_cuda_runtime_cu12"),
            QStringLiteral("nvidia_cufft_cu12"),
            QStringLiteral("nvidia_cuda_nvrtc_cu12"),
        };
        const QStringList sha256s = {
            QStringLiteral("e4f53a8ca8c5d6e8c492d0d0a3d565ecb59a751b19cfdaa4f6da0ab2104c1702"),
            QStringLiteral("25bba2dfb01d48a9b59ca474a1ac43c6ebf7011f1b0b8cc44f54eb6ac48a96c3"),
            QStringLiteral("c67884f2a7d276b4b80eb56a79322a95df592ae5e765cf1243693365ccab4e28"),
            QStringLiteral("210cf05005a447e29214e9ce50851e83fc5f4358df8b453155d5e1918094dcb4"),
        };
        QCOMPARE(wheels.size(), sha256s.size());

        for (int i = 0; i < wheels.size(); ++i) {
            QVERIFY2(doc.contains(wheels.at(i)),
                     qPrintable(QStringLiteral("DEPENDENCIES.md must name wheel %1").arg(wheels.at(i))));
            QVERIFY2(cmake.contains(wheels.at(i)),
                     qPrintable(QStringLiteral("%1 must still be fetched in CMakeLists.txt").arg(wheels.at(i))));
            QVERIFY2(doc.contains(sha256s.at(i)),
                     qPrintable(QStringLiteral("DEPENDENCIES.md must quote %1's SHA256").arg(wheels.at(i))));
            QVERIFY2(cmake.contains(sha256s.at(i)),
                     qPrintable(QStringLiteral("CMakeLists.txt's %1 SHA256 must match DEPENDENCIES.md").arg(wheels.at(i))));
        }
    }

    // Every submodule path DEPENDENCIES.md lists must still be a submodule
    // in .gitmodules.
    void submodulePaths_matchGitmodules()
    {
        const QString doc = readAll(QStringLiteral(SUOP_REPO_DIR "/DEPENDENCIES.md"));
        QVERIFY2(!doc.isEmpty(), "DEPENDENCIES.md must exist");
        const QString gitmodules = readAll(QStringLiteral(SUOP_REPO_DIR "/.gitmodules"));
        QVERIFY2(!gitmodules.isEmpty(), ".gitmodules must exist");

        const QStringList paths = {
            QStringLiteral("third_party/llama.cpp"),
            QStringLiteral("third_party/whisper.cpp"),
            QStringLiteral("third_party/sherpa-onnx"),
        };
        for (const QString &path : paths) {
            QVERIFY2(doc.contains(path),
                     qPrintable(QStringLiteral("DEPENDENCIES.md must list submodule path %1").arg(path)));
            QVERIFY2(gitmodules.contains(QStringLiteral("path = %1").arg(path)),
                     qPrintable(QStringLiteral("%1 must still be a submodule in .gitmodules").arg(path)));
        }
    }

    // The CMakeLists.txt patch-guard FATAL_ERROR must still point readers at
    // DEPENDENCIES.md — it's the only in-code tripwire for the sherpa-onnx
    // bump procedure, so it must name the runbook that covers it.
    void patchGuardError_pointsAtDependenciesDoc()
    {
        const QString cmake = readAll(QStringLiteral(SUOP_REPO_DIR "/CMakeLists.txt"));
        QVERIFY2(!cmake.isEmpty(), "CMakeLists.txt must exist");
        QVERIFY2(cmake.contains(QStringLiteral("re-derive the patch against the current pinned commit rather")),
                 "the sherpa-onnx patch-guard FATAL_ERROR anchor text must still exist");
        QVERIFY2(cmake.contains(QStringLiteral("DEPENDENCIES.md")),
                 "the sherpa-onnx patch-guard FATAL_ERROR must point at DEPENDENCIES.md");
    }
};

QTEST_GUILESS_MAIN(TstDependencyPins)
#include "tst_dependency_pins.moc"
