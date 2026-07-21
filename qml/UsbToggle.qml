import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// Labeled on/off pill used by the USB grabber's per-device toggles — same
// visual language as Sidebar.qml's search "Transcriptions" toggle
// (includeTranscriptionsToggle): a rounded rect that goes green when on, with
// an always-visible text label so each toggle says what it controls (the two
// bare QQC2 Switches it replaces were indistinguishable side by side).
Item {
    id: root
    // `enabled` is Item's own built-in property — a disabled toggle is
    // non-interactive (its handlers don't fire) and dims via the opacity
    // binding below. Not redeclared here on purpose.
    property string text: ""
    property bool checked: false
    property string tooltip: ""
    signal toggled(bool checked)

    implicitWidth: toggleLabel.implicitWidth + 16
    implicitHeight: 24

    HoverHandler {
        id: hover
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        enabled: root.enabled
        onTapped: root.toggled(!root.checked)
    }
    Rectangle {
        anchors.fill: parent
        radius: AppSettings.borderRadius
        opacity: root.enabled ? 1.0 : 0.4
        color: root.checked
               ? (hover.hovered ? "#2ecc71" : "#25a25a")
               : (hover.hovered ? "#3a3a3a" : "#2a2a2a")
        border.width: 1
        border.color: root.checked ? "#2ecc71" : "#4a4a4a"
        Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 } }
    }
    Label {
        id: toggleLabel
        anchors.centerIn: parent
        text: root.text
        color: root.checked ? "white" : "#aaaaaa"
        font.pixelSize: 11
    }
    ToolTip.visible: hover.hovered && root.tooltip !== ""
    ToolTip.delay: 500
    ToolTip.text: root.tooltip
}
