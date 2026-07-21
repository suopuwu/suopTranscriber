import QtQuick
import QtQuick.Controls
import SuopTranscriber

// The single way this app draws a clickable ICON button — the sibling of
// AppIcon.qml, which draws the glyph but has no interaction.
//
//     AppIconButton {
//         iconName: "transcribe"
//         tooltip: qsTr("Transcribe selected")
//         enabled: somethingIsSelected
//         onTapped: controller.transcribeAll(...)
//     }
//
// This existed ~30 times as copy-pasted boilerplate before being pulled out:
// Sidebar's ribbon (~19 instances) and MainPanel's per-file action cluster
// (11 instances) each repeated the same five-element skeleton — HoverHandler
// for the cursor, TapHandler for the click, HoverBorder for the hover ring,
// AppIcon for the glyph, and a ToolTip triad — differing only in icon name,
// tooltip text, `enabled` expression, and what onTapped does. Those four are
// exactly the properties below.
//
// SIZING: `implicitWidth`/`implicitHeight` (not width/height) is load-bearing.
// The ribbon parents these into plain Items that set width/height directly,
// while MainPanel's cluster puts them in a RowLayout that reads implicit sizes
// to fill Layout.preferredWidth/Height. Implicit sizing satisfies both without
// the call site choosing; a hard width/height here would break the layout case.
//
// NOTE: deliberately NOT a QQC2 Button. The app has a second, incompatible
// button idiom (Controls.Button with a background Rectangle) used for LABELLED
// buttons; this one is for bare icons, where a Button's padding/background
// would have to be undone anyway. Keeping them separate is intentional.
Item {
    id: root

    // A semantic icon name — see IconTheme::iconNames(), same vocabulary AppIcon takes.
    property string iconName: ""
    // Empty tooltip means no tooltip at all (the ToolTip stays invisible).
    property string tooltip: ""
    // The icon's box, in logical px. 15 is the ribbon/cluster canonical size.
    property real iconSize: 15
    // Overridable so a call site can render, say, a destructive action in
    // Theme.danger. The default is the recurring enabled/disabled idiom these
    // buttons all shared.
    property color iconColor: root.enabled ? Theme.textPrimary : Theme.textDisabled

    // True while the pointer is over the button — exposed because some call
    // sites drive sibling visuals (a label's color, a peer highlight) off the
    // button's own hover state.
    readonly property alias hovered: hover.hovered

    // Names the INNER AppIcon so a test can find the glyph itself rather than
    // the button. Needed because some tests assert on the icon's painted
    // `color` (e.g. tst_mainpanel_deleteundo checks the delete glyph flips to
    // Theme.danger and back as the queue state changes) — the button's own
    // objectName can't answer that. Empty (the default) leaves the icon
    // unnamed, which is what nearly every call site wants.
    property string iconObjectName: ""

    signal tapped()

    implicitWidth: 22
    implicitHeight: 22

    HoverHandler {
        id: hover
        cursorShape: Qt.PointingHandCursor
        enabled: root.enabled
    }
    TapHandler {
        enabled: root.enabled
        onTapped: root.tapped()
    }
    HoverBorder {
        anchors.fill: parent
        active: root.enabled && hover.hovered
    }
    AppIcon {
        anchors.centerIn: parent
        objectName: root.iconObjectName
        name: root.iconName
        iconSize: root.iconSize
        color: root.iconColor
    }

    ToolTip.visible: hover.hovered && root.tooltip !== ""
    ToolTip.text: root.tooltip
    ToolTip.delay: 500
}
