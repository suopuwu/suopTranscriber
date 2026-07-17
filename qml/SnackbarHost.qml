pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import SuopTranscriber

// The one place floating notifications appear (StyleGuide.md "Snackbars").
// Instantiated ONCE, in Main.qml; renders the Notifications singleton's queue
// in the bottom-right corner. Nothing else should parent a Snackbar.
//
// Reparented onto Overlay.overlay with a high z, the same treatment
// Main.qml's ResizeGrips and the old import-error banner use — that's what
// keeps it above both panels AND above SettingsPopup's modal block-plane, so
// a notification posted while Settings is open is still visible and
// dismissable rather than trapped behind the modal scrim.
Item {
    id: host

    parent: Overlay.overlay
    anchors.fill: parent
    z: 1001

    // An empty queue takes this whole surface out of the scene rather than
    // leaving a full-overlay Item sitting over the app forever. It provably
    // doesn't paint or eat clicks when empty (an Item with no background and no
    // handlers of its own), so this isn't load-bearing for input — but a
    // permanent z:1001 layer across the entire window, above every panel and
    // every popup, is a big thing to leave switched on for the ~99% of the time
    // there's nothing to show. Cheaper to not exist.
    visible: Notifications.queue.count > 0

    // Newest at the BOTTOM, nearest the corner the eye is already drawn to;
    // older entries push upward. Bottom-right rather than bottom-center so a
    // notification never covers BottomBar's task rows / transport controls.
    Column {
        id: stack
        objectName: "snackbarStack"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 16
        anchors.bottomMargin: 16
        spacing: 8

        // A positioner's own entry/exit animation hooks — the correct place for
        // the slide that must NOT live on the delegate's y (see Snackbar.qml).
        add: Transition {
            NumberAnimation {
                property: "y"; from: 8
                duration: AppSettings.animationsEnabled ? 150 : 0
                easing.type: Easing.OutCubic
            }
        }
        move: Transition {
            NumberAnimation {
                properties: "y"
                duration: AppSettings.animationsEnabled ? 150 : 0
                easing.type: Easing.OutCubic
            }
        }

        // Pausing every entry's expiry timer while the pointer is over the
        // stack: reaching for "Undo" on a 6s snackbar must not race the timer
        // that's about to delete it. Covers the gaps between snackbars too
        // (the Column, not each Snackbar), so crossing from one to another
        // doesn't briefly un-pause.
        HoverHandler {
            id: stackHover
        }
        Binding {
            target: Notifications
            property: "paused"
            value: stackHover.hovered
        }

        // No `required property var model` + model.token/model.text/... here:
        // that hands the AOT compiler an untyped QVariant, so every role read
        // falls back to the interpreter. Snackbar declares its roles as
        // required properties whose NAMES match the queue's roles, so Qt
        // injects each one directly and typed.
        Repeater {
            model: Notifications.queue
            delegate: Snackbar {}
        }
    }
}
