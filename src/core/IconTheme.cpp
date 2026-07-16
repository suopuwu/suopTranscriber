#include "IconTheme.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPainter>
#include <QSvgRenderer>
#include <QUrlQuery>

namespace {

// Where qt_add_qml_module's RESOURCES block puts assets/icons/ given
// URI SuopTranscriber and the default RESOURCE_PREFIX (/qt/qml/).
constexpr auto kIconRoot = ":/qt/qml/SuopTranscriber/assets/icons";

Q_LOGGING_CATEGORY(lcIcons, "suop.icons")

}  // namespace

IconTheme::IconTheme(QObject *parent) : QObject(parent)
{
    loadPacksFrom(kIconRoot);
    if (!m_packs.contains(QString::fromLatin1(kFallbackPack))) {
        // Not recoverable by falling back to another pack: resolve()'s last
        // resort IS this pack, so every icon in the app would silently draw
        // nothing. Louder than a qCWarning for that reason.
        qCCritical(lcIcons) << "fallback icon pack" << kFallbackPack
                            << "missing from" << kIconRoot << "- icons will not render";
    }
}

void IconTheme::loadPacksFrom(const QString &root)
{
    const QDir dir(root);
    // Discovery, not a hardcoded list — dropping in a pack directory is all
    // it takes to register one.
    const QStringList ids = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &id : ids) {
        if (!loadPack(dir.filePath(id), id))
            qCWarning(lcIcons) << "skipped unloadable icon pack" << id;
    }
}

bool IconTheme::loadPack(const QString &dir, const QString &id)
{
    QFile manifest(dir + "/pack.json");
    if (!manifest.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(manifest.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcIcons) << "icon pack" << id << "has invalid pack.json:" << parseError.errorString();
        return false;
    }

    const QJsonObject root = doc.object();
    IconPack pack;
    pack.id = id;
    pack.name = root.value("name").toString(id);
    pack.description = root.value("description").toString();
    pack.kind = root.value("kind").toString("glyph");
    pack.scale = root.value("scale").toDouble(1.0);

    const QJsonObject icons = root.value("icons").toObject();
    for (auto it = icons.constBegin(); it != icons.constEnd(); ++it) {
        QVariantMap entry;
        // Two accepted forms per icon: a bare string (inherits the pack's
        // kind and scale — the common case), or an object overriding either.
        // The object form is what lets one pack mix kinds, e.g. `classic`
        // supplying most icons as glyphs but play/pause as builtin drawn
        // shapes, and what carries per-icon optical correction via `scale`.
        if (it.value().isString()) {
            entry["kind"] = pack.kind;
            entry["value"] = it.value().toString();
            entry["scale"] = pack.scale;
        } else if (it.value().isObject()) {
            const QJsonObject o = it.value().toObject();
            entry["kind"] = o.value("kind").toString(pack.kind);
            entry["value"] = o.value("value").toString();
            entry["scale"] = o.value("scale").toDouble(pack.scale);
        } else {
            qCWarning(lcIcons) << "icon pack" << id << "entry" << it.key() << "is neither string nor object";
            continue;
        }
        entry["pack"] = id;
        pack.icons.insert(it.key(), entry);
    }

    m_packs.insert(id, pack);
    return true;
}

QVariantMap IconTheme::resolve(const QString &name,
                               const QString &packId,
                               const QVariantMap &overrides) const
{
    // Override -> active pack -> fallback. Each step is skipped rather than
    // terminal if that pack doesn't define the name, so a partial pack (one
    // that only redefines a handful of icons) degrades to the active pack
    // per-icon instead of leaving holes.
    const QStringList candidates = {
        overrides.value(name).toString(),
        packId,
        QString::fromLatin1(kFallbackPack),
    };

    for (const QString &candidate : candidates) {
        if (candidate.isEmpty())
            continue;
        const auto pack = m_packs.constFind(candidate);
        if (pack == m_packs.constEnd())
            continue;
        const auto icon = pack->icons.constFind(name);
        if (icon != pack->icons.constEnd())
            return *icon;
    }

    return {};
}

QStringList IconTheme::packIds() const
{
    QStringList ids = m_packs.keys();
    ids.sort();
    // Fallback pack heads the list: it's the default and the one guaranteed
    // complete, so it should be the first thing offered in the picker.
    const QString fallback = QString::fromLatin1(kFallbackPack);
    if (ids.removeOne(fallback))
        ids.prepend(fallback);
    return ids;
}

QString IconTheme::packName(const QString &packId) const
{
    return m_packs.value(packId).name;
}

QString IconTheme::packDescription(const QString &packId) const
{
    return m_packs.value(packId).description;
}

bool IconTheme::packHasIcon(const QString &packId, const QString &name) const
{
    const auto pack = m_packs.constFind(packId);
    return pack != m_packs.constEnd() && pack->icons.contains(name);
}

QStringList IconTheme::iconNames() const
{
    QStringList names;
    for (const IconPack &pack : m_packs) {
        for (auto it = pack.icons.constBegin(); it != pack.icons.constEnd(); ++it) {
            if (!names.contains(it.key()))
                names.append(it.key());
        }
    }
    names.sort();
    return names;
}

// ---------------------------------------------------------------------------

IconImageProvider::IconImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage IconImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    Q_UNUSED(requestedSize)

    {
        QMutexLocker locker(&m_mutex);
        const auto cached = m_cache.constFind(id);
        if (cached != m_cache.constEnd()) {
            if (size)
                *size = cached->size();
            return *cached;
        }
    }

    // id is "<pack>/<file>?color=<#rrggbb>&size=<px>". Split on the FIRST
    // '?' — QUrl would need the path percent-encoded to survive, and the
    // path here is always a plain pack/filename pair.
    const qsizetype queryAt = id.indexOf('?');
    const QString path = queryAt < 0 ? id : id.left(queryAt);
    const QUrlQuery query(queryAt < 0 ? QString() : id.mid(queryAt + 1));

    const QColor color = QColor::fromString(query.queryItemValue("color", QUrl::FullyDecoded));
    const int px = qMax(1, query.queryItemValue("size").toInt());

    QSvgRenderer renderer(QString(kIconRoot) + '/' + path);
    if (!renderer.isValid()) {
        qCWarning(lcIcons) << "icon svg missing or invalid:" << path;
        return {};
    }

    // Square by construction: every Material Symbols icon is authored on a
    // square viewBox, and the call sites all reserve square boxes.
    QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        renderer.render(&painter, QRectF(0, 0, px, px));
        // The SVGs ship with their fills stripped (see fetch script), so
        // they render in QPainter's default black. SourceIn replaces that
        // with `color` while keeping the coverage/antialiasing alpha — the
        // standard monochrome-icon tint.
        if (color.isValid()) {
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(image.rect(), color);
        }
    }

    if (size)
        *size = image.size();

    {
        QMutexLocker locker(&m_mutex);
        m_cache.insert(id, image);
    }
    return image;
}
