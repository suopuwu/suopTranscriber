#pragma once

#include <QObject>
#include <QUrl>
#include <qqml.h>

class QDBusMessage;

// QML-facing wrapper around the desktop's file manager, via the freedesktop.org
// org.freedesktop.FileManager1 D-Bus interface (the standard cross-DE way to
// reveal a file with it SELECTED — Qt.openUrlExternally can only open a folder,
// never select something inside it). QML never builds these calls itself so
// this class stays the single owner of them, and of the xdg-activation dance
// they need to actually raise the file manager.
//
// Not on VaultTreeModel (where reveal used to live): this pulls in
// KF6::WindowSystem, and VaultTreeModel.cpp compiles into eight headless test
// binaries that have no business linking a window-activation library.
class DesktopIntegration : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit DesktopIntegration(QObject *parent = nullptr);

    // Opens fileUrl's containing folder in the file manager with fileUrl
    // itself selected, raising and focusing that window.
    Q_INVOKABLE void revealInFileManager(const QUrl &fileUrl);

    // Opens folderUrl itself in the file manager (nothing selected), raising
    // and focusing that window. For when there's no specific file to point at.
    Q_INVOKABLE void openInFileManager(const QUrl &folderUrl);

    // The FileManager1 `method`(as URIs, s StartupID) call that would be sent
    // for `url`. Split out from the sending so a test can assert the message's
    // shape — above all that the activation token really rides along as the
    // StartupID, which is the whole reason the file manager comes to the front.
    static QDBusMessage fileManagerCall(const QString &method, const QUrl &url,
                                        const QString &activationToken);

private:
    void callFileManager(const QString &method, const QUrl &url);
};
