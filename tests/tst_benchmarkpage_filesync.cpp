// Living spec + runtime verification for the Benchmark page's file selection.
// Loads the REAL BenchmarkPage.qml inside a ScrollView inside a `modal: true`
// Popup — the exact nesting it has in SettingsPopup.qml — against a real
// VaultTreeModel. Same QQuickView harness shape as tst_sidebar_expand.cpp (read
// its top-of-file comment first).
//
// This replaces the old tst_benchmarkpage_filecombo: the in-page audio-file
// ComboBox was REMOVED. The benchmark file now always follows the left panel's
// current file (BenchmarkPage.currentFilePath, fed from MainPanel), so clicking
// a file in the tree switches what gets benchmarked without a redundant picker.
// These tests keep that honest:
//   1. selectedFilePath follows currentFilePath — i.e. a left-panel selection
//      switches the benchmark file automatically, and while the page is hidden.
//   2. there is NO file-picker ComboBox left in the page.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QUrl>

#include "VaultTreeModel.h"

namespace {

bool touch(const QString &path, const QByteArray &content = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content);
    return true;
}

// BenchmarkPage.qml guards every transcriptionController use on it being
// non-null; this harness never assigns one, so the property only needs its
// TYPE to resolve. Same trick as the sidebar harnesses.
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
};

// The Basic style's ComboBox is a QML type ("ComboBox_QMLTYPE_*"), not the bare
// C++ QQuickComboBox — matched by QML type-name prefix.
QQuickItem *findByClassNamePrefix(QQuickItem *root, const QByteArray &prefix)
{
    for (QQuickItem *child : root->findChildren<QQuickItem *>()) {
        if (QByteArray(child->metaObject()->className()).startsWith(prefix))
            return child;
    }
    return nullptr;
}

} // namespace

class TstBenchmarkPageFileSync : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    void buildModule();

    QQuickItem *load(QQuickView *view, const QString &vaultPath, QObject **settingsOut)
    {
        view->engine()->addImportPath(m_moduleDir.path());
        view->rootContext()->setContextProperty(QStringLiteral("vaultUrl"),
                                                 QUrl::fromLocalFile(vaultPath));
        view->setSource(QUrl::fromLocalFile(m_driverQmlPath));
        if (view->status() != QQuickView::Ready) {
            QString msg;
            for (const auto &e : view->errors())
                msg += e.toString() + QStringLiteral("\n");
            qWarning("%s", qPrintable(msg.isEmpty() ? QStringLiteral("QML failed to load") : msg));
            return nullptr;
        }
        view->resize(600, 600);
        view->show();
        if (!QTest::qWaitForWindowExposed(view))
            return nullptr;
        QQuickItem *root = view->rootObject();
        if (!root)
            return nullptr;
        if (settingsOut)
            *settingsOut = root->findChild<QObject *>(QStringLiteral("settings"));
        return root->findChild<QQuickItem *>(QStringLiteral("bench"));
    }

private slots:
    void initTestCase() { buildModule(); }

    // The headline claim: setting currentFilePath (what a left-panel selection
    // does) switches the benchmarked file — selectedFilePath mirrors it, even
    // before the page is shown.
    void selectedFilePathFollowsCurrentFilePath()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("one.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("two.wav"))));

        QQuickView view;
        QObject *settings = nullptr;
        QQuickItem *bench = load(&view, vault.path(), &settings);
        QVERIFY2(bench, "could not load BenchmarkPage");
        QVERIFY(settings);

        const QString first = vault.filePath(QStringLiteral("one.mp3"));
        bench->setProperty("currentFilePath", first);
        QTRY_COMPARE(bench->property("selectedFilePath").toString(), first);

        // Switching the left-panel file switches the benchmark file too.
        const QString second = vault.filePath(QStringLiteral("two.wav"));
        bench->setProperty("currentFilePath", second);
        QTRY_COMPARE(bench->property("selectedFilePath").toString(), second);
    }

    // The redundant in-page file picker is gone: no ComboBox is left in the
    // page (its job is served by the left panel now).
    void noFilePickerComboBox()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("one.mp3"))));

        QQuickView view;
        QObject *settings = nullptr;
        QQuickItem *bench = load(&view, vault.path(), &settings);
        QVERIFY2(bench, "could not load BenchmarkPage");

        QVERIFY2(!findByClassNamePrefix(bench, "ComboBox"),
                 "the in-page file-picker ComboBox must be gone — the benchmark file follows the "
                 "left-panel selection (currentFilePath) now");
    }
};

void TstBenchmarkPageFileSync::buildModule()
{
    if (!m_driverQmlPath.isEmpty())
        return;
    QVERIFY(m_moduleDir.isValid());
    const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
    QVERIFY(QDir().mkpath(suopDir));
    // Symlink (not copy) to the real checked-out qml/ — must reflect what's on
    // disk right now, never a stale build-time copy.
    QVERIFY2(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")),
             "failed to symlink qml/ into the scratch QML module dir");

    QFile qmldir(suopDir + QStringLiteral("/qmldir"));
    QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
    qmldir.write("module SuopTranscriber\n"
                  "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                  "singleton Notifications 1.0 qml/Notifications.qml\n"
                  "HoverBorder 1.0 qml/HoverBorder.qml\n"
                  "BenchmarkPage 1.0 qml/BenchmarkPage.qml\n");
    qmldir.close();

    qmlRegisterType<VaultTreeModel>("SuopTranscriber", 1, 0, "VaultTreeModel");
    qmlRegisterUncreatableType<TranscriptionControllerStub>(
        "SuopTranscriber", 1, 0, "TranscriptionController",
        QStringLiteral("stub — this harness never assigns one"));

    // Mirrors the app's real nesting: a modal Popup -> ScrollView -> the page.
    const QString driverQml = QStringLiteral(
        "import QtQuick\n"
        "import QtQuick.Controls\n"
        "import SuopTranscriber\n"
        "Item {\n"
        "    id: root\n"
        "    width: 600\n"
        "    height: 600\n"
        "    VaultTreeModel { id: vaultModel; objectName: \"vaultModel\"\n"
        "                     vaultPath: Qt.resolvedUrl(vaultUrl) }\n"
        "    Popup {\n"
        "        id: settings\n"
        "        objectName: \"settings\"\n"
        "        modal: true\n"
        "        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside\n"
        "        x: 40; y: 40; width: 500; height: 500\n"
        "        Component.onCompleted: open()\n"
        "        contentItem: ScrollView {\n"
        "            id: scrollArea\n"
        "            clip: true\n"
        "            BenchmarkPage {\n"
        "                objectName: \"bench\"\n"
        "                width: scrollArea.availableWidth\n"
        "                transcriptionController: null\n"
        "                vaultTreeModel: vaultModel\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n");
    m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
    QFile driverFile(m_driverQmlPath);
    QVERIFY(driverFile.open(QIODevice::WriteOnly | QIODevice::Text));
    driverFile.write(driverQml.toUtf8());
    driverFile.close();
}

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: BenchmarkPage.qml transitively loads
    // AppSettings.qml's Settings{} element, which must never touch the real
    // ~/.config/suopTranscriber (CLAUDE.md). Same setup as tst_sidebar_expand.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_benchmarkpage_filesync"));
    QGuiApplication app(argc, argv);
    TstBenchmarkPageFileSync tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_benchmarkpage_filesync.moc"
