// Living spec for the icon-pack system (DESIGN.md "Iconography"): pack
// discovery, the override -> active-pack -> fallback resolution order, and
// IconImageProvider's tinting/caching.
//
// The last two slots load AppIcon.qml into a REAL (offscreen) QQuickView
// rather than stopping at the C++ layer. That's deliberate: a clean cmake
// build says nothing about whether QML actually works — AppIcon's binding to
// IconTheme.resolve() could fail to resolve, or Screen.devicePixelRatio could
// be unavailable, and either would only show up at load. Same offscreen +
// symlinked-scratch-module harness tst_sidebar_keynav.cpp uses; see its
// comments for why that shape.

#include <QtTest>

#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>

#include "IconTheme.h"

class TstIconTheme : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_moduleDir;

    // Builds the same scratch QML module tst_sidebar_keynav.cpp does, so
    // AppIcon.qml can `import SuopTranscriber` for IconTheme/AppSettings.
    QString buildScratchModule()
    {
        const QString suopDir = m_moduleDir.filePath(QStringLiteral("SuopTranscriber"));
        if (!QDir().mkpath(suopDir))
            return {};
        // Symlink to the real checked-out qml/, never a build-time copy.
        if (!QFile::exists(suopDir + QStringLiteral("/qml"))
            && !QFile::link(QStringLiteral(SUOP_QML_DIR), suopDir + QStringLiteral("/qml")))
            return {};

        QFile qmldir(suopDir + QStringLiteral("/qmldir"));
        if (!qmldir.open(QIODevice::WriteOnly | QIODevice::Text))
            return {};
        // Enough of the module for AppIcon plus (for the env-gated settings
        // grab) the whole SettingsPopup and everything it imports.
        qmldir.write("module SuopTranscriber\n"
                      "singleton AppSettings 1.0 qml/AppSettings.qml\n"
                      "AppIcon 1.0 qml/AppIcon.qml\n"
                      "PlayGlyph 1.0 qml/PlayGlyph.qml\n"
                      "PauseGlyph 1.0 qml/PauseGlyph.qml\n"
                      "HoverBorder 1.0 qml/HoverBorder.qml\n"
                      "UnderlineField 1.0 qml/UnderlineField.qml\n"
                      "CombineGroupOutline 1.0 qml/CombineGroupOutline.qml\n"
                      "SettingsPopup 1.0 qml/SettingsPopup.qml\n");
        qmldir.close();
        return suopDir;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_moduleDir.isValid());
        QVERIFY(!buildScratchModule().isEmpty());
        qmlRegisterSingletonType<IconTheme>(
            "SuopTranscriber", 1, 0, "IconTheme",
            [](QQmlEngine *, QJSEngine *) -> QObject * { return new IconTheme; });
    }

    // Packs are found by scanning the resource tree, not from a hardcoded
    // list — that's what makes "a pack is just a directory" true.
    void packs_discoveredFromResourceTree()
    {
        IconTheme theme;
        const QStringList ids = theme.packIds();
        QVERIFY2(ids.contains(QStringLiteral("material")), qPrintable("got: " + ids.join(',')));
        QVERIFY2(ids.contains(QStringLiteral("classic")), qPrintable("got: " + ids.join(',')));
        // The fallback pack heads the list so it's the first thing the
        // Settings picker offers.
        QCOMPARE(ids.first(), QString::fromLatin1(IconTheme::kFallbackPack));
        QVERIFY(!theme.packName(QStringLiteral("material")).isEmpty());
        QVERIFY(!theme.packDescription(QStringLiteral("classic")).isEmpty());
    }

    // The fallback pack is resolve()'s last resort, so a name it lacks can
    // resolve to nothing at all. It must therefore be a superset of every
    // other pack's names.
    void fallbackPack_definesEveryNameAnyPackDefines()
    {
        IconTheme theme;
        const QString fallback = QString::fromLatin1(IconTheme::kFallbackPack);
        for (const QString &name : theme.iconNames()) {
            QVERIFY2(theme.packHasIcon(fallback, name),
                      qPrintable(QStringLiteral("fallback pack '%1' is missing '%2', so that icon "
                                                "could resolve to nothing").arg(fallback, name)));
        }
    }

    // The real failure mode of ANY icon edit: update one pack's manifest and
    // leave the other with a missing key. The slot above only proves the
    // fallback is a SUPERSET; this pins every pack to the SAME key set, so a
    // name added to material alone (which would silently fall back, showing
    // users on classic a different icon than the one just authored) fails
    // here instead of shipping.
    void packs_declareIdenticalIconKeySets()
    {
        IconTheme theme;
        const QStringList names = theme.iconNames();
        QVERIFY(!names.isEmpty());
        for (const QString &pack : theme.packIds()) {
            QStringList missing;
            for (const QString &name : names) {
                if (!theme.packHasIcon(pack, name))
                    missing.append(name);
            }
            QVERIFY2(missing.isEmpty(),
                      qPrintable(QStringLiteral("pack '%1' is missing icon key(s) another pack "
                                                "defines: %2").arg(pack, missing.join(", "))));
        }
    }

    // Companion to the key-set guard: a key can be present in the manifest and
    // still point at an SVG that was never committed (or that failed to
    // vendor), which renders nothing at all. Rasterizing each one through the
    // real provider is the cheap way to catch that.
    void materialPack_everySvgEntryActuallyRenders()
    {
        IconTheme theme;
        IconImageProvider provider;
        for (const QString &name : theme.iconNames()) {
            const QVariantMap r = theme.resolve(name, QStringLiteral("material"), {});
            if (r.value(QStringLiteral("kind")).toString() != QStringLiteral("svg"))
                continue;  // classic-style builtin/glyph entries have no file to miss
            QSize size;
            const QString id = QStringLiteral("material/%1?color=%23ffffff&size=24")
                                   .arg(r.value(QStringLiteral("value")).toString());
            QVERIFY2(!provider.requestImage(id, &size, {}).isNull(),
                      qPrintable(QStringLiteral("material icon '%1' -> '%2' rendered nothing — "
                                                "missing or invalid svg").arg(name, id)));
        }
    }

    void resolve_usesActivePackWhenNoOverride()
    {
        IconTheme theme;
        const QVariantMap r = theme.resolve(QStringLiteral("transcribe"),
                                            QStringLiteral("classic"), {});
        QCOMPARE(r.value("pack").toString(), QStringLiteral("classic"));
        QCOMPARE(r.value("kind").toString(), QStringLiteral("glyph"));
        QCOMPARE(r.value("value").toString(), QStringLiteral("⤒"));
    }

    // The per-icon override is the whole point of the feature: pull one icon
    // from a different pack than the active one.
    void resolve_perIconOverrideBeatsActivePack()
    {
        IconTheme theme;
        QVariantMap overrides;
        overrides.insert(QStringLiteral("play"), QStringLiteral("classic"));

        const QVariantMap overridden = theme.resolve(QStringLiteral("play"),
                                                     QStringLiteral("material"), overrides);
        QCOMPARE(overridden.value("pack").toString(), QStringLiteral("classic"));
        QCOMPARE(overridden.value("kind").toString(), QStringLiteral("builtin"));

        // ...and only that icon — its neighbours still come from the active
        // pack.
        const QVariantMap sibling = theme.resolve(QStringLiteral("pause"),
                                                  QStringLiteral("material"), overrides);
        QCOMPARE(sibling.value("pack").toString(), QStringLiteral("material"));
    }

    // A pack that only redefines a handful of icons must degrade per-icon to
    // the next candidate rather than leaving holes — each step in the chain
    // is skipped, not terminal, when the pack doesn't define the name.
    void resolve_skipsPacksThatLackTheName()
    {
        IconTheme theme;
        QVariantMap overrides;
        // A pack id that doesn't exist at all must not swallow the lookup.
        overrides.insert(QStringLiteral("transcribe"), QStringLiteral("no-such-pack"));
        const QVariantMap r = theme.resolve(QStringLiteral("transcribe"),
                                            QStringLiteral("classic"), overrides);
        QCOMPARE(r.value("pack").toString(), QStringLiteral("classic"));

        // An active pack that doesn't exist falls through to the fallback.
        const QVariantMap viaFallback = theme.resolve(QStringLiteral("transcribe"),
                                                      QStringLiteral("no-such-pack"), {});
        QCOMPARE(viaFallback.value("pack").toString(), QString::fromLatin1(IconTheme::kFallbackPack));
    }

    void resolve_unknownNameYieldsEmptyMap()
    {
        IconTheme theme;
        QVERIFY(theme.resolve(QStringLiteral("not-an-icon"), QStringLiteral("material"), {}).isEmpty());
    }

    // The object entry form (vs a bare glyph string) is what lets one pack
    // mix kinds — classic keeps its hand-drawn play/pause shapes as `builtin`
    // while supplying everything else as glyphs — and is also the carrier for
    // per-icon optical `scale` when a pack needs it.
    void manifest_objectFormCarriesKindOverride()
    {
        IconTheme theme;
        const QVariantMap play = theme.resolve(QStringLiteral("play"), QStringLiteral("classic"), {});
        QCOMPARE(play.value("kind").toString(), QStringLiteral("builtin"));
        QCOMPARE(play.value("value").toString(), QStringLiteral("PlayGlyph"));

        // A bare-string entry in the same pack inherits the pack's kind, and
        // its scale defaults to the pack scale (1.0 here).
        const QVariantMap sort = theme.resolve(QStringLiteral("sort"), QStringLiteral("classic"), {});
        QCOMPARE(sort.value("kind").toString(), QStringLiteral("glyph"));
        QCOMPARE(sort.value("value").toString(), QStringLiteral("⇅"));
        QCOMPARE(sort.value("scale").toDouble(), 1.0);
    }

    // An svg pack's global `scale` compensates for SVGs filling their box
    // while a font glyph floats padded in its em-box — every material entry
    // inherits it, so call sites pass one size and both packs look right.
    void manifest_packScaleAppliesToBareEntries()
    {
        IconTheme theme;
        const QVariantMap transcribe = theme.resolve(QStringLiteral("transcribe"),
                                                     QStringLiteral("material"), {});
        QCOMPARE(transcribe.value("kind").toString(), QStringLiteral("svg"));
        // Whatever material's pack scale is, a bare entry must carry it
        // rather than a bare 1.0 — this is the knob that keeps svg icons from
        // rendering visibly larger than the glyphs they replaced.
        QVERIFY(transcribe.contains("scale"));
    }

    void provider_rendersRequestedSize()
    {
        IconImageProvider provider;
        QSize size;
        const QImage image = provider.requestImage(
            QStringLiteral("material/transcribe.svg?color=%23ff0000&size=32"), &size, {});
        QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(32, 32));
        QCOMPARE(size, QSize(32, 32));
    }

    // The SVGs ship with fills stripped so the provider's tint owns color —
    // if a fill ever survives the vendoring step, this catches it.
    void provider_tintsEveryOpaquePixelToRequestedColor()
    {
        IconImageProvider provider;
        QSize size;
        const QImage image = provider.requestImage(
            QStringLiteral("material/close.svg?color=%2300ff00&size=48"), &size, {}).convertToFormat(
                QImage::Format_ARGB32);
        QVERIFY(!image.isNull());

        int opaque = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor px = image.pixelColor(x, y);
                if (px.alpha() < 250)
                    continue;  // antialiased edge or empty — only judge solid pixels
                ++opaque;
                QCOMPARE(px.rgb(), QColor(Qt::green).rgb());
            }
        }
        QVERIFY2(opaque > 0, "close.svg rendered nothing opaque — is the icon actually drawing?");
    }

    void provider_missingIconYieldsNullImageNotCrash()
    {
        IconImageProvider provider;
        QSize size;
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("icon svg missing or invalid"));
        QVERIFY(provider.requestImage(QStringLiteral("material/nope.svg?color=%23fff&size=16"),
                                      &size, {}).isNull());
    }

    void provider_cachesByFullId()
    {
        IconImageProvider provider;
        QSize size;
        const QString id = QStringLiteral("material/search.svg?color=%23abcdef&size=24");
        const QImage first = provider.requestImage(id, &size, {});
        const QImage second = provider.requestImage(id, &size, {});
        QVERIFY(!first.isNull());
        // Same cache entry handed back, not re-rastered.
        QCOMPARE(first.constBits(), second.constBits());

        // Color is part of the key — a recolor (e.g. hover) must not be
        // served the previous color's raster.
        const QImage other = provider.requestImage(
            QStringLiteral("material/search.svg?color=%23123456&size=24"), &size, {});
        QVERIFY(other.constBits() != first.constBits());
    }

    // Proves AppIcon.qml actually loads and resolves — the thing a clean
    // build can't tell us.
    void appIcon_rendersSvgPackAgainstRealEngine()
    {
        QQuickView view;
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.setSource(QUrl::fromLocalFile(QStringLiteral(SUOP_QML_DIR) + "/../tests/data/icon_driver.qml"));
        QVERIFY2(view.status() == QQuickView::Ready,
                  qPrintable(view.errors().isEmpty() ? QStringLiteral("no errors reported")
                                                     : view.errors().first().toString()));
        QQuickItem *root = view.rootObject();
        QVERIFY(root);

        QQuickItem *icon = root->findChild<QQuickItem *>(QStringLiteral("icon"));
        QVERIFY(icon);
        QCOMPARE(icon->property("kind").toString(), QStringLiteral("svg"));
        // The Loader actually produced something with non-zero extent.
        QTRY_VERIFY(icon->childItems().size() == 1);
        QVERIFY(icon->property("appliedSize").toReal() > 0);
    }

    // The builtin path is the one that keeps classic's hand-drawn play
    // triangle alive; it loads a QML component by name, so it's the most
    // breakable of the three render paths.
    void appIcon_rendersBuiltinPathForClassicPlay()
    {
        QQuickView view;
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.setSource(QUrl::fromLocalFile(QStringLiteral(SUOP_QML_DIR) + "/../tests/data/icon_driver.qml"));
        QVERIFY(view.status() == QQuickView::Ready);
        QQuickItem *root = view.rootObject();
        QVERIFY(root);

        root->setProperty("pack", QStringLiteral("classic"));
        root->setProperty("iconName", QStringLiteral("play"));

        QQuickItem *icon = root->findChild<QQuickItem *>(QStringLiteral("icon"));
        QVERIFY(icon);
        QTRY_COMPARE(icon->property("kind").toString(), QStringLiteral("builtin"));
        QTRY_VERIFY(icon->childItems().size() == 1);
        // PlayGlyph is a Canvas; if the builtin map failed to match, the
        // Loader would hold nothing.
        QQuickItem *loader = icon->childItems().first();
        QTRY_VERIFY(loader->childItems().size() == 1);
    }

    // packOverride forces a specific pack regardless of AppSettings.iconPack —
    // the Settings pack-picker chips rely on it to preview a pack the user
    // hasn't selected.
    void appIcon_packOverridePreviewsForcedPack()
    {
        QQuickView view;
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.setSource(QUrl::fromLocalFile(QStringLiteral(SUOP_QML_DIR) + "/../tests/data/icon_driver.qml"));
        QVERIFY(view.status() == QQuickView::Ready);
        QQuickItem *root = view.rootObject();
        QVERIFY(root);

        // Active pack stays material; force the preview icon to classic.
        root->setProperty("pack", QStringLiteral("material"));
        root->setProperty("iconName", QStringLiteral("play"));
        root->setProperty("previewPack", QStringLiteral("classic"));

        QQuickItem *preview = root->findChild<QQuickItem *>(QStringLiteral("previewIcon"));
        QVERIFY(preview);
        // classic's play is the builtin drawn shape; material's is an svg. The
        // override must win, proving the chip previews its own pack.
        QTRY_COMPARE(preview->property("kind").toString(), QStringLiteral("builtin"));

        QQuickItem *active = root->findChild<QQuickItem *>(QStringLiteral("icon"));
        QVERIFY(active);
        QTRY_COMPARE(active->property("kind").toString(), QStringLiteral("svg"));
    }

    // Not a spec assertion — a developer tool. Env-gated so it never runs in a
    // normal ctest sweep. `SUOP_ICON_GALLERY=1 ./tst_icontheme` writes a PNG
    // per pack under the dir named by SUOP_ICON_GALLERY_DIR (default: the
    // scratch tmp), for eyeballing Phase-A parity and tuning the material
    // global scale. Renders the real AppIcon path through the real provider,
    // so it doubles as a "does the whole stack actually paint" smoke check.
    void grabGallery()
    {
        if (qEnvironmentVariable("SUOP_ICON_GALLERY") != QStringLiteral("1"))
            QSKIP("set SUOP_ICON_GALLERY=1 to render the icon gallery PNGs");

        const QString outDir = qEnvironmentVariable("SUOP_ICON_GALLERY_DIR", QDir::tempPath());
        QQuickView view;
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.setSource(QUrl::fromLocalFile(QStringLiteral(SUOP_QML_DIR)
                                           + "/../tests/data/icon_gallery.qml"));
        QVERIFY2(view.status() == QQuickView::Ready,
                  view.errors().isEmpty() ? "no error" : qPrintable(view.errors().first().toString()));
        QQuickItem *root = view.rootObject();
        QVERIFY(root);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));

        for (const QString &pack : {QStringLiteral("material"), QStringLiteral("classic")}) {
            root->setProperty("pack", pack);
            QTest::qWait(200);  // let re-resolution + async provider textures land
            const QImage shot = view.grabWindow();
            QVERIFY(!shot.isNull());
            const QString path = outDir + "/icon_gallery_" + pack + ".png";
            QVERIFY2(shot.save(path), qPrintable("failed to save " + path));
            qInfo("wrote %s (%dx%d)", qPrintable(path), shot.width(), shot.height());
        }
    }

    // Companion dev tool to grabGallery — renders the real SettingsPopup's
    // Appearance page (icon-pack picker + expanded per-icon override grid) to
    // a PNG, so the new settings UI's layout can be eyeballed. Same env gate.
    void grabSettings()
    {
        if (qEnvironmentVariable("SUOP_ICON_GALLERY") != QStringLiteral("1"))
            QSKIP("set SUOP_ICON_GALLERY=1 to render the settings PNG");

        const QString outDir = qEnvironmentVariable("SUOP_ICON_GALLERY_DIR", QDir::tempPath());
        QQuickView view;
        view.resize(720, 760);
        view.engine()->addImportPath(m_moduleDir.path());
        view.engine()->addImageProvider(QString::fromLatin1(IconImageProvider::kProviderId),
                                        new IconImageProvider);
        view.setSource(QUrl::fromLocalFile(QStringLiteral(SUOP_QML_DIR)
                                           + "/../tests/data/settings_driver.qml"));
        QVERIFY2(view.status() == QQuickView::Ready,
                  view.errors().isEmpty() ? "no error" : qPrintable(view.errors().first().toString()));
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        QTest::qWait(300);  // popup open animation + async icon textures
        const QImage shot = view.grabWindow();
        QVERIFY(!shot.isNull());
        const QString path = outDir + "/icon_settings.png";
        QVERIFY2(shot.save(path), qPrintable("failed to save " + path));
        qInfo("wrote %s (%dx%d)", qPrintable(path), shot.width(), shot.height());
    }
};

int main(int argc, char *argv[])
{
    // Offscreen + sandboxed org/app name: AppSettings.qml's QtCore Settings{}
    // element is loaded transitively by AppIcon.qml, and CLAUDE.md forbids
    // tests touching the real ~/.config/suopTranscriber.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriberTestHarness"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_icontheme"));
    QGuiApplication app(argc, argv);
    TstIconTheme tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_icontheme.moc"
