#pragma once

#include <QObject>
#include <QString>
#include <qqml.h>

// QML-facing wrapper around QGuiApplication::clipboard(). Qt Quick exposes no
// clipboard API of its own — the usual workaround is a hidden TextEdit you
// fill, selectAll() and copy() — so this exists to keep that hack out of the
// QML, and to give the one caller that needs it something real (and testable)
// to call.
//
// That caller is MainPanel's transcript copy: a chapter title is NOT a
// character in transcriptArea's document (displayText reserves a blank line
// for each and an overlay field draws the title), so the native Ctrl+C drops
// every title the selection swept over. Splicing them back in means building
// the string in QML and putting THAT on the clipboard, which is only possible
// with something like this.
class Clipboard : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit Clipboard(QObject *parent = nullptr);

    // Replaces the system clipboard's contents with `text`.
    Q_INVOKABLE void setText(const QString &text);

    // The clipboard's current text. Exists so a test can read back what
    // setText() put there.
    Q_INVOKABLE QString text() const;
};
