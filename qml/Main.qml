import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Qt5Compat.GraphicalEffects
import SuopTranscriber

ApplicationWindow {
    id: window
    width: 1024
    height: 720
    visible: true
    color: "transparent"
    title: qsTr("suopTranscriber")
    flags: AppSettings.useSystemTitlebar ? Qt.Window : (Qt.Window | Qt.FramelessWindowHint)

    VaultTreeModel {
        id: vaultModel
    }

    AudioPlayer {
        id: player
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Open Vault")
        onAccepted: vaultModel.vaultPath = selectedFolder
    }

    TitleBar {
        id: customTitleBar
        window: window
        title: window.title
        // When assigned to `header:` below, ApplicationWindow controls this
        // item's position/size; when unassigned (system titlebar mode) it's
        // still a plain child of contentItem, so hide it explicitly to avoid
        // it floating over the content at its last header-slot geometry.
        visible: !AppSettings.useSystemTitlebar
    }

    header: AppSettings.useSystemTitlebar ? null : customTitleBar

    SettingsPopup {
        id: settingsPopup
        parent: window.contentItem
        onChangeVaultRequested: folderDialog.open()
    }

    function updateBlurRegions() {
        if (!sidebar || !mainPanel || !window.contentItem)
            return
        const sidebarRect = sidebar.mapToItem(window.contentItem, 0, 0, sidebar.width, sidebar.height)
        const mainRect = mainPanel.mapToItem(window.contentItem, 0, 0, mainPanel.width, mainPanel.height)
        WindowEffects.updateBlur(window,
            Qt.rect(sidebarRect.x, sidebarRect.y, sidebarRect.width, sidebarRect.height), AppSettings.sidebarBlur,
            Qt.rect(mainRect.x, mainRect.y, mainRect.width, mainRect.height), AppSettings.mainBlur)
    }

    Connections {
        target: AppSettings
        function onSidebarBlurChanged() { updateBlurRegions() }
        function onMainBlurChanged() { updateBlurRegions() }

        function onUseSystemTitlebarChanged() {
            // Wayland doesn't redecorate an already-shown window in place;
            // force a hide/show cycle so the new frameless/framed flags
            // actually take effect (window position isn't persisted — x/y
            // is unreadable/unsettable on Wayland anyway).
            const w = window.width
            const h = window.height
            window.visible = false
            titlebarNudgeTimer.savedW = w
            titlebarNudgeTimer.savedH = h
            titlebarNudgeTimer.start()
        }
    }

    Timer {
        id: titlebarNudgeTimer
        property int savedW: 0
        property int savedH: 0
        interval: 50
        onTriggered: {
            window.visible = true
            window.width = savedW
            window.height = savedH
        }
    }

    onWidthChanged: Qt.callLater(updateBlurRegions)
    onHeightChanged: Qt.callLater(updateBlurRegions)

    Connections {
        target: vaultModel
        function onVaultPathChanged() {
            AppSettings.lastVaultPath = vaultModel.vaultPath
        }
    }

    Component.onCompleted: {
        if (initialVaultPath.toString() !== "")
            vaultModel.vaultPath = initialVaultPath
        else if (AppSettings.lastVaultPath.toString() !== "")
            vaultModel.vaultPath = AppSettings.lastVaultPath
        Qt.callLater(updateBlurRegions)
    }

    Item {
        id: rootContent
        anchors.fill: parent
        layer.enabled: !AppSettings.useSystemTitlebar
        layer.effect: OpacityMask {
            maskSource: Rectangle {
                width: rootContent.width
                height: rootContent.height
                radius: AppSettings.borderRadius
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            SplitView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                orientation: Qt.Horizontal

                Sidebar {
                    id: sidebar
                    SplitView.preferredWidth: 320
                    SplitView.minimumWidth: 200
                    model: vaultModel
                    onFileActivated: (path) => {
                        player.source = "file://" + path
                        player.play()
                    }
                    onSettingsRequested: settingsPopup.open()
                    onWidthChanged: Qt.callLater(window.updateBlurRegions)
                    onHeightChanged: Qt.callLater(window.updateBlurRegions)
                }

                MainPanel {
                    id: mainPanel
                    SplitView.fillWidth: true
                    vaultPath: vaultModel.vaultPath
                    player: player
                    onOpenVaultRequested: folderDialog.open()
                    onWidthChanged: Qt.callLater(window.updateBlurRegions)
                    onHeightChanged: Qt.callLater(window.updateBlurRegions)
                }
            }

            BottomBar {
                id: bottomBar
                Layout.fillWidth: true
                vaultPath: vaultModel.vaultPath
            }
        }
    }
}
