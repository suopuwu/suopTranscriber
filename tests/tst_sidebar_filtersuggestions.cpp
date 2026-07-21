// Living spec + runtime verification for the sidebar's search filter
// autocomplete rework (queue item 046): the shared FilterSuggestions dropdown
// behind BOTH the tag and speaker filters — non-modal, anchored directly under
// its field, dismissable, keyboard-navigable.
//
// Same harness shape as tst_sidebar_rename.cpp (read its top-of-file comment
// first): a REAL offscreen QQuickView loading the actual Sidebar.qml against a
// real VaultTreeModel + a TranscriptionController STUB that supplies fixed
// tag/speaker vocabularies (the suggestions are empty without a controller).
// Every assertion runs against BOTH fields via a data row, because the whole
// point of the rework is that the two behave identically.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QUrl>

#include "IconTheme.h"
#include "VaultSearchProxyModel.h"
#include "VaultTreeModel.h"

namespace {

// Fixed vocabularies so "a" yields two matches in each field (apple/apricot,
// Alice/Alfred) and one non-match (banana, Bob).
class TranscriptionControllerStub : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE QVariantMap readMetadata(const QUrl &) const { return {}; }
    Q_INVOKABLE bool matchesExistingTranscription(const QUrl &, const QString &) const { return false; }
    Q_INVOKABLE int jobStatusForPath(const QString &) const { return -1; }
    Q_INVOKABLE QVariantList detectCombineGroups(const QVariantList &, const QStringList &) const { return {}; }
    Q_INVOKABLE QStringList existingVaultTags(const QUrl &) const
    { return {QStringLiteral("apple"), QStringLiteral("apricot"), QStringLiteral("banana")}; }
    Q_INVOKABLE QStringList existingVaultSpeakers(const QUrl &) const
    { return {QStringLiteral("Alice"), QStringLiteral("Alfred"), QStringLiteral("Bob")}; }

signals:
    void finished(const QUrl &fileUrl, bool ok, const QString &error);
    void jobStatusesChanged();
};

} // namespace

class TstSidebarFilterSuggestions : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;
    QString m_driverQmlPath;
    QTemporaryDir m_vaultDir;
    TranscriptionControllerStub *m_controller = nullptr;
    QQuickView *m_view = nullptr;
    QQuickItem *m_sidebar = nullptr;

    void typeText(const QString &text)
    {
        for (const QChar &c : text)
            QTest::keyClick(m_view, c.toLatin1());
    }

    QObject *popup(const QString &name) const { return m_sidebar->findChild<QObject *>(name); }
    QQuickItem *field(const QString &name) const { return m_sidebar->findChild<QQuickItem *>(name); }

    void openAndExpand()
    {
        m_view = new QQuickView;
        m_view->engine()->addImportPath(m_moduleDir.path());
        m_view->engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                            new IconImageProvider);
        m_view->rootContext()->setContextProperty(QStringLiteral("vaultUrl"),
                                                  QUrl::fromLocalFile(m_vaultDir.path()));
        m_view->rootContext()->setContextProperty(QStringLiteral("controllerStub"),
                                                  static_cast<QObject *>(m_controller));
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
        m_sidebar = m_view->rootObject()->findChild<QQuickItem *>(QStringLiteral("sidebar"));
        QVERIFY(m_sidebar);
        m_sidebar->setProperty("searchExpanded", true);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        QVERIFY(m_vaultDir.isValid());
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        QVERIFY(QDir().mkpath(suopDir));
        QVERIFY2(QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")),
                 "failed to symlink qml/ into the test's scratch QML module dir");

        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly | QIODevice::Text));
        qmldir.write("module SuopTranscriber\n"
                      "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                      "singleton Notifications 1.0 qml/Notifications.qml\n"
                      "HoverBorder 1.0 qml/HoverBorder.qml\n"
                      "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
                      "UnderlineField 1.0 qml/UnderlineField.qml\n"
                      "FilterSuggestions 1.0 qml/FilterSuggestions.qml\n"
                      "AppIcon 1.0 qml/AppIcon.qml\n"
                      "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                      "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                      "Sidebar 1.0 qml/Sidebar.qml\n");
        qmldir.close();

        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
        qmlRegisterType<VaultTreeModel>("SuopTranscriber", 1, 0, "VaultTreeModel");
        qmlRegisterType<VaultSearchProxyModel>("SuopTranscriber", 1, 0, "VaultSearchProxyModel");
        qmlRegisterUncreatableType<TranscriptionControllerStub>(
            "SuopTranscriber", 1, 0, "TranscriptionController",
            QStringLiteral("stub — assigned from C++ via a context property"));

        const QString driverQml = QStringLiteral(
            "import QtQuick\n"
            "import SuopTranscriber\n"
            "Item {\n"
            "    id: root\n"
            "    width: 320\n"
            "    height: 400\n"
            "    property string openPath: \"\"\n"
            "    VaultTreeModel { id: vaultModel; objectName: \"vaultModel\"; vaultPath: Qt.resolvedUrl(vaultUrl) }\n"
            "    Sidebar {\n"
            "        id: sidebar\n"
            "        objectName: \"sidebar\"\n"
            "        anchors.fill: parent\n"
            "        model: vaultModel\n"
            "        transcriptionController: controllerStub\n"
            "        vaultPath: Qt.resolvedUrl(vaultUrl)\n"
            "        currentPath: root.openPath\n"
            "        onFileActivated: (path) => root.openPath = path\n"
            "        onOrphanedMdActivated: (path) => root.openPath = path\n"
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
        m_controller = new TranscriptionControllerStub;
        openAndExpand();
    }

    void cleanup()
    {
        delete m_view;
        m_view = nullptr;
        m_sidebar = nullptr;
        delete m_controller;
        m_controller = nullptr;
    }

    // Both fields, each with (fieldObjectName, popupObjectName, first, second).
    void bothFilters_data()
    {
        QTest::addColumn<QString>("fieldName");
        QTest::addColumn<QString>("popupName");
        QTest::addColumn<QString>("first");
        QTest::addColumn<QString>("second");
        // A second char that keeps the "a" fragment matching in THIS field's
        // vocabulary ("ap" -> apple/apricot; "al" -> Alice/Alfred).
        QTest::addColumn<QString>("reviveChar");
        QTest::newRow("tags") << QStringLiteral("tagFilterField") << QStringLiteral("tagSuggestionsPopup")
                              << QStringLiteral("apple") << QStringLiteral("apricot") << QStringLiteral("p");
        QTest::newRow("speakers") << QStringLiteral("speakerFilterField") << QStringLiteral("speakerSuggestionsPopup")
                                  << QStringLiteral("Alice") << QStringLiteral("Alfred") << QStringLiteral("l");
    }

    // Regression guard for the y: ribbon.height bug: the popup anchors to its
    // own field (parent), directly beneath it — not several rows down the
    // ribbon.
    void suggestionsPopupSitsDirectlyBelowItsField_data() { bothFilters_data(); }
    void suggestionsPopupSitsDirectlyBelowItsField()
    {
        QFETCH(QString, fieldName);
        QFETCH(QString, popupName);
        QQuickItem *f = field(fieldName);
        QObject *p = popup(popupName);
        QVERIFY(f && p);
        f->forceActiveFocus();
        typeText(QStringLiteral("a"));
        QTRY_VERIFY(p->property("visible").toBool());

        QCOMPARE(p->property("parent").value<QObject *>(), static_cast<QObject *>(f));
        // y is in field coordinates now (parent == field), just below it.
        QCOMPARE(p->property("y").toReal(), f->property("height").toReal() + 2.0);
    }

    // The dimming defect, pinned as a property so a future O21-style sweep
    // can't silently re-dim the field.
    void suggestionsPopupIsNotModal_data() { bothFilters_data(); }
    void suggestionsPopupIsNotModal()
    {
        QFETCH(QString, popupName);
        QObject *p = popup(popupName);
        QVERIFY(p);
        QCOMPARE(p->property("modal").toBool(), false);
    }

    // Escape closes the list; the next keystroke brings it back — dismiss
    // suppresses the current fragment, not the field's autocomplete forever.
    void escapeDismissesSuggestions_data() { bothFilters_data(); }
    void escapeDismissesSuggestions()
    {
        QFETCH(QString, fieldName);
        QFETCH(QString, popupName);
        QQuickItem *f = field(fieldName);
        QObject *p = popup(popupName);
        QVERIFY(f && p);
        f->forceActiveFocus();
        typeText(QStringLiteral("a"));
        QTRY_VERIFY(p->property("visible").toBool());

        QTest::keyClick(m_view, Qt::Key_Escape);
        QTRY_VERIFY(!p->property("visible").toBool());

        QFETCH(QString, reviveChar);
        typeText(reviveChar); // keeps the fragment matching in this vocabulary
        QTRY_VERIFY2(p->property("visible").toBool(), "typing after Escape must revive the suggestions");
    }

    // Arrow + Enter picks a suggestion; a fresh popup starts on index 0 so one
    // Enter takes the best match.
    void arrowKeysAndEnterPickASuggestion_data() { bothFilters_data(); }
    void arrowKeysAndEnterPickASuggestion()
    {
        QFETCH(QString, fieldName);
        QFETCH(QString, popupName);
        QFETCH(QString, first);
        QFETCH(QString, second);
        QQuickItem *f = field(fieldName);
        QObject *p = popup(popupName);
        QVERIFY(f && p);

        f->forceActiveFocus();
        typeText(QStringLiteral("a"));
        QTRY_VERIFY(p->property("visible").toBool());
        QTest::keyClick(m_view, Qt::Key_Down);
        QTest::keyClick(m_view, Qt::Key_Return);
        QCOMPARE(f->property("text").toString(), second + QStringLiteral(", "));

        // Fresh popup: no Down, one Enter takes the first (best) match.
        f->setProperty("text", QString());
        f->forceActiveFocus();
        typeText(QStringLiteral("a"));
        QTRY_VERIFY(p->property("visible").toBool());
        QTest::keyClick(m_view, Qt::Key_Return);
        QCOMPARE(f->property("text").toString(), first + QStringLiteral(", "));
    }

    // Tab commits the highlighted suggestion just like Enter (queue item 075).
    void tabPicksASuggestion_data() { bothFilters_data(); }
    void tabPicksASuggestion()
    {
        QFETCH(QString, fieldName);
        QFETCH(QString, popupName);
        QFETCH(QString, first);
        QFETCH(QString, second);
        QQuickItem *f = field(fieldName);
        QObject *p = popup(popupName);
        QVERIFY(f && p);

        // Arrow to the second match, then Tab commits it.
        f->forceActiveFocus();
        typeText(QStringLiteral("a"));
        QTRY_VERIFY(p->property("visible").toBool());
        QTest::keyClick(m_view, Qt::Key_Down);
        QTest::keyClick(m_view, Qt::Key_Tab);
        QCOMPARE(f->property("text").toString(), second + QStringLiteral(", "));

        // Fresh popup: one Tab takes the first (best) match, no arrow needed.
        f->setProperty("text", QString());
        f->forceActiveFocus();
        typeText(QStringLiteral("a"));
        QTRY_VERIFY(p->property("visible").toBool());
        QTest::keyClick(m_view, Qt::Key_Tab);
        QCOMPARE(f->property("text").toString(), first + QStringLiteral(", "));
    }

    // The pre-existing mouse path must survive the refactor.
    void clickingASuggestionStillWorks_data() { bothFilters_data(); }
    void clickingASuggestionStillWorks()
    {
        QFETCH(QString, fieldName);
        QFETCH(QString, popupName);
        QFETCH(QString, first);
        QQuickItem *f = field(fieldName);
        QObject *p = popup(popupName);
        QVERIFY(f && p);

        f->forceActiveFocus();
        typeText(QStringLiteral("a"));
        QTRY_VERIFY(p->property("visible").toBool());

        QQuickItem *list = p->findChild<QQuickItem *>(QStringLiteral("suggestionsList"));
        QVERIFY(list);
        QQuickItem *row0 = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Qt::DirectConnection,
                                   Q_RETURN_ARG(QQuickItem *, row0), Q_ARG(int, 0));
        QVERIFY2(row0, "first suggestion delegate not realized");
        const QPoint c = row0->mapToScene(QPointF(row0->width() / 2, row0->height() / 2)).toPoint();
        QTest::mouseClick(m_view, Qt::LeftButton, {}, c);
        QTRY_COMPARE(f->property("text").toString(), first + QStringLiteral(", "));
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_sidebar_filtersuggestions"));
    QGuiApplication app(argc, argv);
    TstSidebarFilterSuggestions tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_sidebar_filtersuggestions.moc"
