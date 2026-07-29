// Living spec + runtime verification for UsbRegexField.qml — the USB grab
// rules' filename-regex editor (pattern field + live ✓/✗ validity indicator +
// insert-token toolbar), which deliberately mirrors the split-recording combine
// rules' pattern authoring UI in SettingsPopup.qml.
//
// Drives a REAL QQuickView (offscreen, sandboxed config — see main() below)
// loading the actual UsbRegexField.qml against a real UsbGrabberController.
// Same harness shape as tst_sidebar_expand.cpp / tst_sidebar_dragmove.cpp —
// read their top-of-file comments first.
//
// Why this file exists rather than leaving it to tst_styleguide.cpp's
// source-scan guard (usbGrabber_filenameRegexAuthoringAids): a green cmake
// build does NOT prove a QML file loads — AOT/qmlcachegen happily compiles a
// file that still fails at load time — and two of this component's behaviors
// are only observable at runtime:
//   - the validity indicator's text/color, which is bound through a C++
//     invokable (a JS-side reimplementation would look identical to a grep);
//   - the insert-token buttons' focusPolicy: Qt.NoFocus, whose absence a scan
//     can spot but whose PRESENCE only means something if a token click
//     actually leaves the field's text edited and its focus intact.

#include <QtTest>

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#include "UsbGrabberController.h"

class TstUsbRegexField : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // Builds the view + returns the loaded UsbRegexField, or nullptr (having
    // already reported the QML errors).
    QQuickItem *loadField(QQuickView *view)
    {
        view->engine()->addImportPath(m_moduleDir.path());
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : view->errors())
                msg += e.toString() + QStringLiteral("\n");
            // The exact failure a green cmake build does NOT catch — report it
            // verbatim rather than as a null item.
            qWarning("%s", qPrintable(msg.isEmpty() ? QStringLiteral("QML failed to load, no error text")
                                                     : msg));
            return nullptr;
        }
        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        QQuickItem *root = view->rootObject();
        return root ? root->findChild<QQuickItem *>(QStringLiteral("regexField")) : nullptr;
    }

    // Walks the VISUAL tree (childItems()), NOT QObject parentage.
    // QObject::findChild() structurally cannot see a Repeater's delegates: they
    // are owned by the internal QQmlDelegateModel and have a null QObject
    // parent — only their visual parent is set. The insert-token buttons are
    // Repeater delegates, so findChild() finds patternField/validityLabel and
    // then silently returns null for every token button.
    static QQuickItem *child(QQuickItem *root, const QString &objectName)
    {
        if (!root)
            return nullptr;
        if (root->objectName() == objectName)
            return root;
        const QList<QQuickItem *> kids = root->childItems();
        for (QQuickItem *kid : kids) {
            if (QQuickItem *found = child(kid, objectName))
                return found;
        }
        return nullptr;
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
                      "HoverBorder 1.0 qml/HoverBorder.qml\n"
                      "UnderlineField 1.0 qml/UnderlineField.qml\n"
                      "UsbRegexField 1.0 qml/UsbRegexField.qml\n");
        qmldir.close();

        qmlRegisterType<UsbGrabberController>("SuopTranscriber", 1, 0, "UsbGrabberController");

        // The field is a pure editor over `pattern` + `controller`: it never
        // saves anything itself, it emits committed() and the rule cards in
        // MainPanel.qml route that to commitRuleField(). So the driver just
        // records what it emitted.
        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    id: root\n"
            "    width: 400\n"
            "    height: 300\n"
            "    property int commitCount: 0\n"
            "    property string lastCommitted: \"\"\n"
            "    UsbGrabberController { id: usbController; objectName: \"usbController\" }\n"
            "    UsbRegexField {\n"
            "        id: regexField\n"
            "        objectName: \"regexField\"\n"
            "        anchors.fill: parent\n"
            "        controller: usbController\n"
            "        onCommitted: (p) => { root.commitCount++; root.lastCommitted = p }\n"
            "    }\n"
            "}\n");
        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driverFile(m_driverQmlPath);
        QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
        driverFile.write(driverQml.toUtf8());
        driverFile.close();
    }

    // The headline claim a source scan cannot make: the component actually
    // LOADS (no AOT-compiles-but-fails-at-runtime), and a valid pattern is
    // reported valid on screen.
    void validPattern_showsValidIndicator()
    {
        QQuickView view;
        QQuickItem *field = loadField(&view);
        QVERIFY2(field, "could not load UsbRegexField.qml");

        QQuickItem *patternField = child(field, QStringLiteral("patternField"));
        QQuickItem *validity = child(field, QStringLiteral("validityLabel"));
        QVERIFY(patternField && validity);

        // Blank = "no regex constraint", not an error — nothing to say yet.
        QVERIFY(!validity->isVisible());

        patternField->setProperty("text", QStringLiteral("^REC_(\\d{4})\\.wav$"));
        QTRY_VERIFY(validity->isVisible());
        QCOMPARE(validity->property("validationError").toString(), QString());
        QVERIFY(validity->property("text").toString().startsWith(QStringLiteral("✓")));
    }

    // An invalid pattern must be reported through the C++ engine that will
    // actually filter (UsbGrabberController::regexValidationError ->
    // QRegularExpression), carrying Qt's own message — and must say that the
    // rule now grabs NOTHING, since UsbGrabJob fails closed on it.
    void invalidPattern_showsQtErrorAndGrabsNothingWarning()
    {
        QQuickView view;
        QQuickItem *field = loadField(&view);
        QVERIFY2(field, "could not load UsbRegexField.qml");

        QQuickItem *patternField = child(field, QStringLiteral("patternField"));
        QQuickItem *validity = child(field, QStringLiteral("validityLabel"));
        QQuickItem *failsClosed = child(field, QStringLiteral("failsClosedWarning"));
        QVERIFY(patternField && validity && failsClosed);

        patternField->setProperty("text", QStringLiteral("(unterminated["));
        QTRY_VERIFY(!validity->property("validationError").toString().isEmpty());
        QVERIFY(validity->property("text").toString().startsWith(QStringLiteral("✗")));
        QVERIFY2(failsClosed->isVisible(),
                 "an invalid pattern matches nothing at grab time — the field must say the rule "
                 "currently grabs nothing, not merely that the regex is malformed");

        // Recovers: fixing the pattern clears both.
        patternField->setProperty("text", QStringLiteral("^REC_"));
        QTRY_VERIFY(validity->property("validationError").toString().isEmpty());
        QVERIFY(!failsClosed->isVisible());
    }

    // The validity indicator must agree with the engine the grab filters with,
    // for patterns where JS RegExp and QRegularExpression genuinely disagree.
    // A lone unescaped ']' is legal in JS (it's a literal) but is NOT accepted
    // as a bare quantifier target here — the point is simply that this answer
    // comes from Qt, so it matches what UsbGrabJob will do, whatever Qt says.
    void validityAnswerComesFromTheSameEngineAsTheGrab()
    {
        QQuickView view;
        QQuickItem *field = loadField(&view);
        QVERIFY2(field, "could not load UsbRegexField.qml");

        QQuickItem *patternField = child(field, QStringLiteral("patternField"));
        QQuickItem *validity = child(field, QStringLiteral("validityLabel"));
        QObject *controller = view.rootObject()->findChild<QObject *>(QStringLiteral("usbController"));
        QVERIFY(patternField && validity && controller);

        const QString pattern = QStringLiteral("a{2,1}"); // min > max
        patternField->setProperty("text", pattern);

        QString fromCpp;
        QVERIFY(QMetaObject::invokeMethod(controller, "regexValidationError", Qt::DirectConnection,
                                           Q_RETURN_ARG(QString, fromCpp), Q_ARG(QString, pattern)));
        // Whatever Qt's verdict is, the label must be showing exactly it —
        // never a second opinion computed in JS.
        QTRY_COMPARE(validity->property("validationError").toString(), fromCpp);
    }

    // focusPolicy: Qt.NoFocus on the token buttons, verified by consequence
    // rather than by grep: clicking one must leave the pattern edited AND the
    // field still focused. Without NoFocus the press steals focus, which in
    // MainPanel.qml fires onEditingFinished -> commitRuleField() -> the rule
    // store is re-read and deviceRows reassigned, destroying the button
    // mid-click so onClicked never lands.
    void tokenButton_insertsWithoutStealingFocusOrCommitting()
    {
        QQuickView view;
        QQuickItem *field = loadField(&view);
        QVERIFY2(field, "could not load UsbRegexField.qml");

        QQuickItem *patternField = child(field, QStringLiteral("patternField"));
        QVERIFY(patternField);
        patternField->forceActiveFocus();
        QTRY_VERIFY(patternField->hasActiveFocus());
        patternField->setProperty("text", QStringLiteral("^REC_"));
        QMetaObject::invokeMethod(patternField, "forceActiveFocus");

        QQuickItem *digits = child(field, QStringLiteral("tokenButton_0")); // "Digits" -> \d+
        QVERIFY2(digits, "the insert-token toolbar's first button must exist");

        const int commitsBefore = view.rootObject()->property("commitCount").toInt();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                          digits->mapToScene(QPointF(digits->width() / 2, digits->height() / 2)).toPoint());

        QTRY_COMPARE(patternField->property("text").toString(), QStringLiteral("^REC_\\d+"));
        QVERIFY2(patternField->hasActiveFocus(),
                 "the token button must not take focus — typing has to continue straight after an "
                 "insert, and a focus steal here would commit the rule mid-click");
        QCOMPARE(view.rootObject()->property("commitCount").toInt(), commitsBefore);
    }

    // Commit is on editing-finished (Enter/focus-out), the same per-field save
    // path every other rule field uses — never per keystroke, because a commit
    // re-runs a preview that walks a physical device.
    void editingFinished_emitsCommittedWithTypedPattern()
    {
        QQuickView view;
        QQuickItem *field = loadField(&view);
        QVERIFY2(field, "could not load UsbRegexField.qml");

        QQuickItem *patternField = child(field, QStringLiteral("patternField"));
        QVERIFY(patternField);
        patternField->forceActiveFocus();
        QTRY_VERIFY(patternField->hasActiveFocus());

        // Typed one key at a time: QTest::keyClicks() is QWidget-only (there's
        // no QWindow overload), and this harness is deliberately widget-free.
        for (const Qt::Key key : {Qt::Key_R, Qt::Key_E, Qt::Key_C})
            QTest::keyClick(&view, key);
        QCOMPARE(patternField->property("text").toString(), QStringLiteral("rec"));
        QCOMPARE(view.rootObject()->property("commitCount").toInt(), 0); // never per keystroke

        QTest::keyClick(&view, Qt::Key_Return);
        QTRY_COMPARE(view.rootObject()->property("commitCount").toInt(), 1);
        QCOMPARE(view.rootObject()->property("lastCommitted").toString(), QStringLiteral("rec"));
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppSettings.qml's Settings{} element
    // is loaded transitively (UnderlineField/HoverBorder bind it) and must
    // never touch the real ~/.config/suopTranscriber (tests must never touch
    // the user's real vault or app config). UsbGrabberController
    // also persists its rules app-scoped via QSettings, so the same isolation
    // keeps this harness off the user's real grab rules. Same setup as
    // tst_sidebar_expand.cpp.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_usbregexfield"));
    QGuiApplication app(argc, argv);
    TstUsbRegexField tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_usbregexfield.moc"
