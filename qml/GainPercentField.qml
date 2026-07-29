import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// The gain readout beside a GainSlider — and the way to set that gain exactly.
//
// WHY EDITABLE: the sliders reach 400%, so unity sits a quarter of the way
// along and every ordinary correction is a few pixels of drag. Typing "150" is
// the only way to land on a number you can repeat across sources or restore
// after a mistake — and the only way past the slider's ceiling at all, for a
// source so quiet that 4x isn't enough (`maxGain` here is the higher, typed
// ceiling; the slider keeps the comfortable drag range).
//
// Always-editable per StyleGuide.md: one TextField present at all times, plain
// until hovered — never a Label that swaps into an editor on click.
//
// The row geometry lives here, not at the three call sites: fixed width (an
// implicitly-sized readout grows from "0%" to "400%" as the user drags and
// shortens the slider under their own cursor) and a negative left margin that
// trims the row's 12px spacing, since the slider and its number are one
// reading and the peak column beside them is not.
TextField {
    id: field

    // Gain as a multiplier, 1.0 == 100%. Read from the persisted setting, not
    // from the slider beside it — a Slider clamps its value to `to`, so a gain
    // above the slider's ceiling would come back through it as the ceiling.
    property real gain: 1.0
    // The TYPED ceiling, higher than the slider's on purpose (see
    // RecorderPage.maxTypedGain) — typing is how you get past the drag range.
    property real maxGain: 20.0
    // Emitted only for a real edit, the way Slider.moved() is: pushing `gain`
    // in from the setting must never echo straight back out as a user edit.
    signal gainEdited(real gain)

    Layout.preferredWidth: 46
    Layout.leftMargin: -8

    horizontalAlignment: TextInput.AlignRight
    font.pixelSize: 11
    color: field.enabled ? Theme.textPrimary : Theme.textMuted
    hoverEnabled: true
    // QQC2's own padding would inset the number from the column the peak
    // readout beside it aligns to; the underline is the whole background.
    padding: 0
    rightPadding: 2
    inputMethodHints: Qt.ImhDigitsOnly
    // Accepts "250" and "250%" alike: the suffix is part of the displayed
    // text, so editing in place must not require deleting it first.
    validator: RegularExpressionValidator { regularExpression: /[0-9]{0,4}%?/ }

    background: UnderlineField {
        focused: field.activeFocus
        plainUntilHovered: true
    }

    function formatGain() {
        return Math.round(field.gain * 100) + "%"
    }

    onEditingFinished: {
        const percent = parseInt(field.text, 10)
        if (!isNaN(percent))
            field.gainEdited(Math.max(0, Math.min(field.maxGain, percent / 100)))
        // Re-render from `gain`, not from what was typed: an out-of-range
        // entry was just clamped, and the field has to show where the value
        // actually landed.
        field.text = field.formatGain()
    }
    // The text carries a "%" suffix, so a click-then-type would otherwise
    // append to it. Selecting on focus makes typing a replacement one action.
    onActiveFocusChanged: {
        if (field.activeFocus)
            field.selectAll()
    }

    // A Binding rather than a plain `text:` binding because a plain one would
    // be destroyed the moment onEditingFinished assigns text. The `when` guard
    // is the other half: while the field has focus the user is mid-type, and
    // letting the gain push a reformatted value back into `text` would rewrite
    // the characters under the cursor (same idiom as SliderSetting's field).
    Binding {
        target: field
        property: "text"
        value: field.formatGain()
        when: !field.activeFocus
    }

    HoverHandler {
        cursorShape: Qt.IBeamCursor
        enabled: field.enabled
    }
}
