// Living spec + runtime proof for the settings persistence chain
// (SettingsPopup control -> AppSettings.<prop> -> onXChanged -> scheduleSave()
// -> saveTimer (250ms) -> saveToVault() -> VaultTreeModel::writeVaultSettings()
// -> <vault>/.transcriber/settings.json -> loadFromVault() on next open).
//
// "Two settings never persist" was a real shipped bug: a
// setting could be changed in the UI, look correct for the session, and be
// silently gone on the next launch. Nothing before this file exercised
// AppSettings.qml's save/load machinery through a live QML engine — the only
// coverage was tst_styleguide.cpp's source-scan for two specific keys, and no
// test instantiated SettingsPopup.qml at all (its only instantiation site in
// the app is qml/Main.qml, and no other test loads Main.qml either — see
// tst_main_window.cpp, which proves the popup LOADS under the real window;
// this file proves the chain it drives actually WORKS end to end).
//
// Same harness shape as tst_snackbar.cpp: real ApplicationWindow (SettingsPopup
// is a Popup and needs a real Overlay, which a bare QQuickView root does not
// provide), offscreen platform, sandboxed org/app name so AppSettings.qml's
// QtCore Settings{} element (lastVaultPath) never touches the user's real
// ~/.config/suopTranscriber (tests must never touch the user's real vault
// or app config).
//
// Uses a REAL VaultTreeModel against a scratch QTemporaryDir vault — the
// assertion is what actually lands in <vault>/.transcriber/settings.json, and
// a stubbed writeVaultSettings would only prove the stub agrees with itself.
// TranscriptionController is stubbed (below): SettingsPopup's diarization
// model combo boxes call transcriptionController.availableSegmentationModels()/
// availableEmbeddingModels() UNCONDITIONALLY at load (not guarded by a null
// check, unlike the combine-rule preview calls), so a real driver must supply
// something answering those two calls — the rest of the popup's controller
// usage is guarded and irrelevant to persistence.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QUrl>

#include "IconTheme.h"
#include "VaultTreeModel.h"

namespace {

// Minimal stand-in for TranscriptionController. availableSegmentationModels()/
// availableEmbeddingModels() are the two calls the file's top comment singles
// out (unconditional at load); the rest here (detectCombineGroups/
// regexValidationError/manageableModels/the two signals ModelsPage.qml wires
// via Connections) exist only because setting combineRules to a non-empty
// list (mapValuedSettings_roundTrip) instantiates real rule-card delegates
// that call them from Component.onCompleted/reactive text bindings — every
// answer here is an empty/no-op placeholder, since none of these calls are
// what persistence is testing.
class TranscriptionControllerStub : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE QVariantList availableSegmentationModels() const { return {}; }
    Q_INVOKABLE QVariantList availableEmbeddingModels() const { return {}; }
    Q_INVOKABLE QVariantList manageableModels() const { return {}; }
    Q_INVOKABLE QVariantList detectCombineGroups(const QVariantList &, const QStringList &) const { return {}; }
    Q_INVOKABLE QString regexValidationError(const QString &) const { return {}; }
    Q_INVOKABLE QStringList differentiatorNonParticipatingFiles(const QString &, int, const QStringList &) const
    {
        return {};
    }

signals:
    void modelsChanged();
    void modelOperationFinished(const QString &operation, const QString &modelId, bool ok, const QString &error);
};

} // namespace

class TstSettingsPopupPersist : public QObject
{
    Q_OBJECT

    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;

    // Fresh engine (and fresh vault) per test: AppSettings is a per-engine
    // singleton holding live state (suppressSave, activeVaultModel, every
    // property), so reusing one engine/vault would leak state between tests.
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow *m_window = nullptr;
    QStringList m_warnings;

    bool openWindow()
    {
        m_warnings.clear();
        m_engine = std::make_unique<QQmlApplicationEngine>();
        m_engine->addImportPath(m_moduleDir.path());
        m_engine->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                   new IconImageProvider);
        connect(m_engine.get(), &QQmlApplicationEngine::warnings, this,
                [this](const QList<QQmlError> &errors) {
                    for (const QQmlError &e : errors)
                        m_warnings.append(e.toString());
                });
        m_engine->load(QUrl::fromLocalFile(m_driverQmlPath));
        if (m_engine->rootObjects().isEmpty()) {
            qWarning("driver QML failed to load — see the QML errors above");
            return false;
        }
        m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().first());
        if (!m_window)
            return false;
        return QTest::qWaitForWindowExposed(m_window);
    }

    // Visual-tree walk, same as tst_snackbar.cpp's collectByName — a Popup's
    // CONTENT reparents onto Overlay.overlay once opened, so it's findable
    // this way once visible; the Popup object itself is a plain QObject and
    // is found via findChildren instead (see settingsPopup() below).
    static void collectByName(QQuickItem *item, const QString &name, QList<QQuickItem *> &out)
    {
        if (!item)
            return;
        if (item->objectName() == name)
            out.append(item);
        const auto kids = item->childItems();
        for (QQuickItem *k : kids)
            collectByName(k, name, out);
    }

    QQuickItem *findByName(const QString &name) const
    {
        QList<QQuickItem *> out;
        collectByName(m_window->contentItem(), name, out);
        return out.isEmpty() ? nullptr : out.first();
    }

    // SettingsPopup is a QQuickPopup (plain QObject, not a QQuickItem) — same
    // lookup shape as tst_main_window.cpp's findSettingsPopup().
    QObject *settingsPopup() const
    {
        for (QObject *child : m_window->findChildren<QObject *>()) {
            if (QByteArray(child->metaObject()->className()).startsWith("SettingsPopup"))
                return child;
        }
        return nullptr;
    }

    QString settingsJsonPath(const QTemporaryDir &vaultDir) const
    {
        return vaultDir.filePath(QStringLiteral(".transcriber/settings.json"));
    }

    QJsonObject readSettingsJson(const QTemporaryDir &vaultDir) const
    {
        QFile f(settingsJsonPath(vaultDir));
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(f.readAll()).object();
    }

    void openVault(const QTemporaryDir &vaultDir)
    {
        QVariant ret;
        QMetaObject::invokeMethod(m_window, "openVault", Q_RETURN_ARG(QVariant, ret),
                                  Q_ARG(QVariant, QUrl::fromLocalFile(vaultDir.path())));
    }

    void setValue(const QString &key, const QVariant &value)
    {
        QMetaObject::invokeMethod(m_window, "setValue", Q_ARG(QVariant, key), Q_ARG(QVariant, value));
    }

    QVariant getValue(const QString &key)
    {
        QVariant ret;
        QMetaObject::invokeMethod(m_window, "getValue", Q_RETURN_ARG(QVariant, ret), Q_ARG(QVariant, key));
        return ret;
    }

    void flush() { QMetaObject::invokeMethod(m_window, "flush"); }
    void reload() { QMetaObject::invokeMethod(m_window, "reload"); }

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

        // Hand-written, intentionally minimal — only the QML-defined types
        // SettingsPopup.qml transitively needs (verified by grepping which
        // capitalized component names it and its children actually
        // instantiate — not the full app module list). AppIconButton,
        // ConfirmPopup's siblings PlayGlyph/PauseGlyph come in via AppIcon.qml
        // (its Loader references both by name even though only one is ever
        // active); ConfirmPopup and Notifications come in via ModelsPage.qml
        // (the delete-confirmation popup and its post()/error() calls).
        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
        qmldir.write("module SuopTranscriber\n"
                     "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                     "singleton Theme 1.0 qml/Theme.qml\n"
                     "singleton Notifications 1.0 qml/Notifications.qml\n"
                     "HoverBorder 1.0 qml/HoverBorder.qml\n"
                     "AppIcon 1.0 qml/AppIcon.qml\n"
                     "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                     "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                     "SettingResetButton 1.0 qml/SettingResetButton.qml\n"
                     "ToggleSetting 1.0 qml/ToggleSetting.qml\n"
                     "SliderSetting 1.0 qml/SliderSetting.qml\n"
                     "ConfirmPopup 1.0 qml/ConfirmPopup.qml\n"
                     "MiddleDragScroll 1.0 qml/MiddleDragScroll.qml\n"
                     "ModelsPage 1.0 qml/ModelsPage.qml\n"
                     "SettingsPopup 1.0 qml/SettingsPopup.qml\n");
        qmldir.close();

        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterType<VaultTreeModel>("SuopTranscriber", 1, 0, "VaultTreeModel");
        qmlRegisterType<TranscriptionControllerStub>("SuopTranscriber", 1, 0, "TranscriptionController");

        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import QtQuick.Controls\n"
            "import SuopTranscriber\n"
            "ApplicationWindow {\n"
            "    id: win\n"
            "    width: 1100; height: 800; visible: true\n"
            "    TranscriptionController { id: fakeController }\n"
            "    VaultTreeModel { id: vaultModel }\n"
            "    SettingsPopup {\n"
            "        id: popup\n"
            "        objectName: \"settingsPopup\"\n"
            "        parent: win.contentItem\n"
            "        vaultTreeModel: vaultModel\n"
            "        transcriptionController: fakeController\n"
            "    }\n"
            "    function openVault(path) { vaultModel.vaultPath = path; AppSettings.loadFromVault(vaultModel) }\n"
            "    function openPopup() { popup.open() }\n"
            "    function setValue(key, v) { AppSettings[key] = v }\n"
            "    function getValue(key) { return AppSettings[key] }\n"
            "    function flush() { AppSettings.flushPendingSave() }\n"
            "    function reload() { AppSettings.loadFromVault(vaultModel) }\n"
            "}\n");

        m_driverQmlPath = m_moduleDir.filePath(QStringLiteral("driver.qml"));
        QFile driver(m_driverQmlPath);
        QVERIFY(driver.open(QIODevice::WriteOnly | QIODevice::Text));
        driver.write(driverQml.toUtf8());
        driver.close();
    }

    void cleanup()
    {
        m_window = nullptr;
        m_engine.reset();
    }

    // First time 2554+ lines of SettingsPopup.qml reach a live engine at all
    // (besides tst_main_window.cpp's load-only proof) — this is the dedicated
    // persistence-chain harness, so it re-asserts the load succeeds cleanly
    // with zero QML warnings before trusting anything downstream.
    void settingsPopup_instantiatesAndOpens()
    {
        QVERIFY(openWindow());
        QVERIFY2(m_warnings.isEmpty(),
                 qPrintable(QStringLiteral("expected zero QML warnings loading SettingsPopup, got: ")
                            + m_warnings.join(QStringLiteral(" | "))));
        QObject *popup = settingsPopup();
        QVERIFY2(popup, "SettingsPopup must be findable in the window's QObject tree");

        QMetaObject::invokeMethod(m_window, "openPopup");
        QTRY_VERIFY2(popup->property("visible").toBool(), "the popup must actually open");
    }

    // Pins the debounce as intended behavior rather than luck: a change must
    // NOT be on disk immediately, only after saveTimer's 250ms interval
    // (qml/AppSettings.qml's Timer).
    void changedSetting_reachesTheVaultJsonAfterTheDebounce()
    {
        QTemporaryDir vaultDir;
        QVERIFY(vaultDir.isValid());
        QVERIFY(openWindow());
        openVault(vaultDir);

        QCOMPARE(getValue(QStringLiteral("useSystemTitlebar")).toBool(), true);
        setValue(QStringLiteral("useSystemTitlebar"), false);

        // Immediately after the change: not written yet.
        QJsonObject before = readSettingsJson(vaultDir);
        QVERIFY2(!before.contains(QStringLiteral("useSystemTitlebar"))
                     || before.value(QStringLiteral("useSystemTitlebar")).toBool(),
                 "the debounced write must not land synchronously");

        QTRY_VERIFY2(readSettingsJson(vaultDir).value(QStringLiteral("useSystemTitlebar")).toBool() == false,
                     "the new value must land on disk once the debounce elapses");
    }

    // The path qml/Main.qml's onClosing uses before the app's hard ::_exit(0)
    // (TranscriptionController::terminateApplicationImmediately), which never
    // lets saveTimer fire — a real "setting silently lost" mechanism with no
    // coverage at all before this test.
    void flushPendingSave_writesBeforeTheHardExit()
    {
        QTemporaryDir vaultDir;
        QVERIFY(vaultDir.isValid());
        QVERIFY(openWindow());
        openVault(vaultDir);

        setValue(QStringLiteral("sortMode"), QStringLiteral("dateDesc"));
        flush();

        const QJsonObject data = readSettingsJson(vaultDir);
        QCOMPARE(data.value(QStringLiteral("sortMode")).toString(), QStringLiteral("dateDesc"));
    }

    // Every `defaults` key round-trips, and loading must not itself trigger a
    // re-save — the suppressSave guard (qml/AppSettings.qml's loadFromVault()/
    // saveToVault()). Verified by mtime: a re-save during load would touch it.
    void loadFromVault_restoresEveryDefaultsKey_andDoesNotReSave()
    {
        QTemporaryDir vaultDir;
        QVERIFY(vaultDir.isValid());
        QVERIFY(QDir().mkpath(vaultDir.filePath(QStringLiteral(".transcriber"))));

        QJsonObject data;
        data[QStringLiteral("useSystemTitlebar")] = false;
        data[QStringLiteral("sortMode")] = QStringLiteral("dateAsc");
        data[QStringLiteral("timestampDecimalPlaces")] = 2;
        data[QStringLiteral("chapterDensity")] = 0.75;
        {
            QFile f(settingsJsonPath(vaultDir));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(data).toJson());
        }
        const QDateTime mtimeBeforeLoad = QFileInfo(settingsJsonPath(vaultDir)).lastModified();

        QVERIFY(openWindow());
        openVault(vaultDir);

        QCOMPARE(getValue(QStringLiteral("useSystemTitlebar")).toBool(), false);
        QCOMPARE(getValue(QStringLiteral("sortMode")).toString(), QStringLiteral("dateAsc"));
        QCOMPARE(getValue(QStringLiteral("timestampDecimalPlaces")).toInt(), 2);
        QCOMPARE(getValue(QStringLiteral("chapterDensity")).toDouble(), 0.75);

        // A key NOT in the written file must fall back to its default rather
        // than staying stale/undefined.
        QCOMPARE(getValue(QStringLiteral("borderRadius")).toDouble(), 8.0);

        // Give any wrongly-scheduled debounced save a chance to fire, then
        // confirm the file is untouched — loadFromVault() must not itself
        // provoke a write.
        QTest::qWait(400);
        QCOMPARE(QFileInfo(settingsJsonPath(vaultDir)).lastModified(), mtimeBeforeLoad);
    }

    // saveToVault() stringifies QColor via isColorValue() (QJsonValue::
    // fromVariant can't serialize a QColor) — nothing before this proved the
    // string form survives the round trip back into a `color` property.
    void colorSetting_roundTripsAsAString()
    {
        QTemporaryDir vaultDir;
        QVERIFY(vaultDir.isValid());
        QVERIFY(openWindow());
        openVault(vaultDir);

        const QColor newColor(QStringLiteral("#112233"));
        setValue(QStringLiteral("accentColor"), newColor);
        flush();

        const QJsonObject data = readSettingsJson(vaultDir);
        QVERIFY2(data.value(QStringLiteral("accentColor")).isString(),
                 "a color setting must be stored as a string, not a QVariant color object");

        reload();
        QCOMPARE(getValue(QStringLiteral("accentColor")).value<QColor>(), newColor);
    }

    // combineRules/iconOverrides/recordingAppGains/recordingAppCaptureEnabled
    // are deliberately NOT in `defaults` (see AppSettings.qml's doc comments)
    // and are loaded/saved by four explicit lines each — exactly the shape
    // most likely to be forgotten when a fifth map/list is added.
    void mapValuedSettings_roundTrip()
    {
        QTemporaryDir vaultDir;
        QVERIFY(vaultDir.isValid());
        QVERIFY(openWindow());
        openVault(vaultDir);

        QVariantList rules;
        QVariantMap rule;
        rule[QStringLiteral("name")] = QStringLiteral("Part N");
        rule[QStringLiteral("pattern")] = QStringLiteral("part(\\d+)");
        rule[QStringLiteral("enabled")] = true;
        rules.append(rule);
        setValue(QStringLiteral("combineRules"), rules);

        QVariantMap icons;
        icons[QStringLiteral("play")] = QStringLiteral("classic");
        setValue(QStringLiteral("iconOverrides"), icons);

        QVariantMap gains;
        gains[QStringLiteral("Discord")] = 1.5;
        setValue(QStringLiteral("recordingAppGains"), gains);

        QVariantMap enabled;
        enabled[QStringLiteral("Discord")] = true;
        setValue(QStringLiteral("recordingAppCaptureEnabled"), enabled);

        flush();
        reload();

        const QVariantList rulesBack = getValue(QStringLiteral("combineRules")).toList();
        QCOMPARE(rulesBack.size(), 1);
        QCOMPARE(rulesBack.first().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("Part N"));

        const QVariantMap iconsBack = getValue(QStringLiteral("iconOverrides")).toMap();
        QCOMPARE(iconsBack.value(QStringLiteral("play")).toString(), QStringLiteral("classic"));

        const QVariantMap gainsBack = getValue(QStringLiteral("recordingAppGains")).toMap();
        QCOMPARE(gainsBack.value(QStringLiteral("Discord")).toDouble(), 1.5);

        const QVariantMap enabledBack = getValue(QStringLiteral("recordingAppCaptureEnabled")).toMap();
        QCOMPARE(enabledBack.value(QStringLiteral("Discord")).toBool(), true);
    }
};

int main(int argc, char *argv[])
{
    // Offscreen: no real compositor/display needed. Must be set before
    // QGuiApplication reads it. Sandboxed org/app name so AppSettings.qml's
    // QtCore Settings{} element (lastVaultPath) never touches the real app's
    // ~/.config/suopTranscriber (tests must never touch the user's real
    // vault or app config).
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_settingspopup_persist"));
    QGuiApplication app(argc, argv);
    TstSettingsPopupPersist tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_settingspopup_persist.moc"
