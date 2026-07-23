import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// The sidebar ribbon's bulk-action button flow: select-all, the bulk ASR
// actions (transcribe / relabel / chapters / detect-speakers / run-all),
// combine, the sort-mode menu, the search toggle, the destructive trio, and
// the multi-select count + clear. Extracted from Sidebar as a pure relocation;
// `sidebarRoot` is threaded in at the instantiation site so every
// sidebarRoot.* reference below resolves unchanged. The LHS name deliberately
// differs from the outer id (self-bind trap — see TreeRowDelegate.qml).
//
// A Flow, not a RowLayout: with 13+ items a fixed row silently squeezed and
// clipped buttons at a narrow sidebar width. Flow ignores Layout attached
// properties entirely, which is why every child sets width/height directly.
Flow {
    id: ribbonFlow
    property var sidebarRoot: null
    spacing: 4

    // A left-click on empty ribbon space — the gap to the right of the
    // "N selected" indicator, or between wrapped buttons — clears the
    // multi-selection, matching the empty-tree-space behavior
    // (treeEmptyLeftTap). childAt() is the ribbon's analogue of the
    // tree's cellAtPosition bail: it returns the direct Flow child under
    // the tap (a button/label/divider) or null for genuinely empty
    // space, so this only fires on empty space. That bail is
    // load-bearing, not defensive — overlapping TapHandlers aren't
    // mutually exclusive in Qt Quick, so this ALSO fires when a bulk
    // action button is tapped, and a file-action button must keep the
    // selection it's about to act on.
    TapHandler {
        id: ribbonEmptyLeftTap
        acceptedButtons: Qt.LeftButton
        onTapped: {
            const p = ribbonEmptyLeftTap.point.position
            if (ribbonFlow.childAt(p.x, p.y))
                return // a real control owns this tap
            sidebarRoot.deselectExceptCurrent()
        }
    }

    // Prepended select-all: mirrors the Ctrl+A shortcut above, and
    // is what makes the bulk buttons to its right ("apply to the
    // current selection", see refreshSelectionEligibility()) able to reach
    // "everything" again without requiring a manual Shift-click
    // drag across the whole tree.
    AppIconButton {
        id: selectAllButton
        objectName: "selectAllButton"
        iconName: "select-all"
        tooltip: qsTr("Select all (Ctrl+A)")
        enabled: sidebarRoot.visibleNotePaths().length > 0
        onTapped: sidebarRoot.selectAllVisible()
    }

    // Separates select-all from the bulk action buttons it feeds —
    // same border color as the rest of the app's thin-line dividers
    // (e.g. sortModeMenu's Popup border below).
    //
    // A full-height Item wrapping a shorter line, rather than a bare
    // 18px Rectangle with Layout.alignment: Qt.AlignVCenter — a Flow
    // has no per-item alignment, so a short child would hang off the
    // top of its row instead of centering against the 22px buttons.
    // Same shape as ribbonDeleteDivider below.
    Item {
        id: ribbonDivider
        objectName: "ribbonDivider"
        width: 1
        height: 22

        Rectangle {
            anchors.centerIn: parent
            width: 1
            height: 18
            color: Theme.border
        }
    }

    AppIconButton {
        id: transcribeAllButton
        objectName: "transcribeAllButton"
        iconName: "transcribe"
        tooltip: qsTr("Transcribe selected untranscribed audio")
        // Item 3 (reworked): the ribbon always narrows to the
        // current selection now (see refreshSelectionEligibility()) —
        // an empty selection means nothing to transcribe, so dim
        // like the other bulk buttons rather than silently no-op.
        enabled: sidebarRoot.transcriptionController && sidebarRoot.selectionTranscribePaths.length > 0
        onTapped: {
            const paths = sidebarRoot.selectionTranscribePaths
            // Model paths are raw local paths, not file:// URLs
            // (see VaultTreeModel's PathRole) — Q_INVOKABLE
            // QUrl params need the scheme or QUrl::toLocalFile()
            // silently returns empty.
            const urls = paths.map((p) => "file://" + p)
            sidebarRoot.transcriptionController.transcribeAll(urls, sidebarRoot.vaultPath, AppSettings.defaultModel,
                                                            AppSettings.autoLabelEnabled,
                                                            AppSettings.restrictTagsToExisting,
                                                            AppSettings.tagReuseBias, AppSettings.wordiness)
        }
    }

    // Wave D item 7: bulk "regenerate auto-title" mirroring
    // transcribeAllButton above, over already-transcribed files
    // instead of untranscribed ones.
    AppIconButton {
        id: relabelAllButton
        iconName: "relabel"
        tooltip: qsTr("Regenerate titles for selected files")
        // Item 3 (reworked): same selection-narrowing as
        // transcribeAllButton above.
        enabled: sidebarRoot.transcriptionController && sidebarRoot.selectionRelabelPaths.length > 0
        onTapped: {
            const paths = sidebarRoot.selectionRelabelPaths
            const urls = paths.map((p) => "file://" + p)
            sidebarRoot.transcriptionController.relabelAll(urls, sidebarRoot.vaultPath,
                                                         AppSettings.restrictTagsToExisting,
                                                         AppSettings.tagReuseBias, AppSettings.wordiness)
        }
    }

    // Bulk "regenerate chapters", same shape as relabelAllButton
    // above but over chaptersPaths (a superset of transcribedPaths —
    // see treeDelegate.chaptersEligible for why orphaned .md rows
    // qualify too) and calls the chapters-only bulk entry point,
    // generateChaptersAll().
    AppIconButton {
        id: chaptersAllButton
        iconName: "chapters"
        tooltip: qsTr("Regenerate chapters for selected files")
        // Item 3 (reworked): same selection-narrowing as
        // transcribeAllButton above.
        enabled: sidebarRoot.transcriptionController && sidebarRoot.selectionChaptersPaths.length > 0
        onTapped: {
            const paths = sidebarRoot.selectionChaptersPaths
            const urls = paths.map((p) => "file://" + p)
            sidebarRoot.transcriptionController.generateChaptersAll(urls, sidebarRoot.vaultPath,
                                                                  AppSettings.chapterDensity)
        }
    }

    // Item 52 (CLAUDE.md per-file-action parity): bulk "detect speakers"
    // — runs standalone diarization over the transcribed audio files
    // (same transcribedPaths eligibility relabelAllButton uses, since
    // diarization needs an existing transcript), calling
    // diarizeFilesForPaths(). Mirrors MainPanel's detectSpeakersButton.
    AppIconButton {
        id: detectSpeakersAllButton
        iconName: "speakers"
        iconSize: 14
        tooltip: qsTr("Detect speakers for selected files")
        // Same selection-narrowing as the other ribbon buttons.
        enabled: sidebarRoot.transcriptionController && sidebarRoot.selectionRelabelPaths.length > 0
        onTapped: {
            const paths = sidebarRoot.selectionRelabelPaths
            const urls = paths.map((p) => "file://" + p)
            sidebarRoot.transcriptionController.diarizeFilesForPaths(urls, sidebarRoot.vaultPath,
                                                                AppSettings.diarizationSegmentationModel,
                                                                AppSettings.diarizationEmbeddingModel,
                                                                AppSettings.diarizationSensitivity)
        }
    }

    // Items 24/53: bulk "run all jobs" — the full transcribe->label->
    // chapters pipeline for every untranscribed file (same eligibility
    // as transcribeAllButton above: an already-transcribed file's
    // label/chapters are the relabelAll/chaptersAll buttons' job).
    // runAllJobs() itself skips the transcribe step for anything that
    // slips through already-transcribed, so it's safe regardless.
    AppIconButton {
        id: runAllButton
        iconName: "run-all"
        tooltip: qsTr("Run all jobs on selected files (transcribe, label, chapters)")
        enabled: sidebarRoot.transcriptionController
                 && sidebarRoot.selectionTranscribePaths.length > 0
        onTapped: {
            const paths = sidebarRoot.selectionTranscribePaths
            if (paths.length === 0)
                return
            const urls = paths.map((p) => "file://" + p)
            sidebarRoot.transcriptionController.runAllJobsForPaths(urls, sidebarRoot.vaultPath,
                                                                AppSettings.defaultModel,
                                                                AppSettings.restrictTagsToExisting,
                                                                AppSettings.tagReuseBias,
                                                                AppSettings.wordiness,
                                                                AppSettings.chapterDensity,
                                                                AppSettings.diarizationSegmentationModel,
                                                                AppSettings.diarizationEmbeddingModel,
                                                                AppSettings.diarizationSensitivity)
        }
    }

    // Separates the per-file bulk pipeline (transcribe/label/chapters/
    // speakers/run-all, all narrowed to the current selection via the
    // selection*Paths) from combine, which is a different kind of
    // action: a vault-wide scan over allAudioPaths() that ignores the
    // selection entirely. Same shape/reasoning as ribbonDivider above.
    Item {
        id: ribbonCombineDivider
        objectName: "ribbonCombineDivider"
        width: 1
        height: 22

        Rectangle {
            anchors.centerIn: parent
            width: 1
            height: 18
            color: Theme.border
        }
    }

    // Split-recording combine, run on demand rather than
    // automatically: it used to fire on every vault-tree rebuild
    // (open + every filesystem change, via VaultTreeModel's
    // modelReset), silently merging files the moment a recorder
    // dropped in a new part. That surprised users watching a vault
    // fill up mid-recording, so it's now an explicit action here —
    // see TranscriptionController::runCombineScan()'s doc comment.
    AppIconButton {
        id: combineButton
        iconName: "combine"
        tooltip: qsTr("Combine split recordings")
        enabled: sidebarRoot.transcriptionController && AppSettings.combineRules.length > 0
        onTapped: {
            sidebarRoot.transcriptionController.runCombineScan(
                        AppSettings.combineRules, sidebarRoot.model.allAudioPaths(),
                        sidebarRoot.vaultPath, AppSettings.deleteOriginalsAfterCombine)
        }
    }

    // File-tree sort mode (Name / Date Modified newest-first /
    // Date Modified oldest-first) — see AppSettings.sortMode's doc
    // comment. Combine-candidate clustering (refreshTreeOrdering())
    // is automatic whenever combine rules exist and composes with
    // whichever of these is active, so there's no separate toggle
    // for it here.
    Item {
        id: sortModeButton
        width: 22
        height: 22

        HoverHandler {
            id: sortModeHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            onTapped: sortModeMenu.visible ? sortModeMenu.close() : sortModeMenu.open()
        }
        HoverBorder {
            anchors.fill: parent
            active: sortModeHover.hovered
        }
        AppIcon {
            anchors.centerIn: parent
            name: "sort"
            color: Theme.textPrimary
            iconSize: 15
        }

        ToolTip.visible: sortModeHover.hovered
        ToolTip.text: qsTr("Sort files")
        ToolTip.delay: 500

        // Same custom-Popup shape as TitleBar.qml's "⋯" overflow
        // menu (moreActionsMenu) — this codebase has no QQC2
        // Menu/MenuItem usage anywhere, just a Popup anchored off its
        // trigger button with plain Item rows inside.
        Popup {
            id: sortModeMenu
            x: 0
            y: sortModeButton.height + 4
            width: 220
            padding: 4
            modal: true
            dim: true
            focus: false
            closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape

            background: Rectangle {
                color: Theme.surfaceDialog
                radius: AppSettings.borderRadius
                border.width: 1
                border.color: Theme.border
            }

            Column {
                width: sortModeMenu.width - sortModeMenu.padding * 2
                spacing: 2

                Repeater {
                    model: [
                        { value: "name", label: qsTr("Name") },
                        { value: "dateDesc", label: qsTr("Date modified (newest first)") },
                        { value: "dateAsc", label: qsTr("Date modified (oldest first)") }
                    ]
                    delegate: Item {
                        id: sortOptionRow
                        required property var modelData
                        width: parent.width
                        height: 28

                        HoverHandler {
                            id: sortOptionHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            onTapped: {
                                AppSettings.sortMode = sortOptionRow.modelData.value
                                sortModeMenu.close()
                            }
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: AppSettings.borderRadius
                            color: sortOptionHover.hovered ? Theme.surfaceHover : "transparent"
                        }
                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: (AppSettings.sortMode === sortOptionRow.modelData.value ? "✓ " : "")
                                  + sortOptionRow.modelData.label
                            color: "white"
                            font.pixelSize: 12
                        }
                    }
                }
            }
        }
    }

    AppIconButton {
        id: searchToggleButton
        iconName: "search"
        onTapped: {
            sidebarRoot.searchExpanded = true
            sidebarRoot.focusSearchField()
        }
    }

    // Separates the constructive actions from the destructive trio
    // below — same shape/reasoning as ribbonDivider above.
    Item {
        id: ribbonDeleteDivider
        objectName: "ribbonDeleteDivider"
        width: 1
        height: 22

        Rectangle {
            anchors.centerIn: parent
            width: 1
            height: 18
            color: Theme.border
        }
    }

    // The destructive trio — CLAUDE.md per-file-action parity with
    // MainPanel's deleteTranscriptButton/deleteAudioOnlyButton/
    // deleteFileButton, which had no bulk counterpart here.
    //
    // A bulk ribbon delete was deliberately left out once before, as
    // "too dangerous" (see tst_styleguide's
    // delete_transcriptAndFile_wiredInSidebarAndMainPanel). That call
    // was made while every delete was a confirm-then-maybe-permanent
    // one. It isn't anymore: these are the same trash-first calls the
    // context menu uses, so the recoverable case (all but a filesystem
    // refusal) costs an Undo click from the snackbar rather than the
    // file — and the Delete key needs a visible affordance to advertise
    // anyway. Destructive-red icons (#e07a7a), matching MainPanel's.
    //
    // Eligibility mirrors MainPanel's gating exactly: the transcript/
    // audio-only pair need a transcribed AUDIO row (transcribedPaths —
    // the same map relabelAll/detectSpeakersAll use), since "keep the
    // other half" is meaningless without both halves. "Delete file"
    // takes any selected note.
    AppIconButton {
        id: deleteTranscriptAllButton
        objectName: "deleteTranscriptAllButton"
        iconName: "delete-transcript"
        iconSize: 14
        iconColor: deleteTranscriptAllButton.enabled ? Theme.danger : Theme.textDisabled
        tooltip: qsTr("Delete transcripts for selected files (keep audio)")
        enabled: sidebarRoot.model && sidebarRoot.selectionRelabelPaths.length > 0
        onTapped: sidebarRoot.beginDeleteTranscript(
                      sidebarRoot.selectionRelabelPaths)
    }

    AppIconButton {
        id: deleteAudioOnlyAllButton
        objectName: "deleteAudioOnlyAllButton"
        iconName: "delete-audio"
        iconSize: 14
        iconColor: deleteAudioOnlyAllButton.enabled ? Theme.danger : Theme.textDisabled
        tooltip: qsTr("Delete audio for selected files (keep transcripts)")
        enabled: sidebarRoot.model && sidebarRoot.selectionRelabelPaths.length > 0
        onTapped: sidebarRoot.beginDeleteAudioOnly(
                      sidebarRoot.selectionRelabelPaths)
    }

    // The Delete key's visible affordance — see the Delete Shortcut
    // above, which runs this same beginDeleteFile(selection).
    AppIconButton {
        id: deleteFileAllButton
        objectName: "deleteFileAllButton"
        iconName: "delete-file"
        iconSize: 14
        iconColor: deleteFileAllButton.enabled ? Theme.danger : Theme.textDisabled
        tooltip: qsTr("Delete selected files (Delete)")
        enabled: sidebarRoot.model && sidebarRoot.selectedPaths.length > 0
        onTapped: sidebarRoot.beginDeleteFile(sidebarRoot.selectedPaths.slice())
    }

    // Item 3: "N selected" indicator, only shown once a genuine
    // multi-selection exists (2+ rows) — a single selected row is
    // already communicated by the existing O7 "currently open"
    // outline in the tree below, so showing "1 selected" here too
    // would be redundant. Same typography as BottomBar.qml's
    // tasksLabel (the closest existing "small status readout next
    // to a toolbar" precedent in this codebase).
    //
    // Flows along with the buttons rather than being pushed to the far
    // right by a Layout.fillWidth spacer, as it was under the old
    // RowLayout: a Flow gives its items no alignment and a
    // fillWidth spacer nothing to fill, so a spacer would just wrap the
    // label onto a line of its own. The explicit height centers it
    // against the 22px buttons.
    Label {
        id: multiSelectCountLabel
        visible: sidebarRoot.selectedPaths.length > 1
        text: qsTr("%1 selected").arg(sidebarRoot.selectedPaths.length)
        color: Theme.textMuted
        font.pixelSize: 11
        height: 22
        verticalAlignment: Text.AlignVCenter
    }

    // Explicit deselect: clears the multi-selection back to just the
    // open file (deselectExceptCurrent), the same thing Escape and a
    // click on empty sidebar space do. Shown only alongside the "N
    // selected" indicator (2+ rows) — with nothing multi-selected
    // there's nothing to clear. Standard icon-button convention
    // (AppIconButton).
    AppIconButton {
        id: deselectButton
        objectName: "deselectButton"
        iconName: "close"
        tooltip: qsTr("Clear selection (Esc)")
        visible: sidebarRoot.selectedPaths.length > 1
        onTapped: sidebarRoot.deselectExceptCurrent()
    }

}
