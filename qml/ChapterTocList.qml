import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// Collapsible chapter list (table of contents): tap a timestamp to seek, click
// a title to rename, add at the playhead, delete. Extracted from MainPanel;
// `panel` is threaded in at the instantiation site. Shown for an orphaned .md
// too (chapters read the .md body, never audio) — seeking is just inert there
// since panel.player has no source.
ColumnLayout {
    id: chapterListColumn
    property var panel: null
    spacing: 4

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        AppIcon {
            name: panel.chaptersExpanded ? "collapse" : "expand"
            iconSize: 11
            color: Theme.textMuted

            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: panel.chaptersExpanded = !panel.chaptersExpanded }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Chapters (%1)").arg(panel.chapters.length)
            color: Theme.textMuted
            font.pixelSize: 11
            font.bold: true

            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: panel.chaptersExpanded = !panel.chaptersExpanded }
        }

        // Add a MANUAL chapter at the current playhead. Player-backed only (an
        // orphaned .md has no audio to position against). Item+TapHandler, not
        // a focusable Button, so tapping can't steal focus from a title being
        // renamed — it just adds and drops into editing the new one.
        Item {
            id: addChapterButton
            Layout.preferredWidth: 18
            Layout.preferredHeight: 18
            visible: panel.hasAudio

            ToolTip.visible: addChapterHover.hovered
            ToolTip.text: qsTr("Add chapter at playhead")
            ToolTip.delay: 500

            HoverHandler {
                id: addChapterHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler { onTapped: panel.addChapterAtPlayhead() }
            HoverBorder {
                anchors.fill: parent
                active: addChapterHover.hovered
            }
            AppIcon {
                anchors.centerIn: parent
                name: "add"
                iconSize: 14
                color: "#8fdf9f"
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 20
        spacing: 2
        visible: panel.chaptersExpanded

        Repeater {
            model: panel.chapters

            RowLayout {
                id: chapterRow
                required property var modelData
                Layout.fillWidth: true
                spacing: 8

                // Pointer cursor + click-to-seek scoped to JUST the timestamp;
                // the title gets its own independent I-beam/selectable treatment.
                Label {
                    id: chapterTimestampLabel
                    text: panel.formatGutterTimestamp(chapterRow.modelData.t0)
                    color: "#777777"
                    font.pixelSize: 10

                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: panel.player.position = chapterRow.modelData.t0 * 1000 }
                }

                // Manual chapters get a subtle pencil marker (readChapters()'
                // `manual` bool), distinguishing them from auto-generated ones.
                AppIcon {
                    visible: !!chapterRow.modelData.manual
                    name: "edit"
                    iconSize: 10
                    color: "#8fdf9f"
                }

                // Always-editable (StyleGuide.md "Always-editable text"): one
                // TextField present at all times, plain via UnderlineField until
                // hovered/focused, committing on blur/Return through
                // commitChapterTitle -> setChapterTitle. The transcript heading
                // keeps the read-only-until-armed idiom instead — its title is
                // swept into the body selection, which an always-editable field
                // would swallow (StyleGuide's carve-out).
                TextField {
                    id: tocChapterTitle
                    objectName: "tocChapterTitle"
                    Layout.fillWidth: true
                    color: "#cccccc"
                    font.pixelSize: 11
                    padding: 0
                    leftPadding: 0
                    rightPadding: 0
                    topPadding: 0
                    bottomPadding: 0
                    text: chapterRow.modelData.title

                    background: UnderlineField { implicitHeight: 0; plainUntilHovered: true; focused: tocChapterTitle.activeFocus }

                    onAccepted: panel.commitChapterTitle(chapterRow.modelData.t0,
                                                         chapterRow.modelData.title,
                                                         tocChapterTitle.text)
                    onActiveFocusChanged: if (!tocChapterTitle.activeFocus)
                        panel.commitChapterTitle(chapterRow.modelData.t0,
                                                 chapterRow.modelData.title,
                                                 tocChapterTitle.text)

                    Keys.onEscapePressed: tocChapterTitle.text = chapterRow.modelData.title

                    HoverHandler { cursorShape: Qt.IBeamCursor }
                }

                // Delete this chapter marker. Item+TapHandler (not a focusable
                // Button) for the same focus-grab reason as addChapterButton.
                Item {
                    id: deleteChapterButton
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16

                    ToolTip.visible: deleteChapterHover.hovered
                    ToolTip.text: qsTr("Delete chapter")
                    ToolTip.delay: 500

                    HoverHandler {
                        id: deleteChapterHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: panel.deleteChapterAt(chapterRow.modelData.t0,
                                                        chapterRow.modelData.title,
                                                        !!chapterRow.modelData.manual)
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: deleteChapterHover.hovered
                    }
                    AppIcon {
                        anchors.centerIn: parent
                        name: "close"
                        iconSize: 13
                        color: "#c07a7a"
                    }
                }
            }
        }
    }
}
