// Living spec + runtime verification for the Benchmark page's audio-file
// picker. Loads the REAL BenchmarkPage.qml inside a ScrollView inside a
// `modal: true` Popup — the exact nesting it has in SettingsPopup.qml — against
// a real VaultTreeModel, and proves the file ComboBox is actually POPULATED and
// SELECTABLE once the page shows. Same QQuickView harness shape as
// tst_sidebar_expand.cpp (read its top-of-file comment first).
//
// This file exists because the original bug was invisible to a source review
// and to a green build: the combo bound `model:` straight to
// vaultTreeModel.allAudioPaths(), a Q_INVOKABLE with NO NOTIFY signal. A
// declarative binding on it evaluates exactly once — when BenchmarkPage is
// created, which is at app startup while SettingsPopup is built, long before
// any vault is loaded — against an empty list, and then never re-evaluates. The
// dropdown was permanently empty, so "the audio file selector doesn't work."
// BenchmarkPage now gathers the list imperatively (refreshAudioFiles(), driven
// by onVisibleChanged) instead; comboIsPopulated_whenPageShows is what keeps
// that honest, and would fail against the old declarative binding.
//
// NOTE on the offscreen QPA plugin: a ComboBox popup's ListView does not
// realize its delegate rows on its own here (no live render/polish loop), so
// itemAtIndex(0) is used to force the first row into existence before clicking
// it — that's a test-environment accommodation, not part of the behavior under
// test.

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

// The Basic style's ComboBox / ItemDelegate are QML types
// ("ComboBox_QMLTYPE_*", "ItemDelegate_QMLTYPE_*"), not the bare C++
// QQuick* classes — so they're matched by QML type-name prefix.
QQuickItem *findByClassNamePrefix(QQuickItem *root, const QByteArray &prefix)
{
    for (QQuickItem *child : root->findChildren<QQuickItem *>()) {
        if (QByteArray(child->metaObject()->className()).startsWith(prefix))
            return child;
    }
    return nullptr;
}

} // namespace

class TstBenchmarkPageFileCombo : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    void buildModule();

    // Loads the driver, returns the ComboBox (or fails). Also hands back the
    // BenchmarkPage and the enclosing modal Popup.
    QQuickItem *load(QQuickView *view, const QString &vaultPath, QQuickItem **benchOut,
                     QObject **settingsOut)
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
        QQuickItem *bench = root->findChild<QQuickItem *>(QStringLiteral("bench"));
        // The enclosing Settings Popup is a QQuickPopup (a QObject, NOT a
        // QQuickItem) — find it as a QObject.
        QObject *settings = root->findChild<QObject *>(QStringLiteral("settings"));
        if (benchOut)
            *benchOut = bench;
        if (settingsOut)
            *settingsOut = settings;
        return bench ? findByClassNamePrefix(bench, "ComboBox") : nullptr;
    }

private slots:
    void initTestCase() { buildModule(); }

    // The headline claim: with a vault already loaded, opening the page fills
    // the combo. Against the old declarative allAudioPaths() binding this
    // evaluated too early (empty vault) and stuck at count 0 forever.
    void comboIsPopulated_whenPageShows()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("one.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("two.wav"))));

        QQuickView view;
        QQuickItem *bench = nullptr;
        QObject *settings = nullptr;
        QQuickItem *combo = load(&view, vault.path(), &bench, &settings);
        QVERIFY2(combo, "could not load BenchmarkPage / locate its ComboBox");
        QVERIFY(bench && settings);

        QTRY_COMPARE(combo->property("count").toInt(), 2);
    }

    // The follow-through: clicking a dropdown row selects that file AND leaves
    // the enclosing modal Settings popup open (a nested-popup click must not be
    // read as a press-outside that closes the parent).
    void clickingRow_selectsFile_andKeepsSettingsOpen()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(touch(vault.filePath(QStringLiteral("one.mp3"))));
        QVERIFY(touch(vault.filePath(QStringLiteral("two.wav"))));

        QQuickView view;
        QQuickItem *bench = nullptr;
        QObject *settings = nullptr;
        QQuickItem *combo = load(&view, vault.path(), &bench, &settings);
        QVERIFY2(combo, "could not load BenchmarkPage / locate its ComboBox");
        QVERIFY(bench && settings);
        QTRY_COMPARE(combo->property("count").toInt(), 2);
        QVERIFY(settings->property("visible").toBool());

        // Open the dropdown via the real click gesture.
        const QPointF comboCenter =
            combo->mapToScene(QPointF(combo->width() / 2, combo->height() / 2));
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, comboCenter.toPoint());
        QTest::qWait(80);

        QObject *popup = combo->property("popup").value<QObject *>();
        QVERIFY2(popup, "combo has no popup object");
        QTRY_VERIFY2(popup->property("visible").toBool(),
                     "the ComboBox dropdown never became visible after clicking it");

        auto *popupContent = popup->property("contentItem").value<QQuickItem *>();
        QVERIFY(popupContent);
        // Offscreen: force row 0 into existence (see the top-of-file NOTE).
        QMetaObject::invokeMethod(popupContent, "forceLayout", Qt::DirectConnection);
        QQuickItem *firstItem = nullptr;
        QMetaObject::invokeMethod(popupContent, "itemAtIndex", Qt::DirectConnection,
                                  Q_RETURN_ARG(QQuickItem *, firstItem), Q_ARG(int, 0));
        if (!firstItem)
            firstItem = findByClassNamePrefix(popupContent, "ItemDelegate");
        QVERIFY2(firstItem, "no delegate rows found in the dropdown");

        const QPointF itemCenter =
            firstItem->mapToScene(QPointF(firstItem->width() / 2, firstItem->height() / 2));
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, itemCenter.toPoint());
        QTest::qWait(80);

        QVERIFY2(settings->property("visible").toBool(),
                 "clicking a dropdown row CLOSED the enclosing modal Settings popup");
        QVERIFY2(!bench->property("selectedFilePath").toString().isEmpty(),
                 "clicking a dropdown row did NOT set selectedFilePath");
    }
};

void TstBenchmarkPageFileCombo::buildModule()
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
    QCoreApplication::setApplicationName(QStringLiteral("tst_benchmarkpage_filecombo"));
    QGuiApplication app(argc, argv);
    TstBenchmarkPageFileCombo tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_benchmarkpage_filecombo.moc"
