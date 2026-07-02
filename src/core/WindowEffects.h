#pragma once

#include <QObject>
#include <QRectF>
#include <QQuickWindow>
#include <qqml.h>

// QML-facing wrapper around KWindowEffects::enableBlurBehind. QML never calls
// KWindowEffects directly so this class stays the single owner of the call.
class WindowEffects : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit WindowEffects(QObject *parent = nullptr);

    // enableBlurBehind is last-writer-wins on a given window, not additive: a
    // second call silently replaces the region from a first call rather than
    // merging with it. So panels can't each make their own call — this takes
    // both panels' state at once and makes exactly one authoritative call
    // with their union. region1/region2 must already be in window-relative
    // (contentItem-scene) coordinates; map item-local rects via mapToItem/
    // mapRectToItem against the root window's contentItem before calling.
    Q_INVOKABLE void updateBlur(QQuickWindow *window,
                                 const QRectF &region1, bool enabled1,
                                 const QRectF &region2, bool enabled2);
};
