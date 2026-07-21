// Bound is required, not stylistic: the action Repeater's delegate reaches the
// outer `snackbar` id (for token/kindColor), which is unqualified access from a
// nested component without this — a qmllint warning AND the only two bindings
// in this file the AOT compiler can't compile.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import SuopTranscriber

// One floating notification (see StyleGuide.md "Snackbars"). Rendered only by
// SnackbarHost, one per Notifications.queue row — not instantiated directly at
// a call site; post to the Notifications singleton instead.
Rectangle {
    id: snackbar

    // Named for tst_snackbar.cpp's findChildren — the only way that harness
    // can locate delegates the Repeater created.
    objectName: "snackbar"

    required property int token
    required property string text
    required property string kind          // "info" | "warning" | "error"
    required property int timeout          // 0 = sticky
    required property var actionLabels     // child ListModel from the queue row

    // Colors follow StyleGuide.md "Warning banners" so a snackbar and an
    // inline banner reporting the same class of thing read the same:
    // #e07a7a error, #e0a25a soft warning. Info borrows the accent.
    readonly property color kindColor: kind === "error" ? Theme.danger
                                     : kind === "warning" ? "#e0a25a"
                                     : AppSettings.accentColor

    // The width cap lives on the message Text's Layout.maximumWidth below, NOT
    // here: this Rectangle's parent is SnackbarHost's Column — a positioner,
    // not a Layout — so a Layout.maximumWidth attached to it would silently do
    // nothing. Bounding the one fillWidth child instead bounds this implicitly.
    implicitWidth: layout.implicitWidth + 24
    implicitHeight: layout.implicitHeight + 20

    radius: AppSettings.borderRadius
    color: Qt.rgba(AppSettings.mainBackgroundColor.r,
                   AppSettings.mainBackgroundColor.g,
                   AppSettings.mainBackgroundColor.b, 0.97)
    border.width: 1
    border.color: Qt.rgba(snackbar.kindColor.r, snackbar.kindColor.g,
                          snackbar.kindColor.b, 0.55)

    // Fade in only — never bind `y` here. This is a Column child, and a
    // positioner owns its children's y: an explicit `y` binding is clobbered
    // by the Column, and a `Behavior on y` would then animate every reflow
    // (e.g. an entry above this one expiring) rather than just this one's
    // arrival. The slide-in lives in SnackbarHost's Column `add` transition,
    // which is the positioner-aware place for it.
    opacity: 0
    Component.onCompleted: opacity = 1
    Behavior on opacity {
        NumberAnimation { duration: AppSettings.animationsEnabled ? 150 : 0; easing.type: Easing.OutCubic }
    }

    // Auto-dismiss. Paused while the pointer is anywhere over the stack, so an
    // entry can't expire while the user is reaching for its Undo.
    Timer {
        running: snackbar.timeout > 0 && !Notifications.paused
        interval: snackbar.timeout
        onTriggered: Notifications.dismiss(snackbar.token)
    }

    // A left-edge kind stripe rather than a full colored fill: an error
    // snackbar sits over the transcript, and a solid #e07a7a block that size
    // is loud enough to be worse than the thing it's reporting.
    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: 1
        width: 3
        radius: width / 2
        color: snackbar.kindColor
    }

    RowLayout {
        id: layout
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 10
        anchors.topMargin: 10
        anchors.bottomMargin: 10
        spacing: 10

        Text {
            Layout.fillWidth: true
            Layout.maximumWidth: 260
            text: snackbar.text
            color: Theme.textPrimary
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

        // Action buttons. Item + TapHandler, never a QQC2 Button / MouseArea:
        // a focusable control here would steal focus from whatever TextField
        // is open behind the snackbar, and this app commits fields on blur —
        // so pressing "Undo" could fire a transcript commit, whose own failure
        // posts a NEW snackbar, mutating the queue mid-click. TapHandler takes
        // no focus, so that cascade can't start. (Same reasoning as the
        // repo's other non-focusable icon clusters.)
        Repeater {
            model: snackbar.actionLabels
            delegate: Item {
                id: actionButton
                objectName: "snackbarAction"
                required property int index
                required property string label

                implicitWidth: actionText.implicitWidth + 16
                implicitHeight: 24
                Layout.alignment: Qt.AlignVCenter

                HoverHandler {
                    id: actionHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: Notifications.trigger(snackbar.token, actionButton.index)
                }
                HoverBorder {
                    anchors.fill: parent
                    active: actionHover.hovered
                }
                Text {
                    id: actionText
                    anchors.centerIn: parent
                    text: actionButton.label
                    color: snackbar.kindColor
                    font.pixelSize: 12
                    font.bold: true
                }
            }
        }

        // Dismiss. Same Item + HoverHandler + TapHandler + HoverBorder wrapper
        // every icon surface in this app uses (StyleGuide.md "Iconography").
        Item {
            id: closeButton
            objectName: "snackbarClose"
            implicitWidth: 20
            implicitHeight: 20
            Layout.alignment: Qt.AlignVCenter

            HoverHandler {
                id: closeHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: Notifications.dismiss(snackbar.token)
            }
            HoverBorder {
                anchors.fill: parent
                active: closeHover.hovered
            }
            AppIcon {
                anchors.centerIn: parent
                name: "close"
                iconSize: 13
                color: closeHover.hovered ? Theme.textPrimary : Theme.textSecondary
            }
        }
    }
}
