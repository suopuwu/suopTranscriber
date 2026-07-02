#include "WindowEffects.h"

#include <KWindowEffects>

WindowEffects::WindowEffects(QObject *parent)
    : QObject(parent)
{
}

void WindowEffects::updateBlur(QQuickWindow *window,
                                const QRectF &region1, bool enabled1,
                                const QRectF &region2, bool enabled2)
{
    if (!window)
        return;

    QRegion region;
    if (enabled1)
        region += region1.toAlignedRect();
    if (enabled2)
        region += region2.toAlignedRect();

    if (region.isEmpty())
        KWindowEffects::enableBlurBehind(window, false);
    else
        KWindowEffects::enableBlurBehind(window, true, region);
}
