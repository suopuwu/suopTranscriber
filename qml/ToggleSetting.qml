import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// One boolean row of SettingsPopup: name on the left, Switch on the right,
// SettingResetButton after it.
//
//     ToggleSetting {
//         id: sidebarBlurToggle
//         label: qsTr("Blur")
//         propertyName: "sidebarBlur"
//     }
//
// Collapses 14 copies of the same ~48-line RowLayout, each of which named its
// AppSettings property FOUR times (the Switch's `checked` binding, the
// `onToggled` writeback, and the reset button's isDefault/resetProperty pair).
// The row IS the RowLayout, so a call site still sets `visible:` on it
// directly — several of these are gated on a master switch above them.
//
// The two-way binding is the same shape every copy used, and it matters that
// it stays that shape: `checked: AppSettings[...]` is a one-way binding IN,
// and `onToggled` writes OUT only on genuine user interaction. Writing back
// from `onCheckedChanged` instead would fire on the inbound binding too and
// turn a reset into a write loop.
RowLayout {
    id: root

    // Row caption. Pass it through qsTr() at the call site so lupdate sees the
    // literal in context, not a bare property assignment here.
    property string label: ""
    // The AppSettings property this row edits — see SettingResetButton.
    property string propertyName: ""
    // Long captions ("Delete originals after combining") need to wrap rather
    // than run under the Switch. Off by default because the popup's short
    // captions all fit one line, and turning wrapping on for them would let a
    // narrow popup break them mid-phrase where they used not to.
    property bool labelWrap: false

    // Several sliders and hint Labels below a master toggle bind their
    // `visible` to it, so the switch's state has to stay readable from
    // outside the row.
    readonly property alias checked: control.checked

    Layout.fillWidth: true

    Label {
        Layout.fillWidth: true
        wrapMode: root.labelWrap ? Text.WordWrap : Text.NoWrap
        color: "white"
        text: root.label
    }
    Switch {
        id: control
        checked: AppSettings[root.propertyName]
        hoverEnabled: true
        onToggled: AppSettings[root.propertyName] = checked

        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
        HoverBorder {
            anchors.fill: parent
            active: control.hovered
        }
    }
    SettingResetButton {
        propertyName: root.propertyName
    }
}
