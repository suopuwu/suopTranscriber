import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// One numeric row of SettingsPopup: name, Slider, an optional companion
// TextField showing/accepting the exact number, and a SettingResetButton.
//
//     SliderSetting {
//         id: radiusSlider
//         label: qsTr("Border radius")
//         propertyName: "borderRadius"
//         to: 24
//     }
//
// Collapses 15 copies of the same RowLayout (~85 lines each with the field,
// ~57 without). Every copy named its AppSettings property SEVEN times — the
// slider's value binding, its onMoved writeback, the field's clamp-and-write,
// the field's redisplay after editing, the Binding's value, and the reset
// button's isDefault/resetProperty pair — which is six chances to paste a row
// and miss one.
//
// The row IS the RowLayout, so a call site sets `visible:` on it directly
// (several of these are gated on a master switch) and can still read `.value`
// and `.pressed` off it — SettingsPopup's `anyPressed` fades the whole dialog
// out while any of these sliders is being dragged, so live-previewing a
// color/opacity change isn't hidden behind the popup, and several rows print
// a live phrasing hint under themselves from `.value`.
RowLayout {
    id: root

    // Row caption — pass through qsTr() at the call site (see ToggleSetting).
    property string label: ""
    // The AppSettings property this row edits — see SettingResetButton.
    property string propertyName: ""

    property real from: 0
    property real to: 1
    // 0 (the default) means a continuous slider. Any other value snaps, which
    // is the only configuration the snapping rows ever used — so snapMode is
    // derived rather than being a second knob that can disagree with this one.
    property real stepSize: 0
    // Digits the companion field shows and accepts. 0 means the setting is an
    // INTEGER: the field validates as an int and the slider rounds on write,
    // so a fractional value can never reach an int-typed AppSettings property.
    property int decimals: 1
    // A few rows (the 0-1 "tier" sliders, chapter/diarization tuning) are
    // qualitative — a raw number next to them would be noise, and they print
    // a phrasing hint under the row instead. They keep the slider and the
    // reset button but drop the field. An invisible item is skipped entirely
    // by QtQuick.Layouts, so this is layout-identical to omitting it.
    property bool showField: true

    readonly property alias value: control.value
    readonly property alias pressed: control.pressed

    Layout.fillWidth: true

    function formatValue(v) {
        return Number(v).toFixed(root.decimals)
    }

    // Shared name column for every slider/combo row in this popup. Long names
    // ("Left content margin", "Tag reuse bias") don't fit 90px on one line, so
    // they WRAP — an elide here would let the slider cut the name off. The
    // rows have no fixed height, so they grow to fit.
    Label {
        Layout.preferredWidth: 90
        Layout.alignment: Qt.AlignVCenter
        wrapMode: Text.WordWrap
        color: "white"
        text: root.label
    }

    Slider {
        id: control
        Layout.fillWidth: true
        Layout.minimumWidth: 80
        from: root.from
        to: root.to
        stepSize: root.stepSize
        snapMode: root.stepSize > 0 ? Slider.SnapAlways : Slider.NoSnap
        value: AppSettings[root.propertyName]
        hoverEnabled: true
        // One-way binding IN above, explicit write OUT here. onMoved (not
        // onValueChanged) is what keeps a reset or an external change from
        // being echoed straight back as a user edit.
        onMoved: AppSettings[root.propertyName] = root.decimals === 0 ? Math.round(value) : value

        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
        HoverBorder {
            anchors.fill: parent
            active: control.hovered
        }
    }

    TextField {
        id: field
        visible: root.showField
        Layout.preferredWidth: 56
        horizontalAlignment: TextInput.AlignRight
        hoverEnabled: true
        validator: root.decimals === 0 ? intValidator : doubleValidator
        onEditingFinished: {
            const v = root.decimals === 0 ? parseInt(text, 10) : parseFloat(text)
            if (!isNaN(v))
                AppSettings[root.propertyName] = Math.max(root.from, Math.min(root.to, v))
            // Re-render from the setting, not from what was typed: an
            // out-of-range entry was just clamped, and the field has to show
            // where the value actually landed.
            text = root.formatValue(AppSettings[root.propertyName])
        }

        background: UnderlineField { focused: field.activeFocus }

        // A Binding rather than a plain `text:` binding because a plain one
        // would be destroyed the moment onEditingFinished assigns text. The
        // `when` guard is the other half: while the field has focus the user
        // is mid-type, and letting the setting push a reformatted value back
        // into `text` would rewrite the characters under the cursor.
        Binding {
            target: field
            property: "text"
            value: root.formatValue(AppSettings[root.propertyName])
            when: !field.activeFocus
        }

        IntValidator {
            id: intValidator
            bottom: root.from
            top: root.to
        }
        DoubleValidator {
            id: doubleValidator
            bottom: root.from
            top: root.to
            decimals: root.decimals
        }

        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
        HoverBorder {
            anchors.fill: parent
            active: field.hovered
        }
    }

    SettingResetButton {
        propertyName: root.propertyName
    }
}
