// Living spec + runtime verification for MainPanel.qml's chapter-title <->
// transcript CROSS-SELECTION: a chapter title is not a character in
// transcriptArea's document (displayData reserves a ZERO-LENGTH blank line for
// it and an overlay TextEdit draws the title), so both "does the selection
// sweep this title" and "what does Ctrl+C actually put on the clipboard" are
// pure offset math against those reserved lines. This file is the one that
// proves that math. tst_styleguide.cpp's source-scan guards can only see that
// the code is SHAPED right; they cannot catch an off-by-one.
//
// Same harness shape as tst_bottombar_pathsegments.cpp (read its top-of-file
// comment first): QQuickView, offscreen, scratch QML module dir symlinking the
// real qml/, sandboxed org/app name in main(). Three things are specific here:
//
//  - **Clipboard is REAL, not stubbed.** copySelectionWithChapters() ends in
//    Clipboard.setText(), and the whole point of the splice is what lands on
//    the system clipboard — so the test reads it back through
//    QGuiApplication::clipboard() rather than trusting a recording stub. The
//    offscreen platform gives the process its own clipboard, so this touches
//    nothing outside the test.
//
//  - **The five C++ property types are empty stubs** (AudioPlayer,
//    AudioRecorder, TranscriptionController, VaultTreeModel,
//    UsbGrabberController). Same reasoning as tst_sidebar_expand.cpp's
//    TranscriptionController stub: MainPanel's every use site guards on the
//    property being non-null, so the properties stay NULL and only their TYPE
//    has to resolve for the file to load.
//
//  - **The document is driven straight through `sentences`/`chapters`.**
//    Those are plain properties feeding displayData, so setting them is enough
//    to populate transcriptArea without a vault, a file, or a transcription.
//
// Nothing here touches the user's vault or ~/.config/suopTranscriber.

#include <QtTest>

#include <QClipboard>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>

#include "Clipboard.h"
#include "TranscriptDisplayBuilder.h"
#include "UndoStack.h"
#include "IconTheme.h"

namespace {

// See tst_sidebar_expand.cpp's identical TranscriptionController stub: every
// MainPanel use site of these guards on the property being non-null (bindings)
// or sits in a handler this harness never fires, so each property only needs
// its TYPE to resolve. All five stay null for every test below.
class AudioPlayerStub : public QObject { Q_OBJECT };
class AudioRecorderStub : public QObject { Q_OBJECT };
class TranscriptionControllerStub : public QObject { Q_OBJECT };
class VaultTreeModelStub : public QObject { Q_OBJECT };
class UsbGrabberControllerStub : public QObject { Q_OBJECT };

QVariantMap sentence(qreal t0, const QString &text)
{
    QVariantMap m;
    m.insert(QStringLiteral("t0"), t0);
    m.insert(QStringLiteral("text"), text);
    m.insert(QStringLiteral("speaker"), QString());
    m.insert(QStringLiteral("repeatCount"), 1);
    return m;
}

QVariantMap chapter(qreal t0, const QString &title)
{
    QVariantMap m;
    m.insert(QStringLiteral("t0"), t0);
    m.insert(QStringLiteral("title"), title);
    m.insert(QStringLiteral("manual"), true);
    return m;
}

} // namespace

class TstMainPanelChapterSelection : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // Walks the VISUAL tree (childItems), NOT the QObject tree (findChildren).
    // A Repeater's delegates — every chapterHeadingRow below — are only ever
    // given a visual parent, so findChildren() cannot see them and silently
    // returns nothing at all. Copied from tst_bottombar_pathsegments.cpp.
    static void collectByObjectName(QQuickItem *root, const QString &name, QList<QQuickItem *> &out)
    {
        for (QQuickItem *child : root->childItems()) {
            if (child->objectName() == name)
                out.append(child);
            collectByObjectName(child, name, out);
        }
    }

    static QQuickItem *firstByObjectName(QQuickItem *root, const QString &name)
    {
        QList<QQuickItem *> found;
        collectByObjectName(root, name, found);
        return found.isEmpty() ? nullptr : found.first();
    }

    // The rendered chapter headings, in TOP-TO-BOTTOM display order — sorted by
    // y rather than trusting Repeater tree-walk order, so the assertions are
    // about what's on screen.
    static QList<QQuickItem *> headingRowsOf(QQuickItem *panel)
    {
        QList<QQuickItem *> rows;
        collectByObjectName(panel, QStringLiteral("chapterHeadingRow"), rows);
        std::sort(rows.begin(), rows.end(), [](QQuickItem *a, QQuickItem *b) {
            return a->mapToScene(QPointF(0, 0)).y() < b->mapToScene(QPointF(0, 0)).y();
        });
        return rows;
    }

    static QQuickItem *titleFieldOf(QQuickItem *headingRow)
    {
        return firstByObjectName(headingRow, QStringLiteral("chapterHeadingTitle"));
    }

    // Each rendered heading's scene y, keyed by its title — keyed rather than
    // indexed so an assertion can't be satisfied by the rows swapping places.
    static QMap<QString, qreal> headingYByTitle(QQuickItem *panel)
    {
        QMap<QString, qreal> out;
        for (QQuickItem *row : headingRowsOf(panel)) {
            if (QQuickItem *title = titleFieldOf(row))
                out.insert(title->property("text").toString(), row->mapToScene(QPointF(0, 0)).y());
        }
        return out;
    }

    QQuickItem *m_panel = nullptr;
    QQuickItem *m_area = nullptr;

    // Builds the view + populates the transcript. Returns the located MainPanel
    // (also caching it and transcriptArea in m_panel/m_area), or nullptr.
    QQuickItem *openPanel(QQuickView *view, const QVariantList &sentences, const QVariantList &chapters)
    {
        m_panel = nullptr;
        m_area = nullptr;
        view->engine()->addImportPath(m_moduleDir.path());
        view->engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                          new IconImageProvider);
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : view->errors())
                msg += e.toString() + QStringLiteral("\n");
            // The exact failure a green cmake build does NOT catch: qmlcachegen
            // happily AOT-compiles a file that still fails to load. Verbatim,
            // not as "null item".
            qWarning("%s", qPrintable(msg.isEmpty() ? QStringLiteral("MainPanel.qml failed to load, no error text")
                                                     : msg));
            return nullptr;
        }
        QQuickItem *root = view->rootObject();
        if (!root)
            return nullptr;
        QQuickItem *panel = root->findChild<QQuickItem *>(QStringLiteral("mainPanel"));
        if (!panel)
            return nullptr;

        // transcriptArea gates on transcriptBody (the raw .md body the real app
        // fills from disk) being non-empty — set it so the area is visible and
        // laid out. Its CONTENT comes from displayText/`sentences`, not this.
        panel->setProperty("transcriptBody", QStringLiteral("body"));
        panel->setProperty("sentences", sentences);
        panel->setProperty("chapters", chapters);

        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        // Let the Repeaters instantiate and the text layout settle so
        // gutterLineRects (which the heading rows position against) is real.
        QTest::qWait(50);

        m_panel = panel;
        m_area = firstByObjectName(panel, QStringLiteral("transcriptArea"));
        return panel;
    }

    void select(int start, int end)
    {
        QMetaObject::invokeMethod(m_area, "select", Q_ARG(int, start), Q_ARG(int, end));
    }

    // A QML-declared function always carries a QVariant return type in the
    // metaobject, never the JS value's own type — Q_RETURN_ARG(bool) here
    // silently fails the invocation entirely ("return type mismatch"), leaving
    // the function UNCALLED and the assertion comparing against a stale
    // clipboard. Both call sites below go through QVariant for that reason.
    QString copyWithChapters()
    {
        QGuiApplication::clipboard()->clear();
        QVariant handled;
        QMetaObject::invokeMethod(m_panel, "copySelectionWithChapters", Q_RETURN_ARG(QVariant, handled));
        return QGuiApplication::clipboard()->text();
    }

    bool sweepsHeading(int headingStart)
    {
        QVariant result;
        QMetaObject::invokeMethod(m_panel, "selectionSweepsHeading", Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QVariant, QVariant(headingStart)));
        return result.toBool();
    }

    QString displayText() const { return m_panel->property("displayText").toString(); }

    // The fixture every offset below is reasoned against:
    //   line 0: "Hello world"          (sentence @0)
    //   line 1: ""                     (chapter "Chapter One" @3 — reserved)
    //   line 2: "Second line"          (sentence @5)
    // so the heading's reserved line starts at offset 12.
    static QVariantList basicSentences()
    {
        return QVariantList{sentence(0, QStringLiteral("Hello world")),
                             sentence(5, QStringLiteral("Second line"))};
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        // Symlink (not copy) to the real checked-out qml/ — must reflect what's
        // on disk right now, never a stale build-time copy.
        QVERIFY2(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")),
                 "failed to symlink qml/ into the test's scratch QML module dir");

        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
        qmldir.write("module SuopTranscriber\n"
                      "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                      "singleton Theme 1.0 qml/Theme.qml\n"
                      "singleton Notifications 1.0 qml/Notifications.qml\n"
                      "AppIcon 1.0 qml/AppIcon.qml\n"
                      "AppIconButton 1.0 qml/AppIconButton.qml\n"
                      "ConfirmPopup 1.0 qml/ConfirmPopup.qml\n"
                      "HoverBorder 1.0 qml/HoverBorder.qml\n"
                      "UnderlineField 1.0 qml/UnderlineField.qml\n"
                      "MiddleDragScroll 1.0 qml/MiddleDragScroll.qml\n"
                      "UsbRegexField 1.0 qml/UsbRegexField.qml\n"
                      "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                      "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                      "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
                      "MainPanel 1.0 qml/MainPanel.qml\n");
        qmldir.close();

        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        // The REAL Clipboard (see this file's top comment) — the assertions read
        // back what it put on the system clipboard.
        qmlRegisterSingletonType<Clipboard>(
            "SuopTranscriber", 1, 0, "Clipboard",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new Clipboard; });
        // MainPanel's displayData binding calls into this (the transcript
        // display-model builder lifted out of the QML) — without it the whole
        // panel fails to load.
        qmlRegisterSingletonType<TranscriptDisplayBuilder>(
            "SuopTranscriber", 1, 0, "TranscriptDisplayBuilder",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new TranscriptDisplayBuilder; });
        // MainPanel now owns an instantiable `UndoStack { id: undoStack }`
        // (per-panel undo/redo storage); without this the .qml fails to load.
        qmlRegisterType<UndoStack>("SuopTranscriber", 1, 0, "UndoStack");
        qmlRegisterUncreatableType<AudioPlayerStub>(
            "SuopTranscriber", 1, 0, "AudioPlayer", QStringLiteral("stub — this harness leaves it null"));
        qmlRegisterUncreatableType<AudioRecorderStub>(
            "SuopTranscriber", 1, 0, "AudioRecorder", QStringLiteral("stub — this harness leaves it null"));
        qmlRegisterUncreatableType<TranscriptionControllerStub>(
            "SuopTranscriber", 1, 0, "TranscriptionController", QStringLiteral("stub — this harness leaves it null"));
        qmlRegisterUncreatableType<VaultTreeModelStub>(
            "SuopTranscriber", 1, 0, "VaultTreeModel", QStringLiteral("stub — this harness leaves it null"));
        qmlRegisterUncreatableType<UsbGrabberControllerStub>(
            "SuopTranscriber", 1, 0, "UsbGrabberController", QStringLiteral("stub — this harness leaves it null"));

        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    width: 900\n"
            "    height: 700\n"
            "    MainPanel {\n"
            "        objectName: \"mainPanel\"\n"
            "        anchors.fill: parent\n"
            "    }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driverFile(m_driverQmlPath);
        QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
        driverFile.write(driverQml.toUtf8());
        driverFile.close();
    }

    // Item 1: the shape everything else depends on. Each chapter heading
    // reserves a ZERO-LENGTH blank line in transcriptArea's document — there is
    // a real empty line exactly where the user sees a title. Pinning this pins
    // the offsets the splice math is written against.
    void chapterHeadingReservesAZeroLengthBlankLine()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, basicSentences(),
                            QVariantList{chapter(3, QStringLiteral("Chapter One"))}),
                 "could not load MainPanel.qml / locate it");
        QVERIFY2(m_area, "could not locate transcriptArea");

        QCOMPARE(displayText(), QStringLiteral("Hello world\n\nSecond line"));
        QCOMPARE(displayText().split(QLatin1Char('\n')),
                 (QStringList{QStringLiteral("Hello world"), QString(), QStringLiteral("Second line")}));
        // ...and transcriptArea really holds it (the `text:` binding is live
        // until the user types).
        QCOMPARE(m_area->property("text").toString(), displayText());

        // The heading's model entry points AT that empty line, with length 0.
        const QVariantList headings = m_panel->property("chapterHeadingModel").toList();
        QCOMPARE(headings.size(), 1);
        const QVariantMap h = headings.first().toMap();
        QCOMPARE(h.value(QStringLiteral("start")).toInt(), 12);
        QCOMPARE(h.value(QStringLiteral("length")).toInt(), 0);
        QCOMPARE(h.value(QStringLiteral("chapterTitle")).toString(), QStringLiteral("Chapter One"));
    }

    // Item 2: the headline claim — a selection spanning body + a heading puts
    // the title back on its own line, at the offset its blank line reserved.
    void selectionSpanningAHeading_splicesTheTitleInPlace()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, basicSentences(),
                            QVariantList{chapter(3, QStringLiteral("Chapter One"))}),
                 "could not load MainPanel.qml / locate it");

        select(0, displayText().length());
        QCOMPARE(copyWithChapters(), QStringLiteral("Hello world\nChapter One\nSecond line"));
    }

    // Two adjacent headings (a chapter marker with no speech between it and the
    // next) — both titles, in source order. This is what the back-to-front
    // splice loop exists for: front-to-back, the first insert would shift every
    // later offset and the second title would land in the wrong place.
    void selectionSpanningTwoAdjacentHeadings_splicesBothInOrder()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, basicSentences(),
                            QVariantList{chapter(3, QStringLiteral("Alpha")),
                                          chapter(4, QStringLiteral("Beta"))}),
                 "could not load MainPanel.qml / locate it");

        QCOMPARE(displayText(), QStringLiteral("Hello world\n\n\nSecond line"));
        select(0, displayText().length());
        QCOMPARE(copyWithChapters(), QStringLiteral("Hello world\nAlpha\nBeta\nSecond line"));
    }

    // A chapter at offset 0 (marker at/before the first sentence) — its heading
    // reserves the very first line, so the title has to come out FIRST, spliced
    // at offset 0 with nothing before it.
    void chapterAtOffsetZero_splicesTheTitleFirst()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, basicSentences(),
                            QVariantList{chapter(0, QStringLiteral("Intro"))}),
                 "could not load MainPanel.qml / locate it");

        QCOMPARE(displayText(), QStringLiteral("\nHello world\nSecond line"));
        const QVariantList headings = m_panel->property("chapterHeadingModel").toList();
        QCOMPARE(headings.size(), 1);
        QCOMPARE(headings.first().toMap().value(QStringLiteral("start")).toInt(), 0);

        select(0, displayText().length());
        QCOMPARE(copyWithChapters(), QStringLiteral("Intro\nHello world\nSecond line"));
    }

    // THE boundary, and the reason selectionSweepsHeading uses a STRICT `>` on
    // the end: a selection that stops exactly AT the heading's reserved line
    // (i.e. one ending with the previous sentence's newline) has not reached the
    // heading, and must not drag the next chapter's title into the clipboard.
    // `>=` here would make every such selection silently steal a title.
    void selectionEndingExactlyAtAHeading_excludesThatTitle()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, basicSentences(),
                            QVariantList{chapter(3, QStringLiteral("Chapter One"))}),
                 "could not load MainPanel.qml / locate it");

        select(0, 12); // "Hello world\n" — up to, not into, the reserved line
        QCOMPARE(displayText().mid(0, 12), QStringLiteral("Hello world\n"));
        QVERIFY2(!sweepsHeading(12),
                 "a selection ending exactly at the heading's start must not sweep it");
        QCOMPARE(copyWithChapters(), QStringLiteral("Hello world\n"));
    }

    // The other half of the boundary: a selection STARTING at the reserved line
    // and running past it does sweep the heading, and the title comes out at the
    // very front of the copied text.
    void selectionStartingAtAHeading_includesThatTitle()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, basicSentences(),
                            QVariantList{chapter(3, QStringLiteral("Chapter One"))}),
                 "could not load MainPanel.qml / locate it");

        select(12, displayText().length());
        QVERIFY2(sweepsHeading(12),
                 "a selection starting at the heading's start and running past it must sweep it");
        QCOMPARE(copyWithChapters(), QStringLiteral("Chapter One\nSecond line"));
    }

    // A selection inside one sentence touches no heading, so the copy is exactly
    // the native selection — the splice must not invent anything.
    void selectionWithinOneSentence_copiesSelectedTextUnchanged()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, basicSentences(),
                            QVariantList{chapter(3, QStringLiteral("Chapter One"))}),
                 "could not load MainPanel.qml / locate it");

        select(2, 7);
        const QString selected = m_area->property("selectedText").toString();
        QCOMPARE(selected, QStringLiteral("llo w"));
        QCOMPARE(copyWithChapters(), selected);
    }

    // Item 3: the copied text must contain REAL '\n', never U+2029. QTextCursor::
    // selectedText() returns paragraph separators; QQuickTextEdit::selectedText
    // is supposed to go through toPlainText() for a plain-text document — this
    // verifies it actually does, end to end onto the clipboard. A U+2029 here
    // would be a real bug (the user pastes one unbroken line), not a quirk to
    // normalize away.
    void copiedTextUsesRealNewlinesNotParagraphSeparators()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, basicSentences(),
                            QVariantList{chapter(3, QStringLiteral("Chapter One"))}),
                 "could not load MainPanel.qml / locate it");

        select(0, displayText().length());
        const QString selectedText = m_area->property("selectedText").toString();
        QVERIFY2(!selectedText.contains(QChar(0x2029)),
                 qPrintable(QStringLiteral("transcriptArea.selectedText contains U+2029 paragraph separators: %1")
                                .arg(QString(selectedText).replace(QChar(0x2029), QStringLiteral("<U+2029>")))));
        QCOMPARE(selectedText, QStringLiteral("Hello world\n\nSecond line"));

        const QString copied = copyWithChapters();
        QVERIFY2(!copied.contains(QChar(0x2029)),
                 qPrintable(QStringLiteral("clipboard text contains U+2029 paragraph separators: %1")
                                .arg(QString(copied).replace(QChar(0x2029), QStringLiteral("<U+2029>")))));
        QCOMPARE(copied.count(QLatin1Char('\n')), 2);
    }

    // Item 4: what you SEE is what you copy. A transcript selection sweeping a
    // heading mirrors onto that heading's own overlay field (a separate document
    // — a native drag can never reach it), so the title highlights along with the
    // body around it. Keyed off the same predicate as the splice, so the two can
    // never disagree.
    void transcriptSelectionSweepingAHeading_highlightsThatTitle()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, basicSentences(),
                            QVariantList{chapter(3, QStringLiteral("Chapter One"))}),
                 "could not load MainPanel.qml / locate it");

        const QList<QQuickItem *> rows = headingRowsOf(m_panel);
        QCOMPARE(rows.size(), 1);
        QQuickItem *title = titleFieldOf(rows.first());
        QVERIFY2(title, "could not locate the heading's title field");
        QCOMPARE(title->property("text").toString(), QStringLiteral("Chapter One"));

        // Nothing selected yet: no mirrored highlight.
        QCOMPARE(title->property("selectedText").toString(), QString());

        select(0, displayText().length());
        // The title highlights WHOLE — its reserved line is zero-length, so
        // there is no sub-title offset a transcript selection could land on.
        QCOMPARE(title->property("selectedText").toString(), QStringLiteral("Chapter One"));

        // ...and collapsing back to a selection that stops at the heading drops
        // the highlight again, matching what the copy would produce.
        select(0, 12);
        QCOMPARE(title->property("selectedText").toString(), QString());
    }

    // The mirrored highlight and the clipboard splice must agree title-for-title
    // with two headings: sweep only the SECOND, and only the second highlights
    // AND only the second is copied.
    void mirroredHighlightAndClipboardAgreePerTitle()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view, basicSentences(),
                            QVariantList{chapter(3, QStringLiteral("Alpha")),
                                          chapter(4, QStringLiteral("Beta"))}),
                 "could not load MainPanel.qml / locate it");

        // "Hello world\n\n\nSecond line": Alpha's line at 12, Beta's at 13.
        const QList<QQuickItem *> rows = headingRowsOf(m_panel);
        QCOMPARE(rows.size(), 2);
        QQuickItem *alpha = titleFieldOf(rows.at(0));
        QQuickItem *beta = titleFieldOf(rows.at(1));
        QVERIFY(alpha && beta);
        QCOMPARE(alpha->property("text").toString(), QStringLiteral("Alpha"));
        QCOMPARE(beta->property("text").toString(), QStringLiteral("Beta"));

        select(13, displayText().length());
        QCOMPARE(alpha->property("selectedText").toString(), QString());
        QCOMPARE(beta->property("selectedText").toString(), QStringLiteral("Beta"));
        QCOMPARE(copyWithChapters(), QStringLiteral("Beta\nSecond line"));
    }

    // The overlay offsets must track the LIVE document, not the model's
    // precomputed `start` values. displaySentences' offsets index displayText,
    // and the `text: mainPanel.displayText` binding is severed the moment the
    // user types — so mid-edit every offset past the caret is stale by the edit
    // delta, and positionToRectangle() resolves each overlay to the tail of the
    // PREVIOUS line. On a heading (a full-width bold row) that meant a title
    // painted directly on top of a sentence with its own reserved blank line
    // left empty. Typing one character used to move every heading below the
    // caret up exactly one line height; this pins that it moves nothing.
    void headingsStayOnTheirLineWhileTyping()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view,
                            QVariantList{sentence(0, QStringLiteral("Hello world")),
                                          sentence(5, QStringLiteral("Second line")),
                                          sentence(10, QStringLiteral("Third line"))},
                            QVariantList{chapter(3, QStringLiteral("Chapter One")),
                                          chapter(7, QStringLiteral("Chapter Two"))}),
                 "could not load MainPanel.qml / locate it");
        QVERIFY2(m_area, "could not locate transcriptArea");

        // Lines 0..4, with a heading's reserved blank line at 1 and 3 — so
        // there is body both above and below each heading.
        QCOMPARE(displayText(), QStringLiteral("Hello world\n\nSecond line\n\nThird line"));
        const int lineCount = m_panel->property("displaySentences").toList().size();
        QCOMPARE(lineCount, 5);

        const QMap<QString, qreal> before = headingYByTitle(m_panel);
        QCOMPARE(before.size(), 2);
        QVERIFY(before.value(QStringLiteral("Chapter One"))
                < before.value(QStringLiteral("Chapter Two")));

        // Mutate the LIVE DOCUMENT, not the model — the defect lives entirely
        // in the divergence between the two, so setting `sentences` would
        // paper over it. insert() is used rather than QTest::keyClick because
        // the offscreen platform never activates the window, so synthesized
        // key events reach no focus item; insert() severs the
        // `text: mainPanel.displayText` binding exactly as typing does, and
        // focus is held so the onDisplayTextChanged resync stays skipped —
        // which is precisely the mid-edit state the user hits.
        m_area->forceActiveFocus();
        QVERIFY2(m_area->hasActiveFocus(), "transcriptArea did not take focus");
        QMetaObject::invokeMethod(m_area, "insert", Q_ARG(int, 5), Q_ARG(QString, QStringLiteral("X")));
        QTRY_COMPARE(m_area->property("text").toString(),
                     QStringLiteral("HelloX world\n\nSecond line\n\nThird line"));

        // The invariant the whole fix rests on: one live line per entry. A
        // future change that let Return through would break this first.
        QCOMPARE(m_area->property("liveLineStarts").toList().size(), lineCount);

        const QMap<QString, qreal> afterInsert = headingYByTitle(m_panel);
        QCOMPARE(afterInsert.keys(), before.keys());
        for (const QString &title : before.keys()) {
            QVERIFY2(qFuzzyCompare(1.0 + afterInsert.value(title), 1.0 + before.value(title)),
                      qPrintable(QStringLiteral("heading '%1' moved from y=%2 to y=%3 on a keystroke")
                                     .arg(title).arg(before.value(title)).arg(afterInsert.value(title))));
        }

        // ...and the drift runs the other way on a delete.
        QMetaObject::invokeMethod(m_area, "remove", Q_ARG(int, 5), Q_ARG(int, 6));
        QTRY_COMPARE(m_area->property("text").toString(),
                     QStringLiteral("Hello world\n\nSecond line\n\nThird line"));
        QCOMPARE(m_area->property("liveLineStarts").toList().size(), lineCount);
        const QMap<QString, qreal> afterDelete = headingYByTitle(m_panel);
        for (const QString &title : before.keys()) {
            QVERIFY2(qFuzzyCompare(1.0 + afterDelete.value(title), 1.0 + before.value(title)),
                      qPrintable(QStringLiteral("heading '%1' moved on a delete").arg(title)));
        }
    }

    // The overlap condition itself, stated directly: a heading's row must never
    // land on the same y as a line of actual transcript text. This is what the
    // user saw — bold green title painted over a sentence.
    void headingsNeverShareALineWithTranscriptText()
    {
        QQuickView view;
        QVERIFY2(openPanel(&view,
                            QVariantList{sentence(0, QStringLiteral("Hello world")),
                                          sentence(5, QStringLiteral("Second line")),
                                          sentence(10, QStringLiteral("Third line"))},
                            QVariantList{chapter(3, QStringLiteral("Chapter One")),
                                          chapter(7, QStringLiteral("Chapter Two"))}),
                 "could not load MainPanel.qml / locate it");
        QVERIFY(m_area);

        m_area->forceActiveFocus();
        QMetaObject::invokeMethod(m_area, "insert", Q_ARG(int, 5), Q_ARG(QString, QStringLiteral("X")));
        QTRY_COMPARE(m_area->property("text").toString().length(), 37);

        // gutterLineRects is what positions every overlay; a heading's rect
        // colliding with a body line's rect IS the overlap.
        const QVariantList rects = m_area->property("gutterLineRects").toList();
        QCOMPARE(rects.size(), 5);
        const QVariantList entries = m_panel->property("displaySentences").toList();
        QCOMPARE(entries.size(), 5);

        QList<qreal> headingYs;
        QList<qreal> bodyYs;
        for (int i = 0; i < entries.size(); ++i) {
            const qreal y = rects.at(i).toRectF().y();
            if (entries.at(i).toMap().value(QStringLiteral("isChapterHeading")).toBool())
                headingYs.append(y);
            else
                bodyYs.append(y);
        }
        QCOMPARE(headingYs.size(), 2);
        QCOMPARE(bodyYs.size(), 3);
        for (qreal hy : headingYs) {
            for (qreal by : bodyYs) {
                QVERIFY2(!qFuzzyCompare(1.0 + hy, 1.0 + by),
                          qPrintable(QStringLiteral("a chapter heading is painted on a transcript "
                                                    "line — both at y=%1").arg(hy)));
            }
        }
    }

    // firstRealSentenceIndex is the playback highlight's front-edge clamp: it
    // skips any chapter heading(s) that open the transcript so a leading heading
    // never gets lit. It's a pure derived property over displaySentences, so it
    // is testable here even though the full activeSentenceIndex binding needs a
    // controller this harness leaves null.
    void firstRealSentenceIndex_skipsLeadingChapterHeadings()
    {
        {
            QQuickView view;
            QVERIFY2(openPanel(&view, basicSentences(),
                                QVariantList{chapter(0, QStringLiteral("Intro"))}),
                     "could not load MainPanel.qml / locate it");
            // displaySentences[0] is the leading heading -> first real is 1.
            QCOMPARE(m_panel->property("firstRealSentenceIndex").toInt(), 1);
        }
        {
            QQuickView view;
            QVERIFY2(openPanel(&view, basicSentences(),
                                QVariantList{chapter(3, QStringLiteral("Mid"))}),
                     "could not load MainPanel.qml / locate it");
            // The heading falls between the two sentences -> first real is 0.
            QCOMPARE(m_panel->property("firstRealSentenceIndex").toInt(), 0);
        }
        {
            QQuickView view;
            QVERIFY2(openPanel(&view, QVariantList{},
                                QVariantList{chapter(0, QStringLiteral("Empty"))}),
                     "could not load MainPanel.qml / locate it");
            // Only a heading (no spoken sentences) -> -1.
            QCOMPARE(m_panel->property("firstRealSentenceIndex").toInt(), -1);
        }
    }

    // Queue item 054: collapsing a chapter drops its body sentences from
    // displaySentences AND stamps `collapsed: true` on the surviving heading
    // entry — the one bit the C++ activeSentenceIndex reads to hold the
    // playback highlight on a collapsed chapter's heading.
    void collapsedChapterDropsBodyAndStampsHeading()
    {
        QQuickView view;
        // basicSentences(): "Hello world" @0, "Second line" @5. Chapter "Two"
        // @5 covers "Second line".
        QVERIFY2(openPanel(&view, basicSentences(),
                            QVariantList{chapter(5, QStringLiteral("Two"))}),
                 "could not load MainPanel.qml / locate it");

        // Not collapsed: 3 entries (2 sentences + heading), heading.collapsed=false.
        {
            const QVariantList entries = m_panel->property("displaySentences").toList();
            QCOMPARE(entries.size(), 3);
            bool sawHeading = false;
            for (const QVariant &v : entries) {
                const QVariantMap e = v.toMap();
                if (e.value(QStringLiteral("isChapterHeading")).toBool()) {
                    sawHeading = true;
                    QCOMPARE(e.value(QStringLiteral("collapsed")).toBool(), false);
                }
            }
            QVERIFY(sawHeading);
        }

        // Collapse chapter @5: its body ("Second line") drops, heading stays
        // with collapsed=true.
        QVariantMap collapsed;
        collapsed.insert(QStringLiteral("5"), true);
        m_panel->setProperty("collapsedChapters", collapsed);
        QTest::qWait(50);

        const QVariantList entries = m_panel->property("displaySentences").toList();
        QCOMPARE(entries.size(), 2); // "Hello world" + the collapsed heading
        bool sawCollapsedHeading = false;
        for (const QVariant &v : entries) {
            const QVariantMap e = v.toMap();
            QVERIFY2(e.value(QStringLiteral("text")).toString() != QStringLiteral("Second line"),
                     "the collapsed chapter's body sentence must be dropped");
            if (e.value(QStringLiteral("isChapterHeading")).toBool()) {
                sawCollapsedHeading = true;
                QCOMPARE(e.value(QStringLiteral("collapsed")).toBool(), true);
            }
        }
        QVERIFY2(sawCollapsedHeading, "the heading entry must survive with collapsed=true");
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: MainPanel.qml loads the AppSettings
    // singleton, whose Settings{} element must NEVER touch the real
    // ~/.config/suopTranscriber (CLAUDE.md: never let testing touch the user's
    // real vault or app config). Same setup as tst_bottombar_pathsegments.cpp.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_mainpanel_chapterselection"));
    QGuiApplication app(argc, argv);
    TstMainPanelChapterSelection tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_mainpanel_chapterselection.moc"
