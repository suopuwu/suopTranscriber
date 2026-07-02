import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SuopTranscriber

Popup {
    id: settingsPopup

    signal changeVaultRequested()

    width: 380
    height: Math.min(contentColumn.implicitHeight + 32, maxHeight)
    modal: true
    focus: true
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    readonly property real maxHeight: parent ? parent.height * 0.9 : 600

    background: Rectangle {
        color: "#242424"
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: "#3a3a3a"
    }

    ColorDialog {
        id: sidebarColorDialog
        title: qsTr("Sidebar Background Color")
        selectedColor: AppSettings.sidebarBackgroundColor
        onAccepted: AppSettings.sidebarBackgroundColor = selectedColor
    }

    ColorDialog {
        id: mainColorDialog
        title: qsTr("Main Panel Background Color")
        selectedColor: AppSettings.mainBackgroundColor
        onAccepted: AppSettings.mainBackgroundColor = selectedColor
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: 16
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            id: contentColumn
            width: parent.width
            spacing: 16

            Label {
                text: qsTr("Settings")
                color: "white"
                font.bold: true
                font.pixelSize: 16
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Vault")
                }
                Button {
                    id: changeVaultButton
                    text: qsTr("Change Vault")
                    hoverEnabled: true
                    onClicked: settingsPopup.changeVaultRequested()

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: changeVaultButton.hovered
                    }
                }
            }

            Label {
                text: qsTr("Appearance")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Border radius")
                }
                Slider {
                    id: radiusSlider
                    from: 0
                    to: 24
                    value: AppSettings.borderRadius
                    hoverEnabled: true
                    onMoved: AppSettings.borderRadius = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: radiusSlider.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Use system titlebar")
                }
                Switch {
                    id: titlebarSwitch
                    checked: AppSettings.useSystemTitlebar
                    hoverEnabled: true
                    onToggled: AppSettings.useSystemTitlebar = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: titlebarSwitch.hovered
                    }
                }
            }

            Label {
                text: qsTr("Sidebar")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Background color")
                }
                Button {
                    id: sidebarColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: sidebarColorDialog.open()

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarColorButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Opacity")
                }
                Slider {
                    id: sidebarOpacitySlider
                    from: 0.1
                    to: 1
                    value: AppSettings.sidebarOpacity
                    hoverEnabled: true
                    onMoved: AppSettings.sidebarOpacity = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarOpacitySlider.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Blur")
                }
                Switch {
                    id: sidebarBlurSwitch
                    checked: AppSettings.sidebarBlur
                    hoverEnabled: true
                    onToggled: AppSettings.sidebarBlur = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: sidebarBlurSwitch.hovered
                    }
                }
            }

            Label {
                text: qsTr("Main panel")
                color: "white"
                font.bold: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Background color")
                }
                Button {
                    id: mainColorButton
                    text: qsTr("Choose…")
                    hoverEnabled: true
                    onClicked: mainColorDialog.open()

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainColorButton.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Opacity")
                }
                Slider {
                    id: mainOpacitySlider
                    from: 0.1
                    to: 1
                    value: AppSettings.mainOpacity
                    hoverEnabled: true
                    onMoved: AppSettings.mainOpacity = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainOpacitySlider.hovered
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    color: "white"
                    text: qsTr("Blur")
                }
                Switch {
                    id: mainBlurSwitch
                    checked: AppSettings.mainBlur
                    hoverEnabled: true
                    onToggled: AppSettings.mainBlur = checked

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: mainBlurSwitch.hovered
                    }
                }
            }
        }
    }
}
