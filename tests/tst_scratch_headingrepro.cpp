// SCRATCH diagnostic harness — real mouse gestures against the real
// MainPanel.qml chapter headings. Copy of tst_mainpanel_chapterselection.cpp's
// setup (offscreen QQuickView, scratch QML module dir symlinking the real qml/,
// sandboxed org/app name, five null C++ stubs), plus:
//   - a qInstallMessageHandler capturing QML TypeErrors (a throwing signal
//     handler is otherwise silent to the test)
//   - QTest::mousePress/Move/Release against the view, mapped via mapToScene
// Delete when the diagnosis lands.

#include <QtTest>

#include <QClipboard>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>

#include "Clipboard.h"
#include "IconTheme.h"

namespace {

class AudioPlayerStub : public QObject { Q_OBJECT };
class AudioRecorderStub : public QObject { Q_OBJECT };
class TranscriptionControllerStub : public QObject { Q_OBJECT };
class VaultTreeModelStub : public QObject { Q_OBJECT };
class UsbGrabberControllerStub : public QObject { Q_OBJECT };

QStringList g_messages;
QtMessageHandler g_prevHandler = nullptr;

void capturingHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    g_messages.append(msg);
    if (g_prevHandler)
        g_prevHandler(type, ctx, msg);
}

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

class TstScratchHeadingRepro : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

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

    QQuickItem *m_panel = nullptr;
    QQuickItem *m_area = nullptr;

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

        // MainPanel's whole content ColumnLayout is gated on
        // `vaultPath !== "" && !recording && !usbGrabberOpen` (line ~2004).
        // Without a vaultPath the entire transcript subtree is visible=false —
        // and an invisible subtree is PRUNED from hit testing, so no real mouse
        // event can reach it. The model-API test never noticed. Scratch dir
        // only; nothing reads it here (vaultTreeModel is null).
        panel->setProperty("vaultPath", QUrl::fromLocalFile(m_moduleDir.path()));
        panel->setProperty("transcriptBody", QStringLiteral("body"));
        panel->setProperty("sentences", sentences);
        panel->setProperty("chapters", chapters);

        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        // >120ms: transcriptRepaintNudgeTimer (MainPanel.qml:1860) fires
        // selectAll()+deselect() on transcriptArea 120ms after transcriptBody
        // goes non-empty. In the real app that lands at file-open, long before
        // any user gesture; in a test it lands MID-DRAG and clobbers the
        // anchor (cursor -> end of document). Let it fire and settle first.
        QTest::qWait(300);

        m_panel = panel;
        m_area = firstByObjectName(panel, QStringLiteral("transcriptArea"));
        return panel;
    }

    static QVariantList basicSentences()
    {
        return QVariantList{sentence(0, QStringLiteral("Hello world")),
                             sentence(5, QStringLiteral("Second line"))};
    }

    // A transcript LONG enough that transcriptScrollView's Flickable is
    // actually draggable (contentHeight > height) — the real app's normal
    // state, and the one thing the 3-line fixture can never exercise: a
    // non-scrollable Flickable never competes for the press.
    static QVariantList longSentences()
    {
        QVariantList v;
        for (int i = 0; i < 80; ++i)
            v.append(sentence(i, QStringLiteral("Sentence number %1 in the body").arg(i)));
        return v;
    }

    static QPoint scenePt(QQuickItem *item, qreal x, qreal y)
    {
        return item->mapToScene(QPointF(x, y)).toPoint();
    }

    void dumpMessages(const char *tag)
    {
        for (const QString &m : g_messages)
            qWarning("  [%s] QML MSG: %s", tag, qPrintable(m));
    }

    // Print the ancestor chain of `leaf`, with everything the delivery agent's
    // hit test prunes on: visible/enabled/clip/opacity, plus whether the item
    // contains the scene point.
    static void dumpAncestors(QQuickItem *leaf, const QPointF &scenePos)
    {
        QList<QQuickItem *> chain;
        for (QQuickItem *i = leaf; i; i = i->parentItem())
            chain.prepend(i);
        for (QQuickItem *i : chain) {
            const QPointF local = i->mapFromScene(scenePos);
            qWarning("  ANC %-28s %-34s vis=%d ena=%d clip=%d op=%.2f w=%.1f h=%.1f local=(%.1f,%.1f) contains=%d",
                     qPrintable(i->objectName().isEmpty() ? QStringLiteral("-") : i->objectName()),
                     i->metaObject()->className(), i->isVisible(), i->isEnabled(), i->clip(),
                     i->opacity(), i->width(), i->height(), local.x(), local.y(),
                     i->contains(local));
        }
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        QVERIFY2(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")),
                 "failed to symlink qml/ into the test's scratch QML module dir");

        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
        qmldir.write("module SuopTranscriber\n"
                      "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                      "singleton Notifications 1.0 qml/Notifications.qml\n"
                      "AppIcon 1.0 qml/AppIcon.qml\n"
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
        qmlRegisterSingletonType<Clipboard>(
            "SuopTranscriber", 1, 0, "Clipboard",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new Clipboard; });
        qmlRegisterUncreatableType<AudioPlayerStub>(
            "SuopTranscriber", 1, 0, "AudioPlayer", QStringLiteral("stub"));
        qmlRegisterUncreatableType<AudioRecorderStub>(
            "SuopTranscriber", 1, 0, "AudioRecorder", QStringLiteral("stub"));
        qmlRegisterUncreatableType<TranscriptionControllerStub>(
            "SuopTranscriber", 1, 0, "TranscriptionController", QStringLiteral("stub"));
        qmlRegisterUncreatableType<VaultTreeModelStub>(
            "SuopTranscriber", 1, 0, "VaultTreeModel", QStringLiteral("stub"));
        qmlRegisterUncreatableType<UsbGrabberControllerStub>(
            "SuopTranscriber", 1, 0, "UsbGrabberController", QStringLiteral("stub"));

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

    void init() { g_messages.clear(); }

    // Hypothesis C: is headingTitle actually stretched full-width?
    void geometry_report()
    {
        QQuickView view;
        QVERIFY(openPanel(&view, basicSentences(), QVariantList{chapter(3, QStringLiteral("Chapter One"))}));
        QList<QQuickItem *> rows = headingRowsOf(m_panel);
        QCOMPARE(rows.size(), 1);
        QQuickItem *row = rows.first();
        QQuickItem *title = titleFieldOf(row);
        QVERIFY(title);

        const QPointF rowScene = row->mapToScene(QPointF(0, 0));
        const QPointF titleScene = title->mapToScene(QPointF(0, 0));
        qWarning("ROW    x=%.1f y=%.1f w=%.1f h=%.1f scene=(%.1f,%.1f)", row->x(), row->y(),
                 row->width(), row->height(), rowScene.x(), rowScene.y());
        qWarning("TITLE  x=%.1f y=%.1f w=%.1f h=%.1f scene=(%.1f,%.1f) contentWidth=%.1f",
                 title->x(), title->y(), title->width(), title->height(), titleScene.x(),
                 titleScene.y(), title->property("contentWidth").toReal());
        qWarning("TITLE  editing=%d readOnly=%d selectByMouse=%d activeFocusOnPress=%d",
                 title->property("editing").toBool(), title->property("readOnly").toBool(),
                 title->property("selectByMouse").toBool(),
                 title->property("activeFocusOnPress").toBool());
        qWarning("AREA   w=%.1f h=%.1f scene=(%.1f,%.1f)", m_area->width(), m_area->height(),
                 m_area->mapToScene(QPointF(0, 0)).x(), m_area->mapToScene(QPointF(0, 0)).y());
        dumpMessages("geom");
    }

    // BUG 1: a real click on the title should open it for editing.
    void realClickOnTitle_shouldBeginEdit()
    {
        QQuickView view;
        QVERIFY(openPanel(&view, basicSentences(), QVariantList{chapter(3, QStringLiteral("Chapter One"))}));
        QQuickItem *row = headingRowsOf(m_panel).first();
        QQuickItem *title = titleFieldOf(row);
        QVERIFY(title);

        qWarning("BEFORE editingChapterTs=%.1f titleFocus=%d areaFocus=%d areaCursor=%d",
                 m_panel->property("editingChapterTs").toReal(), title->hasActiveFocus(),
                 m_area->hasActiveFocus(), m_area->property("cursorPosition").toInt());

        // Click over the TEXT itself (a few px in), not the empty strip.
        const QPoint pt = scenePt(title, 12, title->height() / 2);
        qWarning("CLICK at scene (%d,%d) -- ancestor chain of headingTitle:", pt.x(), pt.y());
        dumpAncestors(title, QPointF(pt));

        // Control: does a click land on the transcript body at all in this
        // harness? Click into "Hello world" (first text line).
        const QPoint bodyPt = scenePt(m_area, 40, 8);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, bodyPt);
        QTest::qWait(50);
        qWarning("CONTROL click body at (%d,%d): areaFocus=%d areaCursor=%d",
                 bodyPt.x(), bodyPt.y(), m_area->hasActiveFocus(),
                 m_area->property("cursorPosition").toInt());

        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, pt);
        QTest::qWait(50);

        qWarning("AFTER  editingChapterTs=%.1f titleFocus=%d titleEditing=%d areaFocus=%d areaCursor=%d titleCursor=%d",
                 m_panel->property("editingChapterTs").toReal(), title->hasActiveFocus(),
                 title->property("editing").toBool(), m_area->hasActiveFocus(),
                 m_area->property("cursorPosition").toInt(),
                 title->property("cursorPosition").toInt());
        dumpMessages("click");

        QVERIFY2(m_panel->property("editingChapterTs").toReal() == 3.0,
                 "BUG 1: a real click on the chapter title did not open it for editing");
    }

    // BUG 1, in the real app's actual state: a SCROLLABLE transcript, so the
    // ScrollView's Flickable is a live competitor for the press.
    void realClickOnTitle_scrollableTranscript()
    {
        QQuickView view;
        QVERIFY(openPanel(&view, longSentences(), QVariantList{chapter(5.5, QStringLiteral("Chapter One"))}));
        QQuickItem *flick = nullptr;
        for (QQuickItem *i = m_area; i; i = i->parentItem()) {
            if (QLatin1String(i->metaObject()->className()).contains(QLatin1String("Flickable")))
                flick = i;
        }
        QVERIFY(flick);
        qWarning("FLICK h=%.1f contentHeight=%.1f interactive=%d -> draggable=%d", flick->height(),
                 flick->property("contentHeight").toReal(), flick->property("interactive").toBool(),
                 flick->property("contentHeight").toReal() > flick->height());

        QList<QQuickItem *> rows = headingRowsOf(m_panel);
        QCOMPARE(rows.size(), 1);
        QQuickItem *title = titleFieldOf(rows.first());
        QVERIFY(title);
        qWarning("TITLE scene y=%.1f h=%.1f", title->mapToScene(QPointF(0, 0)).y(), title->height());

        const QPoint pt = scenePt(title, 12, title->height() / 2);
        qWarning("CLICK title at scene (%d,%d)", pt.x(), pt.y());
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, pt);
        QTest::qWait(50);

        qWarning("AFTER  editingChapterTs=%.1f titleFocus=%d titleEditing=%d areaFocus=%d areaCursor=%d",
                 m_panel->property("editingChapterTs").toReal(), title->hasActiveFocus(),
                 title->property("editing").toBool(), m_area->hasActiveFocus(),
                 m_area->property("cursorPosition").toInt());
        dumpMessages("click-scroll");

        QVERIFY2(m_panel->property("editingChapterTs").toReal() == 5.5,
                 "BUG 1: a real click on the chapter title did not open it for editing");
    }

    // "Editable" as the USER means it: click, then type, and see the title change.
    void clickThenType()
    {
        QQuickView view;
        QVERIFY(openPanel(&view, basicSentences(), QVariantList{chapter(3, QStringLiteral("Chapter One"))}));
        QQuickItem *title = titleFieldOf(headingRowsOf(m_panel).first());
        QVERIFY(title);

        // Click at the very END of the text (past the last character), which is
        // what Layout.fillWidth is supposed to make reachable.
        const qreal cw = title->property("contentWidth").toReal();
        for (const auto &spot : QList<QPair<const char *, qreal>>{
                 {"on-text", 12}, {"past-end-of-text", cw + 60}}) {
            m_panel->setProperty("editingChapterTs", -1);
            QTest::qWait(20);
            const QPoint pt = scenePt(title, spot.second, title->height() / 2);
            QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, pt);
            QTest::qWait(50);
            qWarning("[%s] click x=%.0f -> editingTs=%.1f editing=%d focus=%d readOnly=%d cursor=%d",
                     spot.first, spot.second, m_panel->property("editingChapterTs").toReal(),
                     title->property("editing").toBool(), title->hasActiveFocus(),
                     title->property("readOnly").toBool(), title->property("cursorPosition").toInt());

            QTest::keyClick(&view, 'X');
            QTest::qWait(50);
            qWarning("[%s] after typing 'X': text='%s' editingTs=%.1f", spot.first,
                     qPrintable(title->property("text").toString()),
                     m_panel->property("editingChapterTs").toReal());
        }
        dumpMessages("type");
    }

    // BUG 2, rendered: selectedText being right proves only that select() ran.
    // Grab the frame and look at the pixels behind the title to see whether the
    // mirrored highlight is actually DRAWN.
    void sweptTitle_rendersAHighlight()
    {
        QQuickView view;
        QVERIFY(openPanel(&view, basicSentences(), QVariantList{chapter(3, QStringLiteral("Chapter One"))}));
        QQuickItem *title = titleFieldOf(headingRowsOf(m_panel).first());
        QVERIFY(title);

        qWarning("TITLE selectionColor=%s selectedTextColor=%s focus=%d",
                 qPrintable(title->property("selectionColor").value<QColor>().name()),
                 qPrintable(title->property("selectedTextColor").value<QColor>().name()),
                 title->hasActiveFocus());

        auto titleHistogram = [&](const char *tag) {
            const QImage img = view.grabWindow();
            const QPointF o = title->mapToScene(QPointF(0, 0));
            QHash<QRgb, int> hist;
            for (int y = o.y(); y < o.y() + title->height(); ++y)
                for (int x = o.x(); x < o.x() + title->property("contentWidth").toReal(); ++x)
                    if (img.valid(x, y))
                        ++hist[img.pixel(x, y)];
            QList<QPair<int, QRgb>> top;
            for (auto it = hist.begin(); it != hist.end(); ++it)
                top.append({it.value(), it.key()});
            std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.first > b.first; });
            QString s;
            for (int i = 0; i < qMin(4, top.size()); ++i)
                s += QStringLiteral("%1 x%2  ").arg(QColor(top[i].second).name()).arg(top[i].first);
            qWarning("  [%s] title pixels: %s", tag, qPrintable(s));
        };

        titleHistogram("no-selection");
        QMetaObject::invokeMethod(m_area, "select", Q_ARG(int, 0),
                                  Q_ARG(int, m_panel->property("displayText").toString().length()));
        QTest::qWait(50);
        qWarning("swept=%d selectedText='%s'", title->property("sweptBySelection").toBool(),
                 qPrintable(title->property("selectedText").toString()));
        titleHistogram("swept");
    }

    // BUG 2: a real drag through the transcript across the heading should
    // extend transcriptArea's selection past the heading's reserved line.
    void realDragAcrossHeading_shouldSweepTitle()
    {
        QQuickView view;
        QVERIFY(openPanel(&view, basicSentences(), QVariantList{chapter(3, QStringLiteral("Chapter One"))}));
        QQuickItem *row = headingRowsOf(m_panel).first();
        QQuickItem *title = titleFieldOf(row);
        QVERIFY(title);

        // Drag from the very start of the document to the end of "Second line".
        const QVariant r0v = m_panel->property("gutterLineRects");
        Q_UNUSED(r0v);
        const QRectF areaRect(m_area->mapToScene(QPointF(0, 0)),
                              QSizeF(m_area->width(), m_area->height()));
        qWarning("AREA scene rect %.1f %.1f %.1f %.1f", areaRect.x(), areaRect.y(),
                 areaRect.width(), areaRect.height());

        // Use positionToRectangle to find real scene points for offsets 2 and 22.
        // positionToRectangle is a real C++ Q_INVOKABLE taking int / returning
        // QRectF — Q_ARG(QVariant,...) silently fails to resolve it.
        auto pointForOffset = [&](int offset) {
            QRectF r;
            const bool ok = QMetaObject::invokeMethod(m_area, "positionToRectangle",
                                                      Q_RETURN_ARG(QRectF, r), Q_ARG(int, offset));
            if (!ok)
                qWarning("positionToRectangle(%d) INVOKE FAILED", offset);
            return scenePt(m_area, r.x() + 1, r.y() + r.height() / 2);
        };

        const QPoint start = pointForOffset(2);
        const QPoint end = pointForOffset(20);
        qWarning("DRAG %d,%d -> %d,%d", start.x(), start.y(), end.x(), end.y());

        // Spies on the two things that would reset a drag anchor: the document
        // being re-assigned, and the area losing focus.
        QSignalSpy textSpy(m_area, SIGNAL(textChanged()));
        QSignalSpy focusSpy(m_area, SIGNAL(activeFocusChanged(bool)));

        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, start);
        QTest::qWait(20);
        // Step through in fine increments, sampling as we cross the heading.
        const int kSteps = 40;
        for (int i = 1; i <= kSteps; ++i) {
            const QPoint p(start.x() + (end.x() - start.x()) * i / kSteps,
                           start.y() + (end.y() - start.y()) * i / kSteps);
            QTest::mouseMove(&view, p);
            QTest::qWait(10);
            qWarning("  mid i=%2d p=(%d,%d) sel=[%d,%d] cur=%d areaFocus=%d textChg=%lld focusChg=%lld titleSel='%s'",
                     i, p.x(), p.y(), m_area->property("selectionStart").toInt(),
                     m_area->property("selectionEnd").toInt(),
                     m_area->property("cursorPosition").toInt(), m_area->hasActiveFocus(),
                     (long long)textSpy.count(), (long long)focusSpy.count(),
                     qPrintable(title->property("selectedText").toString()));
        }
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, end);
        QTest::qWait(50);

        const int selStart = m_area->property("selectionStart").toInt();
        const int selEnd = m_area->property("selectionEnd").toInt();
        QVariant sweeps;
        QMetaObject::invokeMethod(m_panel, "selectionSweepsHeading", Q_RETURN_ARG(QVariant, sweeps),
                                  Q_ARG(QVariant, QVariant(12)));
        qWarning("AFTER DRAG selStart=%d selEnd=%d selectedText='%s' sweeps12=%d titleSel='%s' titleSwept=%d",
                 selStart, selEnd, qPrintable(m_area->property("selectedText").toString()),
                 sweeps.toBool(), qPrintable(title->property("selectedText").toString()),
                 title->property("sweptBySelection").toBool());
        dumpMessages("drag");

        QVERIFY2(title->property("selectedText").toString() == QStringLiteral("Chapter One"),
                 "BUG 2: a real drag across the heading did not highlight the title");
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_scratch_headingrepro"));
    QGuiApplication app(argc, argv);
    g_prevHandler = qInstallMessageHandler(capturingHandler);
    TstScratchHeadingRepro tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_scratch_headingrepro.moc"
