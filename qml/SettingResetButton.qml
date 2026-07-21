import QtQuick
import QtQuick.Controls
import SuopTranscriber

// The per-setting "revert this one knob to its default" button — the small
// bordered square that sits at the right end of every row in SettingsPopup.
//
//     SettingResetButton { propertyName: "borderRadius" }
//
// This block existed 42 times, verbatim, in SettingsPopup.qml: the single
// most-duplicated piece of markup in the repo. Every copy differed by exactly
// one thing — the AppSettings property name, which appeared THREE times per
// copy (the isDefault() gate, the resetProperty() call, and nothing else) —
// so a typo in one of the three would silently produce a button that either
// never enables or resets the wrong knob. One property here makes that
// impossible to get half-right.
//
// NOTE: deliberately NOT built on AppIconButton, even though that component
// also bakes in HoverHandler + TapHandler + HoverBorder + AppIcon. Two things
// don't fit: AppIconButton is a bare Item with no background at all, while
// this button's whole visual identity is the 1px Theme.border box around it;
// and the disabled state here is a 0.35 opacity fade of the WHOLE control
// (border included), not AppIconButton's Theme.textDisabled glyph recolor.
// Reproducing both through AppIconButton would mean adding a background and
// an opacity override to it for this one caller's benefit — so this stays a
// Controls.Button, matching the labelled-button idiom the rest of the popup
// uses. (AppIconButton.qml's own header documents that split.)
Button {
    id: root

    // The AppSettings property this row edits — the key into AppSettings'
    // defaults map, so it must match the property name exactly.
    property string propertyName: ""

    contentItem: AppIcon { name: "reset"; iconSize: 14; color: Theme.textPrimary; anchors.centerIn: parent }
    implicitWidth: 22
    implicitHeight: 22

    background: Rectangle {
        radius: AppSettings.borderRadius
        color: "transparent"
        border.width: 1
        border.color: Theme.border
    }

    // Nothing to revert while the setting still holds its default, so the
    // button greys out rather than disappearing — the row keeps its width and
    // the layout doesn't shift as a user edits a value.
    enabled: !AppSettings.isDefault(root.propertyName)
    opacity: enabled ? 1 : 0.35
    hoverEnabled: enabled
    onClicked: AppSettings.resetProperty(root.propertyName)

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
        enabled: root.enabled
    }
    HoverBorder {
        anchors.fill: parent
        active: root.enabled && root.hovered
    }
}
