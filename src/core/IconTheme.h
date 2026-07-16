#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QQuickImageProvider>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <qqml.h>

// An icon pack: one directory under assets/icons/<id>/ holding a pack.json
// manifest (plus, for svg packs, the SVG files it names). Parsed from the
// manifest by IconTheme::loadPack — see icons.md / DESIGN.md "Iconography".
struct IconPack {
    QString id;
    QString name;
    QString description;
    QString kind;  // default kind for entries that are a bare string
    double scale = 1.0;
    // semantic name -> {kind, value, scale} — always fully resolved here, so
    // lookups never have to re-apply the pack's defaults.
    QHash<QString, QVariantMap> icons;
};

// Registry of icon packs plus the resolution rules that decide which pack
// actually supplies a given semantic icon name.
//
// Packs are discovered by scanning assets/icons/ in the Qt resource system
// rather than being hardcoded, so a pack is genuinely just a directory with a
// manifest. Loading packs from disk later (a user-dropped pack directory) is
// the same loadPack() over a different root — deliberately the only thing
// that would need to change.
//
// resolve() is a PURE function of (name, packId, overrides) rather than
// reading a packId/overrides property off this singleton. That's what lets
// AppIcon.qml bind `IconTheme.resolve(name, AppSettings.iconPack,
// AppSettings.iconOverrides)` and have every icon in the app re-resolve
// automatically when either setting changes — QML's binding tracker sees the
// AppSettings reads as dependencies. Mirroring those two values into
// properties here would duplicate the settings' state and need hand-written
// change notification to achieve the same thing.
class IconTheme : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit IconTheme(QObject *parent = nullptr);

    // The pack consulted when neither the per-icon override nor the active
    // pack supplies a name. Must be an svg pack that defines every name, so
    // resolution can't bottom out at "nothing to draw".
    static constexpr auto kFallbackPack = "material";

    // Resolves a semantic name (e.g. "transcribe") to a renderable, applying
    // per-icon override -> active pack -> kFallbackPack in that order,
    // skipping any pack that doesn't define the name. Returns
    // {kind, value, scale, pack}, or an empty map if no pack defines it
    // (AppIcon renders nothing and warns once).
    //
    // `overrides` maps a semantic name to a pack id, letting a user pull a
    // single icon from a pack other than the active one.
    Q_INVOKABLE QVariantMap resolve(const QString &name,
                                    const QString &packId,
                                    const QVariantMap &overrides) const;

    // Pack ids in stable (alphabetical) order — kFallbackPack first, since
    // it's the default and should head the Settings picker.
    Q_INVOKABLE QStringList packIds() const;
    Q_INVOKABLE QString packName(const QString &packId) const;
    Q_INVOKABLE QString packDescription(const QString &packId) const;
    Q_INVOKABLE bool packHasIcon(const QString &packId, const QString &name) const;

    // Every semantic name any pack defines, sorted — drives the Settings
    // per-icon override grid, so a pack adding an icon shows up there with
    // no separate list to update.
    Q_INVOKABLE QStringList iconNames() const;

private:
    void loadPacksFrom(const QString &root);
    bool loadPack(const QString &dir, const QString &id);

    QHash<QString, IconPack> m_packs;
};

// Renders an SVG from an icon pack at a requested size and tints it a flat
// color, caching the result.
//
// Why an image provider instead of `Image` + `MultiEffect { colorization }`
// at each call site: the app instantiates well over a hundred icons (the
// Sidebar's per-row action clusters exist for every row, opacity-0 until
// hover, but instantiated all the same). Each MultiEffect is a live
// offscreen render pass, so that approach costs ~100+ FBOs to draw what are
// ultimately static monochrome images. Tinting once into a cached QImage
// keyed by (pack, file, color, size) costs one raster per distinct
// appearance — hover recolors resolve to a handful of cached entries.
//
// Image ids are "<pack>/<file>?color=<#rrggbb>&size=<px>"; AppIcon.qml
// builds them via iconSource(). requestImage runs on Qt Quick's texture
// thread, hence the mutex around the cache.
class IconImageProvider : public QQuickImageProvider {
public:
    IconImageProvider();

    static constexpr auto kProviderId = "suopicons";

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    QMutex m_mutex;
    QHash<QString, QImage> m_cache;
};
