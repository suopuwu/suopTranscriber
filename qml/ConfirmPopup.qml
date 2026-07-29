import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// A modal "are you sure?" dialog: a wrapping message and a Cancel / confirm
// button pair. The confirm button is styled destructive (Theme.danger) by
// default because every current caller is a delete.
//
//     ConfirmPopup {
//         id: deleteConfirm
//         message: qsTr("Delete this permanently? This can't be undone.")
//         confirmText: qsTr("Delete")
//         onConfirmed: controller.doDelete()
//     }
//     // ... later: deleteConfirm.open()
//
// This chrome was copy-pasted across ModelsPage, MainPanel and Sidebar (and a
// divergent Dialog variant in BenchmarkPage) — the delete dialogs were meant
// to "behave like siblings" but were four hand-kept copies that had already
// started to drift (one forgot dim/focus/closePolicy). The
// popup chrome and the two-button row are identical every time; only the
// message text and what the confirm button does differ, which are exactly the
// `message`/`confirmText` properties and the `confirmed()` signal.
//
// `message` and `confirmText` are plain properties, so a caller with a
// multi-branch message (MainPanel's transcript/audio/file delete) just binds
// an expression. Callers that need something richer than "one message, two
// buttons" (an input field, a list of destinations, a third button) are NOT
// confirm dialogs and should stay their own Popup — don't force them here.
Popup {
    id: root

    property string message: ""
    property string confirmText: qsTr("Delete")
    property string cancelText: qsTr("Cancel")
    // Colors the confirm label; delete flows want the red. A non-destructive
    // confirmation (rare) can set this false for the normal text color.
    property bool destructive: true

    signal confirmed()
    signal cancelled()

    anchors.centerIn: Overlay.overlay
    width: 320
    padding: 12
    modal: true
    dim: true
    focus: true
    closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape

    background: Rectangle {
        color: Theme.surfaceDialog
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: Theme.border
    }

    ColumnLayout {
        width: root.width - root.padding * 2
        spacing: 10

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.message
            color: "white"
            font.pixelSize: 12
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 6

            // Cancel and confirm are the labelled-text-button idiom (Item +
            // HoverHandler + TapHandler + HoverBorder + Text), NOT AppIconButton
            // — these carry a text label whose width drives the button, which
            // the icon component doesn't model.
            Item {
                id: cancelButton
                objectName: "confirmPopupCancelButton"
                Layout.preferredWidth: cancelLabel.implicitWidth + 16
                Layout.preferredHeight: 24
                HoverHandler {
                    id: cancelHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        root.cancelled()
                        root.close()
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: cancelHover.hovered
                }
                Text {
                    id: cancelLabel
                    anchors.centerIn: parent
                    text: root.cancelText
                    color: Theme.textMuted
                    font.pixelSize: 12
                }
            }

            Item {
                id: confirmButton
                objectName: "confirmPopupConfirmButton"
                Layout.preferredWidth: confirmLabel.implicitWidth + 16
                Layout.preferredHeight: 24
                HoverHandler {
                    id: confirmHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        root.confirmed()
                        root.close()
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: confirmHover.hovered
                }
                Text {
                    id: confirmLabel
                    anchors.centerIn: parent
                    text: root.confirmText
                    color: root.destructive ? Theme.danger : Theme.textPrimary
                    font.pixelSize: 12
                }
            }
        }
    }
}
