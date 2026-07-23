import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// The find bar: a search toggle that expands to a query field, the regex/case
// mode toggles, a match counter, the Replace-arm checkbox, and a close button.
// It sits inline in the sticky title RowLayout, so its width competes with
// titleContainer — the call site gates Layout.fillWidth on searchExpanded so a
// collapsed bar claims no row space. Extracted from MainPanel; `panel` is
// threaded in. forceSearchFocus() lets the panel focus the field when a
// shortcut or a snippet click opens the bar.
RowLayout {
    id: searchReplaceBar
    property var panel: null
    spacing: 8

    function forceSearchFocus() { searchField.forceActiveFocus() }

    AppIconButton {
        id: searchToggleButton
        visible: !panel.searchExpanded

        iconName: "search"

        onTapped: {
            panel.searchExpanded = true
            searchField.forceActiveFocus()
        }
    }

    TextField {
        id: searchField
        visible: panel.searchExpanded
        // Shrinkable rather than a fixed 200 so it gives width back to
        // titleContainer under pressure on a narrow panel (item 048).
        Layout.fillWidth: true
        Layout.minimumWidth: 90
        Layout.maximumWidth: 200
        // The placeholder tracks the mode — the same field is a literal string
        // or a regex depending on the .* toggle.
        placeholderText: panel.findRegexEnabled
                         ? qsTr("Pattern in transcript, title, tags…")
                         : qsTr("Search transcript, title, tags…")
        text: panel.searchQuery
        onTextChanged: panel.searchQuery = text
        onAccepted: panel.jumpToNextMatch()
        // An invalid regex reddens the text (the banner below spells out why).
        color: panel.findReplaceError !== "" ? Theme.danger : Theme.textPrimary

        background: UnderlineField { focused: searchField.activeFocus }

        HoverHandler {
            cursorShape: Qt.IBeamCursor
        }
    }

    // Regex mode toggle (.*) — active state uses the app accent so it reads as
    // a mode, not a one-shot button. Governs live search as well as replace.
    Item {
        id: regexToggle
        visible: panel.searchExpanded
        Layout.preferredWidth: 26
        Layout.preferredHeight: 22
        ToolTip.visible: regexToggleHover.hovered
        ToolTip.text: qsTr("Regular expression")
        ToolTip.delay: 500
        HoverHandler { id: regexToggleHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: panel.findRegexEnabled = !panel.findRegexEnabled }
        HoverBorder { anchors.fill: parent; active: regexToggleHover.hovered }
        Text {
            anchors.centerIn: parent
            text: ".*"
            color: panel.findRegexEnabled ? AppSettings.accentColor : Theme.textSecondary
            font.pixelSize: 13
            font.bold: panel.findRegexEnabled
        }
    }

    // Case-sensitivity toggle (Aa) — on means case-INsensitive.
    Item {
        id: caseToggle
        visible: panel.searchExpanded
        Layout.preferredWidth: 26
        Layout.preferredHeight: 22
        ToolTip.visible: caseToggleHover.hovered
        ToolTip.text: qsTr("Ignore case")
        ToolTip.delay: 500
        HoverHandler { id: caseToggleHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: panel.findCaseInsensitive = !panel.findCaseInsensitive }
        HoverBorder { anchors.fill: parent; active: caseToggleHover.hovered }
        Text {
            anchors.centerIn: parent
            text: "Aa"
            color: panel.findCaseInsensitive ? AppSettings.accentColor : Theme.textSecondary
            font.pixelSize: 12
            font.bold: panel.findCaseInsensitive
        }
    }

    // "a/b" match position, updating as Enter cycles through matches.
    Label {
        visible: panel.searchExpanded && panel.searchQuery !== ""
        text: panel.searchMatchCount > 0
              ? qsTr("%1/%2").arg(panel.searchMatchOrdinal).arg(panel.searchMatchCount)
              : qsTr("0/0")
        color: Theme.textSecondary
        font.pixelSize: 11
    }

    // The checkbox that arms replace. A minimal pill toggle rather than a QQC2
    // CheckBox, which would steal focus from searchField on press.
    Item {
        id: replaceToggle
        visible: panel.searchExpanded
        Layout.preferredWidth: replaceToggleLabel.implicitWidth + 16
        Layout.preferredHeight: 22

        HoverHandler { id: replaceToggleHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: panel.replaceEnabled = !panel.replaceEnabled }
        Rectangle {
            anchors.fill: parent
            radius: AppSettings.borderRadius
            color: panel.replaceEnabled
                   ? (replaceToggleHover.hovered ? Theme.success : Theme.successDim)
                   : (replaceToggleHover.hovered ? Theme.surfaceHover : Theme.surface)
            border.width: 1
            border.color: panel.replaceEnabled ? Theme.success : Theme.borderStrong

            Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 } }
        }
        Label {
            id: replaceToggleLabel
            anchors.centerIn: parent
            text: qsTr("Replace")
            color: panel.replaceEnabled ? "white" : Theme.textMuted
            font.pixelSize: 11
        }
    }

    AppIconButton {
        id: searchCloseButton
        visible: panel.searchExpanded
        // A 20x20 box, smaller than the action-cluster icons.
        Layout.preferredWidth: 20
        Layout.preferredHeight: 20

        iconName: "close"

        onTapped: {
            searchField.text = ""
            panel.searchQuery = ""
            panel.searchExpanded = false
            // Closing disarms replace and drops the banner, so the bar can't
            // reopen mid-edit already armed with a stale pattern.
            panel.replaceEnabled = false
            panel.findReplaceError = ""
        }
    }
}
