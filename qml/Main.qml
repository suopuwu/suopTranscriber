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
    title: qsTr("Suop Transcriber")
    flags: AppSettings.useSystemTitlebar ? Qt.Window : (Qt.Window | Qt.FramelessWindowHint)

    VaultTreeModel {
        id: vaultModel
    }

    AudioPlayer {
        id: player
    }

    TranscriptionController {
        id: transcriptionController
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Open Vault")
        onAccepted: vaultModel.vaultPath = selectedFolder
    }

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

        // Sidebar/MainPanel only round the corners touching the SplitView
        // divider; those cutouts need a filler behind them (matching the
        // divider color) or the desktop shows through. Sized/positioned to
        // exactly the rounded corner itself so it never shows behind the
        // panels' own (independently opaque/blurred) bodies.
        const r = AppSettings.borderRadius
        cornerFillSidebarTR.x = sidebarRect.x + sidebarRect.width - r
        cornerFillSidebarTR.y = sidebarRect.y
        cornerFillSidebarBR.x = sidebarRect.x + sidebarRect.width - r
        cornerFillSidebarBR.y = sidebarRect.y + sidebarRect.height - r
        cornerFillMainTL.x = mainRect.x
        cornerFillMainTL.y = mainRect.y
        cornerFillMainBL.x = mainRect.x
        cornerFillMainBL.y = mainRect.y + mainRect.height - r
    }

    Connections {
        target: AppSettings
        function onSidebarBlurChanged() { updateBlurRegions() }
        function onMainBlurChanged() { updateBlurRegions() }
        function onBorderRadiusChanged() { Qt.callLater(updateBlurRegions) }

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

        // Sidebar/MainPanel are independently semi-transparent (blur-behind
        // glass) — these fillers sit only behind their divider-facing rounded
        // corners (sized/positioned in updateBlurRegions), not full-bleed,
        // so they don't sit behind the panels' own translucent bodies and
        // defeat the blur-behind effect.
        Rectangle {
            id: cornerFillSidebarTR
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: window.palette.button
        }
        Rectangle {
            id: cornerFillSidebarBR
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: window.palette.button
        }
        Rectangle {
            id: cornerFillMainTL
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: window.palette.button
        }
        Rectangle {
            id: cornerFillMainBL
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: window.palette.button
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            TitleBar {
                id: customTitleBar
                Layout.fillWidth: true
                window: window
                title: window.title
                visible: !AppSettings.useSystemTitlebar
                onSettingsRequested: settingsPopup.open()
            }

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
                    }
                    onWidthChanged: Qt.callLater(window.updateBlurRegions)
                    onHeightChanged: Qt.callLater(window.updateBlurRegions)
                }

                MainPanel {
                    id: mainPanel
                    SplitView.fillWidth: true
                    vaultPath: vaultModel.vaultPath
                    vaultLocalPath: vaultModel.vaultLocalPath
                    player: player
                    transcriptionController: transcriptionController
                    onOpenVaultRequested: folderDialog.open()
                    onWidthChanged: Qt.callLater(window.updateBlurRegions)
                    onHeightChanged: Qt.callLater(window.updateBlurRegions)
                }
            }

            BottomBar {
                id: bottomBar
                Layout.fillWidth: true
                vaultPath: vaultModel.vaultPath
                vaultLocalPath: vaultModel.vaultLocalPath
                transcriptionController: transcriptionController
                onSettingsRequested: settingsPopup.open()
            }
        }
    }
}
