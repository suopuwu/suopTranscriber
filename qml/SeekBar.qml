import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// The playback / seek bar: play-pause, follow-playback + skip-silence
// toggles, position/duration label, and the seek Slider with its custom
// handle and per-chapter tick Repeater. Extracted verbatim from MainPanel's
// sticky header (the RowLayout gated on hasAudio); see `property var
// panel` below for how the panel.* references still resolve.
RowLayout {
    // The MainPanel root, threaded so the many panel.* references
    // inside this block (player, chapters, formatDuration,
    // scrollActiveSentenceToTop, hasAudio, hasPlayableSource) resolve
    // exactly as they did inline. Bound at the instantiation site.
    property var panel: null

    Layout.fillWidth: true
    spacing: 12
    visible: panel.hasAudio

    Item {
        id: playPauseButton
        Layout.preferredWidth: 28
        Layout.preferredHeight: 28
        enabled: panel.hasPlayableSource

        HoverHandler {
            id: playPauseHover
            cursorShape: Qt.PointingHandCursor
            enabled: playPauseButton.enabled
        }
        TapHandler {
            enabled: playPauseButton.enabled
            onTapped: panel.player.togglePlay()
        }
        // Distinct button background (not just the bare panel) —
        // fixed dark shades so it reads as a button regardless of
        // the user's configurable panel background color.
        Rectangle {
            anchors.fill: parent
            radius: AppSettings.borderRadius
            color: (playPauseHover.hovered && playPauseButton.enabled) ? Theme.surfaceHover : Theme.surface
        }
        HoverBorder {
            anchors.fill: parent
            active: playPauseButton.enabled && playPauseHover.hovered
        }
        // The play/pause icon. Under the classic pack these resolve
        // to the hand-drawn PlayGlyph/PauseGlyph shapes (a font "▶"
        // can't be reliably centered on its triangle — see
        // PlayGlyph.qml); under material, to the play_arrow/pause SVGs.
        AppIcon {
            id: playPauseIcon
            anchors.centerIn: parent
            name: (panel.player && panel.player.playing) ? "pause" : "play"
            iconSize: 14
            color: playPauseButton.enabled ? "white" : Theme.textDisabled
        }
    }

    // Auto-scroll toggle: when on (the default), the transcript follows
    // playback, pinning the active sentence near the top of the view
    // (scrollActiveSentenceToTop). A per-vault preference so someone who
    // prefers to scroll manually isn't fought by the viewport every
    // sentence. Lives in the playback row (visible: hasAudio), so it
    // only shows when there's audio to follow.
    Item {
        id: followPlaybackButton
        Layout.preferredWidth: 28
        Layout.preferredHeight: 28

        ToolTip.visible: followPlaybackHover.hovered
        ToolTip.text: AppSettings.followPlaybackScroll
            ? qsTr("Auto-scroll on — transcript follows playback")
            : qsTr("Auto-scroll off — scroll manually")
        ToolTip.delay: 500

        HoverHandler {
            id: followPlaybackHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            onTapped: {
                AppSettings.followPlaybackScroll = !AppSettings.followPlaybackScroll
                // Snap to the current sentence the instant it's turned
                // back on, rather than waiting for the next boundary.
                if (AppSettings.followPlaybackScroll)
                    panel.scrollActiveSentenceToTop()
            }
        }
        Rectangle {
            anchors.fill: parent
            radius: AppSettings.borderRadius
            color: followPlaybackHover.hovered ? Theme.surfaceHover : Theme.surface
        }
        HoverBorder {
            anchors.fill: parent
            active: followPlaybackHover.hovered
        }
        AppIcon {
            anchors.centerIn: parent
            name: "follow-playback"
            iconSize: 14
            // Lit when following, dimmed when off — same on/off palette
            // as the play button's enabled/disabled icon.
            color: AppSettings.followPlaybackScroll ? "white" : Theme.textDisabled
        }
    }

    // Skip-silence toggle (silence phase 2, queue item 084): mirrors
    // followPlaybackButton immediately above — same 28x28 box,
    // hover/tap idiom, background/border treatment — just gating a
    // different playback behavior (auto-seek past long stored
    // silence spans, see the Connections on player.positionChanged
    // above) instead of auto-scroll. Lives in the same playback row
    // (visible: hasAudio) for the same reason: it only means
    // anything when there's audio and stored silence spans to skip.
    Item {
        id: skipSilenceButton
        Layout.preferredWidth: 28
        Layout.preferredHeight: 28

        ToolTip.visible: skipSilenceHover.hovered
        ToolTip.text: AppSettings.skipSilenceEnabled
            ? qsTr("Skip silence on — jumps past stored gaps ≥ %1s").arg(AppSettings.skipSilenceThresholdSeconds)
            : qsTr("Skip silence off")
        ToolTip.delay: 500

        HoverHandler {
            id: skipSilenceHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            onTapped: AppSettings.skipSilenceEnabled = !AppSettings.skipSilenceEnabled
        }
        Rectangle {
            anchors.fill: parent
            radius: AppSettings.borderRadius
            color: skipSilenceHover.hovered ? Theme.surfaceHover : Theme.surface
        }
        HoverBorder {
            anchors.fill: parent
            active: skipSilenceHover.hovered
        }
        AppIcon {
            anchors.centerIn: parent
            name: "skip-silence"
            iconSize: 14
            color: AppSettings.skipSilenceEnabled ? "white" : Theme.textDisabled
        }
    }

    // Threshold (seconds) — only worth showing once skip-silence is
    // actually on. A compact inline version of the SettingsPopup
    // numeric-setting idiom (e.g. rimWidth's Slider + validated
    // TextField): StyleGuide.md "Text fields" requires every
    // TextField use the UnderlineField background, never a bare one.
    // Vault-scoped like every other AppSettings value — edits here
    // persist the same debounced-save way rimWidth's does.
    RowLayout {
        id: skipSilenceThresholdRow
        visible: AppSettings.skipSilenceEnabled
        spacing: 4

        Slider {
            id: skipSilenceThresholdSlider
            Layout.preferredWidth: 60
            from: 0.5
            to: 10
            stepSize: 0.5
            hoverEnabled: true
            value: AppSettings.skipSilenceThresholdSeconds
            onMoved: AppSettings.skipSilenceThresholdSeconds = value

            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
            HoverBorder {
                anchors.fill: parent
                active: skipSilenceThresholdSlider.hovered
            }
        }
        TextField {
            id: skipSilenceThresholdField
            Layout.preferredWidth: 40
            horizontalAlignment: TextInput.AlignRight
            hoverEnabled: true
            validator: DoubleValidator {
                bottom: skipSilenceThresholdSlider.from
                top: skipSilenceThresholdSlider.to
                decimals: 1
            }
            onEditingFinished: {
                const v = parseFloat(text)
                if (!isNaN(v)) {
                    AppSettings.skipSilenceThresholdSeconds = Math.max(
                        skipSilenceThresholdSlider.from,
                        Math.min(skipSilenceThresholdSlider.to, v))
                }
                text = AppSettings.skipSilenceThresholdSeconds.toFixed(1)
            }

            background: UnderlineField { focused: skipSilenceThresholdField.activeFocus }

            Binding {
                target: skipSilenceThresholdField
                property: "text"
                value: AppSettings.skipSilenceThresholdSeconds.toFixed(1)
                when: !skipSilenceThresholdField.activeFocus
            }

            // Item 13 (StyleGuide "Cursor affordances"): an editable
            // text control gets the I-beam, not a pointing hand — the
            // rimWidthField sibling in SettingsPopup.qml uses a
            // pointing hand instead, but this file's own guard
            // (mainPanel_editableTextControlsHaveIBeamCursor) requires
            // IBeam for every TextField declared here.
            HoverHandler {
                cursorShape: Qt.IBeamCursor
            }
            HoverBorder {
                anchors.fill: parent
                active: skipSilenceThresholdField.hovered
            }
        }
        Label {
            text: qsTr("s")
            color: Theme.textMuted
            font.pixelSize: 11
        }
    }

    // Playback position/total duration — own display on the
    // playback bar itself, deliberately separate from BottomBar's
    // task progress text (which only ever shows task elapsed/ETA,
    // never anything playback-related).
    Label {
        text: panel.player
            ? (panel.formatDuration(panel.player.position) + " / " + panel.formatDuration(panel.player.duration))
            : "0:00 / 0:00"
        color: Theme.textMuted
        font.pixelSize: 11
    }

    // Item 10: the hover highlight rings only the handle/puck, not
    // the whole track — custom `handle:` so we own that geometry
    // instead of the default QQC2 style's whole-control HoverBorder.
    Slider {
        id: seekSlider
        Layout.fillWidth: true
        hoverEnabled: true
        // No file open (or the text-only orphaned-md view) means
        // nothing to scrub — mirror the play/pause button so the track
        // can't be dragged with no audio loaded. Shares the same
        // hasPlayableSource binding, so a deletion that closes the file
        // greys this out the same instant it greys the play button.
        enabled: panel.hasPlayableSource
        // The custom handle: below doesn't dim itself on disable, so
        // fade the whole track to read as inert (same affordance as the
        // play button greying its icon).
        opacity: enabled ? 1 : 0.4
        from: 0
        to: panel.player ? Math.max(panel.player.duration, 1) : 1
        value: panel.player ? panel.player.position : 0
        onMoved: panel.player.position = value

        // Item 5 (Wave E): undo Wave D item 4's border-on-hover puck
        // (reported as not clear enough) — back to a separate
        // expanding ring around the puck (Wave C item 5's tightened
        // +2px/1px-gap version), which reads more clearly as a hover
        // state at a glance.
        handle: Item {
            x: seekSlider.leftPadding + seekSlider.visualPosition * (seekSlider.availableWidth - width)
            y: seekSlider.topPadding + seekSlider.availableHeight / 2 - height / 2
            implicitWidth: 16
            implicitHeight: 16

            Rectangle {
                anchors.centerIn: parent
                width: parent.width + 2
                height: parent.height + 2
                radius: width / 2
                color: "transparent"
                border.width: 1
                border.color: AppSettings.accentColor
                opacity: seekSlider.hovered ? 1 : 0
                Behavior on opacity {
                    NumberAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 }
                }
            }
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: seekSlider.pressed ? Theme.textPrimary : "#ffffff"
                border.width: 1
                border.color: Theme.textSecondary
            }
        }

        // Feature 3b: one tick mark per chapter (see
        // panel.chapters/readChapters()), positioned with the
        // EXACT SAME leftPadding/availableWidth math as the handle
        // above (:1071) — visualPosition there is
        // (value - from) / (to - from) with value/to in player
        // position milliseconds, so a chapter's t0 (seconds) is
        // scaled to milliseconds here to land in the same space.
        // Gated on a real duration so a not-yet-loaded player (to
        // falls back to 1, see seekSlider.to above) never produces
        // broken 0/0-style math.
        Repeater {
            model: (panel.player && panel.player.duration > 0) ? panel.chapters : []

            Rectangle {
                id: chapterTick
                required property var modelData
                x: seekSlider.leftPadding
                   + (chapterTick.modelData.t0 * 1000 / panel.player.duration) * (seekSlider.availableWidth - width)
                y: seekSlider.topPadding + seekSlider.availableHeight / 2 - height / 2
                width: 2
                height: 10
                radius: 1
                color: "#ffffff"
                opacity: chapterTickArea.containsMouse ? 0.9 : 0.55

                ToolTip.visible: chapterTickArea.containsMouse
                ToolTip.text: chapterTick.modelData.title
                ToolTip.delay: 500

                MouseArea {
                    id: chapterTickArea
                    anchors.fill: parent
                    anchors.margins: -3
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.player.position = chapterTick.modelData.t0 * 1000
                }
            }
        }
    }
}
