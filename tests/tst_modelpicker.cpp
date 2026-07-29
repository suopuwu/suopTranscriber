// Living spec + runtime verification for AppComboBox.qml's currentIndex —
// the arbiter for the "model pickers snap back to the first entry on
// restart" bug. AppComboBox is the shared combo used by the MainPanel model
// pickers AND the Recorder page's mic/system-source device pickers (item
// 034 generalized it from the model-only ModelComboBox.qml), so this harness
// also covers the labelRole indirection those device lists need.
//
// Same harness shape as tst_middledragscroll.cpp / tst_sidebar_expand.cpp
// (read their top-of-file comments first): a REAL offscreen QQuickView
// loading the actual checked-out qml/, so this exercises the genuine
// QQuickComboBox::setModel() behavior rather than a guess about it.
//
// The central claim source review alone can't settle: QQuickComboBox's
// documented default is that assigning a model AFTER the combo is
// component-complete force-resets currentIndex to 0 whenever count > 0. This
// file's headline test, selectionSurvivesLateModelAssignment, is the arbiter
// for that — it drives exactly the sequence MainPanel does (empty list at
// build time, the real list assigned a tick later) and must FAIL on the
// unmodified AppComboBox.qml.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace {

QVariantMap modelEntry(const QString &id, const QString &label, bool downloaded)
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("label")] = label;
    m[QStringLiteral("downloaded")] = downloaded;
    return m;
}

// Device-shaped entry ({id, name}, no downloaded field) — what
// availableMicDevices()/systemSources actually hand AppComboBox, exercised
// via labelRole: "name" instead of the model lists' default "label".
QVariantMap deviceEntry(const QString &id, const QString &name)
{
    QVariantMap m;
    m[QStringLiteral("id")] = id;
    m[QStringLiteral("name")] = name;
    return m;
}

} // namespace

class TstModelPicker : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;
    QQuickView *m_view = nullptr;

    QQuickItem *root() const { return m_view->rootObject(); }
    QQuickItem *combo() const { return root()->findChild<QQuickItem *>(QStringLiteral("combo")); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        // Symlink (not copy) to the real checked-out qml/ — must reflect
        // what's on disk right now, never a stale build-time copy.
        QVERIFY2(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")),
                 "failed to symlink qml/ into the test's scratch QML module dir");

        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
        // AppComboBox.qml needs AppSettings (borderRadius), Theme (colors)
        // and HoverBorder (its hover ring) from the module — nothing else
        // (it never touches AppIcon/IconTheme).
        qmldir.write("module SuopTranscriber\n"
                     "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                     "singleton Theme 1.0 qml/Theme.qml\n"
                     "HoverBorder 1.0 qml/HoverBorder.qml\n"
                     "AppComboBox 1.0 qml/AppComboBox.qml\n");
        qmldir.close();

        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    id: root\n"
            "    width: 300\n"
            "    height: 100\n"
            "    property var modelList: []\n"
            "    property string selId: \"\"\n"
            "    property string labelRole: \"label\"\n"
            "    AppComboBox {\n"
            "        id: combo\n"
            "        objectName: \"combo\"\n"
            "        model: root.modelList\n"
            "        selectedId: root.selId\n"
            "        labelRole: root.labelRole\n"
            "        onPicked: (id) => root.selId = id\n"
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
            // A QML error here is the exact failure mode a green cmake build
            // does NOT catch (AOT/qmlcachegen can compile a file that still
            // fails to load) — report it verbatim rather than as "null item".
            qWarning("%s", qPrintable(msg.isEmpty() ? QStringLiteral("QML failed to load, no error text") : msg));
        }
        QCOMPARE(m_view->status(), QQuickView::Ready);
        m_view->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_view));
        QVERIFY(combo());
    }

    void cleanup()
    {
        delete m_view;
        m_view = nullptr;
    }

    // THE arbiter. Empty list at completion (like MainPanel's build-time []),
    // then selectedId lands on a saved-but-not-yet-visible id, THEN the real
    // list is assigned — exactly MainPanel's Component.onCompleted /
    // onModelsChanged sequence. QQuickComboBox::setModel()'s force-reset
    // fires on this THIRD step, after the combo is long since complete.
    // Fails on the unmodified AppComboBox.qml (Qt's reset wins); passes
    // once onModelChanged re-asserts currentIndex via Qt.binding (change A).
    void selectionSurvivesLateModelAssignment()
    {
        QCOMPARE(combo()->property("currentIndex").toInt(), -1);

        QVERIFY(root()->setProperty("selId", QStringLiteral("b")));

        const QVariantList list{modelEntry(QStringLiteral("a"), QStringLiteral("A"), true),
                                modelEntry(QStringLiteral("b"), QStringLiteral("B"), true),
                                modelEntry(QStringLiteral("c"), QStringLiteral("C"), false)};
        QVERIFY(root()->setProperty("modelList", list));

        QTRY_COMPARE(combo()->property("currentIndex").toInt(), 1);
        QCOMPARE(combo()->property("currentValue").toString(), QStringLiteral("b"));
    }

    // The onModelsChanged re-fetch case: the list is already populated and
    // selectedId sits on a non-first entry, then a DIFFERENT array of the
    // SAME length replaces it (MainPanel's refreshModelLists() reassigning a
    // fresh 6-entry array over the old 6-entry one) — `count` alone can never
    // catch this, since it never changes. The selected id also moves to a
    // different index in the new array, so this pins that resolveIndex()
    // re-searches the NEW model rather than just leaving the old index alone.
    void selectionSurvivesSameLengthModelReplacement()
    {
        const QVariantList list1{modelEntry(QStringLiteral("a"), QStringLiteral("A"), true),
                                 modelEntry(QStringLiteral("b"), QStringLiteral("B"), true),
                                 modelEntry(QStringLiteral("c"), QStringLiteral("C"), false)};
        QVERIFY(root()->setProperty("modelList", list1));
        QVERIFY(root()->setProperty("selId", QStringLiteral("b")));
        QTRY_COMPARE(combo()->property("currentIndex").toInt(), 1);

        const QVariantList list2{modelEntry(QStringLiteral("x"), QStringLiteral("X"), false),
                                 modelEntry(QStringLiteral("c"), QStringLiteral("C"), false),
                                 modelEntry(QStringLiteral("b"), QStringLiteral("B"), true)};
        QVERIFY(root()->setProperty("modelList", list2));

        QTRY_COMPARE(combo()->property("currentIndex").toInt(), 2);
        QCOMPARE(combo()->property("currentValue").toString(), QStringLiteral("b"));
    }

    // Pins the existing intended fallback so the fix doesn't reintroduce an
    // empty box: an unknown/stale selectedId against a populated list falls
    // back to the FIRST entry, never -1.
    void unknownIdStillFallsBackToFirstEntry()
    {
        const QVariantList list{modelEntry(QStringLiteral("a"), QStringLiteral("A"), true),
                                modelEntry(QStringLiteral("b"), QStringLiteral("B"), true)};
        QVERIFY(root()->setProperty("modelList", list));
        QVERIFY(root()->setProperty("selId", QStringLiteral("b")));
        QTRY_COMPARE(combo()->property("currentIndex").toInt(), 1);

        QVERIFY(root()->setProperty("selId", QStringLiteral("nope")));
        QTRY_COMPARE(combo()->property("currentIndex").toInt(), 0);
        QCOMPARE(combo()->property("currentValue").toString(), QStringLiteral("a"));
    }

    // The Recorder page's device pickers reuse this same component
    // via labelRole: "name" — the device lists ({id, name}, no downloaded
    // field) don't share the model lists' "label" key. Proves labelRole
    // drives the closed box AND that the late-model-assignment hardening
    // covers device-shaped lists too; the harness's init() already
    // fails loudly on a QML load error, so this test also proves the
    // delegate's `downloaded !== undefined` guard doesn't throw when the
    // field is simply absent.
    void deviceShapedListUsesLabelRole()
    {
        QVERIFY(root()->setProperty("labelRole", QStringLiteral("name")));

        const QVariantList list{deviceEntry(QStringLiteral("m1"), QStringLiteral("Built-in Audio")),
                                deviceEntry(QStringLiteral("m2"), QStringLiteral("USB Mic"))};
        QVERIFY(root()->setProperty("modelList", list));

        QTRY_COMPARE(combo()->property("currentText").toString(), QStringLiteral("Built-in Audio"));

        QVERIFY(root()->setProperty("selId", QStringLiteral("m2")));
        QVERIFY(root()->setProperty("modelList", list));
        QTRY_COMPARE(combo()->property("currentIndex").toInt(), 1);
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppComboBox.qml loads
    // AppSettings.qml (Settings{} element) transitively, and must never touch
    // the real ~/.config/suopTranscriber (tests must never touch the user's
    // real vault or app config). Same setup as tst_sidebar_expand.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_modelpicker"));
    QGuiApplication app(argc, argv);
    TstModelPicker tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_modelpicker.moc"
