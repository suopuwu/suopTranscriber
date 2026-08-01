import QtQuick
import QtQuick.Controls

// A text-only click target: hovering underlines the text instead of drawing
// the 1px HoverBorder ring (StyleGuide "Interactable hover behavior" →
// "Text-only click targets"). A box around a run of inline text reads as a
// button it isn't, and around a variable-width readout it jitters as the text
// resizes; the underline is the affordance text already carries everywhere.
//
// `interactive` gates BOTH the underline and the tap, so a label that isn't
// currently clickable can never advertise that it is.
Label {
    id: root

    property bool interactive: true
    readonly property bool hovered: labelHover.hovered
    signal tapped()

    font.underline: root.interactive && labelHover.hovered

    HoverHandler {
        id: labelHover
        cursorShape: root.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor
    }
    TapHandler {
        enabled: root.interactive
        // Small text targets sit beside other controls and are easy to nudge
        // between press and release — see BottomBar's pauseButton TapHandler
        // for the full rationale against the DragThreshold default.
        gesturePolicy: TapHandler.ReleaseWithinBounds
        onTapped: root.tapped()
    }
}
