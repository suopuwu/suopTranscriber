// Spec for DesktopIntegration — the QML-facing wrapper around the desktop's
// file manager (org.freedesktop.FileManager1).
//
// Only the message SHAPE is testable here: actually sending needs a session bus
// with a file manager on it, and the xdg-activation token needs a real Wayland
// compositor and a focused window. So DesktopIntegration splits the message
// construction out as a static, and this pins the part that's easy to silently
// get wrong — the StartupID argument. An empty StartupID is what a
// reveal-that-doesn't-come-to-the-front looks like: the file manager still
// selects the file, but the compositor denies it the raise, so it happens
// behind our own window.

#include "DesktopIntegration.h"

#include <QDBusMessage>
#include <QTest>

class TstDesktopIntegration : public QObject
{
    Q_OBJECT

private slots:
    // The reveal call must address FileManager1's ShowItems and carry the
    // activation token as the StartupID argument — that token is the entire
    // reason the file manager's window is raised and focused on Wayland.
    void fileManagerCall_addressesShowItems_andPassesTokenAsStartupId()
    {
        const QDBusMessage msg = DesktopIntegration::fileManagerCall(
            QStringLiteral("ShowItems"), QUrl(QStringLiteral("file:///vault/talk.mp3")),
            QStringLiteral("token-abc"));

        QCOMPARE(msg.service(), QStringLiteral("org.freedesktop.FileManager1"));
        QCOMPARE(msg.path(), QStringLiteral("/org/freedesktop/FileManager1"));
        QCOMPARE(msg.interface(), QStringLiteral("org.freedesktop.FileManager1"));
        QCOMPARE(msg.member(), QStringLiteral("ShowItems"));

        // ShowItems(as URIs, s StartupID) — exactly two arguments, in that order.
        QCOMPARE(msg.arguments().size(), 2);
        QCOMPARE(msg.arguments().at(0).toStringList(),
                 QStringList{QStringLiteral("file:///vault/talk.mp3")});
        QCOMPARE(msg.arguments().at(1).toString(), QStringLiteral("token-abc"));
    }

    // The URI argument is an `as` (array of strings), not a bare `s` — passing
    // a plain string would make the call fail to match FileManager1's signature.
    void fileManagerCall_passesUriAsArrayOfStrings()
    {
        const QDBusMessage msg = DesktopIntegration::fileManagerCall(
            QStringLiteral("ShowFolders"), QUrl(QStringLiteral("file:///vault")), QString());
        QCOMPARE(msg.arguments().at(0).typeId(), QMetaType::QStringList);
    }

    // The URI must be percent-encoded on the wire — a vault full of
    // "My Recording.mp3" would otherwise put a raw space inside it. This is the
    // difference between toString()'s PrettyDecoded default and FullyEncoded,
    // and it's invisible until a filename has a space in it.
    void fileManagerCall_percentEncodesTheUri()
    {
        const QDBusMessage msg = DesktopIntegration::fileManagerCall(
            QStringLiteral("ShowItems"), QUrl(QStringLiteral("file:///vault/My Recording.mp3")),
            QString());
        QCOMPARE(msg.arguments().at(0).toStringList(),
                 QStringList{QStringLiteral("file:///vault/My%20Recording.mp3")});
    }

    // Opening a folder (no file to select) goes through the same call shape,
    // token included — it must come to the front just like a reveal does.
    void fileManagerCall_showFoldersTakesSameTokenPath()
    {
        const QDBusMessage msg = DesktopIntegration::fileManagerCall(
            QStringLiteral("ShowFolders"), QUrl(QStringLiteral("file:///vault")),
            QStringLiteral("token-xyz"));
        QCOMPARE(msg.member(), QStringLiteral("ShowFolders"));
        QCOMPARE(msg.arguments().at(0).toStringList(), QStringList{QStringLiteral("file:///vault")});
        QCOMPARE(msg.arguments().at(1).toString(), QStringLiteral("token-xyz"));
    }
};

QTEST_GUILESS_MAIN(TstDesktopIntegration)
#include "tst_desktopintegration.moc"
