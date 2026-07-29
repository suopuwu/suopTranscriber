// Living spec + runtime verification for TextRedoShortcut.qml — the window-wide
// Ctrl+Y => redo alias for text boxes.
//
// Same harness shape as tst_middledragscroll.cpp (read its top-of-file comment
// first): a REAL offscreen QQuickView loading the actual checked-out qml/,
// driven with real QTest KEY events. The whole claim is that the Ctrl+Y chord
// is ROUTED to the focused text item's redo() on Linux (where Qt maps redo to
// Ctrl+Shift+Z only), so every test presses real keys rather than calling
// redo() directly.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QUrl>

class TstTextRedoShortcut : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;
    QQuickView *m_view = nullptr;

    QQuickItem *byName(const QString &n) const
    {
        return m_view->rootObject()->findChild<QQuickItem *>(n);
    }

    // QTest::keyClicks is QtWidgets-only; for a QWindow send each char. A
    // no-modifier keyClick with a latin1 char types that character.
    void typeString(const QString &text)
    {
        for (const QChar &c : text)
            QTest::keyClick(m_view, c.toLatin1(), Qt::NoModifier);
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
                     "TextRedoShortcut 1.0 qml/TextRedoShortcut.qml\n");
        qmldir.close();

        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import QtQuick.Controls\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    width: 400\n"
            "    height: 300\n"
            "    TextRedoShortcut { objectName: \"redoShortcut\" }\n"
            "    Column {\n"
            "        anchors.fill: parent\n"
            "        TextField { objectName: \"field\"; width: 400 }\n"
            "        TextArea { objectName: \"area\"; width: 400; height: 100 }\n"
            "        Button { objectName: \"button\"; text: \"b\" }\n"
            "    }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driverFile(m_driverQmlPath);
        QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
        driverFile.write(driverQml.toUtf8());
        driverFile.close();
    }

    void init()
    {
        m_view = new QQuickView;
        m_view->engine()->addImportPath(m_moduleDir.path());
        m_view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (m_view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : m_view->errors())
                msg += e.toString() + QStringLiteral("\n");
            qWarning("%s", qPrintable(msg.isEmpty() ? QStringLiteral("QML failed to load, no error text") : msg));
        }
        QCOMPARE(m_view->status(), QQuickView::Ready);
        m_view->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_view));
    }

    void cleanup()
    {
        delete m_view;
        m_view = nullptr;
    }

    void ctrlY_redoesInTextField()
    {
        QQuickItem *field = byName(QStringLiteral("field"));
        QVERIFY(field);
        field->forceActiveFocus();
        QVERIFY(field->hasActiveFocus());

        typeString(QStringLiteral("hello"));
        QCOMPARE(field->property("text").toString(), QStringLiteral("hello"));

        QTest::keyClick(m_view, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY2(field->property("text").toString() != QStringLiteral("hello"),
                 "Ctrl+Z must undo the typing");

        QTest::keyClick(m_view, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(field->property("text").toString(), QStringLiteral("hello"));
    }

    void ctrlY_redoesInTextArea()
    {
        QQuickItem *area = byName(QStringLiteral("area"));
        QVERIFY(area);
        area->forceActiveFocus();
        QVERIFY(area->hasActiveFocus());

        typeString(QStringLiteral("world"));
        QCOMPARE(area->property("text").toString(), QStringLiteral("world"));

        QTest::keyClick(m_view, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(area->property("text").toString() != QStringLiteral("world"));

        QTest::keyClick(m_view, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(area->property("text").toString(), QStringLiteral("world"));
    }

    // The native Linux redo chord must keep working — this adds an alias, it
    // does not replace one.
    void ctrlShiftZ_stillRedoes()
    {
        QQuickItem *field = byName(QStringLiteral("field"));
        QVERIFY(field);
        field->forceActiveFocus();

        typeString(QStringLiteral("abc"));
        QTest::keyClick(m_view, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(field->property("text").toString() != QStringLiteral("abc"));
        QTest::keyClick(m_view, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(field->property("text").toString(), QStringLiteral("abc"));
    }

    // Anti-ambiguity guard: with a non-text item focused the shortcut must be
    // disabled, so it can never fire at the same time as MainPanel's own
    // panel-level Ctrl+Y (which Qt would then report as ambiguous, firing
    // neither).
    void shortcutDisabledWhenFocusItemHasNoRedo()
    {
        QQuickItem *button = byName(QStringLiteral("button"));
        QQuickItem *redoShortcut = byName(QStringLiteral("redoShortcut"));
        QVERIFY(button && redoShortcut);
        button->forceActiveFocus();
        QVERIFY(button->hasActiveFocus());
        QVERIFY2(!redoShortcut->property("canRedoHere").toBool(),
                 "canRedoHere must be false when the focus item exposes no redo()");
    }

    // The positive half: focusing a real text field re-evaluates the
    // Window.window.activeFocusItem binding and enables the shortcut.
    void shortcutEnabledWhenTextFieldFocused()
    {
        QQuickItem *field = byName(QStringLiteral("field"));
        QQuickItem *redoShortcut = byName(QStringLiteral("redoShortcut"));
        QVERIFY(field && redoShortcut);
        field->forceActiveFocus();
        QVERIFY(field->hasActiveFocus());
        QTRY_VERIFY2(redoShortcut->property("canRedoHere").toBool(),
                     "canRedoHere must become true when a TextField holds focus");
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_textredoshortcut"));
    QGuiApplication app(argc, argv);
    TstTextRedoShortcut tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_textredoshortcut.moc"
