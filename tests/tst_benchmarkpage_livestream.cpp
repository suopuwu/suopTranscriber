// Source-scan guard for the Benchmark page's LIVE transcript streaming.
// There is no QQuickView harness that can drive BenchmarkPage's
// `Connections` handlers directly — the target transcriptionController is a
// null stub in the filesync harness, so its signals never fire — so this
// falls back to the source-scan-guard convention (see tst_styleguide.cpp)
// over the real qml/BenchmarkPage.qml source.
//
// It pins that the running card streams in: onBenchmarkModelStarted creates a
// `running: true` placeholder, a NEW onBenchmarkModelProgressText handler grows
// it, and onBenchmarkModelDone no longer blindly `concat`s (it REPLACES the
// running placeholder in place, keyed on `running`). A regression back to
// append-only-at-done is caught here.
//
// Uses stripComments()/functionBody() (copied from tst_styleguide.cpp) rather
// than raw indexOf over the whole file — a raw window would match the very doc
// comments that describe this behavior and silently stop guarding (memory
// feedback_styleguide_guard_helpers).

#include <QtTest>

#include <QFile>
#include <QString>

namespace {

QString readAll(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// `src` with its // line comments removed, so a "does the code do X" scan can't
// be satisfied (or defeated) by prose merely MENTIONING X. Skips '//' inside
// double-quoted strings so literals like "file://" survive. (Copied from
// tst_styleguide.cpp — those helpers are private to that TestClass.)
QString stripComments(const QString &src)
{
    QString out;
    out.reserve(src.size());
    bool inString = false;
    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src.at(i);
        if (inString) {
            if (c == QLatin1Char('\\') && i + 1 < src.size()) {
                out += c;
                out += src.at(++i);
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
                out += QLatin1Char('\n');
            continue;
        }
        out += c;
    }
    return out;
}

// The body of the JS function whose declaration is `signature`, brace-matched
// FORWARD from the brace that opens it. Returns empty if not found.
QString functionBody(const QString &src, const QString &signature)
{
    const int anchorIdx = src.indexOf(signature);
    if (anchorIdx < 0)
        return QString();
    const int open = src.indexOf(QLatin1Char('{'), anchorIdx);
    if (open < 0)
        return QString();
    int depth = 0;
    for (int i = open; i < src.size(); ++i) {
        if (src.at(i) == QLatin1Char('{')) {
            ++depth;
        } else if (src.at(i) == QLatin1Char('}')) {
            if (--depth == 0)
                return src.mid(open, i - open + 1);
        }
    }
    return src.mid(open);
}

} // namespace

class TstBenchmarkPageLivestream : public QObject
{
    Q_OBJECT

private:
    QString m_src;

private slots:
    void initTestCase()
    {
        m_src = stripComments(readAll(QStringLiteral(SUOP_QML_DIR "/BenchmarkPage.qml")));
        QVERIFY2(!m_src.isEmpty(), "qml/BenchmarkPage.qml must exist and be readable");
    }

    // A live progress handler exists — the running card is grown as segments
    // stream in, not only populated at completion.
    void hasLiveProgressHandler()
    {
        const QString body =
            functionBody(m_src, QStringLiteral("function onBenchmarkModelProgressText("));
        QVERIFY2(!body.isEmpty(),
                 "BenchmarkPage.qml must wire onBenchmarkModelProgressText to grow the "
                 "running card live");
        QVERIFY2(body.contains(QStringLiteral("text")),
                 "onBenchmarkModelProgressText must update the card's text");
    }

    // onBenchmarkModelStarted must create a running placeholder card so the card
    // shows immediately (in a transcribing state) rather than only at done.
    void startedCreatesRunningPlaceholder()
    {
        const QString body =
            functionBody(m_src, QStringLiteral("function onBenchmarkModelStarted("));
        QVERIFY2(!body.isEmpty(), "onBenchmarkModelStarted must exist");
        QVERIFY2(body.contains(QStringLiteral("running: true")),
                 "onBenchmarkModelStarted must append a `running: true` placeholder card the "
                 "live text grows into");
        QVERIFY2(body.contains(QStringLiteral("results")),
                 "the placeholder must be added to benchmarkPage.results");
    }

    // onBenchmarkModelDone must REPLACE the running placeholder in place, not
    // blindly append — a regression back to append-only would leave a duplicate
    // card and never finalize the running one.
    void doneReplacesRunningPlaceholder()
    {
        const QString body =
            functionBody(m_src, QStringLiteral("function onBenchmarkModelDone("));
        QVERIFY2(!body.isEmpty(), "onBenchmarkModelDone must exist");
        QVERIFY2(body.contains(QStringLiteral("running")),
                 "onBenchmarkModelDone must find/replace the running placeholder (keyed on "
                 "`running`), not append-only — otherwise the live card is orphaned");
        QVERIFY2(!body.contains(QStringLiteral("results.concat")),
                 "onBenchmarkModelDone must not blindly concat a new card (the pre-066 "
                 "append-at-done behavior) — it replaces the running placeholder in place");
    }
};

QTEST_MAIN(TstBenchmarkPageLivestream)
#include "tst_benchmarkpage_livestream.moc"
