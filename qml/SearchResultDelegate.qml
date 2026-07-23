import QtQuick
import SuopTranscriber
import "TextFormat.js" as TextFormat

// One flat search-result row: searchResultsView's delegate, shown instead of
// the tree while a text query is active. Extracted from Sidebar as a pure
// relocation; `sidebarRoot` (the Sidebar root) and `listView` (its
// searchResultsView) are threaded in at the delegate instantiation site, so
// every sidebarRoot.* / listView.* reference below resolves unchanged. The LHS
// names deliberately differ from the outer ids (self-bind trap — see
// TreeRowDelegate.qml).
Item {
    id: resultDelegate
    property var sidebarRoot: null
    property var listView: null
    required property var modelData
    // Same unified right-side gap as tree.columnWidthProvider — but a plain
    // ListView delegate keeps this width binding live-reactive on its own.
    width: listView.width - AppSettings.sidebarRightMargin

    readonly property string resultPath: resultDelegate.modelData.path
    readonly property string resultName: resultDelegate.modelData.name
    readonly property string resultLabel: resultDelegate.modelData.label
    readonly property int resultStatus: resultDelegate.modelData.status
    readonly property bool resultIsAudio: resultDelegate.modelData.isAudio
    readonly property bool resultIsOrphanedMd: resultDelegate.modelData.isOrphanedMd
    readonly property string resultTranscriptBody: resultDelegate.modelData.transcriptBody

    // Same snippet-building idiom as treeDelegate, fed from this result's own
    // transcriptBody — reuses the exact TextFormat helpers so the two never drift.
    readonly property string cleanedBody: (sidebarRoot.searchIncludeTranscriptions
                                             && resultDelegate.resultTranscriptBody)
        ? TextFormat.cleanTranscript(resultDelegate.resultTranscriptBody) : ""
    readonly property string snippetText: (sidebarRoot.searchQuery !== "" && resultDelegate.cleanedBody !== "")
        ? TextFormat.transcriptSnippet(resultDelegate.cleanedBody, sidebarRoot.searchQuery, 40,
                                       AppSettings.highlightColor) : ""
    readonly property bool showSnippet: resultDelegate.snippetText !== ""

    readonly property string displayName: {
        if (AppSettings.useLabelsSidebar && resultDelegate.resultLabel)
            return resultDelegate.resultLabel
        if (!AppSettings.hideExtensionsSidebar)
            return resultDelegate.resultName
        return TextFormat.stripExtension(resultDelegate.resultName)
    }

    implicitHeight: resultDelegate.showSnippet ? (28 + resultSnippetText.implicitHeight + 4) : 28

    HoverHandler {
        id: resultHover
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        id: resultRowTap
        onTapped: {
            // Mirrors treeDelegate's TapHandler (minus the isDir branch —
            // results are always leaves), including the forceActiveFocus() so
            // Up/Down works, and routing Shift/Ctrl through handleNoteClick.
            listView.forceActiveFocus()
            if (resultDelegate.resultIsAudio || resultDelegate.resultIsOrphanedMd)
                sidebarRoot.handleNoteClick(resultDelegate.resultPath, resultDelegate.resultIsOrphanedMd,
                                            resultRowTap.point.modifiers)
        }
    }
    // No anchors.rightMargin — resultDelegate.width already subtracts the full
    // AppSettings.sidebarRightMargin.
    HoverBorder {
        anchors.fill: parent
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
        active: resultHover.hovered
    }

    // Same persistent "currently open" highlight as treeDelegate.
    Rectangle {
        anchors.fill: parent
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
        visible: resultDelegate.resultPath === sidebarRoot.currentPath
        color: AppSettings.sidebarSelectionColor
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: AppSettings.sidebarSelectionBorderColor
    }

    // Same multi-select highlight as treeDelegate's.
    Rectangle {
        anchors.fill: parent
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
        visible: sidebarRoot.selectedPaths.length > 1
                 && sidebarRoot.selectedPaths.indexOf(resultDelegate.resultPath) >= 0
        color: "transparent"
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: AppSettings.highlightColor
    }

    Column {
        id: resultColumn
        x: AppSettings.sidebarEdgeLeftMargin + AppSettings.sidebarContentLeftMargin
        // A plain y binding, not a ternary between anchors.top and
        // anchors.verticalCenter: the anchoring engine doesn't reliably clear
        // and re-apply anchor lines when both are driven by the same changing
        // condition (showSnippet flipping true->false left the Column stuck at
        // its old top-anchored position). An ordinary property re-evaluation
        // has no conflicting-anchor-lines bug possible.
        y: resultDelegate.showSnippet ? 4 : (resultDelegate.height - resultColumn.implicitHeight) / 2
        spacing: 2

        Row {
            spacing: 6

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 8
                height: 8
                radius: 4
                // Same status-dot color logic as treeDelegate (Transcribed
                // green, orphaned-.md blue, otherwise untranscribed yellow).
                color: resultDelegate.resultStatus === 2 ? Theme.success
                       : (resultDelegate.resultIsOrphanedMd ? "#3498db" : "#f1c40f")
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                textFormat: Text.StyledText
                text: TextFormat.highlightMatch(resultDelegate.displayName, sidebarRoot.searchQuery,
                                                AppSettings.highlightColor)
                color: "white"
            }
        }

        Text {
            id: resultSnippetText
            visible: resultDelegate.showSnippet
            x: 14
            width: Math.max(10, resultDelegate.width - x - 16)
            textFormat: Text.StyledText
            text: resultDelegate.snippetText
            color: "#999999"
            font.pixelSize: 11
            wrapMode: Text.WordWrap

            HoverHandler {
                id: resultSnippetHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: sidebarRoot.transcriptMatchActivated(resultDelegate.resultPath,
                                                               resultDelegate.resultIsOrphanedMd,
                                                               sidebarRoot.searchQuery)
            }
            HoverBorder {
                anchors.fill: parent
                anchors.margins: -2
                active: resultSnippetHover.hovered
            }
        }
    }
}
