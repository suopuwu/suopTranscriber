import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// The filename-regex editor for one USB grab rule: pattern field + live ✓/✗
// validity indicator + insert-token toolbar. Deliberately mirrors the
// split-recording combine rules' pattern authoring UI in SettingsPopup.qml
// (same engine, same validity affordance, same insert-a-snippet toolbar), so a
// pattern is written the same way in both features — the two match with
// identical semantics (unanchored, against the file name only).
//
// A component rather than inline QML because the USB grabber has TWO rule cards
// that need it — the "Any USB device" catch-all and every per-device card — and
// they must not drift apart.
//
// What is deliberately NOT carried over from the combine UI: capture groups
// have no meaning here. There, a group's captured text IS the group identity
// that decides which files merge; here the pattern is a pure include-test, so
// the whole differentiator/sort-group/overlap-warning apparatus (and its ~500
// lines of tokenizer living in SettingsPopup) would be machinery with nothing
// to act on. Hence no "Group ()" token either — it would suggest a meaning the
// grab doesn't give it.
ColumnLayout {
    id: root

    // The rule's currently-persisted pattern. The field binds to this, so a
    // commit round-trips through the store and comes back (the rule cards are
    // Repeater delegates that get rebuilt on every save — see
    // usbGrabberPage.commitRuleField()).
    property string pattern: ""
    // Supplies regexValidationError() — the SAME QRegularExpression engine
    // runUsbGrabScan()'s walk filters with, never QML/JS RegExp (whose syntax isn't
    // guaranteed identical, so a JS-valid pattern could still match nothing at
    // grab time). Asserted by tst_styleguide.cpp.
    property UsbGrabberController controller

    // Emitted when the user finishes editing (Enter or focus-out), exactly as
    // the sibling extension/folder fields commit. Never emitted per keystroke:
    // a commit re-runs the on-screen preview, which walks a physical device.
    signal committed(string newPattern)

    spacing: 2

    // Inserts `snippet` at the cursor and places the caret `cursorOffset` chars
    // into it, then refocuses so typing continues immediately.
    //
    // TextInput.insert()/remove() rather than a `text = ...` reassignment: a
    // whole-property reassignment silently clears TextInput's undo history
    // (verified empirically in this codebase — see SettingsPopup.qml's
    // insertPatternToken(), which this mirrors), while insert()/remove() record
    // normal undoable edits, so Ctrl+Z keeps working after a token click. A
    // selection is removed first so a token OVERWRITES it rather than landing
    // beside it.
    function insertToken(snippet, cursorOffset) {
        if (patternField.selectedText.length > 0)
            patternField.remove(patternField.selectionStart, patternField.selectionEnd)
        const pos = patternField.cursorPosition
        patternField.insert(pos, snippet)
        patternField.cursorPosition = pos + cursorOffset
        patternField.forceActiveFocus()
    }

    TextField {
        id: patternField
        objectName: "patternField"
        Layout.fillWidth: true
        placeholderText: qsTr("Filename regex (blank = every file)")
        text: root.pattern
        font.family: "monospace"
        background: UnderlineField { focused: patternField.activeFocus }
        onEditingFinished: root.committed(text)
        // Editable text needs an I-beam.
        HoverHandler { cursorShape: Qt.IBeamCursor }
    }

    // Live validity indicator, reactive per keystroke (unlike the preview,
    // which is an async device walk and so only re-runs on commit). Safe to
    // bind per keystroke: regexValidationError() is a pure string->string call
    // that constructs one QRegularExpression and never throws.
    Label {
        id: validityLabel
        objectName: "validityLabel"
        Layout.fillWidth: true
        font.pixelSize: 11
        elide: Text.ElideRight
        visible: patternField.text.length > 0 && root.controller
        property string validationError: root.controller
                ? root.controller.regexValidationError(patternField.text) : ""
        // Same palette as the combine rules' identical indicator, deliberately:
        // the two fields do the same job and must read the same. (A ✓/⚠ glyph
        // used as a prefix inside a larger label stays a glyph, not an AppIcon
        // — StyleGuide.md "Iconography".)
        color: validityLabel.validationError.length > 0 ? "#e0664f" : "#5fbf7a"
        text: validityLabel.validationError.length > 0
                ? ("✗ " + validityLabel.validationError)
                : ("✓ " + qsTr("Valid"))
        HoverHandler { id: validityHover }
        ToolTip.visible: validityLabel.validationError.length > 0 && validityHover.hovered
        ToolTip.text: validityLabel.validationError
    }

    // An invalid pattern matches NOTHING (UsbGrabJob.cpp's filenameRegexMatches()
    // fails closed), so say so where the user is looking rather than letting a
    // preview full of "filtered" be the only clue. Inline banner, per
    // StyleGuide.md — it describes the state of the field beside it.
    Label {
        objectName: "failsClosedWarning"
        Layout.fillWidth: true
        visible: validityLabel.visible && validityLabel.validationError.length > 0
        text: qsTr("This rule will grab nothing until the pattern is valid.")
        color: "#e0a25a"
        font.pixelSize: 11
        wrapMode: Text.WordWrap
    }

    // Insert-token toolbar. focusPolicy: Qt.NoFocus on every button is
    // load-bearing, not decorative: a click's PRESS steals patternField's focus
    // -> fires onEditingFinished -> commits -> the rule store is re-read and
    // the whole deviceRows array reassigned -> the Repeater destroys and
    // recreates this very button before its RELEASE can fire onClicked. Without
    // NoFocus, tokens only insert on every other click.
    Flow {
        Layout.fillWidth: true
        spacing: 4

        Repeater {
            model: [
                {label: qsTr("Digits"), snippet: "\\d+", offset: 3,
                 tip: qsTr("\\d+ — one or more digits, e.g. the counter in REC_0042.WAV")},
                {label: qsTr("Any"), snippet: ".*?", offset: 3,
                 tip: qsTr(".*? — any characters (as few as possible)")},
                {label: qsTr("Either/Or"), snippet: "(?:|)", offset: 3,
                 tip: qsTr("(?:a|b) — matches a or b. | has the LOWEST precedence of any "
                           + "operator, so it must always be grouped like this.")},
                {label: qsTr("Starts with"), snippet: "^", offset: 1,
                 tip: qsTr("^ — anchor to the start of the file name. Without it the pattern "
                           + "may match anywhere in the name.")},
                {label: qsTr("Ends with"), snippet: "$", offset: 1,
                 tip: qsTr("$ — anchor to the end of the file name.")}
            ]

            delegate: Button {
                id: tokenButton
                required property var modelData
                required property int index
                objectName: "tokenButton_" + tokenButton.index
                text: tokenButton.modelData.label
                implicitHeight: 20
                font.pixelSize: 11
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                onClicked: root.insertToken(tokenButton.modelData.snippet, tokenButton.modelData.offset)
                ToolTip.visible: tokenButton.hovered
                ToolTip.text: tokenButton.modelData.tip
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                HoverBorder { anchors.fill: parent; active: tokenButton.hovered }
            }
        }
    }

    // Always-visible quick reference, same shape (and same wording where the
    // syntax overlaps) as the combine rules' inline hint.
    Label {
        Layout.fillWidth: true
        font.pixelSize: 11
        color: Theme.textSecondary
        wrapMode: Text.WordWrap
        text: qsTr("Matched against the file name only (not the folder path), and matches "
                   + "anywhere in the name unless anchored with ^ or $. \\d digit · \\w letter/digit "
                   + "· . any character · * zero or more · + one or more · ? optional · {n,m} repeat "
                   + "count · [...] any one of these. Combined with the extension filter above: a "
                   + "file is grabbed only if it passes both.")
    }
}
