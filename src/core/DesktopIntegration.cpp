#include "DesktopIntegration.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QFuture>
#include <QGuiApplication>
#include <QStringList>
#include <QWindow>

#include <KWaylandExtras>
#include <KWindowSystem>

DesktopIntegration::DesktopIntegration(QObject *parent)
    : QObject(parent)
{
}

QDBusMessage DesktopIntegration::fileManagerCall(const QString &method, const QUrl &url,
                                                  const QString &activationToken)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                       QStringLiteral("/org/freedesktop/FileManager1"),
                                                       QStringLiteral("org.freedesktop.FileManager1"),
                                                       method);
    // FullyEncoded, not the toString() default (PrettyDecoded): these are URIs
    // on the wire, and a vault full of "My Recording.mp3" would otherwise ship
    // a raw space inside one.
    msg << QStringList{url.toString(QUrl::FullyEncoded)} << activationToken;
    return msg;
}

void DesktopIntegration::callFileManager(const QString &method, const QUrl &url)
{
    if (url.isEmpty())
        return;

    // Wayland only lets one app raise another app's window if it hands over an
    // xdg-activation token proving the request came from a real user action in
    // a focused window; FileManager1's StartupID argument is where that token
    // goes. Pass an empty StartupID and the compositor denies the raise — the
    // file manager still selects the file, just silently behind our own window,
    // which is exactly what an activation-token-less reveal looks like. The
    // token can only be minted asynchronously, hence the continuation.
    //
    // The token's appId is left empty (any app may redeem it) because
    // FileManager1 is D-Bus-activated: the desktop, not us, decides which file
    // manager answers, so we have no id to name here.
    QWindow *window = QGuiApplication::focusWindow();
    if (!window || !KWindowSystem::isPlatformWayland()) {
        // X11/XWayland: no token to fetch, and focus stealing isn't policed the
        // same way. Fire and forget, as this always did.
        QDBusConnection::sessionBus().asyncCall(fileManagerCall(method, url, QString()));
        return;
    }

    KWaylandExtras::xdgActivationToken(window, QString()).then(this, [method, url](const QString &token) {
        QDBusConnection::sessionBus().asyncCall(fileManagerCall(method, url, token));
    });
}

void DesktopIntegration::revealInFileManager(const QUrl &fileUrl)
{
    callFileManager(QStringLiteral("ShowItems"), fileUrl);
}

void DesktopIntegration::openInFileManager(const QUrl &folderUrl)
{
    callFileManager(QStringLiteral("ShowFolders"), folderUrl);
}
