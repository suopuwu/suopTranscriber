// Clipboard is the QML-facing wrapper around QGuiApplication::clipboard() —
// Qt Quick has no clipboard API of its own, and MainPanel's transcript copy
// needs to put a string IT built (the selection, with the swept chapter titles
// spliced back in) onto the clipboard rather than let the TextArea copy its own
// document. See Clipboard.h.
//
// Runs on the offscreen platform, which has no system clipboard: Qt falls back
// to QPlatformClipboard's default, an in-process store. That's exactly the
// contract worth pinning here — that setText() is what text() reads back, i.e.
// the wrapper marshals through to QClipboard rather than dropping the string.

#include "Clipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QTest>

class TstClipboard : public QObject
{
    Q_OBJECT

private slots:
    void setTextIsReadableBack()
    {
        Clipboard clipboard;
        clipboard.setText(QStringLiteral("Chapter 1\nfirst sentence"));
        QCOMPARE(clipboard.text(), QStringLiteral("Chapter 1\nfirst sentence"));
    }

    // The wrapper must not keep its own private copy — the whole point is the
    // SYSTEM clipboard, so a paste into another app sees the text.
    void setTextReachesTheRealClipboard()
    {
        Clipboard clipboard;
        clipboard.setText(QStringLiteral("spliced"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("spliced"));
    }

    void setTextReplacesPreviousContents()
    {
        Clipboard clipboard;
        clipboard.setText(QStringLiteral("first copy"));
        clipboard.setText(QStringLiteral("second copy"));
        QCOMPARE(clipboard.text(), QStringLiteral("second copy"));
    }

    // A transcript selection is multi-line by nature and can legitimately be
    // empty-ish; neither is allowed to be mangled or swallowed.
    void setTextPreservesNewlinesAndEmptyText()
    {
        Clipboard clipboard;
        clipboard.setText(QStringLiteral("a\n\nb"));
        QCOMPARE(clipboard.text(), QStringLiteral("a\n\nb"));
        clipboard.setText(QString());
        QCOMPARE(clipboard.text(), QString());
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TstClipboard tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_clipboard.moc"
