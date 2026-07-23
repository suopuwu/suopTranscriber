import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// The replace row — sticky, right under the title/search row. The find half of
// the bar is the search row above (its field IS the pattern, .*/Aa are its
// modes), so all that lives here is the replacement string, the two actions,
// and the shared error banner. The row appears only once the Replace checkbox
// arms it; the banner does not, since an invalid regex is worth reporting while
// you are merely searching. Replace routes through commitTranscriptEdits() via
// replaceNext()/replaceAll(), inheriting that path's per-sentence line-count
// safety check. Extracted from MainPanel; `panel` is threaded in.
ColumnLayout {
    id: replaceRow
    property var panel: null
    Layout.fillWidth: true
    visible: panel.searchExpanded
    spacing: 4

    RowLayout {
        Layout.fillWidth: true
        visible: panel.replaceEnabled
        spacing: 8

        Label {
            text: qsTr("Replace with")
            color: Theme.textSecondary
            font.pixelSize: 11
        }

        TextField {
            id: replaceField
            Layout.preferredWidth: 180
            placeholderText: qsTr("replacement…")
            text: panel.replaceText
            onTextChanged: panel.replaceText = text
            color: Theme.textPrimary
            background: UnderlineField { focused: replaceField.activeFocus }
            HoverHandler { cursorShape: Qt.IBeamCursor }
        }

        // Replace / Replace-all: Item+TapHandler buttons (NOT focusable QQC2
        // Buttons) so they never steal focus from the find/replace fields on
        // press — a focusable button next to a focused TextField grabs focus.
        Item {
            id: replaceNextButton
            Layout.preferredHeight: 22
            Layout.preferredWidth: replaceNextText.implicitWidth + 16
            HoverHandler { id: replaceNextHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: panel.replaceNext() }
            Rectangle {
                anchors.fill: parent
                radius: AppSettings.borderRadius
                color: "transparent"
                border.width: 1
                border.color: Theme.border
            }
            HoverBorder { anchors.fill: parent; active: replaceNextHover.hovered }
            Text {
                id: replaceNextText
                anchors.centerIn: parent
                text: qsTr("Replace")
                color: Theme.textPrimary
                font.pixelSize: 11
            }
        }

        Item {
            id: replaceAllButton
            Layout.preferredHeight: 22
            Layout.preferredWidth: replaceAllText.implicitWidth + 16
            HoverHandler { id: replaceAllHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: panel.replaceAll() }
            Rectangle {
                anchors.fill: parent
                radius: AppSettings.borderRadius
                color: "transparent"
                border.width: 1
                border.color: Theme.border
            }
            HoverBorder { anchors.fill: parent; active: replaceAllHover.hovered }
            Text {
                id: replaceAllText
                anchors.centerIn: parent
                text: qsTr("Replace all")
                color: Theme.textPrimary
                font.pixelSize: 11
            }
        }

        Item { Layout.fillWidth: true }

        // Disarms replace, folding this row away and leaving the find bar (and
        // your pattern) intact — the "×" on the search row closes the whole thing.
        AppIconButton {
            id: replaceCollapseButton
            // 20x20 to match searchCloseButton's "×", not the implicit 22.
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
            iconName: "close"
            tooltip: qsTr("Hide replace")
            onTapped: panel.replaceEnabled = false
        }
    }

    // Invalid-regex inline error (StyleGuide.md warning-banner convention:
    // backing string, #e07a7a, 11px, wrapping — no separate visible flag
    // beyond the string being non-empty).
    Label {
        Layout.fillWidth: true
        visible: panel.findReplaceError !== ""
        text: panel.findReplaceError
        color: Theme.danger
        font.pixelSize: 11
        wrapMode: Text.WordWrap
    }
}
