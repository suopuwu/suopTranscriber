#include "Clipboard.h"

#include <QClipboard>
#include <QGuiApplication>

Clipboard::Clipboard(QObject *parent) : QObject(parent) {}

void Clipboard::setText(const QString &text)
{
    // Null under a QCoreApplication (no GUI) rather than a QGuiApplication —
    // guarded so a headless caller no-ops instead of crashing.
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}

QString Clipboard::text() const
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    return clipboard ? clipboard->text() : QString();
}
