#include "FocusBlurFilter.h"
#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>

bool FocusBlurFilter::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::MouseButtonPress)
        return false;
    auto *me = static_cast<QMouseEvent *>(event);
    if (me->button() != Qt::LeftButton)
        return false;

    // The object holding input focus IS the editable primitive
    // (QQuickTextInput for TextField, QQuickTextEdit for TextArea) — not the
    // Control wrapper. Only those two are things a click should blur.
    QObject *fo = QGuiApplication::focusObject();
    auto *field = qobject_cast<QQuickItem *>(fo);
    if (!field || !(field->inherits("QQuickTextInput") || field->inherits("QQuickTextEdit")))
        return false;

    // Only act on a press in the SAME window as the field, mapped into the
    // field's coordinates. Inside the field (incl. the start of a drag-select)
    // => leave it. Outside => blur.
    QQuickWindow *win = field->window();
    if (!win || watched != win)
        return false;
    const QPointF local = field->mapFromScene(me->scenePosition());
    if (field->contains(local))
        return false;

    field->setFocus(false);
    return false; // observe only — the press still does whatever it was going to
}
