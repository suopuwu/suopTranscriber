import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// The prelude editor: an always-editable multi-line TextArea that autosaves on
// blur. Plain Enter inserts a newline (the prelude is multi-line), so there is
// no commit-on-Enter and no Save/Cancel chrome. Extracted from MainPanel; the
// owning panel is threaded in as `panel` at the instantiation site.
Item {
    id: preludeEditor
    property var panel: null

    // The panel resyncs/clears/deselects the field from its own imperative
    // handlers (file switch, background deselect). Exposed here so those
    // callers reach the field through the component's API, not across the
    // boundary by id. The text binding is one-shot (the user can type over it),
    // so callers resync it only when the field is not focused.
    property alias fieldText: preludeArea.text
    property alias fieldFocused: preludeArea.activeFocus
    function deselect() { preludeArea.deselect() }

    implicitHeight: preludeArea.implicitHeight

    TextArea {
        id: preludeArea
        anchors.left: parent.left
        anchors.right: parent.right
        wrapMode: TextArea.Wrap
        color: Theme.textPrimary
        font.pixelSize: 12
        selectByMouse: true
        selectByKeyboard: true
        text: panel.prelude
        placeholderText: qsTr("Click to add a prelude…")
        padding: 0
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0
        background: UnderlineField { implicitHeight: 0; plainUntilHovered: true; focused: preludeArea.activeFocus }
        Keys.onEscapePressed: preludeArea.text = panel.prelude
        onActiveFocusChanged: if (!activeFocus) panel.commitPrelude(text)

        // QQC2 TextArea ships no I-beam of its own; restore it.
        HoverHandler { cursorShape: Qt.IBeamCursor }
    }
}
