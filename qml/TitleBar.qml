import QtQuick
import QtQuick.Controls
import SuopTranscriber

Item {
    id: titleBar

    property var window
    property string title: ""
    signal settingsRequested()
    signal forceRetranscribeAllRequested()
    signal recordRequested()
    signal usbGrabberRequested()

    // Inline confirm state for the "Force retranscribe all" menu row (see
    // MainPanel.qml's speaker-merge confirm row for the same swap-in-place
    // pattern) — reset whenever the menu closes so it never reopens
    // mid-confirm.
    property bool forceRetranscribeConfirming: false

    implicitHeight: 36

    Rectangle {
        anchors.fill: parent
        color: "#1a1a1a"
        // Only visible when frameless (see Main.qml), so these radii are
        // always safe to apply — rounds the window's true top corners
        // natively instead of relying on an outer OpacityMask.
        topLeftRadius: AppSettings.borderRadius
        topRightRadius: AppSettings.borderRadius
    }

    // Bar-wide double-click to maximize, the way a system titlebar behaves.
    // DragThreshold (a PASSIVE grab) is deliberate and load-bearing: it has to
    // coexist with the DragHandler below, which needs to be able to take the
    // grab for a window move without this cancelling first.
    //
    // The flip side is that a passive grab does NOT stop at this bar's own
    // buttons — nested TapHandlers don't consume each other's taps the way an
    // accepted MouseArea press would, so both fire. Every button in here is
    // therefore on ReleaseWithinBounds (exclusive grab from press), which is
    // what keeps a double-click on a button from ALSO toggling maximize. See
    // tst_titlebar_buttons.cpp, which fails on all of them without it.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: if (tapCount === 2) titleBar.window.visibility = titleBar.window.visibility === Window.Maximized ? Window.Windowed : Window.Maximized
        gesturePolicy: TapHandler.DragThreshold
    }

    DragHandler {
        target: null
        onActiveChanged: if (active) titleBar.window.startSystemMove()
    }

    Item {
        id: settingsButton
        objectName: "titleBarSettingsButton"
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 24
        height: 24

        HoverHandler {
            id: settingsHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            // Exclusive grab from press — see the bar-wide maximize handler.
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: titleBar.settingsRequested()
        }
        HoverBorder {
            anchors.fill: parent
            active: settingsHover.hovered
        }
        AppIcon {
            anchors.centerIn: parent
            name: "settings"
            iconSize: 14
            color: "white"
        }
    }

    Item {
        id: recordButton
        objectName: "titleBarRecordButton"
        anchors.left: settingsButton.right
        anchors.leftMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        width: 24
        height: 24

        HoverHandler {
            id: recordHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: titleBar.recordRequested()
        }
        HoverBorder {
            anchors.fill: parent
            active: recordHover.hovered
        }
        AppIcon {
            anchors.centerIn: parent
            name: "record"
            iconSize: 14
            color: "white"
        }
    }

    Item {
        id: usbGrabberButton
        objectName: "titleBarUsbGrabberButton"
        anchors.left: recordButton.right
        anchors.leftMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        width: 24
        height: 24

        HoverHandler {
            id: usbGrabberHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: titleBar.usbGrabberRequested()
        }
        HoverBorder {
            anchors.fill: parent
            active: usbGrabberHover.hovered
        }
        AppIcon {
            anchors.centerIn: parent
            name: "usb"
            iconSize: 14
            color: "white"
        }
    }

    // Infrequently used actions (currently just "force retranscribe all")
    // that don't warrant a spot in Settings — kept out of the way next to
    // the gear rather than cluttering the per-row/bulk sidebar controls.
    Item {
        id: moreActionsButton
        objectName: "titleBarMoreActionsButton"
        anchors.left: usbGrabberButton.right
        anchors.leftMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        width: 24
        height: 24

        HoverHandler {
            id: moreActionsHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: moreActionsMenu.visible ? moreActionsMenu.close() : moreActionsMenu.open()
        }
        HoverBorder {
            anchors.fill: parent
            active: moreActionsHover.hovered
        }
        AppIcon {
            anchors.centerIn: parent
            name: "more"
            iconSize: 14
            color: "white"
        }
    }

    Popup {
        id: moreActionsMenu
        x: moreActionsButton.x
        y: titleBar.height + 4
        width: 220
        padding: 4
        // O21: was modal:false — no real dim + no real input block (Qt's
        // automatic block-plane only activates for modal:true). Outside
        // clicks still close it via CloseOnPressOutside, they just no
        // longer also land on whatever's behind the menu.
        modal: true
        dim: true
        focus: false
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
        onClosed: titleBar.forceRetranscribeConfirming = false

        background: Rectangle {
            color: "#242424"
            radius: AppSettings.borderRadius
            border.width: 1
            border.color: "#3a3a3a"
        }

        Item {
            id: forceRetranscribeRow
            width: moreActionsMenu.width - moreActionsMenu.padding * 2
            height: 28

            HoverHandler {
                id: forceRetranscribeHover
                cursorShape: Qt.PointingHandCursor
                enabled: !titleBar.forceRetranscribeConfirming
            }
            TapHandler {
                enabled: !titleBar.forceRetranscribeConfirming
                onTapped: titleBar.forceRetranscribeConfirming = true
            }
            Rectangle {
                anchors.fill: parent
                radius: AppSettings.borderRadius
                color: forceRetranscribeHover.hovered ? "#3a3a3a" : "transparent"
            }
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: !titleBar.forceRetranscribeConfirming
                text: qsTr("Force retranscribe all")
                color: "white"
                font.pixelSize: 12
            }

            // Inline confirm, matching the speaker-merge pill swap in
            // MainPanel.qml — this queues a job for every audio file in the
            // vault (even already-transcribed ones), so a stray click
            // shouldn't fire it outright.
            Row {
                anchors.centerIn: parent
                visible: titleBar.forceRetranscribeConfirming
                spacing: 6
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Retranscribe entire vault?")
                    color: "#dddddd"
                    font.pixelSize: 11
                }
                AppIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "check"
                    iconSize: 12
                    color: "#8fdf9f"
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            titleBar.forceRetranscribeConfirming = false
                            moreActionsMenu.close()
                            titleBar.forceRetranscribeAllRequested()
                        }
                    }
                }
                AppIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: "cancel"
                    iconSize: 12
                    color: "#e07a7a"
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: titleBar.forceRetranscribeConfirming = false }
                }
            }
        }
    }

    Text {
        anchors.left: moreActionsButton.right
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: titleBar.title
        color: "white"
    }

    Row {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        spacing: 0

        component TitleBarButton: Item {
            id: tbButton
            property string iconName: ""
            signal clicked()
            width: 46
            height: parent.height

            HoverHandler {
                id: hover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: tbButton.clicked()
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: 4
                radius: AppSettings.borderRadius
                color: hover.hovered ? "#3a3a3a" : "transparent"
            }
            AppIcon {
                anchors.centerIn: parent
                name: tbButton.iconName
                iconSize: 14
                color: "white"
            }
        }

        TitleBarButton {
            iconName: "window-minimize"
            onClicked: titleBar.window.showMinimized()
        }
        TitleBarButton {
            iconName: "window-maximize"
            onClicked: titleBar.window.visibility = titleBar.window.visibility === Window.Maximized ? Window.Windowed : Window.Maximized
        }
        TitleBarButton {
            iconName: "window-close"
            onClicked: titleBar.window.close()
        }
    }
}
