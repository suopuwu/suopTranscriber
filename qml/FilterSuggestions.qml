import QtQuick
import QtQuick.Controls
import SuopTranscriber

// The shared autocomplete dropdown behind BOTH sidebar search filters (tags
// and speakers). Extracted rather than duplicated: the two filters must stay
// behaviorally identical, and before this existed only the tag field had a
// popup at all.
//
// Declare it as a child of the TextField it serves — `parent` is then that
// field, so the geometry below anchors to the field with no manual coordinate
// mapping (the bug this replaces measured y from the whole ribbon and landed
// several rows too low).
//
// Deliberately `modal: false`, against the O21 convention documented in
// BottomBar.qml. O21's argument (modal gives a real dim + a real input block)
// is correct for menus and dialogs and wrong here: an autocomplete must leave
// its own field visibly lit and still typeable. Non-modal costs the automatic
// block-plane, which this popup doesn't want either.
Popup {
    id: suggestionsPopup

    // The TextField this attaches to. Used for the visibility gate only —
    // geometry rides on `parent`, which is the same item.
    required property Item field
    // Candidate strings, already filtered by the caller.
    property var suggestions: []
    // Extra caller-side gate ANDed into visibility (the sidebar passes
    // searchExpanded).
    property bool enabledWhen: true

    // Emitted when the user takes a suggestion, by click or by Enter.
    signal accepted(string value)

    // Escape / press-outside latch. `visible` is a binding, so Popup.close()
    // alone can't hold it shut — the binding would re-assert on the next
    // dependency change. This latch is what makes a dismiss stick, and the
    // caller clears it on the next keystroke so typing brings the list back.
    property bool dismissed: false

    y: parent.height + 2
    x: 0
    width: parent.width
    padding: 4
    modal: false
    focus: false
    // Escape/press-outside route through onClosed into the latch above.
    closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape

    visible: suggestionsPopup.enabledWhen
             && suggestionsPopup.field.activeFocus
             && suggestionsPopup.suggestions.length > 0
             && !suggestionsPopup.dismissed

    onClosed: suggestionsPopup.dismissed = true

    // Every fresh appearance starts with the first row highlighted, so a
    // single Enter takes the best match.
    onVisibleChanged: if (visible) suggestionsList.currentIndex = 0

    // --- keyboard API, called from the field's Keys handlers ---------------
    // The field keeps focus (focus: false above) so typing never breaks;
    // these are the only way keys reach the list.
    function moveDown() {
        suggestionsList.currentIndex =
            (suggestionsList.currentIndex + 1) % suggestionsList.count
    }
    function moveUp() {
        suggestionsList.currentIndex =
            (suggestionsList.currentIndex - 1 + suggestionsList.count) % suggestionsList.count
    }
    function acceptCurrent() {
        if (suggestionsList.currentIndex < 0
            || suggestionsList.currentIndex >= suggestionsPopup.suggestions.length)
            return
        suggestionsPopup.accepted(suggestionsPopup.suggestions[suggestionsList.currentIndex])
    }
    function dismiss() { suggestionsPopup.dismissed = true }

    background: Rectangle {
        color: Theme.surfaceDialog
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: Theme.border
    }

    contentItem: ListView {
        id: suggestionsList
        objectName: "suggestionsList"
        implicitHeight: Math.min(contentHeight, 140)
        clip: true
        model: suggestionsPopup.suggestions
        currentIndex: 0

        delegate: Item {
            id: suggestionDelegate
            required property string modelData
            required property int index
            width: suggestionsList.width
            height: 22

            HoverHandler {
                id: suggestionHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: suggestionsPopup.accepted(suggestionDelegate.modelData)
            }
            Rectangle {
                anchors.fill: parent
                // Keyboard selection and hover share one highlight — two
                // different-looking "current" rows would be confusing.
                color: (suggestionsList.currentIndex === suggestionDelegate.index
                        || suggestionHover.hovered) ? Theme.surfaceHover : "transparent"
                radius: AppSettings.borderRadius
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 6
                text: suggestionDelegate.modelData
                color: Theme.textPrimary
                font.pixelSize: 11
            }
        }
    }
}
